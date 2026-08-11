#include "display.h"
#include "fileutil.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- internal: count display columns for UTF-8 strings ---- */
/* UTF-8 continuation bytes (0x80-0xBF) add 0 columns;
 * lead bytes and ASCII add 1 column each. */
static int disp_width(const char *s) {
    int w = 0;
    while (*s) {
        if (((unsigned char)*s & 0xC0) != 0x80) w++;
        s++;
    }
    return w;
}

/* ---- ANSI 16-color escape codes ---- */
#define COLOR_GREEN     "\033[32m"
#define COLOR_CYAN      "\033[36m"
#define COLOR_RED       "\033[31m"
#define COLOR_YELLOW    "\033[33m"
#define COLOR_RESET     "\033[0m"

/* ---- layout constants ---- */
#define LINE_COUNT_WIDTH 9
#define SIZE_WIDTH      8
#define SPACER          "  "
#define GAP_SIZE        "    "

/* ---- internal: map change_type to ANSI color ---- */

static const char *change_color(int change_type) {
    switch (change_type) {
    case CHANGE_CREATED:  return COLOR_GREEN;
    case CHANGE_MODIFIED: return COLOR_CYAN;
    case CHANGE_DELETED:  return COLOR_RED;
    case CHANGE_RENAMED:  return COLOR_YELLOW;
    default:              return NULL;
    }
}

/* ---- line buffer for diff-based rendering ---- */

#define MAX_LINES 4096
#define MAX_LINE_LEN 256

static char (*g_line_buf)[MAX_LINE_LEN] = NULL;
static int   g_line_count = 0;

static char (*g_prev_lines)[MAX_LINE_LEN] = NULL;
static int   g_prev_count = 0;

static void flush_display(int first_run) {
    if (first_run) {
        /* full render on first display */
        printf("\033[H\033[J");
        for (int i = 0; i < g_line_count; i++) {
            printf("%s\n", g_line_buf[i]);
        }
        fflush(stdout);
    } else {
        /* diff against previous frame */
        int max_lines = g_line_count > g_prev_count ? g_line_count : g_prev_count;

        for (int i = 0; i < max_lines; i++) {
            int has_new = (i < g_line_count);
            int has_old = (i < g_prev_count);
            int changed = 0;

            if (has_new && has_old) {
                changed = (strcmp(g_line_buf[i], g_prev_lines[i]) != 0);
            } else if (has_new != has_old) {
                changed = 1;
            }

            if (changed) {
                if (has_new) {
                    printf("\033[%d;1H\033[K%s", i + 1, g_line_buf[i]);
                } else {
                    printf("\033[%d;1H\033[K", i + 1);
                }
            }
        }
        fflush(stdout);
    }

    /* save current as previous */
    g_prev_count = g_line_count;
    for (int i = 0; i < g_line_count; i++) {
        memcpy(g_prev_lines[i], g_line_buf[i], MAX_LINE_LEN);
        g_prev_lines[i][MAX_LINE_LEN - 1] = '\0';
    }
}

/* ---- internal: compute tree prefix for an entry ---- */

