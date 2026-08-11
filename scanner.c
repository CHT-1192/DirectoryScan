#include "scanner.h"
#include "fileutil.h"
#include "hash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif

#ifdef _WIN32
#include <windows.h>
#define PATH_SEP '\\'
/* Use wide-char stat on Windows since internal paths are UTF-8 */
#define stat_t struct _stat64
#define file_stat  _wstat64
#else
#include <dirent.h>
#include <unistd.h>
#define PATH_SEP '/'
#define stat_t struct stat
#define file_stat  stat
#endif

/* ---- internal: UTF-8 <-> wide-char conversion (Windows only) ---- */

#ifdef _WIN32

static wchar_t *utf8_to_wide(const char *utf8) {
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (len <= 0) return NULL;
    wchar_t *w = malloc(len * sizeof(wchar_t));
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, len);
    return w;
}

static char *wide_to_utf8(const wchar_t *wide) {
    int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    if (len <= 0) return NULL;
    char *u = malloc(len);
    if (!u) return NULL;
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, u, len, NULL, NULL);
    return u;
}

#endif

/* ---- internal helpers ---- */

static int compare_entries(const void *a, const void *b) {
    const Entry *ea = *(const Entry **)a;
    const Entry *eb = *(const Entry **)b;

    /* directories before files */
    if (ea->is_dir != eb->is_dir)
        return eb->is_dir - ea->is_dir; /* dir (1) before file (0) */

    /* case-insensitive alphabetical */
#ifdef _WIN32
    return _stricmp(ea->name, eb->name);
#else
    return strcasecmp(ea->name, eb->name);
#endif
}

/* Build a full path from directory + name (both UTF-8) */
static char *make_path(const char *dir, const char *name) {
    size_t dlen = strlen(dir);
    size_t nlen = strlen(name);
    char *path = malloc(dlen + nlen + 2);
    if (!path) return NULL;
    memcpy(path, dir, dlen);
    path[dlen] = PATH_SEP;
    memcpy(path + dlen + 1, name, nlen + 1);
    return path;
}

/* Get file mtime and size. Returns 0 on success, -1 on error. */
static int get_file_info(const char *path, time_t *mtime, long *size) {
#ifdef _WIN32
    wchar_t *wpath = utf8_to_wide(path);
    if (!wpath) return -1;
    stat_t st;
    int ret = file_stat(wpath, &st);
    free(wpath);
    if (ret != 0) return -1;
#else
    stat_t st;
    if (file_stat(path, &st) != 0) return -1;
#endif
    *mtime = st.st_mtime;
    *size = st.st_size;
    return 0;
}

/* Check if path is a directory */
static int is_directory(const char *path) {
#ifdef _WIN32
    wchar_t *wpath = utf8_to_wide(path);
    if (!wpath) return 0;
    stat_t st;
    int ret = file_stat(wpath, &st);
    free(wpath);
    if (ret != 0) return 0;
#else
    stat_t st;
    if (file_stat(path, &st) != 0) return 0;
#endif
    return S_ISDIR(st.st_mode) ? 1 : 0;
}

/* ---- platform-specific directory listing ---- */

#ifdef _WIN32

/* Collect sorted directory entries on Windows (paths are UTF-8).
 * rel_dir — relative path of this directory from scan root ("" at root). */
