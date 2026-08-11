#include "watcher.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#define PATH_SEP '\\'
#else
#define PATH_SEP '/'
#endif

/* ---- global deleted entries list ---- */
static DeletedEntry *g_deleted = NULL;

/* ---- internal: build snapshot by walking the tree ---- */

static void collect_entries(Entry *entry, const char *parent_path,
                            FileInfo **array, int *count, int *cap) {
    if (!entry) return;

    size_t plen = strlen(parent_path);
    size_t nlen = strlen(entry->name);
    char *full_path = malloc(plen + nlen + 2);
    if (!full_path) return;

    if (plen > 0 && parent_path[plen - 1] != PATH_SEP) {
        snprintf(full_path, plen + nlen + 2, "%s%c%s", parent_path, PATH_SEP, entry->name);
    } else {
        snprintf(full_path, plen + nlen + 2, "%s%s", parent_path, entry->name);
    }

    if (*count >= *cap) {
        *cap = *cap ? *cap * 2 : 64;
        *array = realloc(*array, *cap * sizeof(FileInfo));
    }
    (*array)[*count].path = full_path;
    (*array)[*count].mtime = entry->mtime;
    (*array)[*count].size = entry->size;
    (*array)[*count].entry = entry;
    (*count)++;

    for (Entry *child = entry->child; child; child = child->next) {
        collect_entries(child, full_path, array, count, cap);
    }
}

FileInfo *build_snapshot(Entry *root, const char *base_path, int *count) {
    *count = 0;
    int cap = 64;
    FileInfo *array = malloc(cap * sizeof(FileInfo));
    if (!array) return NULL;

    array[0].path = strdup(base_path);
    array[0].mtime = root->mtime;
    array[0].size = root->size;
    array[0].entry = root;
    *count = 1;

    for (Entry *child = root->child; child; child = child->next) {
        collect_entries(child, base_path, &array, count, &cap);
    }

    return array;
}

void free_snapshot(FileInfo *snap, int count) {
    if (!snap) return;
    for (int i = 0; i < count; i++) free(snap[i].path);
    free(snap);
}

/* ---- change detection ---- */

int detect_changes(FileInfo *old_snap, int old_count,
                   FileInfo *new_snap, int new_count,
                   time_t now) {
    int changes = 0;

    /* 1. find created and modified entries in new snapshot */
    for (int i = 0; i < new_count; i++) {
        FileInfo *old_fi = NULL;
        for (int j = 0; j < old_count; j++) {
            if (strcmp(new_snap[i].path, old_snap[j].path) == 0) {
                old_fi = &old_snap[j];
                break;
            }
        }

        if (!old_fi) {
            /* created */
            if (new_snap[i].entry) {
                new_snap[i].entry->change_time = now;
                new_snap[i].entry->change_type = CHANGE_CREATED;
                changes++;
            }
        } else if (old_fi->mtime != new_snap[i].mtime ||
                   old_fi->size != new_snap[i].size) {
            /* modified */
            if (new_snap[i].entry) {
                new_snap[i].entry->change_time = now;
                new_snap[i].entry->change_type = CHANGE_MODIFIED;
                changes++;
            }
        }
    }

    /* 2. find deleted entries (in old but not new) */
    for (int i = 0; i < old_count; i++) {
        /* skip root */
        if (old_count > 0 && i == 0) continue;

        int found = 0;
        for (int j = 0; j < new_count; j++) {
            if (strcmp(old_snap[i].path, new_snap[j].path) == 0) {
                found = 1;
                break;
            }
        }

        if (!found) {
            /* deleted — add to global list */
            DeletedEntry *de = calloc(1, sizeof(DeletedEntry));
            if (!de) continue;

            /* basename from path */
            const char *basename = strrchr(old_snap[i].path, PATH_SEP);
            de->name = strdup(basename ? basename + 1 : old_snap[i].path);
            de->rel_path = strdup(old_snap[i].path);
            de->is_dir = old_snap[i].entry ? old_snap[i].entry->is_dir : 0;
            de->del_time = now;
            de->next = g_deleted;
            g_deleted = de;
            changes++;
        }
    }

    return changes;
}

/* ---- deleted entries access ---- */

DeletedEntry *watcher_get_deleted(void) {
    return g_deleted;
}

void watcher_prune_deleted(time_t now) {
    Config *cfg = config_get();
    int duration = cfg ? cfg->change_highlight_secs : DEFAULT_CHANGE_HIGHLIGHT_SECS;

    DeletedEntry **prev = &g_deleted;
    while (*prev) {
        DeletedEntry *d = *prev;
        if (now - d->del_time >= duration) {
            *prev = d->next;
            free(d->name);
            free(d->rel_path);
            free(d);
        } else {
            prev = &d->next;
        }
    }
}

/* ---- highlight relevance check ---- */

static int entry_has_highlight(Entry *e, time_t now, time_t last_display) {
    if (!e) return 0;
    if (e->change_time > 0) {
        Config *cfg = config_get();
        int duration = cfg ? cfg->change_highlight_secs : DEFAULT_CHANGE_HIGHLIGHT_SECS;
        if (e->change_time + duration >= last_display)
            return 1;
    }
    if (entry_has_highlight(e->child, now, last_display)) return 1;
    if (entry_has_highlight(e->next, now, last_display)) return 1;
    return 0;
}

int has_relevant_highlight(Entry *root, DeletedEntry *del,
                           time_t now, time_t last_display) {
    if (entry_has_highlight(root, now, last_display)) return 1;

    Config *cfg = config_get();
    int duration = cfg ? cfg->change_highlight_secs : DEFAULT_CHANGE_HIGHLIGHT_SECS;
    for (DeletedEntry *d = del; d; d = d->next) {
        if (d->del_time + duration >= last_display) return 1;
    }
    return 0;
}
