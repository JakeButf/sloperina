/* Where Link voids out (FINDINGS §9.8).
 *
 * The void check doesn't look at the collision. It looks at the ground map: space cut into chunks of 256^3
 * one-unit cells, each chunk a sparse octree of 8 levels, a cell set where there's walkable space just over
 * the ground. Each frame the game casts down from Link into it (probe A) and keeps the nearest cell it has
 * found round Link (probe C). Link voids when
 *   - nothing is under Link and that nearest cell is 8 or more away level (player flags +0x13D0 bit 25), or
 *   - the cell under Link is a deadly one: its record's field 20 is set and field 21 is over 3 (bit 19).
 * Other voids aren't in the map: the areas placed to void Link (airwall.c draws those, from data.c; the holes
 * down into grottos are among them) and floors tagged Abyss (not drawn).
 *
 * Drawn: the edges of the mapped ground (past them probe A has nothing to hit); the deadly cells as columns,
 * from the cell up to the next mapped one over it (anywhere in one the cast lands on the cell); and a fence
 * of 8 round the point probe C holds now, which is where Link voids while off the map. The rule only
 * measures level, so the fence has no top or bottom: a few units of it are drawn at Link's height.
 * The cells round Link are read out of the octrees every so often, not every frame. */
#include "voidmap.h"

#include "lang.h"
#include "link.h"
#include "sys.h"
#include "z64hd.h"

float sinf(float x); /* from sdk */
float cosf(float x);
float sqrtf(float x);

bool voidmap_shown;
bool voidmap_ground;
bool voidmap_deadly = true, voidmap_ring = true;
bool voidmap_areas = true;
bool voidmap_xray;

/* the ground map: [[[[main + voxel_mgr_ptr]] + this] + this] is its manager */
#define MGR_OWNER 0xC0
#define MGR_IN_OWNER 0x170
#define MGR_LIST_HEAD 0x10  /* a linked list of the loaded areas */
#define MGR_LIST_FIRST 0x18
#define MGR_NODE_OFFSET 0x24 /* i32: where the list's node is in an area */
#define NODE_NEXT 0x08
#define AREA_HOLDER 0x90
#define HOLDER_COUNT 0x28   /* i32 */
#define HOLDER_CHUNKS 0x30  /* pointers to the chunks */
#define MAX_AREAS 4096      /* only so a damaged list can't loop for ever */
#define MAX_CHUNKS 4096     /* in all the areas together, for the same reason */
/* a chunk: its origin (3 x i32) at +0, then per level a record block and the nodes */
#define CHUNK_RECORDS 0x28  /* + level * RECORDS_SIZE: +0x10 data, +0x18 bits a record, +0x1C format */
#define RECORDS_SIZE 0x28
#define RECORDS_DATA 0x10
#define RECORDS_BITS 0x18
#define RECORDS_FORMAT 0x1C
#define CHUNK_NODES 0x168   /* + level * 8: that level's node words */
#define LEVELS 8
#define CHUNK_CELLS 256
#define CHUNK_MARGIN 3      /* chunks overlap: one only answers for cells 3..253 of its own */
#define NODE_CHILDREN 8
#define NODE_CHILD_MASK 0xFFu /* a node word: which of its 8 children exist, and where the first one is */
#define NODE_FIRST_SHIFT 8
/* the record formats (main + voxel_formats): which fields a record has and where */
#define FORMAT_SIZE 0x74
#define FORMAT_COUNT 7      /* a format past these reads as format 0, as the game does */
#define FORMAT_PRESENT 0x04 /* + field: u8, 1 if the format has it */
#define FORMAT_BIT 0x1C     /* + field * 4: i32, its first bit in a record */
#define FIELD_DEADLY 20
#define FIELD_DEADLY_BITS 1
#define FIELD_DEPTH 21      /* times 2/3 */
#define FIELD_DEPTH_BITS 4
#define DEADLY_OVER 4       /* field 21 over 3.0 is 5 or more of its steps */