static Entry *list_directory_win(const char *dir_path, const char *rel_dir) {

    /* build search pattern: dir_path\* */
    size_t plen = strlen(dir_path);
    char *search_pattern = malloc(plen + 3);
    if (!search_pattern) return NULL;
    memcpy(search_pattern, dir_path, plen);
    search_pattern[plen] = '\\';
    search_pattern[plen + 1] = '*';
    search_pattern[plen + 2] = '\0';

    wchar_t *wpattern = utf8_to_wide(search_pattern);
    free(search_pattern);
    if (!wpattern) return NULL;

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(wpattern, &fd);
    free(wpattern);
    if (hFind == INVALID_HANDLE_VALUE) return NULL;

    /* first pass: collect entries into a dynamic array */
    Entry **items = NULL;
    int count = 0;
    int cap = 0;

    do {
        /* skip . and .. */
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;

        int is_symlink = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ? 1 : 0;

        /* determine if entry is hidden */
        int is_hidden = (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) ? 1 : 0;
        if (fd.cFileName[0] == L'.') is_hidden = 1;

        char *name = wide_to_utf8(fd.cFileName);
        if (!name) continue;

        int is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;

        /* build relative path for pattern matching */
        char rel_path[1024];
        if (rel_dir[0]) {
            snprintf(rel_path, sizeof(rel_path), "%s/%s", rel_dir, name);
        } else {
            snprintf(rel_path, sizeof(rel_path), "%s", name);
        }

        /* config filtering */
        Config *cfg = config_get();
        if (!config_should_include(cfg, name, rel_path, is_dir, is_hidden)) {
            free(name);
            continue;
        }

        char *full_path = make_path(dir_path, name);
        if (!full_path) { free(name); continue; }

        Entry *e = calloc(1, sizeof(Entry));
        if (!e) { free(full_path); free(name); continue; }

        e->name = name; /* already UTF-8 */
        e->is_dir = is_dir;
        e->mtime = 0;
        e->size = 0;
        e->line_count = is_symlink ? LINES_SYMLINK : 0;
        e->change_time = 0;

        /* get mtime/size via wide-char stat (skip for symlinks) */
        if (!is_symlink) {
            wchar_t *wfull = utf8_to_wide(full_path);
            if (wfull) {
                stat_t st;
                if (file_stat(wfull, &st) == 0) {
                    e->mtime = st.st_mtime;
                    e->size = st.st_size;
                }
                free(wfull);
            }
        }
        free(full_path);

        /* grow array if needed */
        if (count >= cap) {
            cap = cap ? cap * 2 : 32;
            items = realloc(items, cap * sizeof(Entry *));
            if (!items) { free_entries(e); break; }
        }
        items[count++] = e;

    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);

    /* sort: dirs first, alphabetical */
    qsort(items, count, sizeof(Entry *), compare_entries);

    /* build linked list */
    Entry *head = NULL, *tail = NULL;
    for (int i = 0; i < count; i++) {
        if (!head) {
            head = items[i];
            tail = items[i];
        } else {
            tail->next = items[i];
            tail = items[i];
        }
    }

    free(items);
    return head;
}

#else /* Unix */

/* Collect sorted directory entries on Unix.
 * rel_dir — relative path of this directory from scan root ("" at root). */
static Entry *list_directory_unix(const char *dir_path, const char *rel_dir) {
    DIR *dir = opendir(dir_path);
    if (!dir) return NULL;

    Entry **items = NULL;
    int count = 0;
    int cap = 0;

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        /* skip . and .. */
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;

        /* determine if entry is a symlink */
        int is_symlink = 0;
#ifdef _DIRENT_HAVE_D_TYPE
        is_symlink = (de->d_type == DT_LNK) ? 1 : 0;
#endif

        /* determine if entry is hidden (dot-prefix) */
        int is_hidden = (de->d_name[0] == '.') ? 1 : 0;

        char *full_path = make_path(dir_path, de->d_name);
        if (!full_path) continue;

        int is_dir = is_directory(full_path);

        /* build relative path for pattern matching */
        char rel_path[1024];
        if (rel_dir[0]) {
            snprintf(rel_path, sizeof(rel_path), "%s/%s", rel_dir, de->d_name);
        } else {
            snprintf(rel_path, sizeof(rel_path), "%s", de->d_name);
        }

        /* config filtering */
        Config *cfg = config_get();
        if (!config_should_include(cfg, de->d_name, rel_path, is_dir, is_hidden)) {
            free(full_path);
            continue;
        }

        Entry *e = calloc(1, sizeof(Entry));
        if (!e) { free(full_path); continue; }

        e->name = strdup(de->d_name);
        e->is_dir = is_dir;
        e->line_count = is_symlink ? LINES_SYMLINK : 0;
        e->change_time = 0;
        if (!is_symlink)
            get_file_info(full_path, &e->mtime, &e->size);

        free(full_path);

        if (count >= cap) {
            cap = cap ? cap * 2 : 32;
            items = realloc(items, cap * sizeof(Entry *));
            if (!items) { free_entries(e); break; }
        }
        items[count++] = e;
    }
    closedir(dir);

    qsort(items, count, sizeof(Entry *), compare_entries);

    Entry *head = NULL, *tail = NULL;
    for (int i = 0; i < count; i++) {
        if (!head) {
            head = items[i];
            tail = items[i];
        } else {
            tail->next = items[i];
            tail = items[i];
        }
    }

    free(items);
    return head;
}

