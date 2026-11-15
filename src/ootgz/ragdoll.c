/* Ragdoll Link (FINDINGS 9.9).
 *
 * The game knocks Link down with the event node ExecuteEventPlayerImmediateRagdollDown (main+0x26B3EC0 enter,
 * main+0x26B42C0 leave). Its enter stops Link's body (linear and angular velocity), plays the AS "SlippingDown",
 * and asks Link's ragdoll controller C for the reaction "ImmediateDown" (main+0x3218D30); its leave ends it
 * (main+0x3219090) and sets P+0x1C3E bit 0. C is found from the controller object that cct.c also walks to,
 * [[[actor+0x270]+0x50]+0x20]+0x10, by main+0x5F88C0. While C+0x17C is non-zero a reaction is on.
 *
 * The node's update also drives C each frame while Link is in the air (main+0x321C040 / main+0x38911A0); this
 * doesn't, so a ragdoll started in the air is the controller's own. */
#include "ragdoll.h"

#include <string.h>

#include "gfx.h"
#include "link.h"
#include "sys.h"
#include "z64hd.h"

#define ACTOR_COMPONENTS 0x270
#define COMPONENTS_PLAYER 0x358
#define COMPONENTS_PHYSICS 0x50
#define C_REACTION 0x17C /* u32: the reaction asked for; 0 = none (the end call does nothing then) */
#define C_STATE 0x188    /* u32: the ragdoll's state (3 while down, by the slipping node's leave) */
#define C_SIZE 0x230
#define P_GET_UP 0x1C3E  /* u8: the event node's leave sets bit 0 */
#define P_SIZE 0x1C40

/* the start request, as the event node fills it (stack at main+0x26B3F64) */
typedef struct {
  uintptr_t actor;
  float scale;       /* 1.0 */
  uint8_t pad0c[0x1C - 0x0C];
  uint32_t a;        /* +0x1C: 1 */
  uint32_t b;        /* +0x20: 2 */
  uint32_t preset;   /* +0x24: 0 (the slipping node passes 1) */
  uint32_t kind;     /* +0x28: 0 = "Default" */
  uint8_t pad2c[4];
} start_t;
_Static_assert(sizeof(start_t) == 0x30, "start request");

typedef struct {
  uintptr_t actor;
  uint8_t flag;      /* 0 from the event node */
  uint8_t pad[7];
} end_t;

typedef struct { uintptr_t actor, player, controller; } link_t;

static bool active;
static link_t held;              /* who was ragdolled */
static uint32_t last_reaction, last_state;

bool ragdoll_known(void)
{
  return z64hd_game && z64hd_game->ragdoll_controller && z64hd_game->ragdoll_start && z64hd_game->ragdoll_end &&
         z64hd_game->player_stop_linear && z64hd_game->player_stop_angular && z64hd_game->player_play_as &&
         z64hd_game->as_slipping_down;
}

static uintptr_t next(uintptr_t p, size_t off)
{
  return p && sys_mapped(p + off, 8, SYS_PERM_R) ? sys_ptr(p + off) : 0;
}

static bool writable(uintptr_t p, size_t size)
{
  return p && !(p & 7) && sys_mapped(p, size, SYS_PERM_R | SYS_PERM_W);
}

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

static uint32_t u32_at(uintptr_t a) { return *(const volatile uint32_t *)a; }

bool ragdoll_active(void)
{
  return active;
}

static bool start(void)
{
  link_t l;
  if (!find(&l))
    return false;
  static const vec3f_t zero = { 0, 0, 0 };
  MAIN_FN(void (*)(uintptr_t, const vec3f_t *, bool), z64hd_game->player_stop_linear)(l.player, &zero, false);
  MAIN_FN(void (*)(uintptr_t, const vec3f_t *, bool), z64hd_game->player_stop_angular)(l.player, &zero, false);
  MAIN_FN(void (*)(uintptr_t, uint32_t, uintptr_t, uint32_t, uint32_t), z64hd_game->player_play_as)(
    l.player, 0, MAIN(z64hd_game->as_slipping_down), 0xF, 0);
  start_t req;
  memset(&req, 0, sizeof(req));
  req.actor = l.actor;
  req.scale = 1.0f;
  req.a = 1;
  req.b = 2;
  MAIN_FN(void (*)(uintptr_t, const start_t *), z64hd_game->ragdoll_start)(l.controller, &req);
  held = l;
  last_reaction = u32_at(l.controller + C_REACTION);
  last_state = u32_at(l.controller + C_STATE);
  log_printf("ragdoll: started (controller %#lx, reaction %u, state %u)", (unsigned long)(l.controller - z64hd_main),
             (unsigned)last_reaction, (unsigned)last_state);
  return true;
}

static void stop(void)
{
  link_t l;
  /* only the same Link: after a map change the old controller is gone, and the new Link isn't ragdolled */
  if (find(&l) && l.actor == held.actor && l.controller == held.controller) {
    end_t req = { .actor = l.actor };
    MAIN_FN(void (*)(uintptr_t, const end_t *), z64hd_game->ragdoll_end)(l.controller, &req);
    *(volatile uint8_t *)(l.player + P_GET_UP) |= 1;
    log_printf("ragdoll: ended (reaction %u, state %u)", (unsigned)u32_at(l.controller + C_REACTION),
               (unsigned)u32_at(l.controller + C_STATE));
  }
  active = false;
  held = (link_t){ 0 };
}

bool ragdoll_set(bool on)
{
  if (on == active)
    return true;
  if (!on) {
    stop();
    return true;
  }
  if (!ragdoll_known()) {
    gfx_notice("ragdoll needs the 1.1.1 game");
    return false;
  }
  if (!start()) {
    gfx_notice("ragdoll: link not found");
    return false;
  }
  active = true;
  return true;
}

void ragdoll_frame(void)
{
  if (!active)
    return;
  link_t l;
  if (!find(&l) || l.actor != held.actor || l.controller != held.controller) {
    log_printf("ragdoll: link went away; let go");
    active = false;
    held = (link_t){ 0 };
    return;
  }
  uint32_t reaction = u32_at(l.controller + C_REACTION), state = u32_at(l.controller + C_STATE);
  if (reaction != last_reaction || state != last_state)
    log_printf("ragdoll: reaction %u, state %u", (unsigned)reaction, (unsigned)state);
  last_reaction = reaction;
  last_state = state;
  if (!reaction) { /* the game ended it itself */
    log_printf("ragdoll: the game ended it");
    gfx_notice("ragdoll ended");
    active = false;
    held = (link_t){ 0 };
  }
}
