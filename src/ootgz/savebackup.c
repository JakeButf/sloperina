/* Save backups. Every save the game makes, autosave or from the save screen, ends in nn::fs::CommitSaveData on its
 * save mount. main's GOT slot for it is hooked (got.c); once the game's commit has succeeded, the whole mount is
 * copied to a new folder on the SD card. The copy runs inside the hook, so the game can't write the save again
 * until it's done, and on whatever thread the game saves from: nothing here logs or draws (savebackup_frame
 * reports on the game's thread).
 *
 * The directory functions and DeleteDirectoryRecursively are looked up in the sdk's exports rather than imported:
 * an import rtld couldn't find would stop the game loading. */
#include "savebackup.h"

#include "gfx.h"
#include "gamedata.h"
#include "got.h"
#include "lang.h"
#include "sdfs.h"
#include "settings.h"
#include "sys.h"

bool save_backup_enabled;
bool save_backup_location;
int save_backup_kinds = SAVE_BACKUP_BOTH;

/* Which kind a commit is: the game's menu screen when it happened (the Enum 0x578D41E1, as gd_game_menu_screen;
 * FINDINGS §9.1). None: an autosave (a loading zone, an event). Option: the options alone, not backed up.
 * Any other (the save screen, game over's save prompt): a hard save. */
#define GD_MENU_SCREEN 0x578D41E1u
#define MENU_SCREEN_NONE 0xB6EEDE09u   /* murmur3("None") */
#define MENU_SCREEN_OPTION 0xA60BA395u /* murmur3("Option") */
static uint32_t menu_screen = MENU_SCREEN_NONE; /* as last seen on the game's thread (savebackup_frame) */
enum { KIND_AUTO, KIND_HARD, KIND_OPTIONS };
static const char *const kind_names[] = { "autosave", "hard save", "options" }; /* not menu text */

/* The save caption's location (String64 0xF74E0E8E, kept in caption.sav), e.g. "TimeTemple", and its English
 * name from the game's LocationMsg/Location.msbt (EUen) */
#define GD_STR_SAVE_LOCATION 0xF74E0E8Eu
static const struct { const char *key, *name; } locations[] = {
  { "BottomOfTheWell", "Bottom of the Well" }, { "DeathMountainCrater", "Death Mountain Crater" }, /* not menu text */
  { "DeathMountainTrail", "Death Mountain Trail" }, { "DesertColossus", "Evil Desert Colossus" }, /* not menu text */
  { "DodongoCavern", "Dodongo Cavern" }, { "DreamWorld", "Hall of Fallen Foes" }, { "FireTemple", "Fire Temple" }, /* not menu text */
  { "ForestTemple", "Forest Temple" }, { "GanonCastle", "Ganondorf's Castle" }, /* not menu text */
  { "GerudoFortress", "Gerudo Fortress" }, { "GerudoTrainingGround", "Gerudo Proving Grounds" }, /* not menu text */
  { "GerudoValley", "Gerudo Valley" }, { "GoronCity", "Goron City" }, { "Graveyard", "Graveyard" }, /* not menu text */
  { "HountedWasteland", "Mirage Desert" }, { "HyruleCastle", "Hyrule Castle" }, { "HyruleField", "Hyrule Field" }, /* not menu text */
  { "IceCavern", "Ice Cavern" }, { "IngoRanch", "Ingo Ranch" }, { "InsideGanonCastle", "Inside Ganondorf's Castle" }, /* not menu text */
  { "InsideJabuJabuBelly", "Lord Jabu-Jabu's Belly" }, { "InsideTheDekuTree", "Great Deku Tree" }, /* not menu text */
  { "KakarikoVillage", "Kakariko Village" }, { "KokiriForest", "Kokiri Forest" }, { "LakeHylia", "Lake Hylia" }, /* not menu text */
  { "LonLonRanch", "Lon Lon Ranch" }, { "LostWood", "Lost Woods" }, { "Market", "Hyrule Castle Town" }, /* not menu text */
  { "SacredForestMeadow", "Sacred Grove" }, { "ShadowTemple", "Shadow Temple" }, { "SpiritTemple", "Spirit Temple" }, /* not menu text */
  { "TimeTemple", "Temple of Time" }, { "WaterTemple", "Water Temple" }, { "ZoraDomain", "Zora Domain" }, /* not menu text */
  { "ZoraFountain", "Zora Fountain" }, { "ZoraRiver", "Zora River" }, /* not menu text */
};
#define N_LOCATIONS (sizeof(locations) / sizeof(*locations))
/* the location as last seen on the game's thread (savebackup_frame): the copy may run on another */
static char location_key[64];