static void build_prefix(char *buf, size_t bufsize,
                         int depth, const int *is_last) {
    buf[0] = '\0';
    if (depth == 0) return;

    for (int d = 1; d < depth; d++) {
        if (!is_last[d]) {
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

    char display_name[512];
    if (e->is_dir) {
        snprintf(display_name, sizeof(display_name), "%s%s/", prefix, e->name);
    } else {
        snprintf(display_name, sizeof(display_name), "%s%s", prefix, e->name);
    }

    int w = disp_width(display_name);
    if (w > *max_width) *max_width = w;

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

/* ---- internal: format one tree entry line ---- */

/* append to line buffer. buf must point into g_line_buf[g_line_count] */
static void line_append(const char *s) {
    char *buf = g_line_buf[g_line_count];
    int len = (int)strlen(buf);
    if (len < MAX_LINE_LEN - 1) {
        snprintf(buf + len, MAX_LINE_LEN - len, "%s", s);
    }
}

static void line_append_pad(int count) {
    char *buf = g_line_buf[g_line_count];
    int len = (int)strlen(buf);
    for (int i = 0; i < count && len + i < MAX_LINE_LEN - 1; i++) {
        buf[len + i] = ' ';
    }
    int new_len = len + count;
    if (new_len >= MAX_LINE_LEN) new_len = MAX_LINE_LEN - 1;
    buf[new_len] = '\0';
}

static void line_done(void) {
    g_line_count++;
    if (g_line_count < MAX_LINES) {
        g_line_buf[g_line_count][0] = '\0';
    }
}

static void print_entry(Entry *e, int depth, const int *is_last,
                        int name_width, time_t now, int first_run) {
    char prefix[256];
    build_prefix(prefix, sizeof(prefix), depth, is_last);

    char display_name[512];
    if (e->is_dir) {
        snprintf(display_name, sizeof(display_name), "%s%s/", prefix, e->name);
    } else {
        snprintf(display_name, sizeof(display_name), "%s%s", prefix, e->name);
    }

    /* decide color based on change type */
    const char *color = NULL;
    if (!first_run && e->change_time > 0) {
        Config *cfg = config_get();
        int duration = cfg ? cfg->change_highlight_secs : DEFAULT_CHANGE_HIGHLIGHT_SECS;
        if ((now - e->change_time) < duration)
            color = change_color(e->change_type);
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

    /* name column */
    int dw = disp_width(display_name);
    if (color) line_append(color);
    line_append(display_name);
    line_append_pad(name_width - dw);
    if (color) line_append(COLOR_RESET);

    /* time */
    line_append(SPACER);
    if (color) line_append(color);
    line_append(time_str);
    if (color) line_append(COLOR_RESET);

    /* line count / MAX DEPTH / binary */
    if (e->is_dir) {
        if (e->line_count == LINES_MAXDEPTH) {
            line_append(SPACER);
            line_append(COLOR_RED "MAX DEPTH" COLOR_RESET);
        }
    } else {
        char num_buf[32];
        if (e->line_count == LINES_BINARY) {
            snprintf(num_buf, sizeof(num_buf), "%s%*s", SPACER, LINE_COUNT_WIDTH, "");
        } else {
            snprintf(num_buf, sizeof(num_buf), "%s%*ld", SPACER, LINE_COUNT_WIDTH, e->line_count);
        }
        if (color) line_append(color);
        line_append(num_buf);
        if (color) line_append(COLOR_RESET);

        /* size */
        char size_buf[16];
        format_size(e->size, size_buf, sizeof(size_buf));
        char gap_buf[32];
        snprintf(gap_buf, sizeof(gap_buf), "%s%*s", GAP_SIZE, SIZE_WIDTH, size_buf);
        if (color) line_append(color);
        line_append(gap_buf);
        if (color) line_append(COLOR_RESET);
    }

    line_done();
}

/* ---- internal: render deleted entries ---- */

static void print_deleted(DeletedEntry *del, int name_width,
                          time_t now, int first_run) {
    if (!del) return;
    Config *cfg = config_get();
    int duration = cfg ? cfg->change_highlight_secs : DEFAULT_CHANGE_HIGHLIGHT_SECS;

    for (DeletedEntry *d = del; d; d = d->next) {
        int highlight = (!first_run && (now - d->del_time) < duration);

        char display_name[512];
        if (d->is_dir) {
            snprintf(display_name, sizeof(display_name), "%s/", d->name);
        } else {
            snprintf(display_name, sizeof(display_name), "%s", d->name);
        }

        if (highlight) line_append(COLOR_RED);
        line_append(display_name);
        int dw = disp_width(display_name);
        line_append_pad(name_width - dw);
        if (highlight) line_append(COLOR_RESET);

        line_append(SPACER);
        if (highlight) line_append(COLOR_RED);
        line_append("(deleted)");
        if (highlight) line_append(COLOR_RESET);

        line_done();
    }
}

/* ---- internal: recursive tree walk ---- */

static void render_walk(Entry *e, int depth, int *is_last,
                        int name_width, time_t now, int first_run) {
    if (!e) return;

    print_entry(e, depth, is_last, name_width, now, first_run);

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

void display_enter(void) {
    g_line_buf = malloc(MAX_LINES * MAX_LINE_LEN);
    g_prev_lines = malloc(MAX_LINES * MAX_LINE_LEN);
    printf("\033[?1049h\033[?25l");  /* alt screen + hide cursor */
    fflush(stdout);
}

void display_exit(void) {
    printf("\033[0m\033[?25h\033[?1049l");  /* reset colors + show cursor + normal screen */
    fflush(stdout);
    free(g_line_buf);  g_line_buf = NULL;
    free(g_prev_lines); g_prev_lines = NULL;
}

void display_tree(Entry *root, time_t now, int first_run,
                  int name_width, DeletedEntry *deleted) {
    if (name_width <= 0) {
        name_width = compute_name_width(root);
    }
    if (name_width < 20) name_width = 20;

    /* reset line buffer */
    g_line_count = 0;
    g_line_buf[0][0] = '\0';

    int is_last[16] = {0};

    int child_count = 0;
    for (Entry *c = root->child; c; c = c->next) child_count++;

    int child_idx = 0;
    for (Entry *c = root->child; c; c = c->next) {
        is_last[0] = (child_idx == child_count - 1);
        render_walk(c, 0, is_last, name_width, now, first_run);
        child_idx++;
    }

    if (deleted) {
        print_deleted(deleted, name_width, now, first_run);
    }

    /* diff and flush to terminal */
    flush_display(first_run);
}