#endif /* _WIN32 / Unix list_directory implementations */

/* platform-agnostic wrapper */
static Entry *list_directory(const char *dir_path, const char *rel_dir) {
#ifdef _WIN32
    return list_directory_win(dir_path, rel_dir);
#else
    return list_directory_unix(dir_path, rel_dir);
#endif
}

/* ---- internal recursive scanner with relative-path tracking ---- */

static Entry *scan_internal(const char *path, const char *rel_dir, int depth) {
    Config *cfg = config_get();
    int max_depth = cfg ? cfg->max_depth : DEFAULT_MAX_DEPTH;

    Entry *root = calloc(1, sizeof(Entry));
    if (!root) return NULL;

    const char *basename = strrchr(path, PATH_SEP);
    if (basename)
        basename++;
    else
        basename = path;

    root->name = strdup(basename);
    root->is_dir = 1;
    root->mtime = 0;
    root->size = 0;
    root->line_count = 0;
    root->change_time = 0;

    if (!is_directory(path)) {
        root->is_dir = 0;
        get_file_info(path, &root->mtime, &root->size);
        if (!is_binary(path)) {
            root->line_count = count_lines(path);
        } else {
            root->line_count = LINES_BINARY;
        }
        return root;
    }

    get_file_info(path, &root->mtime, &root->size);

    /* list children with relative path for pattern matching */
    Entry *children = list_directory(path, rel_dir);
    root->child = children;

    for (Entry *e = children; e; e = e->next) {
        if (e->is_dir && e->line_count != LINES_SYMLINK) {
            if (depth + 1 >= max_depth) {
                e->line_count = LINES_MAXDEPTH;
            } else {
                char *sub_path = make_path(path, e->name);
                if (sub_path) {
                    /* compute relative path for subdirectory */
                    char sub_rel[1024];
                    if (rel_dir[0])
                        snprintf(sub_rel, sizeof(sub_rel), "%s/%s", rel_dir, e->name);
                    else
                        snprintf(sub_rel, sizeof(sub_rel), "%s", e->name);

                    Entry *sub = scan_internal(sub_path, sub_rel, depth + 1);
                    if (sub) {
                        e->child = sub->child;
                        sub->child = NULL;
                        free_entries(sub);
                    }
                    free(sub_path);
                }
            }
        } else {
            char *file_path = make_path(path, e->name);
            if (file_path) {
                if (!is_binary(file_path)) {
                    e->line_count = count_lines(file_path);
                } else {
                    e->line_count = LINES_BINARY;
                }
                free(file_path);
            }
        }
    }

    return root;
}

/* ---- public API ---- */

Entry *scan_directory(const char *path, int depth) {
    (void)depth;
    return scan_internal(path, "", 0);
}

void free_entries(Entry *e) {
    if (!e) return;
    free_entries(e->next);
    free_entries(e->child);
    free(e->name);
    free(e->sha1);
    free(e);
}

/* ---- tree merge for change display ---- */

/* shallow clone: copy all fields except next and child pointers */
static Entry *clone_entry(const Entry *src) {
    if (!src) return NULL;
    Entry *e = calloc(1, sizeof(Entry));
    if (!e) return NULL;
    e->name = strdup(src->name);
    e->is_dir = src->is_dir;
    e->mtime = src->mtime;
    e->line_count = src->line_count;
    e->size = src->size;
    e->sha1 = src->sha1 ? strdup(src->sha1) : NULL;
    e->change_time = src->change_time;
    e->change_type = src->change_type;
    e->next = NULL;
    e->child = NULL;
    return e;
}

/* append entry to end of linked list */
static void append_entry(Entry **head, Entry **tail, Entry *e) {
    if (!*head) { *head = e; *tail = e; }
    else { (*tail)->next = e; *tail = e; }
}

/* Merge two sorted entry lists (prev and curr) into a display tree.
 * prev may contain CHANGE_DELETED entries from previous merges.
 * Deleted entries past highlight duration are dropped.
 * Entries only in curr → CHANGE_CREATED.
 * Entries in both with different mtime/size → CHANGE_MODIFIED.
 * Entries only in prev → cloned as CHANGE_DELETED.
 * For directories, children are recursively merged. */
