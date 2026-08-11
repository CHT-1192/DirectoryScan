#ifndef SCANNER_H
#define SCANNER_H

#include <time.h>
#include "config.h"

/* Sentinel values for line_count */
#define LINES_BINARY    -1  /* binary file, no line count shown */
#define LINES_MAXDEPTH  -2  /* directory at max depth, show "MAX DEPTH" */

/* Change types for highlighting */
#define CHANGE_NONE      0
#define CHANGE_CREATED   1
#define CHANGE_MODIFIED  2
#define CHANGE_DELETED   3

typedef struct Entry {
    char *name;           /* file/directory name (without path) */
    int is_dir;           /* 1 if directory, 0 if file */
    time_t mtime;         /* last modified time */
    long line_count;      /* LINES_BINARY, LINES_MAXDEPTH, or >=0 */
    long size;            /* file size in bytes; 0 for directories */
    time_t change_time;   /* when last change was detected, 0 = never */
    int change_type;      /* CHANGE_NONE / _CREATED / _MODIFIED / _DELETED */
    struct Entry *next;   /* next sibling */
    struct Entry *child;  /* first child (NULL for files) */
} Entry;

/* Scan a directory recursively starting from `path`.
 * depth: current recursion depth (0 = root level).
 * Uses config_get()->max_depth (falls back to DEFAULT_MAX_DEPTH).
 * Returns the tree root (a dummy Entry whose name is the path and child
 * holds the actual contents). Returns NULL on error.
 * Caller must free with free_entries(). */
Entry *scan_directory(const char *path, int depth);

/* Free all memory allocated for an entry tree. */
void free_entries(Entry *root);

#endif