/* Link's: [[actor + 0x270] + ...] */
#define ACTOR_PARTS 0x270
#define PARTS_PLAYER 0x358
#define PARTS_PROBE 0x460
#define ACTOR_POS 0x31C         /* Link's feet */
#define PLAYER_GROUND_FLAGS 0x13D0
#define FLAG_DEADLY_CELL 0x80000u
#define FLAG_FAR_FROM_GROUND 0x2000000u
#define PROBE_FLAGS 0xA8        /* u32 */
#define PROBE_OUTSIDE 0x40000u  /* Link is more than 5 outside the probe's own box: sets the deadly flag too */
#define PROBE_A_POINT 0x360     /* where the cast down landed */
#define PROBE_A_HIT 0x37C       /* u8 bit 0: mapped ground under Link */
#define PROBE_C_POINT 0xBA0     /* the nearest cell found */
#define PROBE_C_HIT 0xBBC       /* u8 bit 0 */
#define PROBE_C_LEVEL 0x115C    /* f32: how far it is, level */
#define VOID_DISTANCE 8.0f

/* the cells kept: a box round Link. The cast down has no end to speak of (5000), so a deadly cell voids Link
 * from far overhead: the box reaches a long way under Link */
#define BOX_X 48
#define BOX_Y 64
#define BOX_Z 48
#define BOX_BELOW 48 /* of BOX_Y */
#define BOX_CELLS (BOX_X * BOX_Y * BOX_Z)
#define REBUILD_MOVED 4     /* cells Link has moved from the box's middle */
#define REBUILD_FRAMES 120  /* and this often anyway: chunks load as Link goes */
#define RETRY_FRAMES 30     /* while no chunk is loaded, how often to look again */
#define MAX_EDGES 6000      /* before the nearest VOIDMAP_MAX_LINES of them are kept */
#define MAX_WALLS 2000      /* the columns' sides, before the nearest VOIDMAP_MAX_FACES are kept */
#define LIFT 0.12f          /* over the cell's floor, so the ground doesn't cut the lines */
#define POST_SLOTS 8192     /* a rebuild's upright lines, each once: a hash table, a power of two */
#define POST_SLOT_BITS 13
#define MAX_POSTS (POST_SLOTS / 2)
#define HASH_MULTIPLIER 2654435761u /* Knuth's, 2^32 / the golden ratio */
#define EDGE_STEPS 32       /* the edges are sorted by distance into this many steps */
#define HEIGHT_COUNTS 0.25f /* of an edge's height off Link's in that distance: a column's foot far under Link
                               matters as much as what's beside Link */
#define WALL_RANGE 24.0f    /* the columns' sides farther than this aren't filled in */
#define WALL_RINGS 4        /* they're taken a ring of distance at a time, the nearest first */
#define RING_SEGMENTS 32
#define RING_POST_EVERY 4   /* an upright at every 4th segment: eight in all */
#define RING_REACH 2.0f     /* the fence, this far over and under Link's feet */
#define TWO_PI 6.2831853f

typedef struct { float a[3], b[3]; uint8_t colour; } edge_t;

static const int box_size[3] = { BOX_X, BOX_Y, BOX_Z };
static const int box_middle[3] = { BOX_X / 2, BOX_BELOW, BOX_Z / 2 }; /* where Link's cell is in it */

static uint32_t mapped[BOX_CELLS / 32], deadly[BOX_CELLS / 32];
static bool layer_mapped[BOX_Y], layer_deadly[BOX_Y]; /* whether a layer has any such cell */
static int box_min[3];
static bool box_valid;
static edge_t edges[MAX_EDGES];
static uint8_t edge_step[MAX_EDGES]; /* nearest_edges' */
static int n_edges;
static aw_face_t walls[MAX_WALLS];
static int n_walls;
static uint32_t posts[POST_SLOTS]; /* the uprights made this rebuild; 0: free (a post's x is never 0) */
static int n_posts;
static bool full_logged;           /* the log has said the edges or walls ran out, for this map's stretch */
static struct { bool on; uint8_t colour; float point[3], y; } fence; /* this frame's (ring) */
static uint32_t frames, built_frame;
static bool built_ground, built_deadly;
static uint32_t void_flags_were;   /* voidmap_frame's: the void flags last frame */

/* --- reading the ground map ----------------------------------------------------------------------- */

static bool read_i32(uintptr_t addr, int32_t *out)
{
  return sys_read32(addr, (uint32_t *)out);
}

static uintptr_t map_manager(void)
{
  if (!z64hd_game || !z64hd_game->voxel_mgr_ptr || !z64hd_game->voxel_formats)
    return 0;
  uintptr_t owner = sys_ptr(sys_ptr(MAIN(z64hd_game->voxel_mgr_ptr)));
  uintptr_t inner = owner ? sys_ptr(owner + MGR_OWNER) : 0;
  return inner ? sys_ptr(inner + MGR_IN_OWNER) : 0;
}

