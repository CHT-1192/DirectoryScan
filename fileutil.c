#include "fileutil.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#define BINARY_CHECK_BYTES 8192
#define NONPRINTABLE_THRESHOLD 0.30

#ifdef _WIN32
/* Open a UTF-8 path via wide-char API */
static FILE *utf8_fopen(const char *path, const char *mode) {
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (wlen <= 0) return NULL;
    wchar_t *wpath = malloc(wlen * sizeof(wchar_t));
    if (!wpath) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, wlen);

    wchar_t wmode[16];
    MultiByteToWideChar(CP_UTF8, 0, mode, -1, wmode, 16);

    FILE *f = _wfopen(wpath, wmode);
    free(wpath);
    return f;
}
#define my_fopen utf8_fopen
#else
#define my_fopen(path, mode) fopen(path, mode)
#endif

int is_binary(const char *path) {
    FILE *f = my_fopen(path, "rb");
    if (!f) return 0;

    unsigned char buf[BINARY_CHECK_BYTES];
    size_t n = fread(buf, 1, BINARY_CHECK_BYTES, f);
    fclose(f);

    if (n == 0) return 0;

    int nonprintable = 0;
    for (size_t i = 0; i < n; i++) {
        if (buf[i] == 0) return 1; /* null byte -> binary */
        /* printable: 0x09 (tab), 0x0A (LF), 0x0D (CR), 0x20-0x7E */
        if (buf[i] != 0x09 && buf[i] != 0x0A && buf[i] != 0x0D &&
            (buf[i] < 0x20 || buf[i] > 0x7E)) {
            nonprintable++;
        }
    }

    return ((double)nonprintable / n) > NONPRINTABLE_THRESHOLD;
}

long count_lines(const char *path) {
    FILE *f = my_fopen(path, "rb");
    if (!f) return -1;

    long lines = 0;
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (c == '\n') lines++;
    }
    fclose(f);
    return lines;
}

void format_size(long size, char *buf, size_t bufsize) {
    if (size < 1024) {
        snprintf(buf, bufsize, "1KiB");
    } else if (size < 1024 * 1024) {
        long kib = (size + 512) / 1024;
        if (kib < 1) kib = 1;
        snprintf(buf, bufsize, "%ldKiB", kib);
    } else {
        double mib = size / (1024.0 * 1024.0);
        snprintf(buf, bufsize, "%.1fMiB", mib);
    }
}