static Entry *merge_sorted(Entry *prev, Entry *curr, time_t now) {
    Config *cfg = config_get();
    int duration = cfg ? cfg->change_highlight_secs : DEFAULT_CHANGE_HIGHLIGHT_SECS;
    Entry *head = NULL, *tail = NULL;

    while (prev || curr) {
        int cmp;
        if (!prev) {
            cmp = 1;
        } else if (!curr) {
            cmp = -1;
        } else {
            if (prev->is_dir != curr->is_dir)
                cmp = curr->is_dir - prev->is_dir;
            else
#ifdef _WIN32
                cmp = _stricmp(prev->name, curr->name);
#else
                cmp = strcasecmp(prev->name, curr->name);
#endif
        }

        if (cmp < 0) {
            /* prev entry not in curr → deleted or renamed-away */
            if (prev->change_type == CHANGE_DELETED ||
                prev->change_type == CHANGE_RENAMED) {
                if (now - prev->change_time < duration) {
                    Entry *cl = clone_entry(prev);
                    cl->child = merge_sorted(prev->child, NULL, now);
                    append_entry(&head, &tail, cl);
                }
                /* else: highlight expired, drop it */
            } else {
                Entry *cl = clone_entry(prev);
                cl->change_type = CHANGE_DELETED;
                cl->change_time = now;
                cl->child = merge_sorted(prev->child, NULL, now);
                append_entry(&head, &tail, cl);
            }
            prev = prev->next;

        } else if (cmp > 0) {
            /* curr entry not in prev → created */
            curr->change_type = CHANGE_CREATED;
            curr->change_time = now;
            Entry *nxt = curr->next;
            curr->next = NULL;
            append_entry(&head, &tail, curr);
            curr = nxt;

        } else {
            /* exists in both */
            if (prev->mtime != curr->mtime || prev->size != curr->size) {
                curr->change_type = CHANGE_MODIFIED;
                curr->change_time = now;
            } else if (prev->change_type == CHANGE_DELETED ||
                       prev->change_type == CHANGE_CREATED ||
                       prev->change_type == CHANGE_MODIFIED ||
                       prev->change_type == CHANGE_RENAMED) {
                /* carry over existing highlight */
                curr->change_type = prev->change_type;
                curr->change_time = prev->change_time;
            }
            /* merge children for directories */
            if (curr->is_dir) {
                Entry *merged_children = merge_sorted(
                    prev->child, curr->child, now);
                curr->child = merged_children;
            }
            Entry *nxt = curr->next;
            curr->next = NULL;
            append_entry(&head, &tail, curr);
            curr = nxt;
            prev = prev->next;
        }
    }

    return head;
}

/* ---- rename detection: match deleted + created entries by size ---- */

typedef struct {
    Entry *entry;
    int used;
} MatchEntry;

static void collect_by_type(Entry *e, int wanted_type,
                            MatchEntry **arr, int *count, int *cap) {
    if (!e) return;
    if (e->change_type == wanted_type) {
        if (*count >= *cap) {
            *cap = *cap ? *cap * 2 : 16;
            *arr = realloc(*arr, *cap * sizeof(MatchEntry));
        }
        (*arr)[*count].entry = e;
        (*arr)[*count].used = 0;
        (*count)++;
    }
    collect_by_type(e->child, wanted_type, arr, count, cap);
    collect_by_type(e->next, wanted_type, arr, count, cap);
}

/* count total entries in tree (excluding root) */
static int count_tree_entries(Entry *e) {
    if (!e) return 0;
    int n = 1;
    for (Entry *c = e->child; c; c = c->next) n += count_tree_entries(c);
    return n;
}