/* one field of a level's records, as the chunk has it */
typedef struct {
  bool present;
  uintptr_t data;
  uint32_t bits; /* a record */
  int32_t first; /* the field's first bit in one */
} field_t;

typedef struct {
  uintptr_t nodes[LEVELS];
  field_t deadly, depth;
  int deadly_level, depth_level; /* the deepest level whose records have the field */
  int origin[3];
} chunk_t;

static field_t level_field(uintptr_t chunk, int level, int field, uint32_t width)
{
  field_t f = { 0 };
  uintptr_t records = chunk + CHUNK_RECORDS + (uintptr_t)level * RECORDS_SIZE;
  uint32_t format;
  if (!sys_read32(records + RECORDS_BITS, &f.bits) || !sys_read32(records + RECORDS_FORMAT, &format))
    return f;
  uintptr_t layout = MAIN(z64hd_game->voxel_formats) + (format < FORMAT_COUNT ? format : 0) * FORMAT_SIZE;
  if (!sys_mapped(layout, FORMAT_SIZE, SYS_PERM_R) || ((const uint8_t *)layout)[FORMAT_PRESENT + field] != 1)
    return f;
  f.first = ((const int32_t *)(layout + FORMAT_BIT))[field];
  f.data = sys_ptr(records + RECORDS_DATA);
  f.present = f.data && f.first >= 0 && (uint32_t)f.first + width <= f.bits;
  return f;
}

static bool open_chunk(uintptr_t addr, chunk_t *chunk)
{
  if (!sys_mapped(addr, CHUNK_NODES + LEVELS * sizeof(uintptr_t), SYS_PERM_R))
    return false;
  for (int i = 0; i < 3; i++)
    chunk->origin[i] = ((const int32_t *)addr)[i];
  for (int level = 0; level < LEVELS; level++)
    chunk->nodes[level] = ((const uintptr_t *)(addr + CHUNK_NODES))[level];
  chunk->deadly_level = chunk->depth_level = -1;
  for (int level = LEVELS - 1; level >= 0; level--) {
    if (chunk->deadly_level < 0 &&
        (chunk->deadly = level_field(addr, level, FIELD_DEADLY, FIELD_DEADLY_BITS)).present)
      chunk->deadly_level = level;
    if (chunk->depth_level < 0 && (chunk->depth = level_field(addr, level, FIELD_DEPTH, FIELD_DEPTH_BITS)).present)
      chunk->depth_level = level;
  }
  return true;
}

/* `width` bits of record `index`, lowest first (a field is short: it's in two bytes) */
static uint32_t field_bits(const field_t *f, uint32_t index, uint32_t width)
{
  uint64_t bit = (uint64_t)index * f->bits + (uint32_t)f->first;
  uintptr_t at = f->data + (uintptr_t)(bit >> 3);
  if (!sys_mapped(at, 2, SYS_PERM_R))
    return 0;
  const uint8_t *bytes = (const uint8_t *)at;
  return ((uint32_t)bytes[0] | (uint32_t)bytes[1] << 8) >> (bit & 7) & ((1u << width) - 1);
}

static void set_cell(uint32_t *cells, int x, int y, int z)
{
  int i = (y * BOX_Z + z) * BOX_X + x;
  cells[i >> 5] |= 1u << (i & 31);
}

static bool cell(const uint32_t *cells, int x, int y, int z)
{
  if (x < 0 || y < 0 || z < 0 || x >= BOX_X || y >= BOX_Y || z >= BOX_Z)
    return false;
  int i = (y * BOX_Z + z) * BOX_X + x;
  return cells[i >> 5] >> (i & 31) & 1;
}

/* whether the cube of `size` cells at corner[] (cells of the chunk) has any of the box in it */
static bool reaches_box(const chunk_t *chunk, const int corner[3], int size)
{
  for (int axis = 0; axis < 3; axis++) {
    int low = chunk->origin[axis] + corner[axis] - box_min[axis];
    if (low + size <= 0 || low >= box_size[axis])
      return false;
  }
  return true;
}

