/* Ragdoll Link the way a bomb does (FINDINGS 9.9).
 *
 * Link's scheduler picks his reaction in main+0x82BBB0 (the SelectorPlayerSchedulerReaction node) from his hit
 * component H = [actor+0x270]+0x2B8: with H+0xF4 = 0 and the hit's level H+0x2D1 at 3 or 4 it asks
 * main+0x82AD00, which answers HugeHit for level 4. So a forced hit sets H+0x2D1 = 4 for a frame or so: the
 * game then runs its own ExecutePlayerReactionHugeHit (no attacking, no sinking in water, thrown away from his
 * facing; ragdoll reaction Rolling, then DownLand) and the level goes back as soon as the reaction is on.
 *
 * When the game won't take a hit (a map's arrival, an event, a void-out...) nothing happens. Starting the
 * ragdoll directly instead (main+0x3217550, which an earlier version fell back to) left a ragdoll no reaction
 * drives; across a map change Link (whose actor and controller stay) then flew back to where it lay.
 *
 * C, Link's ragdoll controller, is found from the controller object that cct.c also walks to,
 * [[[actor+0x270]+0x50]+0x20]+0x10, by main+0x5F88C0. While C+0x17C is non-zero a reaction is on.
 *
 * R3 with the menu closed starts it, except while levitating (R3 holds his height then). It can't be stopped
 * early (see ragdoll_set). The game still sees R3.
 *
 * While it isn't forcing, it logs H when a real hit starts a reaction (to compare with a forced one).
 *
 * Momentum: the ragdoll's start (main+0x3217850) launches it with the reaction's throw alone, so a fall or a jump
 * Link was in is lost (logged: -29 units/s before, +8.6 after). His controller then follows the ragdoll: writing
 * its velocity didn't stick (put back on 3 frames, overwritten each time). So his controller body's velocity is
 * sampled every frame the hit is pending, and on the frame the reaction comes on its vertical part is added to
 * every bone of the ragdoll: R = [C+0x40], R+0x60 count, R+0x68 entries of 0x138 bytes with the bone's body at
 * +0x38 (the bodies the start hands ForceNoHit; same in 1.0.0), through the controller's velocity setter (the
 * bones' bodies are of its type, checked by vtable). The first frames are traced to the log. */
#include "ragdoll.h"

#include <string.h>

#include "cct.h"
#include "gamedata.h"
#include "gfx.h"
#include "input.h"
#include "levitate.h"
#include "link.h"
#include "sys.h"
#include "z64hd.h"

#define ACTOR_COMPONENTS 0x270
#define COMPONENTS_PLAYER 0x358
#define COMPONENTS_PHYSICS 0x50
#define COMPONENTS_HIT 0x2B8
#define C_REACTION 0x17C /* u32: the reaction on (1 Rolling, 2 DownLand, 3 ImmediateDown, ...); 0 = none */
#define C_STATE 0x188    /* u32: the ragdoll's mode */
#define C_SIZE 0x230
#define P_SIZE 0x1C40
#define P_FLAGS 0x13D0   /* u32: bit 0 set while Link stands on something */
#define P_VOID_OUT 0x191C /* u32: the void-out's kind (main+0x82C510); 0 = none */
#define H_SIZE 0x1D20
#define H_LEVEL 0x2D1    /* u8: the hit's level; 4 = huge */
#define HUGE_HIT_LEVEL 4
#define FORCE_FRAMES 5   /* the game took it within 1 frame every time it could */
#define CARRY_MIN 1.0f    /* |vy| below this (units/s) isn't momentum worth keeping */
#define C_RAGDOLL 0x40    /* R, the ragdoll: bones at R+0x68 (count R+0x60) */
#define R_BONE_COUNT 0x60
#define R_BONES 0x68
#define BONE_SIZE 0x138
#define BONE_BODY 0x38
#define MAX_BONES 64
#define TRACE_FRAMES 40
#define REC_FROM 0xE0
#define REC_TO 0x300

typedef struct { uintptr_t actor, player, controller; } link_t;

