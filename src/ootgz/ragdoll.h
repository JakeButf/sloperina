/* Ragdoll Link on demand, with the calls the game's event node ExecuteEventPlayerImmediateRagdollDown makes
 * (FINDINGS 9.9). 1.1.1 only. */
#ifndef RAGDOLL_H
#define RAGDOLL_H

#include <stdbool.h>

bool ragdoll_known(void);  /* this build's offsets are known */
bool ragdoll_active(void); /* Link is ragdolled by us (and the game hasn't ended it) */
/* game thread: start (true) or end (false); false if it couldn't be done */
bool ragdoll_set(bool on);

/* game thread, once a frame: notices when the game ends it or Link goes away (a map change) */
void ragdoll_frame(void);

#endif