/* a cell of the chunk at corner[], whose records are path[]'s: into the box, if it's one the chunk answers for */
static void keep_cell(const chunk_t *chunk, const int corner[3], const uint32_t path[LEVELS])
{
  for (int axis = 0; axis < 3; axis++)
    if (corner[axis] < CHUNK_MARGIN || corner[axis] > CHUNK_CELLS - CHUNK_MARGIN)
      return;
  int x = chunk->origin[0] + corner[0] - box_min[0], y = chunk->origin[1] + corner[1] - box_min[1];
  int z = chunk->origin[2] + corner[2] - box_min[2];
  set_cell(mapped, x, y, z);
  layer_mapped[y] = true;
  if (chunk->deadly_level >= 0 && chunk->depth_level >= 0 &&
      field_bits(&chunk->deadly, path[chunk->deadly_level], FIELD_DEADLY_BITS) &&
      field_bits(&chunk->depth, path[chunk->depth_level], FIELD_DEPTH_BITS) > DEADLY_OVER) {
    set_cell(deadly, x, y, z);
    layer_deadly[y] = true;
  }
}

/* The node `index` of `level`, whose corner is at at[] (cells of the chunk): its children that reach into the
 * box, down to the cells. path[]: the record index at each level above. */
static void walk(const chunk_t *chunk, int level, uint32_t index, const int at[3], uint32_t path[LEVELS])
{
  uintptr_t word_at = chunk->nodes[level] + (uintptr_t)index * sizeof(uint32_t);
  if (!chunk->nodes[level] || !sys_mapped(word_at, sizeof(uint32_t), SYS_PERM_R))
    return;
  uint32_t word = *(const uint32_t *)word_at;
  uint32_t mask = word & NODE_CHILD_MASK, first = word >> NODE_FIRST_SHIFT;
  int size = CHUNK_CELLS >> (level + 1); /* a child's, in cells */
  for (int child = 0; child < NODE_CHILDREN; child++) {
    if (!(mask >> child & 1))
      continue;
    int corner[3] = { at[0] + (child & 1) * size, at[1] + (child >> 1 & 1) * size, at[2] + (child >> 2 & 1) * size };
    if (!reaches_box(chunk, corner, size))
      continue;
    /* the children there are come one after another: this one's place is the count of those before it */
    path[level] = first + (uint32_t)__builtin_popcount(mask & ((1u << child) - 1));
    if (level < LEVELS - 1)
      walk(chunk, level + 1, path[level], corner, path);
    else
      keep_cell(chunk, corner, path);
  }
}

static void read_chunk(uintptr_t addr)
{
  chunk_t chunk;
  const int root[3] = { 0, 0, 0 };
  if (!open_chunk(addr, &chunk) || !reaches_box(&chunk, root, CHUNK_CELLS))
    return;
  uint32_t path[LEVELS] = { 0 };
  walk(&chunk, 0, 0, root, path);
}

/* The cells of the box round `middle`, out of every loaded chunk that reaches it. A map can have well over a
 * hundred chunks loaded, most of them far off. */
static void read_cells(const int middle[3])
{
  for (int axis = 0; axis < 3; axis++)
    box_min[axis] = middle[axis] - box_middle[axis];
  memset(mapped, 0, sizeof(mapped));
  memset(deadly, 0, sizeof(deadly));
  memset(layer_mapped, 0, sizeof(layer_mapped));
  memset(layer_deadly, 0, sizeof(layer_deadly));
  box_valid = false;
  uintptr_t mgr = map_manager();
  int32_t node_offset;
  if (!mgr || !read_i32(mgr + MGR_NODE_OFFSET, &node_offset))
    return;
  int seen = 0;
  uintptr_t node = sys_ptr(mgr + MGR_LIST_FIRST);
  for (int area = 0; node && node != mgr + MGR_LIST_HEAD && area < MAX_AREAS; area++) {
    uintptr_t holder = sys_ptr(node - node_offset + AREA_HOLDER);
    uintptr_t list = holder ? sys_ptr(holder + HOLDER_CHUNKS) : 0;
    int32_t count;
    if (list && read_i32(holder + HOLDER_COUNT, &count))
      for (int k = 0; k < count && seen < MAX_CHUNKS; k++, seen++) {
        uintptr_t chunk = sys_ptr(list + (uintptr_t)k * sizeof(uintptr_t));
        if (chunk) {
          read_chunk(chunk);
          box_valid = true;
        }
      }
    node = sys_ptr(node + NODE_NEXT);
  }
}

/* --- the outlines ------------------------------------------------------------------------------- */