static bool active;              /* a reaction we forced is on */
static int forcing;              /* frames left to see the forced hit take; 0: not forcing */
static link_t held;              /* who */
static uintptr_t forced_hit;     /* H we wrote to */
static uint8_t forced_old;       /* its level before */
static uint32_t last_reaction, last_state;
static bool last_ground;
static uint8_t rec_prev[REC_TO - REC_FROM], rec_cur[REC_TO - REC_FROM];
static bool rec_have;
static uint32_t rec_reaction;    /* C+0x17C last frame */
static vec3f_t pre_velocity;     /* Link's body velocity the last frame before the game took the hit */
static bool have_pre;
static int carry, trace;         /* frames left */

bool ragdoll_known(void)
{
  return z64hd_game && z64hd_game->ragdoll_controller;
}

bool ragdoll_active(void)
{
  return active || forcing;
}

static uintptr_t next(uintptr_t p, size_t off)
{
  return p && sys_mapped(p + off, 8, SYS_PERM_R) ? sys_ptr(p + off) : 0;
}

static bool writable(uintptr_t p, size_t size)
{
  return p && !(p & 7) && sys_mapped(p, size, SYS_PERM_R | SYS_PERM_W);
}

static uint32_t u32_at(uintptr_t a) { return *(const volatile uint32_t *)a; }

/* Link, his player component and his ragdoll controller, or false */
static bool find(link_t *out)
{
  if (!ragdoll_known())
    return false;
  uintptr_t actor = link_actor();
  uintptr_t components = next(actor, ACTOR_COMPONENTS);
  uintptr_t player = next(components, COMPONENTS_PLAYER);
  uintptr_t holder = next(next(next(components, COMPONENTS_PHYSICS), 0x20), 0x10);
  /* the lookup walks the holder's list (count +0x50, entries +0x58), as cct.c checks before it calls */
  uint32_t count;
  if (!writable(player, P_SIZE) || !writable(holder, 0x60) || !sys_read32(holder + 0x50, &count) || count > 64)
    return false;
  uintptr_t entries = next(holder, 0x58);
  if (count && (!entries || !sys_mapped(entries, count * sizeof(uintptr_t), SYS_PERM_R)))
    return false;
  for (uint32_t i = 0; i < count; i++) {
    uintptr_t entry = sys_ptr(entries + i * sizeof(uintptr_t));
    if (!entry || !sys_mapped(entry, 8, SYS_PERM_R) || !next(entry, 0))
      return false;
  }
  uintptr_t controller = MAIN_FN(uintptr_t (*)(uintptr_t), z64hd_game->ragdoll_controller)(holder);
  if (!writable(controller, C_SIZE))
    return false;
  *out = (link_t){ actor, player, controller };
  return true;
}

static uintptr_t hit_component(uintptr_t actor)
{
  uintptr_t h = next(next(actor, ACTOR_COMPONENTS), COMPONENTS_HIT);
  return writable(h, H_SIZE) ? h : 0;
}

static void log_hex(const char *what, const uint8_t *b)
{
  for (int o = 0; o < REC_TO - REC_FROM; o += 16) {
    const uint8_t *p = b + o;
    log_printf("ragdoll: %s +%03x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x", what,
               REC_FROM + o, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11], p[12], p[13],
               p[14], p[15]);
  }
}

/* each frame: when a reaction starts that we didn't ask for (a bomb...), log H then and the frame before; log
 * every change of reaction and every map change with what the ragdoll is doing, to see what a loading zone
 * does to one */
static void record(const link_t *l)
{
  static char map[64];
  uintptr_t h = hit_component(l->actor);
  uint32_t reaction = u32_at(l->controller + C_REACTION);
  bool on_ground = u32_at(l->player + P_FLAGS) & 1;
  const char *m = gd_get_str(GD_STR_MAP);
  size_t i = 0;
  while (m && i < sizeof(map) - 1 && m[i] && m[i] == map[i])
    i++;
  if (m && i < sizeof(map) - 1 && m[i] != map[i]) {
    log_printf("ragdoll: map %s -> %s: reaction %u, state %u, %s, void-out %u", map[0] ? map : "-", m,
               (unsigned)reaction, (unsigned)u32_at(l->controller + C_STATE), on_ground ? "on ground" : "in air",
               (unsigned)u32_at(l->player + P_VOID_OUT));
    for (i = 0; i < sizeof(map) - 1 && m[i]; i++)
      map[i] = m[i];
    map[i] = 0;
  }
  if (rec_have && reaction != rec_reaction && !active && !forcing)
    log_printf("ragdoll: reaction %u -> %u (%s, void-out %u)", (unsigned)rec_reaction, (unsigned)reaction,
               on_ground ? "on ground" : "in air", (unsigned)u32_at(l->player + P_VOID_OUT));
  if (!h) {
    rec_have = false;
    return;
  }
  memcpy(rec_cur, (const void *)(h + REC_FROM), sizeof(rec_cur));
  if (rec_have && !rec_reaction && reaction && !active && !forcing) {
    log_printf("ragdoll: a hit started reaction %u (state %u); H %#lx", (unsigned)reaction,
               (unsigned)u32_at(l->controller + C_STATE), (unsigned long)h);
    log_hex("before", rec_prev); /* not menu text */
    log_hex("now", rec_cur);     /* not menu text */
  }
  memcpy(rec_prev, rec_cur, sizeof(rec_prev));
  rec_have = true;
  rec_reaction = reaction;
}

