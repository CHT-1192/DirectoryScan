#include "usnwatcher.h"

#ifdef _WIN32

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- USN Journal data types (compatible with MinGW) ---- */

/* USN_JOURNAL_DATA_V0 is the 64-bit version used since Win2000 */
#ifndef USN_JOURNAL_DATA_V0
typedef struct {
    DWORDLONG UsnJournalID;
    USN       FirstUsn;
    USN       NextUsn;
    USN       LowestValidUsn;
    USN       MaxUsn;
    DWORDLONG MaximumSize;
    DWORDLONG AllocationDelta;
} USN_JOURNAL_DATA_V0;
#endif

#ifndef READ_USN_JOURNAL_DATA_V0
typedef struct {
    USN       StartUsn;
    DWORD     ReasonMask;
    DWORD     ReturnOnlyOnClose;
    DWORDLONG Timeout;
    DWORDLONG BytesToWaitFor;
    DWORDLONG UsnJournalID;
} READ_USN_JOURNAL_DATA_V0;
#endif

/* ---- FRN → path mapping ---- */

#define FRN_MAP_CAP 2048

typedef struct {
    DWORDLONG frn;
    char *path;
    int is_dir;
} FrnEntry;

static FrnEntry *g_frn_map = NULL;
static int g_frn_count = 0;
static int g_frn_cap = 0;

static void frn_map_set(DWORDLONG frn, const char *path, int is_dir) {
    for (int i = 0; i < g_frn_count; i++) {
        if (g_frn_map[i].frn == frn) {
            free(g_frn_map[i].path);
            g_frn_map[i].path = strdup(path);
            g_frn_map[i].is_dir = is_dir;
            return;
        }
    }
    if (g_frn_count >= g_frn_cap) {
        g_frn_cap = g_frn_cap ? g_frn_cap * 2 : FRN_MAP_CAP;
        g_frn_map = realloc(g_frn_map, g_frn_cap * sizeof(FrnEntry));
    }
    g_frn_map[g_frn_count].frn = frn;
    g_frn_map[g_frn_count].path = strdup(path);
    g_frn_map[g_frn_count].is_dir = is_dir;
    g_frn_count++;
}

static const char *frn_map_get_path(DWORDLONG frn) {
    for (int i = 0; i < g_frn_count; i++)
        if (g_frn_map[i].frn == frn) return g_frn_map[i].path;
    return NULL;
}

static void frn_map_remove(DWORDLONG frn) {
    for (int i = 0; i < g_frn_count; i++) {
        if (g_frn_map[i].frn == frn) {
            free(g_frn_map[i].path);
            g_frn_map[i] = g_frn_map[--g_frn_count];
            return;
        }
    }
}

/* ---- get FRN from file path ---- */

static DWORDLONG get_frn_by_path(const char *utf8_path) {
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8_path, -1, NULL, 0);
    if (wlen <= 0) return 0;
    wchar_t *wpath = malloc(wlen * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, utf8_path, -1, wpath, wlen);

    HANDLE h = CreateFileW(wpath, FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(wpath);
    if (h == INVALID_HANDLE_VALUE) return 0;

    BY_HANDLE_FILE_INFORMATION info;
    DWORDLONG frn = 0;
    if (GetFileInformationByHandle(h, &info)) {
        frn = ((DWORDLONG)info.nFileIndexHigh << 32) | info.nFileIndexLow;
    }
    CloseHandle(h);
    return frn;
}

/* ---- build initial FRN map from tree ---- */

static void frn_map_from_tree(Entry *e, const char *parent_path) {
    if (!e) return;
    size_t plen = strlen(parent_path);
    char *full = malloc(plen + strlen(e->name) + 2);
    sprintf(full, "%s\\%s", parent_path, e->name);

    DWORDLONG frn = get_frn_by_path(full);
    if (frn) frn_map_set(frn, full, e->is_dir);

    for (Entry *c = e->child; c; c = c->next)
        frn_map_from_tree(c, full);

    free(full);
}

/* ---- volume handle ---- */

static HANDLE g_hVol = INVALID_HANDLE_VALUE;
static DWORDLONG g_next_usn = 0;
static int g_usn_available = 0;

static BOOL enable_backup_privilege(void) {
    HANDLE hToken;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
        return FALSE;

    TOKEN_PRIVILEGES tp;
    LUID luid;
    if (!LookupPrivilegeValueW(NULL, L"SeBackupPrivilege", &luid)) {
        CloseHandle(hToken);
        return FALSE;
    }
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    BOOL ok = AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), NULL, NULL);
    CloseHandle(hToken);
    return ok && GetLastError() == ERROR_SUCCESS;
}