typedef bool (*kind_t)(int x, int y, int z);

/* what outline() draws round */
typedef struct {
  kind_t kind;
  const bool *layers; /* whether each layer has any cell of the kind */
  uint8_t colour;
  bool columns;       /* each cell stands for the space over it, up to column_top: walls, not only a line */
} outline_t;

/* one side of one row of cells: the row runs along x (along 0, at z = row) or along z (along 1, at x = row), in
 * layer y; the side toward less (0) or more (1) */
typedef struct { int y, along, row, side; } row_side_t;

/* mapped ground: a mapped cell with none under it */
static bool is_ground(int x, int y, int z)
{
  return cell(mapped, x, y, z) && !cell(mapped, x, y - 1, z);
}

static bool is_deadly(int x, int y, int z)
{
  return cell(deadly, x, y, z);
}

/* the same kind of cell next door, level or a step up or down: the ground goes on there */
static bool goes_on(kind_t kind, int x, int y, int z)
{
  return kind(x, y - 1, z) || kind(x, y, z) || kind(x, y + 1, z);
}

/* a line between two corners of cells of the box; y in cells too, lifted */
static void add_edge(int x0, int y0, int z0, int x1, int y1, int z1, uint8_t colour)
{
  if (n_edges >= MAX_EDGES)
    return;
  edges[n_edges++] = (edge_t){
    { (float)(box_min[0] + x0), (float)(box_min[1] + y0) + LIFT, (float)(box_min[2] + z0) },
    { (float)(box_min[0] + x1), (float)(box_min[1] + y1) + LIFT, (float)(box_min[2] + z1) }, colour };
}

/* a column's side as a face: from the line (x0, z0)-(x1, z1) at y0 up to y1, in cells of the box */
static void add_wall(int x0, int z0, int x1, int z1, int y0, int y1, uint8_t colour)
{
  if (n_walls >= MAX_WALLS)
    return;
  walls[n_walls++] = (aw_face_t){
    { (float)(box_min[0] + x0), (float)(box_min[1] + y0) + LIFT, (float)(box_min[2] + z0) },
    { (float)(x1 - x0), 0, (float)(z1 - z0) }, { 0, (float)(y1 - y0), 0 }, colour, true };
}

/* an upright, once: two sides of a column meet in one */
static void add_post(int x, int z, int y0, int y1, uint8_t colour)
{
  uint32_t key = (uint32_t)x | (uint32_t)z << 8 | (uint32_t)y0 << 16 | (uint32_t)y1 << 24;
  uint32_t slot = key * HASH_MULTIPLIER >> (32 - POST_SLOT_BITS);
  while (posts[slot] && posts[slot] != key)
    slot = (slot + 1) & (POST_SLOTS - 1);
  if (posts[slot] || n_posts >= MAX_POSTS)
    return;
  posts[slot] = key;
  n_posts++;
  add_edge(x, y0, z, x, y1, z, colour);
}

/* where the column over a cell ends: the next mapped cell up, or the box's top */
static int column_top(int x, int y, int z)
{
  int top = y + 1;
  while (top < BOX_Y && !cell(mapped, x, top, z))
    top++;
  return top;
}

/* the cells `from` to `to` of a row are open on its side: the line there, and for columns their wall up to top */
static void add_run(const outline_t *o, const row_side_t *r, int from, int to, int top)
{
  int edge = r->row + r->side; /* the row's side, as a cell corner */
  int x0 = r->along ? edge : from, z0 = r->along ? from : edge;
  int x1 = r->along ? edge : to, z1 = r->along ? to : edge;
  add_edge(x0, r->y, z0, x1, r->y, z1, o->colour);
  if (!o->columns)
    return;
  add_edge(x0, top, z0, x1, top, z1, o->colour);
  add_post(x0, z0, r->y, top, o->colour);
  add_post(x1, z1, r->y, top, o->colour);
  add_wall(x0, z0, x1, z1, r->y, top, o->colour);
}

/* One side of a row: where a cell of the kind has none beside it there, that side, the cells in a row joined
 * into one line (for columns, cut where the top changes). The box's own border isn't an edge: what's past it
 * isn't known. */
