#include "scanner.h"
#include "display.h"
#include "watcher.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#define SLEEP_MS(ms) Sleep(ms)
#else
#include <unistd.h>
#define SLEEP_MS(ms) usleep((ms) * 1000)
#endif

/* fallback; actual value from config if available */
#define FALLBACK_SCAN_INTERVAL_MS 2000

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

    FileInfo *prev_snapshot = NULL;
    int prev_count = 0;
    int first_run = 1;
    time_t last_display_time = 0;

    while (1) {
        time_t now = time(NULL);

        /* prune expired deleted entries */
        watcher_prune_deleted(now);

        /* 1. scan directory tree completely */
        Entry *root = scan_directory(scan_path, 0);
        if (!root) {
            fprintf(stderr, "Error: cannot scan directory '%s'\n", scan_path);
            free(utf8_path);
            return 1;
        }

        /* build current snapshot */
        int new_count = 0;
        FileInfo *new_snapshot = build_snapshot(root, scan_path, &new_count);

        int needs_display = 0;
        DeletedEntry *deleted = watcher_get_deleted();

        if (first_run || !prev_snapshot) {
            needs_display = 1;
        } else {
            int changes = detect_changes(prev_snapshot, prev_count,
                                         new_snapshot, new_count, now);
            if (changes > 0) {
                needs_display = 1;
            } else if (has_relevant_highlight(root, deleted, now, last_display_time)) {
                needs_display = 1;
            }
        }

        if (needs_display) {
            /* 2. compute column widths AFTER scanning is complete */
            int name_width = compute_name_width(root);
            /* 3. render the tree + deleted entries */
            display_tree(root, now, first_run, name_width, deleted);
            last_display_time = now;
        }

        /* replace previous snapshot */
        free_snapshot(prev_snapshot, prev_count);
        prev_snapshot = new_snapshot;
        prev_count = new_count;

        first_run = 0;

        SLEEP_MS(scan_interval_ms);
    }

    free(utf8_path);
    free_snapshot(prev_snapshot, prev_count);
    config_free(cfg);
    return 0;
}
