/* settings > back up saves: after every save the game commits (autosave or manual), copy its whole save
 * mount to sd:/sloperinaGZ/save-backups/<YYYY-MM-DD_HH-MM-SS>[ - <location>]/, keeping the newest
 * SAVE_BACKUPS_KEPT. */
#ifndef SAVEBACKUP_H
#define SAVEBACKUP_H

#include <stdbool.h>
#include <stdint.h>

#define SAVE_BACKUPS_KEPT 100

enum { SAVE_BACKUP_AUTO, SAVE_BACKUP_HARD, SAVE_BACKUP_BOTH };

extern bool save_backup_enabled;
extern int save_backup_kinds;     /* which saves: SAVE_BACKUP_* */
extern bool save_backup_location; /* " - <location>" after the date in the folder names */

/* At module init: hook main's nn::fs::CommitSaveData. Returns 0, or what went wrong. */
const char *savebackup_hook_game(uintptr_t main_base);
/* Once a frame, on the game's thread: report backups made since (the copy runs inside the game's commit). */
void savebackup_frame(void);
/* For the menu: the last backup, or why there's none. */
void savebackup_status_text(char *buf, int len);

#endif
