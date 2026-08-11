#include "display.h"
#include "fileutil.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- ANSI escape codes ---- */
#define CLS       "\033[2J\033[H"
#define COLOR_GREEN  "\033[32m"
#define COLOR_RED    "\033[31m"
#define COLOR_RESET  "\033[0m"

/* ---- layout constants ---- */
#define LINE_COUNT_WIDTH 9   /* field for line count / "MAX DEPTH" */
#define SIZE_WIDTH      8    /* field for size (e.g. "1.5MiB") */
#define SPACER          "  " /* spacing between columns */
#define GAP_SIZE        "    " /* gap between line count and size */

/* ---- internal: compute tree prefix for an entry ---- */

static void build_prefix(char *buf, size_t bufsize,
                         int depth, const int *is_last) {
    buf[0] = '\0';
    if (depth == 0) return;

    for (int d = 1; d < depth; d++) {
        if (!is_last[d]) {
            /* ancestor not last -> continuation line */
            strncat(buf, "\xe2\x94\x82 ", bufsize - strlen(buf) - 1);  /* "│ " */
        } else {
            strncat(buf, "  ", bufsize - strlen(buf) - 1);
        }
    }

    if (is_last[depth]) {
        strncat(buf, "\xe2\x95\xb0\xe2\x94\x80", bufsize - strlen(buf) - 1);  /* "╰─" */
    } else {
        strncat(buf, "\xe2\x94\x9c\xe2\x94\x80", bufsize - strlen(buf) - 1);  /* "├─" */
    }
}

/* ---- internal: compute max name width walk ---- */

static void name_width_walk(Entry *e, int depth, int *is_last, int *max_width) {
    if (!e) return;

    char prefix[256];
    build_prefix(prefix, sizeof(prefix), depth, is_last);

    /* compute display name with / for directories */
    char display_name[512];
    if (e->is_dir) {
        snprintf(display_name, sizeof(display_name), "%s%s/", prefix, e->name);
    } else {
        snprintf(display_name, sizeof(display_name), "%s%s", prefix, e->name);
    }

    int w = (int)strlen(display_name);
    if (w > *max_width) *max_width = w;

    /* recurse into children */
    int child_idx = 0;
    int child_count = 0;
    for (Entry *c = e->child; c; c = c->next) child_count++;

    for (Entry *c = e->child; c; c = c->next) {
        is_last[depth + 1] = (child_idx == child_count - 1);
        name_width_walk(c, depth + 1, is_last, max_width);
        child_idx++;
    }
}

int compute_name_width(Entry *root) {
    int max_width = 0;
    int is_last[16] = {0};

    /* skip root entry; measure children starting at depth 0 */
    int child_count = 0;
    for (Entry *c = root->child; c; c = c->next) child_count++;

    int child_idx = 0;
    for (Entry *c = root->child; c; c = c->next) {
        is_last[0] = (child_idx == child_count - 1);
        name_width_walk(c, 0, is_last, &max_width);
        child_idx++;
    }
    return max_width;
}

/* ---- internal: format one line ---- */

static void print_entry(Entry *e, int depth, const int *is_last,
                        int name_width, time_t now, int first_run) {
    /* build tree prefix */
    char prefix[256];
    build_prefix(prefix, sizeof(prefix), depth, is_last);

    /* build full display name (prefix + name [+ /]) */
    char display_name[512];
    if (e->is_dir) {
        snprintf(display_name, sizeof(display_name), "%s%s/", prefix, e->name);
    } else {
        snprintf(display_name, sizeof(display_name), "%s%s", prefix, e->name);
    }

    /* check if this entry should be highlighted green */
    int highlight = 0;
    if (!first_run && e->change_time > 0) {
        Config *cfg = config_get();
        int duration = cfg ? cfg->change_highlight_secs : DEFAULT_CHANGE_HIGHLIGHT_SECS;
        if ((now - e->change_time) < duration)
            highlight = 1;
    }

    /* format time */
    char time_str[16];
    struct tm *tm_info = localtime(&e->mtime);
    if (tm_info) {
        snprintf(time_str, sizeof(time_str), "%02d:%02d",
                 tm_info->tm_hour, tm_info->tm_min);
    } else {
        snprintf(time_str, sizeof(time_str), "--:--");
    }

    /* print name column (left-aligned, padded to name_width) */
    if (highlight) printf(COLOR_GREEN);
    printf("%-*s", name_width, display_name);
    if (highlight) printf(COLOR_RESET);

    /* print time */
    printf("%s", SPACER);
    if (highlight) printf(COLOR_GREEN);
    printf("%s", time_str);
    if (highlight) printf(COLOR_RESET);

    /* line count / MAX DEPTH / binary handling */
    if (e->is_dir) {
        if (e->line_count == LINES_MAXDEPTH) {
            /* directory at max depth: show MAX DEPTH in red */
            printf("%s", SPACER);
            printf(COLOR_RED "MAX DEPTH" COLOR_RESET);
        }
        /* regular directories: no line count or size */
    } else {
        if (e->line_count == LINES_BINARY) {
            /* binary file: leave line count blank but keep spacing */
            printf("%s%*s", SPACER, LINE_COUNT_WIDTH, "");
        } else {
            /* text file: show line count */
            printf("%s", SPACER);
            if (highlight) printf(COLOR_GREEN);
            printf("%*ld", LINE_COUNT_WIDTH, e->line_count);
            if (highlight) printf(COLOR_RESET);
        }

        /* size */
        char size_buf[16];
        format_size(e->size, size_buf, sizeof(size_buf));
        printf("%s", GAP_SIZE);
        if (highlight) printf(COLOR_GREEN);
        printf("%*s", SIZE_WIDTH, size_buf);
        if (highlight) printf(COLOR_RESET);
    }

    printf("\n");
}

/* ---- internal: recursive tree walk ---- */

static void render_walk(Entry *e, int depth, int *is_last,
                        int name_width, time_t now, int first_run) {
    if (!e) return;

    /* print self */
    print_entry(e, depth, is_last, name_width, now, first_run);

    /* recurse into children */
    int child_idx = 0;
    int child_count = 0;
    for (Entry *c = e->child; c; c = c->next) child_count++;

    for (Entry *c = e->child; c; c = c->next) {
        is_last[depth + 1] = (child_idx == child_count - 1);
        render_walk(c, depth + 1, is_last, name_width, now, first_run);
        child_idx++;
    }
}

/* ---- public API ---- */

void display_tree(Entry *root, time_t now, int first_run, int name_width) {
    /* clear screen */
    printf("%s", CLS);

    /* compute name width if not provided */
    if (name_width <= 0) {
        name_width = compute_name_width(root);
    }

    /* ensure reasonable minimum */
    if (name_width < 20) name_width = 20;

    int is_last[16] = {0};

    /* skip root entry itself; render children starting at depth 0 */
    int child_count = 0;
    for (Entry *c = root->child; c; c = c->next) child_count++;

    int child_idx = 0;
    for (Entry *c = root->child; c; c = c->next) {
        is_last[0] = (child_idx == child_count - 1);
        render_walk(c, 0, is_last, name_width, now, first_run);
        child_idx++;
    }
}
