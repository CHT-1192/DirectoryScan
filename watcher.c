#include "watcher.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#define PATH_SEP '\\'
#else
#define PATH_SEP '/'
#endif

/* ---- internal: build snapshot by walking the tree ---- */

static void collect_entries(Entry *entry, const char *parent_path,
                            FileInfo **array, int *count, int *cap) {
    if (!entry) return;

    /* build this entry's path */
    size_t plen = strlen(parent_path);
    size_t nlen = strlen(entry->name);
    char *full_path = malloc(plen + nlen + 2);
    if (!full_path) return;

    if (plen > 0 && parent_path[plen - 1] != PATH_SEP) {
        snprintf(full_path, plen + nlen + 2, "%s%c%s", parent_path, PATH_SEP, entry->name);
    } else {
        snprintf(full_path, plen + nlen + 2, "%s%s", parent_path, entry->name);
    }

    /* add to array */
    if (*count >= *cap) {
        *cap = *cap ? *cap * 2 : 64;
        *array = realloc(*array, *cap * sizeof(FileInfo));
    }
    (*array)[*count].path = full_path;
    (*array)[*count].mtime = entry->mtime;
    (*array)[*count].size = entry->size;
    (*array)[*count].entry = entry;
    (*count)++;

    /* recurse into children */
    for (Entry *child = entry->child; child; child = child->next) {
        collect_entries(child, full_path, array, count, cap);
    }
}

FileInfo *build_snapshot(Entry *root, const char *base_path, int *count) {
    *count = 0;
    FileInfo *array = NULL;

    /* add root entry */
    int cap = 64;
    array = malloc(cap * sizeof(FileInfo));
    if (!array) return NULL;

    /* root entry */
    array[0].path = strdup(base_path);
    array[0].mtime = root->mtime;
    array[0].size = root->size;
    array[0].entry = root;
    *count = 1;

    /* collect children */
    for (Entry *child = root->child; child; child = child->next) {
        collect_entries(child, base_path, &array, count, &cap);
    }

    return array;
}

void free_snapshot(FileInfo *snap, int count) {
    if (!snap) return;
    for (int i = 0; i < count; i++) {
        free(snap[i].path);
    }
    free(snap);
}

/* ---- change detection ---- */

int detect_changes(FileInfo *old_snap, int old_count,
                   FileInfo *new_snap, int new_count,
                   time_t now) {
    int changed = 0;

    /* For each entry in the new snapshot, find matching old entry by path */
    for (int i = 0; i < new_count; i++) {
        FileInfo *old_fi = NULL;
        for (int j = 0; j < old_count; j++) {
            if (strcmp(new_snap[i].path, old_snap[j].path) == 0) {
                old_fi = &old_snap[j];
                break;
            }
        }

        int entry_changed = 0;
        if (!old_fi) {
            /* new file/directory */
            entry_changed = 1;
        } else if (old_fi->mtime != new_snap[i].mtime ||
                   old_fi->size != new_snap[i].size) {
            entry_changed = 1;
        }

        if (entry_changed && new_snap[i].entry) {
            new_snap[i].entry->change_time = now;
            changed++;
        }
    }

    return changed;
}

/* ---- highlight relevance check ---- */

static int check_highlight(Entry *e, time_t now, time_t last_display) {
    if (!e) return 0;
    if (e->change_time > 0) {
        Config *cfg = config_get();
        int duration = cfg ? cfg->change_highlight_secs : DEFAULT_CHANGE_HIGHLIGHT_SECS;
        if (e->change_time + duration >= last_display)
            return 1;
    }
    if (check_highlight(e->child, now, last_display)) return 1;
    if (check_highlight(e->next, now, last_display)) return 1;
    return 0;
}

int has_relevant_highlight(Entry *root, time_t now, time_t last_display) {
    return check_highlight(root, now, last_display);
}