static void outline_row_side(const outline_t *o, const row_side_t *r)
{
  int length = box_size[r->along ? 2 : 0], beside = r->row + (r->side ? 1 : -1);
  int run = -1, run_top = 0;
  for (int i = 1; i < length; i++) {
    int x = r->along ? r->row : i, z = r->along ? i : r->row;
    int beside_x = r->along ? beside : i, beside_z = r->along ? i : beside;
    bool open = i < length - 1 && o->kind(x, r->y, z) && !goes_on(o->kind, beside_x, r->y, beside_z);
    int top = open && o->columns ? column_top(x, r->y, z) : r->y;
    if (run >= 0 && (!open || top != run_top)) {
      add_run(o, r, run, i, run_top);
      run = -1;
    }
    if (open && run < 0) {
      run = i;
      run_top = top;
    }
  }
}

static void outline_layer(const outline_t *o, int y)
{
  for (int along = 0; along < 2; along++) {
    int rows = box_size[along ? 0 : 2];
    for (int row = 1; row < rows - 1; row++)
      for (int side = 0; side < 2; side++) {
        row_side_t r = { y, along, row, side };
        outline_row_side(o, &r);
      }
  }
}

/* The outline of the kind's cells, Link's layer first, then the ones over and under it by turns: if the room
 * for edges runs out, it's the far layers' that are missing. */
static void outline(const outline_t *o)
{
  for (int step = 0; step < 2 * BOX_Y; step++) {
    int y = box_middle[1] + (step & 1 ? -(step + 1) / 2 : step / 2);
    if (y >= 1 && y < BOX_Y - 1 && o->layers[y])
      outline_layer(o, y);
  }
}

static void rebuild(const int middle[3])
{
  read_cells(middle);
  n_edges = 0;
  n_posts = 0;
  n_walls = 0;
  memset(posts, 0, sizeof(posts));
  const outline_t columns = { is_deadly, layer_deadly, AW_VOID_DEADLY, true };
  const outline_t ground = { is_ground, layer_mapped, AW_VOID_GROUND, false };
  if (box_valid && voidmap_deadly)
    outline(&columns); /* first: if the edges run out, it's the ground's that go */
  if (box_valid && voidmap_ground)
    outline(&ground);
  bool full = n_edges >= MAX_EDGES || n_walls >= MAX_WALLS;
  if (full && !full_logged)
    log_printf("void out: more than fits round here (%d lines, %d faces); the farthest left out", n_edges, n_walls);
  full_logged = full;
  built_frame = frames;
  built_ground = voidmap_ground;
  built_deadly = voidmap_deadly;
}

/* --- Link ---------------------------------------------------------------------------------------- */

typedef struct {
  float pos[3];
  uint32_t flags;
  bool on_map;      /* mapped ground under Link */
  float landed[3];  /* on_map: where the cast down landed */
  bool outside;     /* Link is outside the probe's box (PROBE_OUTSIDE) */
  bool has_point;   /* probe C holds a cell */
  float point[3], level;
} link_state_t;

static bool link_state(link_state_t *s)
{
  uintptr_t actor = link_actor();
  uintptr_t parts = actor ? sys_ptr(actor + ACTOR_PARTS) : 0;
  uintptr_t player = parts ? sys_ptr(parts + PARTS_PLAYER) : 0, probe = parts ? sys_ptr(parts + PARTS_PROBE) : 0;
  if (!player || !probe || !sys_mapped(actor + ACTOR_POS, sizeof(s->pos), SYS_PERM_R) ||
      !sys_read32(player + PLAYER_GROUND_FLAGS, &s->flags) ||
      !sys_mapped(probe + PROBE_FLAGS, PROBE_C_LEVEL + sizeof(float) - PROBE_FLAGS, SYS_PERM_R))
    return false;
  memcpy(s->pos, (const void *)(actor + ACTOR_POS), sizeof(s->pos));
  s->outside = *(const uint32_t *)(probe + PROBE_FLAGS) & PROBE_OUTSIDE;
  s->on_map = *(const uint8_t *)(probe + PROBE_A_HIT) & 1;
  memcpy(s->landed, (const void *)(probe + PROBE_A_POINT), sizeof(s->landed));
  s->has_point = *(const uint8_t *)(probe + PROBE_C_HIT) & 1;
  memcpy(s->point, (const void *)(probe + PROBE_C_POINT), sizeof(s->point));
  memcpy(&s->level, (const void *)(probe + PROBE_C_LEVEL), sizeof(s->level));
  return true;
}

static int to_cell(float v)
{
  int i = (int)v;
  return v < (float)i ? i - 1 : i;
}