#define BACKUP_DIR SD_DIR "/save-backups"
#define MAX_DEPTH 4             /* save mount: slot_NN/ inside its root */
#define MAX_BACKUP_FOLDERS 512  /* listed when trimming to SAVE_BACKUPS_KEPT */
#define COPY_CHUNK (64 * 1024)
#define PATH_LEN 256
#define STAMP_LEN 96            /* "2026-11-15_21-30-05 - Inside Ganondorf's Castle", with room to spare */

/* nn::fs's directory types (the SDK's layout) */
typedef struct { void *handle; } fs_dir_t;
typedef struct {
  char name[769];
  uint8_t pad0[3];
  int8_t type;                  /* 0 directory, 1 file */
  uint8_t pad1[3];
  int64_t size;
} fs_dir_entry_t;
enum { FS_ENTRY_DIRECTORY = 0, FS_ENTRY_FILE = 1, FS_OPEN_DIR_ALL = 3 };

typedef nn_result_t (*commit_fn)(const char *mount);
typedef nn_result_t (*open_dir_fn)(fs_dir_t *out, const char *path, int mode);
typedef nn_result_t (*read_dir_fn)(int64_t *count, fs_dir_entry_t *entries, fs_dir_t dir, int64_t max);
typedef void (*close_dir_fn)(fs_dir_t dir);
typedef nn_result_t (*delete_tree_fn)(const char *path);

static open_dir_fn open_dir;
static read_dir_fn read_dir;
static close_dir_fn close_dir;
static delete_tree_fn delete_tree;

/* main's two commit imports: what each wrapper calls (got_hook) */
static void *next_commit_save, *next_commit;

/* the console's clock (nn::time, as flaglog.c) */
typedef struct { int64_t value; } nn_posix_time_t;
typedef struct { int16_t year; int8_t month, day, hour, minute, second; } nn_calendar_time_t;
bool nn_time_initialized(void) __asm__("_ZN2nn4time13IsInitializedEv");
uint32_t nn_time_now(nn_posix_time_t *out)
  __asm__("_ZN2nn4time23StandardUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE");
uint32_t nn_time_to_calendar(nn_calendar_time_t *out, void *info, const nn_posix_time_t *time)
  __asm__("_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeE");
#define CALENDAR_INFO_SIZE 64

/* the copy's working state (one copy at a time: `busy`) */
static int busy;
static _Alignas(8) uint8_t chunk[COPY_CHUNK];
static fs_dir_entry_t entries[MAX_DEPTH + 1];
static char folder_names[MAX_BACKUP_FOLDERS][STAMP_LEN];
static int files_copied;
static int64_t bytes_copied;
static char error[320];

/* what the last backup did, for savebackup_frame and the menu */
static volatile int reports;          /* backups finished; savebackup_frame reports each new one */
static int reported;
static char last_mount[32], last_stamp[STAMP_LEN], last_error[320];
static int last_files;
static int64_t last_bytes;
static uint64_t last_ticks;
static const char *hook_error = "not hooked yet"; /* not menu text */

static bool ends_with_slash(const char *s)
{
  size_t n = strlen(s);
  return n && s[n - 1] == '/';
}

static void join(char *out, const char *dir, const char *name)
{
  snprintf(out, PATH_LEN, ends_with_slash(dir) ? "%s%s" : "%s/%s", dir, name);
}

