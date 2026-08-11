#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/time.h>
#include <unistd.h>
#endif

static FILE *g_logfile = NULL;
static const char *LEVEL_NAMES[] = {"DEBUG", "INFO", "WARN", "ERROR"};

/* Get executable directory (UTF-8), returns malloc'd string */
static char *get_exe_dir(void) {
#ifdef _WIN32
    wchar_t wpath[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, wpath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return NULL;
    int ulen = WideCharToMultiByte(CP_UTF8, 0, wpath, len, NULL, 0, NULL, NULL);
    char *path = malloc(ulen + 1);
    if (!path) return NULL;
    WideCharToMultiByte(CP_UTF8, 0, wpath, len, path, ulen, NULL, NULL);
    path[ulen] = '\0';
    char *sep = strrchr(path, '\\');
    if (sep) *sep = '\0';
    return path;
#else
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0) return NULL;
    buf[len] = '\0';
    char *sep = strrchr(buf, '/');
    if (sep) *sep = '\0';
    return strdup(buf);
#endif
}

/* Get current timestamp as [ YYYY.MM.DD HH:MM:SS.ms ] */
static void format_timestamp(char *buf, size_t bufsize) {
#ifdef _WIN32
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(buf, bufsize, "%04d.%02d.%02d %02d:%02d:%02d.%03d",
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm *tm_info = localtime(&tv.tv_sec);
    int ms = tv.tv_usec / 1000;
    char time_part[32];
    strftime(time_part, sizeof(time_part), "%Y.%m.%d %H:%M:%S", tm_info);
    snprintf(buf, bufsize, "%s.%03d", time_part, ms);
#endif
}

void log_init(void) {
    char *exe_dir = get_exe_dir();
    if (!exe_dir) return;

    size_t len = strlen(exe_dir);
    char *log_path = malloc(len + 32);
    if (!log_path) { free(exe_dir); return; }

#ifdef _WIN32
    snprintf(log_path, len + 32, "%s\\DirectoryScan.log", exe_dir);
#else
    snprintf(log_path, len + 32, "%s/DirectoryScan.log", exe_dir);
#endif
    free(exe_dir);

    g_logfile = fopen(log_path, "w");
    free(log_path);
}

void log_close(void) {
    if (g_logfile) {
        fclose(g_logfile);
        g_logfile = NULL;
    }
}

void log_write(int level, const char *fmt, ...) {
    if (!g_logfile) return;
    if (level < LOG_DEBUG || level > LOG_ERROR) return;

    char ts[64];
    format_timestamp(ts, sizeof(ts));

    fprintf(g_logfile, "[ %s | %-5s ] ", ts, LEVEL_NAMES[level]);

    va_list args;
    va_start(args, fmt);
    vfprintf(g_logfile, fmt, args);
    va_end(args);

    fprintf(g_logfile, "\n");
    fflush(g_logfile);
}
