#ifndef WATCHER_H
#define WATCHER_H

#include "scanner.h"

/* Flat file info record for change detection snapshots */
typedef struct {
    char *path;       /* full path relative to scan root */
    time_t mtime;
    long size;
    Entry *entry;     /* pointer back to tree entry */
} FileInfo;

/* Deleted entry — kept for highlighting after removal */
typedef struct DeletedEntry {
    char *name;       /* basename */
    char *rel_path;   /* relative path from scan root */
    int is_dir;
    time_t del_time;  /* when deletion was detected */
    struct DeletedEntry *next;
} DeletedEntry;

/* Build a flat snapshot array from the entry tree.
 * Returns malloc'd array, sets *count. Caller frees with free_snapshot(). */
FileInfo *build_snapshot(Entry *root, const char *base_path, int *count);

/* Free a snapshot array. */
void free_snapshot(FileInfo *snap, int count);

/* Compare old and new snapshots. Marks tree entries with change_type:
 *   CHANGE_CREATED  — in new but not old
 *   CHANGE_MODIFIED — in both, different mtime/size
 * Deleted entries (in old but not new) are added to the global deleted list.
 * Returns the total number of changes (created + modified + deleted). */
int detect_changes(FileInfo *old_snap, int old_count,
                   FileInfo *new_snap, int new_count,
                   time_t now);

/* Get the linked list of recently-deleted entries. */
DeletedEntry *watcher_get_deleted(void);

/* Remove expired (past highlight duration) entries from the list. */
void watcher_prune_deleted(time_t now);

/* Check if any entry in the tree OR deleted list has an active
 * or recently-expired highlight. */
int has_relevant_highlight(Entry *root, DeletedEntry *del,
                           time_t now, time_t last_display);

#endif