static HANDLE open_volume(const char *vol) {
    /* SE_BACKUP_NAME privilege is required to open volume handles */
    enable_backup_privilege();

    char vol_path[64];
    snprintf(vol_path, sizeof(vol_path), "\\\\.\\%s", vol);
    wchar_t wvol[64];
    MultiByteToWideChar(CP_UTF8, 0, vol_path, -1, wvol, 64);
    HANDLE h = CreateFileW(wvol, GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL, OPEN_EXISTING,
                            FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        /* retry with GENERIC_WRITE if read-only fails */
        h = CreateFileW(wvol, GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_EXISTING,
                        FILE_FLAG_BACKUP_SEMANTICS, NULL);
    }
    return h;
}

/* ---- rename tracking ---- */

typedef struct {
    char *old_name;
    char *parent_path;
    DWORDLONG frn;
} PendingRename;

static PendingRename *g_pending = NULL;
static int g_pending_count = 0;
static RenameGroup *g_renames = NULL;

static void renames_add(const char *parent_path,
                         const char *old_name, const char *new_name) {
    RenameGroup *g = g_renames;
    while (g) {
        if (strcmp(g->parent_path, parent_path) == 0) break;
        g = g->next;
    }
    if (!g) {
        g = calloc(1, sizeof(RenameGroup));
        g->parent_path = strdup(parent_path);
        g->next = g_renames;
        g_renames = g;
    }
    g->pairs = realloc(g->pairs, (g->pair_count + 1) * sizeof(RenamePair));
    g->pairs[g->pair_count].old_name = strdup(old_name);
    g->pairs[g->pair_count].new_name = strdup(new_name);
    g->pair_count++;
}

static void pending_add(DWORDLONG frn, const char *old_name,
                         const char *parent_path) {
    g_pending = realloc(g_pending, (g_pending_count + 1) * sizeof(PendingRename));
    g_pending[g_pending_count].frn = frn;
    g_pending[g_pending_count].old_name = strdup(old_name);
    g_pending[g_pending_count].parent_path = strdup(parent_path);
    g_pending_count++;
}

static int pending_take(DWORDLONG frn, char **old_name, char **parent_path) {
    for (int i = 0; i < g_pending_count; i++) {
        if (g_pending[i].frn == frn) {
            *old_name = g_pending[i].old_name;
            *parent_path = g_pending[i].parent_path;
            free(g_pending[i].parent_path); /* already consumed, just free */
            g_pending[i] = g_pending[--g_pending_count];
            return 1;
        }
    }
    return 0;
}

static void pending_clear(void) {
    for (int i = 0; i < g_pending_count; i++) {
        free(g_pending[i].old_name);
        free(g_pending[i].parent_path);
    }
    free(g_pending);
    g_pending = NULL;
    g_pending_count = 0;
}

static void renames_clear(void) {
    while (g_renames) {
        RenameGroup *next = g_renames->next;
        for (int i = 0; i < g_renames->pair_count; i++) {
            free(g_renames->pairs[i].old_name);
            free(g_renames->pairs[i].new_name);
        }
        free(g_renames->pairs);
        free(g_renames->parent_path);
        free(g_renames);
        g_renames = next;
    }
}

/* ---- public API ---- */

/* ---- public API ---- */

#include "log.h"

int usnwatcher_init(const char *scan_path) {
    /* extract volume letter: "C:\foo" → "C:" */
    if (strlen(scan_path) < 2 || scan_path[1] != ':') {
        log_info("USN: not an absolute path with drive letter: %s", scan_path);
        return 0;
    }
    char vol[4];
    snprintf(vol, sizeof(vol), "%c:", scan_path[0]);

    g_hVol = open_volume(vol);
    if (g_hVol == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        log_info("USN: cannot open volume %s (error %lu, need SeBackupPrivilege)", vol, err);
        return 0;
    }

    /* query journal to get baseline */
    USN_JOURNAL_DATA_V0 ujd = {0};
    DWORD bytes;
    if (!DeviceIoControl(g_hVol, FSCTL_QUERY_USN_JOURNAL,
                         NULL, 0, &ujd, sizeof(ujd), &bytes, NULL)) {
        DWORD err = GetLastError();
        log_info("USN: FSCTL_QUERY_USN_JOURNAL failed (error %lu, journal may be disabled)", err);
        CloseHandle(g_hVol);
        g_hVol = INVALID_HANDLE_VALUE;
        return 0;
    }
    g_next_usn = ujd.NextUsn;
    g_usn_available = 1;
    log_info("USN: journal active, next USN = %llu", (unsigned long long)g_next_usn);
    return 1;
}

