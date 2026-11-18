/* save management (top-level menu): after every save the game commits (autosave or manual), copy its whole save
 * mount to sd:/sloperinaGZ/save-backups/<YYYY-MM-DD_HH-MM-SS>[ - <location>]/, keeping the newest
 * SAVE_BACKUPS_KEPT. From the menu, also: archive the mount now, and restore an archive into it (for the game's
 * own Load to read). */
#ifndef SAVEBACKUP_H
#define SAVEBACKUP_H

#include <stdbool.h>
#include <stdint.h>

#define SAVE_BACKUPS_KEPT 100
#define SAVE_ARCHIVES_LISTED 128    /* of the named archives, and of the backups: offered to restore */
#define SAVE_ARCHIVE_NAME_MAX 30    /* characters typed for a named archive */
#define SAVE_ARCHIVE_NUMBER_MAX 999
#define SAVE_FOLDER_LEN 128         /* a folder's path in save-archives/ (folders in folders) */

enum { SAVE_BACKUP_AUTO, SAVE_BACKUP_HARD, SAVE_BACKUP_BOTH };

extern bool save_backup_enabled;
extern int save_backup_kinds;     /* which saves: SAVE_BACKUP_* */
extern bool save_backup_location; /* " - <location>" after the date in the folder names */
/* a hard save as the next numbered archive where archive now puts them ("#008 - <location> - <date>"), not in
 * save-backups/ with the autosaves */
extern bool save_backup_hard_numbered;

/* At module init: hook main's nn::fs::CommitSaveData. Returns 0, or what went wrong. */
const char *savebackup_hook_game(uintptr_t main_base);
/* Once a frame, on the game's thread: report backups made since (the copy runs inside the game's commit). */
void savebackup_frame(void);
/* For the menu: the last backup, or why there's none. */
void savebackup_status_text(char *buf, int len);

/* The menu's, on the game's thread. Each shows a notice when it's done or can't be. */
/* Copy the save mount, as the game last wrote it, to sd:/sloperinaGZ/save-archives/[<folder>/]#<number> - <name> -
 * <date> (DD-MM-YYYY HH-MM) now; no name typed: the save's location. Archives are never trimmed. The number then
 * goes up by one. */
void savebackup_archive_now(const char *name);
int savebackup_next_number(void);       /* the next archive's (the highest in its folder + 1, until it's changed) */
void savebackup_set_next_number(int number);
const char *savebackup_location_name(void); /* the save's location, e.g. "Temple of Time", or "" */
/* where archive now puts them: a folder in save-archives/ ("a/b"; "": save-archives/ itself), kept in the
 * settings; one that isn't a good path is "" */
const char *savebackup_archive_folder(void);
void savebackup_set_archive_folder(const char *folder);

/* save management > manage archives: lists of one folder at a time. In save-archives/ and the folders made in it,
 * a folder whose name starts with '#' is an archive, any other a folder of them. */
int savebackup_list_named(void);     /* the named archives' folder being looked at: folders, A to Z, then archives */
int savebackup_list_automatic(void); /* save-backups/: newest first */
int savebackup_list_folders(void);   /* how many of the last list's first entries are folders */
const char *savebackup_archive_name(int i); /* an entry of the last list; "" past its end */
const char *savebackup_browsing(void);      /* the folder looked at, as savebackup_archive_folder */
bool savebackup_browse_into(int i);         /* into folder i of the last list */
void savebackup_browse_up(void);
void savebackup_browse_top(void);
void savebackup_archive_here(void);         /* new archives go to the folder looked at */
void savebackup_archive_here_now(const char *name); /* archive now, into the folder looked at (its own numbers) */
bool savebackup_make_folder(const char *name); /* a folder in the one looked at */
bool savebackup_delete_folder(void);        /* the one looked at, and all in it; then up a folder */
/* Archive the save mount as it is, then copy archive i (of the last list) into it, every file but the options,
 * committing each. The game keeps playing what it loaded: its Load screen then loads the restored save. */
bool savebackup_restore(int i);
/* Delete archive i (of the last list) from the SD card. */
bool savebackup_delete(int i);

#endif