/* --- lines ---------------------------------------------------------------------------------------- */

static int n_out, max_out;
static float (*ends_out)[2][3];
static uint8_t *colours_out;

static void line(const float a[3], const float b[3], uint8_t colour)
{
  if (n_out >= max_out)
    return;
  memcpy(ends_out[n_out][0], a, sizeof(float) * 3);
  memcpy(ends_out[n_out][1], b, sizeof(float) * 3);
  colours_out[n_out++] = colour;
}

/* where the fence's segment j starts, level (segment RING_SEGMENTS is segment 0 again) */
static void fence_corner(int j, float *x, float *z)
{
  float angle = TWO_PI * j / RING_SEGMENTS;
  *x = fence.point[0] + VOID_DISTANCE * sinf(angle);
  *z = fence.point[2] + VOID_DISTANCE * cosf(angle);
}

/* The fence Link has to stay in while nothing is under Link: rings at the height of Link's feet and RING_REACH
 * over and under it, joined by uprights, and the way back to the point. */
static void ring(const link_state_t *s)
{
  fence.on = true;
  fence.colour = s->flags & FLAG_FAR_FROM_GROUND ? AW_VOID_DEADLY : AW_VOID_RING;
  fence.y = s->pos[1] + LIFT;
  memcpy(fence.point, s->point, sizeof(fence.point));
  for (int j = 0; j < RING_SEGMENTS; j++) {
    float x0, z0, x1, z1;
    fence_corner(j, &x0, &z0);
    fence_corner(j + 1, &x1, &z1);
    for (int level = -1; level <= 1; level++) {
      float u[3] = { x0, fence.y + level * RING_REACH, z0 }, v[3] = { x1, fence.y + level * RING_REACH, z1 };
      line(u, v, fence.colour);
    }
    if (j % RING_POST_EVERY == 0) {
      float low[3] = { x0, fence.y - RING_REACH, z0 }, high[3] = { x0, fence.y + RING_REACH, z0 };
      line(low, high, fence.colour);
    }
  }
  if (!s->on_map) {
    float feet[3] = { s->pos[0], fence.y, s->pos[2] };
    line(feet, s->point, fence.colour);
  }
}

/* The edges nearest Link, as many as there's room for: a first pass counts them by distance, the second takes
 * the ones within the distance that fits. */
static void nearest_edges(const link_state_t *s, int room)
{
  const float reach = BOX_X; /* farther than any edge is */
  int counts[EDGE_STEPS] = { 0 };
  for (int i = 0; i < n_edges; i++) {
    float dx = (edges[i].a[0] + edges[i].b[0]) / 2 - s->pos[0], dy = (edges[i].a[1] - s->pos[1]) * HEIGHT_COUNTS;
    float dz = (edges[i].a[2] + edges[i].b[2]) / 2 - s->pos[2];
    int step = (int)(sqrtf(dx * dx + dy * dy + dz * dz) / reach * EDGE_STEPS);
    edge_step[i] = (uint8_t)(step < EDGE_STEPS ? step : EDGE_STEPS - 1);
    counts[edge_step[i]]++;
  }
  int last = 0, total = 0;
  while (last < EDGE_STEPS && total + counts[last] <= room)
    total += counts[last++];
  for (int i = 0; i < n_edges; i++)
    if (edge_step[i] < last)
      line(edges[i].a, edges[i].b, edges[i].colour);
}

/* whether the box's cells are due to be read again: Link has left its middle, time has passed, or what's
 * outlined has changed */
static bool rebuild_due(const int here[3])
{
  if (!box_valid)
    return frames - built_frame >= RETRY_FRAMES;
  if (frames - built_frame >= REBUILD_FRAMES || built_ground != voidmap_ground || built_deadly != voidmap_deadly)
    return true;
  for (int axis = 0; axis < 3; axis++) {
    int off = here[axis] - (box_min[axis] + box_middle[axis]);
    if (off > REBUILD_MOVED || off < -REBUILD_MOVED)
      return true;
  }
  return false;
}

int voidmap_lines(float ends[][2][3], uint8_t colours[], int max)
{
  frames++;
  fence.on = false;
  link_state_t s;
  if (!voidmap_shown || !map_manager() || !link_state(&s))
    return 0;
  ends_out = ends;
  colours_out = colours;
  n_out = 0;
  max_out = max;
  if (voidmap_ring && s.has_point)
    ring(&s);
  if (voidmap_ground || voidmap_deadly) {
    int here[3] = { to_cell(s.pos[0]), to_cell(s.pos[1]), to_cell(s.pos[2]) };
    if (rebuild_due(here))
      rebuild(here);
    nearest_edges(&s, max - n_out);
  }
  return n_out;
}

