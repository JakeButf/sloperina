/* The Switch's own keyboard (nn::swkbd, exported by the game's sdk module), for naming things: the system's
 * software keyboard applet, with its own controls (the stick, L/R to move the cursor, B to delete, Y for a space,
 * + to finish), touch and a USB keyboard. The game is held while it's up, as when a game asks for a name. */
#ifndef SWKBD_H
#define SWKBD_H

#include <stdbool.h>

/* settings > menu settings > keyboard: the Switch's (true, the default) or sloperinaGZ's own (osk.c) */
extern bool swkbd_enabled;

enum { SWKBD_OK, SWKBD_CANCELLED, SWKBD_UNAVAILABLE };

/* Game thread. Show it with `header` over it and `initial` in it, at most `max` characters; on SWKBD_OK, `out`
 * (out_len) holds what was typed, ASCII (anything else as '_', which the menu's font can't show).
 * SWKBD_UNAVAILABLE: it couldn't be shown (the caller uses osk.c's keyboard instead). */
int swkbd_ask(const char *header, const char *initial, int max, char *out, int out_len);

#endif