static void fail(const char *what, const char *path, nn_result_t r)
{
  if (!error[0])
    snprintf(error, sizeof(error), "%s %s (0x%x)", what, path, (unsigned)r);
}

static void copy_file(const char *from, const char *to)
{
  fs_file_t in, out;
  int64_t size = 0;
  nn_result_t r;
  if ((r = fs_open(&in, from, FS_READ)) != 0) {
    fail("couldn't open", from, r); /* not menu text */
    return;
  }
  fs_size(&size, in);
  fs_delete_file(to);
  if ((r = fs_create_file(to, size)) != 0 || (r = fs_open(&out, to, FS_WRITE)) != 0) {
    fail("couldn't make", to, r); /* not menu text */
    fs_close(in);
    return;
  }
  for (int64_t at = 0; at < size;) {
    size_t want = size - at < COPY_CHUNK ? (size_t)(size - at) : COPY_CHUNK, got = 0;
    if ((r = fs_read(&got, in, at, chunk, want)) != 0 || !got) {
      fail("couldn't read", from, r); /* not menu text */
      break;
    }
    fs_write_option_t flush = { at + (int64_t)got >= size ? FS_WRITE_FLUSH : 0 };
    if ((r = fs_write(out, at, chunk, got, &flush)) != 0) {
      fail("couldn't write", to, r); /* not menu text */
      break;
    }
    at += got;
    bytes_copied += got;
  }
  fs_close(out);
  fs_close(in);
  files_copied++;
}

static void copy_tree(const char *from, const char *to, int depth)
{
  fs_dir_t dir;
  nn_result_t r = open_dir(&dir, from, FS_OPEN_DIR_ALL);
  if (r != 0) {
    fail("couldn't list", from, r); /* not menu text */
    return;
  }
  fs_dir_entry_t *entry = &entries[depth];
  int64_t count;
  char child_from[PATH_LEN], child_to[PATH_LEN];
  while (read_dir(&count, entry, dir, 1) == 0 && count == 1) {
    join(child_from, from, entry->name);
    join(child_to, to, entry->name);
    if (entry->type == FS_ENTRY_DIRECTORY) {
      if (depth >= MAX_DEPTH) {
        fail("too deep:", child_from, 0); /* not menu text */
        continue;
      }
      fs_create_dir(child_to);
      copy_tree(child_from, child_to, depth + 1);
    } else {
      copy_file(child_from, child_to);
    }
  }
  close_dir(dir);
}

static int compare_names(const char *a, const char *b)
{
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return (unsigned char)*a - (unsigned char)*b;
}

/* Delete the oldest backups (their names sort by date) past SAVE_BACKUPS_KEPT. */
static void trim_backups(void)
{
  if (!delete_tree)
    return;
  fs_dir_t dir;
  if (open_dir(&dir, BACKUP_DIR, FS_OPEN_DIR_ALL) != 0)
    return;
  int n = 0;
  int64_t count;
  while (n < MAX_BACKUP_FOLDERS && read_dir(&count, &entries[0], dir, 1) == 0 && count == 1)
    if (entries[0].type == FS_ENTRY_DIRECTORY && strlen(entries[0].name) < STAMP_LEN)
      snprintf(folder_names[n++], STAMP_LEN, "%s", entries[0].name);
  close_dir(dir);
  for (; n > SAVE_BACKUPS_KEPT; n--) {
    int oldest = 0;
    for (int i = 1; i < n; i++)
      if (compare_names(folder_names[i], folder_names[oldest]) < 0)
        oldest = i;
    char path[PATH_LEN];
    join(path, BACKUP_DIR, folder_names[oldest]);
    delete_tree(path);
    memcpy(folder_names[oldest], folder_names[n - 1], STAMP_LEN);
  }
}