/* the fence's sides, between its lowest and highest rings */
static int fence_faces(aw_face_t faces[], int max)
{
  int n = 0;
  for (int j = 0; j < RING_SEGMENTS && n < max; j++) {
    float x0, z0, x1, z1;
    fence_corner(j, &x0, &z0);
    fence_corner(j + 1, &x1, &z1);
    faces[n++] = (aw_face_t){ { x0, fence.y - RING_REACH, z0 }, { x1 - x0, 0, z1 - z0 }, { 0, 2 * RING_REACH, 0 },
                              fence.colour, !voidmap_xray };
  }
  return n;
}

/* the columns' sides within WALL_RANGE of Link (level), the nearest ring of them first */
static int column_faces(const link_state_t *s, aw_face_t faces[], int max)
{
  const float ring_width = WALL_RANGE / WALL_RINGS;
  int n = 0;
  for (int ring_number = 0; ring_number < WALL_RINGS; ring_number++)
    for (int i = 0; i < n_walls && n < max; i++) {
      const aw_face_t *wall = &walls[i];
      float dx = wall->corner[0] + wall->u[0] / 2 - s->pos[0], dz = wall->corner[2] + wall->u[2] / 2 - s->pos[2];
      if ((int)(sqrtf(dx * dx + dz * dz) / ring_width) != ring_number)
        continue;
      faces[n] = *wall;
      faces[n++].scene = !voidmap_xray;
    }
  return n;
}

int voidmap_faces(aw_face_t faces[], int max)
{
  link_state_t s;
  if (!voidmap_shown || !map_manager() || !link_state(&s))
    return 0;
  int n = fence.on && voidmap_ring ? fence_faces(faces, max) : 0;
  if (voidmap_deadly && box_valid)
    n += column_faces(&s, faces + n, max - n);
  return n;
}

void voidmap_frame(void)
{
  link_state_t s;
  if (!voidmap_shown || !map_manager() || !link_state(&s)) {
    void_flags_were = 0;
    return;
  }
  uint32_t now = s.flags & (FLAG_DEADLY_CELL | FLAG_FAR_FROM_GROUND), set = now & ~void_flags_were;
  void_flags_were = now;
  if (set & FLAG_FAR_FROM_GROUND)
    log_printf("void out: far from ground, link %.2f %.2f %.2f, point %.2f %.2f %.2f, %.2f", (double)s.pos[0],
               (double)s.pos[1], (double)s.pos[2], (double)s.point[0], (double)s.point[1], (double)s.point[2],
               (double)s.level);
  if (set & FLAG_DEADLY_CELL && s.outside)
    log_printf("void out: outside the map's box, link %.2f %.2f %.2f", (double)s.pos[0], (double)s.pos[1],
               (double)s.pos[2]);
  else if (set & FLAG_DEADLY_CELL)
    log_printf("void out: deadly cell, link %.2f %.2f %.2f, cell at %.2f %.2f %.2f", (double)s.pos[0],
               (double)s.pos[1], (double)s.pos[2], (double)s.landed[0], (double)s.landed[1], (double)s.landed[2]);
}

void voidmap_text(char *buf, int len)
{
  link_state_t s;
  if (!z64hd_game || !z64hd_game->voxel_mgr_ptr)
    snprintf(buf, len, TF("not known for this game version"));
  else if (!map_manager() || !link_state(&s))
    snprintf(buf, len, TF("no ground map here"));
  else if (s.flags & FLAG_DEADLY_CELL)
    snprintf(buf, len, s.outside ? TF("VOID  outside the map's box") : TF("VOID  deadly cell"));
  else if (s.flags & FLAG_FAR_FROM_GROUND)
    snprintf(buf, len, TF("VOID  far from the ground"));
  else if (s.on_map)
    snprintf(buf, len, TF("on the map"));
  else if (s.has_point)
    snprintf(buf, len, TF("off the map  %.1f of %.1f"), (double)s.level, (double)VOID_DISTANCE);
  else
    snprintf(buf, len, TF("off the map  no ground found"));
}