/* put the hit's level back */
static void unforce(void)
{
  if (forced_hit && writable(forced_hit, H_SIZE))
    *(volatile uint8_t *)(forced_hit + H_LEVEL) = forced_old;
  forcing = 0;
  forced_hit = 0;
}

static bool force_hit(void)
{
  link_t l;
  uintptr_t h;
  if (!find(&l) || !(h = hit_component(l.actor)))
    return false;
  forced_hit = h;
  forced_old = *(volatile uint8_t *)(h + H_LEVEL);
  *(volatile uint8_t *)(h + H_LEVEL) = HUGE_HIT_LEVEL;
  forcing = FORCE_FRAMES;
  held = l;
  have_pre = cct_velocity(l.actor, &pre_velocity);
  log_printf("ragdoll: forcing a huge hit (H %#lx, level was %u, H+0xF0 %u, H+0xF4 %u), velocity %s%.2f,%.2f,%.2f",
             (unsigned long)h, (unsigned)forced_old, (unsigned)u32_at(h + 0xF0), (unsigned)u32_at(h + 0xF4),
             have_pre ? "" : "(unknown) ", (double)pre_velocity.x, (double)pre_velocity.y, (double)pre_velocity.z);
  return true;
}

bool ragdoll_set(bool on)
{
  if (on == ragdoll_active())
    return true;
  if (!on) {
    if (forcing) { /* not taken yet: take the hit back */
      unforce();
      held = (link_t){ 0 };
      return true;
    }
    /* Ending the controller's reaction (main+0x3219090) under HugeHit left Link frozen until a timeout (~15 s):
     * the node still waits for the tumble to finish. Like a bomb's, it ends by itself. */
    gfx_notice("the ragdoll ends by itself");
    return false;
  }
  if (!ragdoll_known()) {
    gfx_notice("ragdoll isn't known for this game version");
    return false;
  }
  if (!force_hit()) {
    gfx_notice("ragdoll: link not found");
    return false;
  }
  return true;
}

/* the ragdoll's bones' bodies; returns how many (0 if they can't be read) */
static int bone_bodies(const link_t *l, uintptr_t *out)
{
  uintptr_t r = next(l->controller, C_RAGDOLL);
  uint32_t count;
  if (!writable(r, R_BONES + 8) || !sys_read32(r + R_BONE_COUNT, &count) || !count || count > MAX_BONES)
    return 0;
  uintptr_t bones = next(r, R_BONES);
  if (!bones || !sys_mapped(bones, count * BONE_SIZE, SYS_PERM_R))
    return 0;
  for (uint32_t i = 0; i < count; i++)
    out[i] = sys_ptr(bones + i * BONE_SIZE + BONE_BODY);
  return (int)count;
}