/* the folder name's " - <location>", or nothing */
static void location_suffix(char *out, int len)
{
  out[0] = 0;
  if (!save_backup_location || !location_key[0])
    return;
  const char *name = location_key; /* a location the table doesn't know: its key */
  for (unsigned i = 0; i < N_LOCATIONS; i++)
    if (!compare_names(location_key, locations[i].key))
      name = locations[i].name;
  int n = snprintf(out, len, " - ");
  for (; *name && n < len - 1; name++) /* nothing a folder name can't hold */
    out[n++] = (*name == '/' || *name == '\\' || *name == ':' || *name == '*' || *name == '?' || *name == '"' ||
                *name == '<' || *name == '>' || *name == '|') ? '_' : *name;
  out[n] = 0;
}

/* commits not backed up, for savebackup_frame to log: their kind and the menu screen then */
static volatile int skips;
static int skips_reported, last_skip_kind;
static uint32_t last_skip_screen, last_screen;
static int last_kind;

static void back_up(const char *mount)
{
  if (!save_backup_enabled || !settings_sd() || !open_dir || !read_dir || !close_dir || !mount ||
      strlen(mount) >= sizeof(last_mount) - 1)
    return;
  uint32_t screen = __atomic_load_n(&menu_screen, __ATOMIC_ACQUIRE);
  int kind = screen == MENU_SCREEN_NONE ? KIND_AUTO : screen == MENU_SCREEN_OPTION ? KIND_OPTIONS : KIND_HARD;
  bool wanted = kind == KIND_AUTO ? save_backup_kinds != SAVE_BACKUP_HARD
              : kind == KIND_HARD ? save_backup_kinds != SAVE_BACKUP_AUTO : false;
  if (!wanted) {
    last_skip_kind = kind;
    last_skip_screen = screen;
    __atomic_add_fetch(&skips, 1, __ATOMIC_RELEASE);
    return;
  }
  if (__atomic_exchange_n(&busy, 1, __ATOMIC_ACQUIRE))
    return; /* another thread's commit is being copied: that copy is as new */
  last_kind = kind;
  last_screen = screen;
  uint64_t start = sys_tick();
  char stamp[STAMP_LEN] = "undated"; /* not menu text */
  nn_posix_time_t posix;
  nn_calendar_time_t cal;
  static _Alignas(8) uint8_t info[CALENDAR_INFO_SIZE];
  if (nn_time_initialized() && nn_time_now(&posix) == 0 && nn_time_to_calendar(&cal, info, &posix) == 0)
    snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d_%02d-%02d-%02d", cal.year, cal.month, cal.day, cal.hour,
             cal.minute, cal.second);
  char suffix[64];
  location_suffix(suffix, sizeof(suffix));
  size_t at = strlen(stamp);
  snprintf(stamp + at, sizeof(stamp) - at, "%s", suffix);
  char root[40], to[PATH_LEN];
  snprintf(root, sizeof(root), "%s:/", mount);
  fs_create_dir(SD_DIR);
  fs_create_dir(BACKUP_DIR);
  join(to, BACKUP_DIR, stamp);
  /* two saves in one second (an autosave, then the save screen's): the later one replaces it */
  if (delete_tree)
    delete_tree(to);
  error[0] = 0;
  files_copied = 0;
  bytes_copied = 0;
  nn_result_t r = fs_create_dir(to);
  if (r != 0)
    fail("couldn't make", to, r); /* not menu text */
  else
    copy_tree(root, to, 0);
  trim_backups();

  snprintf(last_mount, sizeof(last_mount), "%s", mount);
  memcpy(last_stamp, stamp, sizeof(last_stamp));
  memcpy(last_error, error, sizeof(last_error));
  last_files = files_copied;
  last_bytes = bytes_copied;
  last_ticks = sys_tick() - start;
  __atomic_add_fetch(&reports, 1, __ATOMIC_RELEASE);
  __atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
}

static nn_result_t commit_save_wrap(const char *mount)
{
  nn_result_t r = ((commit_fn)next_commit_save)(mount);
  if (r == 0)
    back_up(mount);
  return r;
}

static nn_result_t commit_wrap(const char *mount)
{
  nn_result_t r = ((commit_fn)next_commit)(mount);
  if (r == 0)
    back_up(mount);
  return r;
}

