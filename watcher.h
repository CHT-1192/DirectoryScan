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

/* Build a flat snapshot array from the entry tree.
 * Returns malloc'd array, sets *count. Caller frees with free_snapshot(). */
FileInfo *build_snapshot(Entry *root, const char *base_path, int *count);

/* Free a snapshot array. */
void free_snapshot(FileInfo *snap, int count);

/* Compare old and new snapshots. For entries whose mtime or size changed,
 * mark the corresponding tree entry with change_time = now.
 * Returns the number of changed entries. */
int detect_changes(FileInfo *old_snap, int old_count,
                   FileInfo *new_snap, int new_count,
                   time_t now);

/* Check if any entry in the tree has an active or recently-expired highlight.
 * An entry is "relevant" if it changed between last_display and now
 * (i.e., its highlight was active at some point since last_display). */
int has_relevant_highlight(Entry *root, time_t now, time_t last_display);

#endif
