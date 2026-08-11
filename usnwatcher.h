#ifndef USNWATCHER_H
#define USNWATCHER_H

#include "scanner.h"

#ifdef _WIN32

/* Rename pair: old_name → new_name (basenames), both in same directory */
typedef struct {
    char *old_name;
    char *new_name;
} RenamePair;

/* Rename group: all renames in one directory (parent path) */
typedef struct RenameGroup {
    char *parent_path;      /* directory containing renamed entries */
    RenamePair *pairs;
    int pair_count;
    struct RenameGroup *next;
} RenameGroup;

/* ---- lifecycle ---- */

/* Try to open the USN Journal for the volume containing `scan_path`.
 * Returns 1 on success, 0 if USN is unavailable. */
int usnwatcher_init(const char *scan_path);

/* Build initial FRN→path mapping from scanned tree (call after first scan). */
void usnwatcher_build_frn_map(Entry *root, const char *scan_path);

void usnwatcher_close(void);

/* ---- change detection ---- */

/* Read incremental USN records since last call.
 * Marks tree entries with change_type + change_time.
 * Returns total number of changes (created + modified + deleted + renamed).
 * On rename, both old and new names are marked CHANGE_RENAMED. */
int usnwatcher_read_changes(const char *scan_path, time_t now);

/* ---- rename info ---- */

/* Get grouped rename info for matching old→new names during display.
 * Each RenameGroup contains pairs of (old_name, new_name) in the same directory.
 * Caller must free with usnwatcher_free_renames(). */
RenameGroup *usnwatcher_get_renames(void);
void usnwatcher_free_renames(RenameGroup *groups);

#endif /* _WIN32 */
#endif /* USNWATCHER_H */