void usnwatcher_build_frn_map(Entry *root, const char *scan_path) {
    for (int i = 0; i < g_frn_count; i++) free(g_frn_map[i].path);
    free(g_frn_map);
    g_frn_map = NULL;
    g_frn_count = g_frn_cap = 0;

    DWORDLONG root_frn = get_frn_by_path(scan_path);
    if (root_frn) frn_map_set(root_frn, scan_path, 1);

    for (Entry *c = root->child; c; c = c->next)
        frn_map_from_tree(c, scan_path);
}

void usnwatcher_close(void) {
    if (g_hVol != INVALID_HANDLE_VALUE) {
        CloseHandle(g_hVol);
        g_hVol = INVALID_HANDLE_VALUE;
    }
    g_usn_available = 0;
    for (int i = 0; i < g_frn_count; i++) free(g_frn_map[i].path);
    free(g_frn_map);
    g_frn_map = NULL;
    g_frn_count = g_frn_cap = 0;
    pending_clear();
    renames_clear();
}

int usnwatcher_read_changes(const char *scan_path, time_t now) {
    (void)now;
    if (!g_usn_available || g_hVol == INVALID_HANDLE_VALUE) return 0;

    pending_clear();
    renames_clear();
    int changes = 0;

    /* read USN records */
    DWORD buf_size = 256 * 1024;
    USN_RECORD *buf = malloc(buf_size);
    if (!buf) return 0;

    READ_USN_JOURNAL_DATA_V0 rujd = {0};
    rujd.StartUsn = g_next_usn;
    rujd.ReasonMask = 0xFFFFFFFF;
    rujd.ReturnOnlyOnClose = 0;
    rujd.Timeout = 0;
    rujd.BytesToWaitFor = 0;
    rujd.UsnJournalID = 0;

    DWORD bytes = 0;
    if (!DeviceIoControl(g_hVol, FSCTL_READ_USN_JOURNAL,
                         &rujd, sizeof(rujd), buf, buf_size, &bytes, NULL)) {
        if (GetLastError() == ERROR_HANDLE_EOF) {
            free(buf);
            return 0; /* no new records */
        }
        free(buf);
        return 0;
    }

    /* walk records */
    DWORD offset = 0;
    while (offset < bytes) {
        USN_RECORD *rec = (USN_RECORD *)((BYTE *)buf + offset);
        if (rec->RecordLength == 0) break;

        DWORDLONG frn = rec->FileReferenceNumber;
        DWORDLONG parent_frn = rec->ParentFileReferenceNumber;
        DWORD reason = rec->Reason;

        WCHAR *wname = (WCHAR *)((BYTE *)rec + rec->FileNameOffset);
        int nlen = rec->FileNameLength / (int)sizeof(WCHAR);
        char name_utf8[512];
        WideCharToMultiByte(CP_UTF8, 0, wname, nlen,
                            name_utf8, sizeof(name_utf8) - 1, NULL, NULL);
        name_utf8[nlen < 511 ? nlen : 511] = '\0';

        const char *parent_path = frn_map_get_path(parent_frn);
        if (!parent_path) parent_path = scan_path;

        /* build full path for FRN map updates */
        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s\\%s", parent_path, name_utf8);

        if (reason & USN_REASON_RENAME_OLD_NAME) {
            pending_add(frn, name_utf8, parent_path);
            changes++;
        }
        else if (reason & USN_REASON_RENAME_NEW_NAME) {
            char *old_name = NULL, *old_parent = NULL;
            if (pending_take(frn, &old_name, &old_parent)) {
                renames_add(parent_path, old_name, name_utf8);
                free(old_name);
            }
            frn_map_set(frn, full_path,
                (rec->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0);
            changes++;
        }
        else if (reason & USN_REASON_FILE_DELETE) {
            frn_map_remove(frn);
            changes++;
        }
        else if (reason & USN_REASON_FILE_CREATE) {
            frn_map_set(frn, full_path,
                (rec->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0);
            changes++;
        }
        else if (reason & (USN_REASON_DATA_OVERWRITE |
                            USN_REASON_DATA_EXTEND |
                            USN_REASON_DATA_TRUNCATION)) {
            changes++;
        }

        offset += rec->RecordLength;
    }

    g_next_usn = *(USN *)buf;
    free(buf);
    return changes;
}

RenameGroup *usnwatcher_get_renames(void) {
    return g_renames;
}

void usnwatcher_free_renames(RenameGroup *groups) {
    (void)groups;
    renames_clear();
}

#endif /* _WIN32 */