/* the frame the reaction came on: add the vertical velocity Link had to the ragdoll's bones; then trace */
static void carry_momentum(const link_t *l, uint32_t reaction)
{
  if (!carry && !trace)
    return;
  uintptr_t bodies[MAX_BONES];
  int n = bone_bodies(l, bodies);
  char did[64] = "";
  if (carry) {
    carry = 0;
    if (have_pre && (pre_velocity.y < -CARRY_MIN || pre_velocity.y > CARRY_MIN)) {
      int added = 0, first = -1;
      vec3f_t before = { 0 };
      for (int i = 0; i < n; i++)
        if (bodies[i] && cct_body_add_vertical_velocity(bodies[i], pre_velocity.y, first < 0 ? &before : 0))
          added++, first = first < 0 ? i : first;
      if (added)
        snprintf(did, sizeof(did), " -> %+.2f added to %d/%d bones (bone %d vy was %.2f)", (double)pre_velocity.y,
                 added, n, first, (double)before.y);
      else
        snprintf(did, sizeof(did), " -> no bone body to add to (%d bones, vt %#lx)", n,
                 n && bodies[0] && sys_mapped(bodies[0], 8, SYS_PERM_R) ? (unsigned long)(sys_ptr(bodies[0]) - z64hd_main) : 0ul);
    }
    have_pre = false;
  }
  if (trace) {
    trace--;
    vec3f_t v = { 0 }, drawn = { 0 }, bone = { 0 };
    bool have_v = cct_velocity(l->actor, &v), have_drawn = link_drawn_pos(&drawn);
    bool have_bone = n && bodies[0] && cct_body_velocity(bodies[0], &bone);
    log_printf("ragdoll: f%d reaction %u state %u vel %s%.2f,%.2f,%.2f bone0 vy %s%.2f drawn y %s%.3f%s",
               TRACE_FRAMES - trace - 1, (unsigned)reaction, (unsigned)u32_at(l->controller + C_STATE),
               have_v ? "" : "? ", (double)v.x, (double)v.y, (double)v.z, have_bone ? "" : "? ", (double)bone.y,
               have_drawn ? "" : "? ", (double)drawn.y, did);
  }
}

void ragdoll_frame(bool menu_open)
{
  if (!menu_open && !levitate_floating() && (input_pressed() & BTN_RSTICK)) {
    ragdoll_set(!ragdoll_active());
    return;
  }
  link_t l;
  bool found = ragdoll_known() && find(&l);
  if (found)
    record(&l);
  if (forcing) {
    if (!found || l.actor != held.actor || l.controller != held.controller) {
      unforce();
      held = (link_t){ 0 };
      return;
    }
    uint32_t reaction = u32_at(l.controller + C_REACTION);
    vec3f_t v;
    if (!reaction && cct_velocity(l.actor, &v)) /* not taken yet: still the velocity from before it */
      pre_velocity = v, have_pre = true;
    if (reaction) { /* the game took it */
      log_printf("ragdoll: the game took the hit after %d frames (reaction %u, state %u)", FORCE_FRAMES - forcing + 1,
                 (unsigned)reaction, (unsigned)u32_at(l.controller + C_STATE));
      unforce();
      carry = 1;
      trace = TRACE_FRAMES;
      last_reaction = reaction;
      last_state = u32_at(l.controller + C_STATE);
      last_ground = u32_at(l.player + P_FLAGS) & 1;
      active = true;
    } else if (--forcing == 0) {
      have_pre = false;
      log_printf("ragdoll: the game didn't take the hit");
      unforce();
      held = (link_t){ 0 };
      gfx_notice("can't knock Link over right now");
    }
    return;
  }
  if (!active)
    return;
  if (!found || l.actor != held.actor || l.controller != held.controller) {
    log_printf("ragdoll: link went away; let go");
    active = false;
    carry = trace = 0;
    held = (link_t){ 0 };
    return;
  }
  uint32_t reaction = u32_at(l.controller + C_REACTION), state = u32_at(l.controller + C_STATE);
  carry_momentum(&l, reaction);
  if (reaction != last_reaction || state != last_state)
    log_printf("ragdoll: reaction %u, state %u (%s, void-out %u)", (unsigned)reaction, (unsigned)state,
               (u32_at(l.player + P_FLAGS) & 1) ? "on ground" : "in air", (unsigned)u32_at(l.player + P_VOID_OUT));
  bool ground = u32_at(l.player + P_FLAGS) & 1;
  if (ground != last_ground)
    log_printf("ragdoll: %s", ground ? "on ground" : "in air");
  last_ground = ground;
  last_reaction = reaction;
  last_state = state;
  if (!reaction) { /* the game ended it itself */
    log_printf("ragdoll: the game ended it");
    gfx_notice("ragdoll ended");
    active = false;
    carry = trace = 0;
    held = (link_t){ 0 };
  }
}
