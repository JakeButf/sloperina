/* The Switch's keyboard (swkbd.h), through nn::swkbd as the game's sdk module exports it. The functions are looked
 * up in the sdk's exports (got.c) rather than imported, as savebackup.c's directory functions are: an import rtld
 * couldn't find would stop the game loading.
 *
 * The layouts, from the sdk's code (1.1.1's sdk; the API is the SDK's, so the same in 1.0.0's):
 *   KeyboardConfig, 0x518 bytes: MakePresetDefault fills it; +0x3AC s32 textMaxLength, +0x3B0 textMinLength.
 *   ShowKeyboardArg: the KeyboardConfig, then +0x518 the work buffer and +0x520 its size (GetRequiredWorkBufferSize:
 *     0x1000 without a dictionary; page-aligned, as it's lent to the applet), +0x528/+0x530 the text check's,
 *     +0x538/+0x540 the dictionaries' (none). SetInitialTextUtf8 writes the initial text into the work buffer.
 *   String: a pointer and a size (GetRequiredStringBufferSize: 0x7D4); the text comes back in UTF-16 unless the
 *     config asks for UTF-8 (it's read as either). */
#include "swkbd.h"

#include "got.h"
#include "lang.h"
#include "sdfs.h"
#include "sys.h"

bool swkbd_enabled = true;

#define CONFIG_SIZE 0x518
#define CONFIG_MAX_LENGTH 0x3AC
#define ARG_WORK 0x518
#define ARG_WORK_SIZE 0x520
#define ARG_SIZE 0x548
#define WORK_SIZE 0x1000   /* GetRequiredWorkBufferSize(false) */
#define STRING_SIZE 0x7D4  /* GetRequiredStringBufferSize() */
#define FAILED_FAST_MS 200 /* a "no" sooner than this: it couldn't be shown, not cancelled */

typedef void (*config_fn)(void *config);
typedef void (*config_text_fn)(void *config, const char *utf8);
typedef void (*arg_text_fn)(void *arg, const char *utf8);
typedef uint32_t (*show_fn)(void *string, const void *arg);
typedef uint64_t (*work_size_fn)(bool dictionary);
typedef uint64_t (*string_size_fn)(void);

static struct {
  bool looked, usable;
  config_fn make_default;
  config_text_fn header, guide;
  arg_text_fn initial;
  show_fn show;
  work_size_fn work_size;
  string_size_fn string_size;
} api;

static _Alignas(0x1000) uint8_t work[WORK_SIZE];
static _Alignas(8) uint8_t arg[ARG_SIZE];
static _Alignas(8) uint8_t text[STRING_SIZE + 4];
static struct { void *ptr; uint64_t size; } string = { text, STRING_SIZE };

static bool look_up(void)
{
  if (api.looked)
    return api.usable;
  api.looked = true;
  uintptr_t sdk = got_module_of((uintptr_t)&fs_open); /* an sdk function this module imports */
  if (!sdk) {
    log_printf("keyboard: sdk module not found; sloperinaGZ's own keyboard instead"); /* not menu text */
    return false;
  }
  api.make_default = (config_fn)got_export(sdk, "_ZN2nn5swkbd17MakePresetDefaultEPNS0_14KeyboardConfigE");
  api.header = (config_text_fn)got_export(sdk, "_ZN2nn5swkbd17SetHeaderTextUtf8EPNS0_14KeyboardConfigEPKc");
  api.guide = (config_text_fn)got_export(sdk, "_ZN2nn5swkbd16SetGuideTextUtf8EPNS0_14KeyboardConfigEPKc");
  api.initial = (arg_text_fn)got_export(sdk, "_ZN2nn5swkbd18SetInitialTextUtf8EPNS0_15ShowKeyboardArgEPKc");
  api.show = (show_fn)got_export(sdk, "_ZN2nn5swkbd12ShowKeyboardEPNS0_6StringERKNS0_15ShowKeyboardArgE");
  api.work_size = (work_size_fn)got_export(sdk, "_ZN2nn5swkbd25GetRequiredWorkBufferSizeEb");
  api.string_size = (string_size_fn)got_export(sdk, "_ZN2nn5swkbd27GetRequiredStringBufferSizeEv");
  api.usable = api.make_default && api.header && api.guide && api.initial && api.show && api.work_size &&
               api.string_size;
  if (api.usable && (api.work_size(false) > WORK_SIZE || api.string_size() > STRING_SIZE)) {
    log_printf("keyboard: buffers %llu and %llu bytes asked for, more than kept; sloperinaGZ's own keyboard",
               (unsigned long long)api.work_size(false), (unsigned long long)api.string_size()); /* not menu text */
    api.usable = false;
  }
  if (!api.usable)
    log_printf("keyboard: nn::swkbd not found in the sdk; sloperinaGZ's own keyboard instead"); /* not menu text */
  return api.usable;
}

/* the text as ASCII: UTF-16 (or UTF-8, if that's what came) */
static void to_ascii(char *out, int out_len)
{
  bool utf16 = text[0] == 0 || text[1] == 0; /* an ASCII character's high byte, or nothing typed */
  int n = 0;
  if (utf16) {
    for (int i = 0; i + 1 < STRING_SIZE && n < out_len - 1; i += 2) {
      unsigned c = text[i] | (unsigned)text[i + 1] << 8;
      if (!c)
        break;
      if (c >= 0xDC00 && c < 0xE000)
        continue; /* a surrogate pair's second half: its first already gave the '_' */
      out[n++] = c >= 0x20 && c < 0x7F ? (char)c : '_';
    }
  } else {
    for (int i = 0; i < STRING_SIZE && text[i] && n < out_len - 1; i++) {
      unsigned c = text[i];
      if ((c & 0xC0) == 0x80)
        continue; /* a multi-byte character's continuation */
      out[n++] = c >= 0x20 && c < 0x7F ? (char)c : '_';
    }
  }
  out[n] = 0;
}

int swkbd_ask(const char *header, const char *initial, int max, char *out, int out_len)
{
  static bool failed; /* it couldn't be shown once: not tried again this session */
  if (failed || !look_up())
    return SWKBD_UNAVAILABLE;
  memset(arg, 0, sizeof(arg));
  memset(text, 0, sizeof(text));
  api.make_default(arg); /* the KeyboardConfig at the start of the ShowKeyboardArg */
  api.header(arg, header ? header : "");
  api.guide(arg, "");
  *(int32_t *)(arg + CONFIG_MAX_LENGTH) = max > 0 ? max : 0;
  *(void **)(arg + ARG_WORK) = work;
  *(uint64_t *)(arg + ARG_WORK_SIZE) = WORK_SIZE;
  api.initial(arg, initial ? initial : "");
  uint64_t start = sys_tick();
  uint32_t result = api.show(&string, arg); /* held here until it's closed */
  unsigned ms = (unsigned)((sys_tick() - start) * 1000 / sys_tick_freq());
  if (result == 0) {
    to_ascii(out, out_len);
    log_printf("keyboard: \"%s\" (%u ms)", out, ms); /* not menu text */
    return SWKBD_OK;
  }
  if (ms < FAILED_FAST_MS) {
    failed = true;
    log_printf("keyboard: the Switch's couldn't be shown (0x%x); sloperinaGZ's own instead", (unsigned)result);
    return SWKBD_UNAVAILABLE;
  }
  log_printf("keyboard: cancelled (0x%x, %u ms)", (unsigned)result, ms); /* not menu text */
  return SWKBD_CANCELLED;
}