static void match_renames(Entry *root) {
    MatchEntry *deleted = NULL, *created = NULL;
    int nd = 0, nc = 0, cap = 0;

    collect_by_type(root, CHANGE_DELETED, &deleted, &nd, &cap);
    cap = 0;
    collect_by_type(root, CHANGE_CREATED, &created, &nc, &cap);

    /* ---- four-factor check (fast path) ---- */
    for (int d = 0; d < nd; d++) {
        if (deleted[d].used) continue;
        for (int c = 0; c < nc; c++) {
            if (created[c].used) continue;
            Entry *de = deleted[d].entry;
            Entry *ce = created[c].entry;
            if (de->size == ce->size &&
                de->mtime == ce->mtime &&
                de->is_dir == ce->is_dir &&
                de->line_count == ce->line_count) {
                de->change_type = CHANGE_RENAMED;
                ce->change_type = CHANGE_RENAMED;
                deleted[d].used = 1;
                created[c].used = 1;
                break;
            }
        }
    }

    /* ---- SHA-1 hash fallback ---- */
    Config *cfg = config_get();
    if (!cfg || !cfg->hash_enabled) goto done;

    /* check file count limit */
    {
        int total = count_tree_entries(root);
        if (cfg->hash_max_files > 0 && total > cfg->hash_max_files) goto done;
    }

    /* compare SHA-1 of remaining unmatched entries */
    for (int d = 0; d < nd; d++) {
        if (deleted[d].used || deleted[d].entry->is_dir) continue;
        if (!deleted[d].entry->sha1) continue;
        for (int c = 0; c < nc; c++) {
            if (created[c].used || created[c].entry->is_dir) continue;
            if (!created[c].entry->sha1) continue;
            if (strcmp(deleted[d].entry->sha1, created[c].entry->sha1) == 0) {
                deleted[d].entry->change_type = CHANGE_RENAMED;
                created[c].entry->change_type = CHANGE_RENAMED;
                deleted[d].used = 1;
                created[c].used = 1;
                break;
            }
        }
    }

done:
    free(deleted);
    free(created);
}

/* ---- post-scan SHA-1 hashing ---- */

static void collect_hash_jobs(Entry *e, const char *parent_path,
                               HashJob **jobs, int *count, int *cap,
                               long max_size) {
    if (!e) return;
    char *full = make_path(parent_path, e->name);
    if (!full) return;

    /* add job for this file if eligible */
    if (!e->is_dir && e->line_count != LINES_BINARY &&
        (max_size == 0 || e->size <= max_size)) {
        if (*count >= *cap) {
            *cap = *cap ? *cap * 2 : 64;
            *jobs = realloc(*jobs, *cap * sizeof(HashJob));
        }
        (*jobs)[*count].path = strdup(full);
        (*jobs)[*count].max_size = max_size;
        (*jobs)[*count].sha1 = NULL;
        (*jobs)[*count].done = 0;
        (*jobs)[*count].ctx = e;
        (*count)++;
    }

    /* recurse into children with this entry's full path */
    for (Entry *c = e->child; c; c = c->next)
        collect_hash_jobs(c, full, jobs, count, cap, max_size);

    free(full);
}

void scan_hash_files(Entry *root, const char *scan_path) {
    Config *cfg = config_get();
    if (!cfg || !cfg->hash_enabled) return;

    int total = 0;
    for (Entry *c = root->child; c; c = c->next)
        total += count_tree_entries(c);
    if (cfg->hash_max_files > 0 && total > cfg->hash_max_files) return;

    long max_size = cfg->hash_max_size_mib > 0
                    ? (long)cfg->hash_max_size_mib * 1024 * 1024 : 0;

    HashJob *jobs = NULL;
    int job_count = 0, cap = 0;
    for (Entry *c = root->child; c; c = c->next)
        collect_hash_jobs(c, scan_path, &jobs, &job_count, &cap, max_size);

    if (job_count == 0) return;

    hash_pool_start(cfg->hash_threads);
    hash_pool_submit(jobs, job_count);
    hash_pool_wait();

    /* store results back to entries */
    for (int i = 0; i < job_count; i++) {
        if (jobs[i].sha1) {
            Entry *e = (Entry *)jobs[i].ctx;
            free(e->sha1);
            e->sha1 = jobs[i].sha1;  /* transfer ownership */
        }
        free((void *)jobs[i].path);
    }
    free(jobs);
    hash_pool_stop();
}

/* Build a display tree by merging previous tree with new scan.
 * prev_root may be NULL (first run).
 * Returns a new tree that includes deleted entries in-place.
 * Caller owns the returned tree. */
Entry *merge_display_tree(Entry *prev_root, Entry *new_root, time_t now) {
    if (!prev_root) {
        /* first run: just return new_root as-is */
        return new_root;
    }

    /* merge children of both roots */
    Entry *merged_children = merge_sorted(
        prev_root->child, new_root->child, now);

    /* update new_root's children to the merged list */
    new_root->child = merged_children;

    /* match deleted+created pairs by size+mtime → rename (yellow) */
    match_renames(new_root);

    /* carry over root change info if any */
    if (prev_root->change_time > 0) {
        new_root->change_time = prev_root->change_time;
        new_root->change_type = prev_root->change_type;
    }

    return new_root;
}
