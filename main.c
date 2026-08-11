#include "scanner.h"
#include "display.h"
#include "watcher.h"
#include "usnwatcher.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#define SLEEP_MS(ms) Sleep(ms)
#else
#include <unistd.h>
#define SLEEP_MS(ms) usleep((ms) * 1000)
#endif

/* fallback; actual value from config if available */
#define FALLBACK_SCAN_INTERVAL_MS 500

static volatile sig_atomic_t g_running = 1;

static void on_signal(int sig) {
    (void)sig;
    g_running = 0;
}

/* Find entry by relative path from scan root in tree */
static Entry *find_entry_by_path(Entry *root, const char *rel_path) {
    if (!root || !rel_path || !*rel_path) return NULL;
    /* split path into components */
    const char *slash = strchr(rel_path, '\\');
    const char *next_slash = slash ? strchr(slash + 1, '\\') : NULL;
    (void)next_slash;

    /* find first component in root's children */
    for (Entry *c = root->child; c; c = c->next) {
        size_t len = slash ? (size_t)(slash - rel_path) : strlen(rel_path);
        if (strlen(c->name) == len && strncmp(c->name, rel_path, len) == 0) {
            if (!slash) return c; /* last component */
            return find_entry_by_path(c, slash + 1); /* recurse */
        }
    }
    return NULL;
}

/* Enable ANSI escape codes + switch terminal to full UTF-8 on Windows */
static void setup_console(void) {
#ifdef _WIN32
    /* enable virtual terminal processing (ANSI escape codes) */
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE) {
        DWORD mode = 0;
        if (GetConsoleMode(hOut, &mode)) {
            mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            SetConsoleMode(hOut, mode);
        }
    }
    /* switch terminal I/O to UTF-8 code page */
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
#endif
}

#ifdef _WIN32
/* Convert a path from system code page (e.g. GBK) to UTF-8.
 * Returns malloc'd string; caller frees. */
static char *acp_to_utf8(const char *acp) {
    int wlen = MultiByteToWideChar(CP_ACP, 0, acp, -1, NULL, 0);
    if (wlen <= 0) return strdup(acp);
    wchar_t *wbuf = malloc(wlen * sizeof(wchar_t));
    if (!wbuf) return strdup(acp);
    MultiByteToWideChar(CP_ACP, 0, acp, -1, wbuf, wlen);

    int ulen = WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, NULL, 0, NULL, NULL);
    char *ubuf = malloc(ulen);
    if (ubuf) {
        WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, ubuf, ulen, NULL, NULL);
    }
    free(wbuf);
    return ubuf ? ubuf : strdup(acp);
}
#endif

int main(int argc, char *argv[]) {
    setup_console();
    log_init();
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    display_enter();

    /* load config from <exe_dir>/DSConfig.jsonc */
    Config *cfg = config_load();
    int scan_interval_ms = cfg ? cfg->scan_interval_ms : FALLBACK_SCAN_INTERVAL_MS;

    char *utf8_path = NULL;
    const char *scan_path;

    if (argc > 1) {
#ifdef _WIN32
        utf8_path = acp_to_utf8(argv[1]);
        scan_path = utf8_path;
#else
        scan_path = argv[1];
#endif
    } else {
        scan_path = "."; /* default: current directory */
    }

    /* resolve to absolute path (USN needs drive letter) */
#ifdef _WIN32
    {
        wchar_t wpath[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, scan_path, -1, wpath, MAX_PATH);
        wchar_t wabs[MAX_PATH];
        DWORD alen = GetFullPathNameW(wpath, MAX_PATH, wabs, NULL);
        if (alen > 0 && alen < MAX_PATH) {
            int ulen = WideCharToMultiByte(CP_UTF8, 0, wabs, alen, NULL, 0, NULL, NULL);
            char *abs_path = malloc(ulen + 1);
            if (abs_path) {
                WideCharToMultiByte(CP_UTF8, 0, wabs, alen, abs_path, ulen, NULL, NULL);
                abs_path[ulen] = '\0';
                free(utf8_path);
                utf8_path = abs_path;
                scan_path = abs_path;
            }
        }
    }
#endif

    Entry *prev_tree = NULL;
    int first_run = 1;
    time_t last_display_time = 0;

    log_info("DirectoryScan started, scanning: %s", scan_path);

    /* try USN Journal mode (Windows + NTFS + Admin) */
    int usn_mode = 0;
#ifdef _WIN32
    usn_mode = usnwatcher_init(scan_path);
    if (usn_mode)
        log_info("USN Journal mode active");
    else
        log_info("Snapshot mode (USN unavailable, run as Admin for NTFS)");
#endif

    while (g_running) {
        time_t now = time(NULL);

        int needs_scan = first_run;

        if (usn_mode) {
            /* USN mode: read journal to check for changes */
            int usn_changes = usnwatcher_read_changes(scan_path, now);
            if (usn_changes > 0) needs_scan = 1;
            /* also re-scan if highlights still active (to clear expired) */
            if (!needs_scan && prev_tree &&
                has_relevant_highlight(prev_tree, NULL, now, last_display_time))
                needs_scan = 1;
        } else {
            /* snapshot mode: always scan, merge will detect changes */
            needs_scan = 1;
        }

        if (needs_scan) {
            /* 1. scan directory tree */
            Entry *new_tree = scan_directory(scan_path, 0);
            if (!new_tree) {
                log_error("Cannot scan directory: %s", scan_path);
                break;
            }

            /* build FRN map after first scan (USN mode only) */
            if (usn_mode && first_run)
                usnwatcher_build_frn_map(new_tree, scan_path);

            /* hash files for rename detection (snapshot mode) */
            scan_hash_files(new_tree, scan_path);

            /* skip USN records generated by the scan itself */
            if (usn_mode) usnwatcher_skip_noise();

            /* 2. merge with previous tree */
            Entry *display_tree_root = merge_display_tree(prev_tree, new_tree, now);

            /* USN mode: override change types with Journal ground truth */
            if (usn_mode) {
                UsnChangeList *cl = usnwatcher_get_changes();
                for (int i = 0; i < cl->count; i++) {
                    UsnChange *ch = &cl->changes[i];

                    if (ch->change_type == CHANGE_RENAMED && ch->rename_old && ch->rename_new) {
                        Entry *old_e = find_entry_by_path(display_tree_root, ch->rename_old);
                        Entry *new_e = find_entry_by_path(display_tree_root, ch->rename_new);
                        if (old_e) { old_e->change_type = CHANGE_RENAMED; old_e->change_time = now; }
                        if (new_e) { new_e->change_type = CHANGE_RENAMED; new_e->change_time = now; }
                    } else {
                        Entry *e = find_entry_by_path(display_tree_root, ch->path);
                        if (e) { e->change_type = ch->change_type; e->change_time = now; }
                    }
                }
            }

            /* 3. display */
            int name_width = compute_name_width(display_tree_root);
            display_tree(display_tree_root, now, first_run, name_width, NULL);
            last_display_time = now;

            /* 4. rotate trees */
            free_entries(prev_tree);
            prev_tree = display_tree_root;
        }

        first_run = 0;
        SLEEP_MS(scan_interval_ms);
    }

    if (usn_mode) usnwatcher_close();
    free(utf8_path);
    free_entries(prev_tree);
    config_free(cfg);
    log_info("DirectoryScan exiting");
    display_exit();
    log_close();
    return 0;
}