const char *savebackup_hook_game(uintptr_t main_base)
{
  uintptr_t sdk = got_module_of((uintptr_t)&fs_open); /* an sdk function this module imports */
  if (!sdk)
    return hook_error = "sdk module not found"; /* not menu text */
  open_dir = (open_dir_fn)got_export(sdk, "_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci");
  read_dir = (read_dir_fn)got_export(sdk, "_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl");
  close_dir = (close_dir_fn)got_export(sdk, "_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE");
  delete_tree = (delete_tree_fn)got_export(sdk, "_ZN2nn2fs26DeleteDirectoryRecursivelyEPKc");
  if (!open_dir || !read_dir || !close_dir)
    return hook_error = "nn::fs directory functions not found in the sdk"; /* not menu text */
  void *commit_save = got_export(sdk, "_ZN2nn2fs14CommitSaveDataEPKc");
  void *commit = got_export(sdk, "_ZN2nn2fs6CommitEPKc");
  const char *e1 = commit_save ? got_hook(main_base, "_ZN2nn2fs14CommitSaveDataEPKc", commit_save_wrap, commit_save,
                                          &next_commit_save)
                               : "CommitSaveData not in the sdk"; /* not menu text */
  const char *e2 = commit ? got_hook(main_base, "_ZN2nn2fs6CommitEPKc", commit_wrap, commit, &next_commit)
                          : "Commit not in the sdk"; /* not menu text */
  if (e1 && e2)
    return hook_error = e1;
  hook_error = 0;
  return 0;
}

void savebackup_frame(void)
{
  uint32_t screen;
  if (save_backup_enabled && gd_get_enum(GD_MENU_SCREEN, &screen))
    __atomic_store_n(&menu_screen, screen, __ATOMIC_RELEASE);
  int skipped = __atomic_load_n(&skips, __ATOMIC_ACQUIRE);
  if (skipped != skips_reported) {
    skips_reported = skipped;
    log_printf("save backup: skipped a commit (%s, menu screen %08x)", kind_names[last_skip_kind],
               (unsigned)last_skip_screen);
  }
  const char *key = save_backup_enabled ? gd_get_str(GD_STR_SAVE_LOCATION) : 0;
  if (key && !__atomic_load_n(&busy, __ATOMIC_ACQUIRE)) {
    char copy[sizeof(location_key)];
    int i = 0;
    for (; i < (int)sizeof(copy) - 1 && key[i]; i++)
      copy[i] = key[i];
    copy[i] = 0;
    memcpy(location_key, copy, sizeof(location_key));
  }
  int now = __atomic_load_n(&reports, __ATOMIC_ACQUIRE);
  if (now == reported || __atomic_load_n(&busy, __ATOMIC_ACQUIRE))
    return;
  reported = now;
  unsigned ms = (unsigned)(last_ticks * 1000 / sys_tick_freq());
  if (last_error[0]) {
    log_printf("save backup %s (%s, menu screen %08x) from %s: %d files, then %s", last_stamp, kind_names[last_kind],
               (unsigned)last_screen, last_mount, last_files, last_error);
    gfx_notice("save backup failed (see the log)");
  } else {
    log_printf("save backup %s (%s, menu screen %08x) from %s: %d files, %lld bytes, %u ms", last_stamp,
               kind_names[last_kind], (unsigned)last_screen, last_mount, last_files, (long long)last_bytes, ms);
    gfx_notice("save backed up");
  }
}

void savebackup_status_text(char *buf, int len)
{
  if (hook_error)
    snprintf(buf, len, TF("unavailable: %s"), hook_error); /* hook_error: not menu text */
  else if (!settings_sd())
    snprintf(buf, len, "%s", T("no SD card access"));
  else if (!reported)
    snprintf(buf, len, "%s", T("no backup yet this session"));
  else if (last_error[0])
    snprintf(buf, len, TF("last: %s failed (see the log)"), last_stamp);
  else
    snprintf(buf, len, TF("last: %s (%s)"), last_stamp, T(kind_names[last_kind]));
}
