#ifndef LOG_H
#define LOG_H

#define LOG_DEBUG 0
#define LOG_INFO  1
#define LOG_WARN  2
#define LOG_ERROR 3

/* Initialize logger. Creates/opens <exe_dir>/DirectoryScan.log. */
void log_init(void);

/* Close logger. */
void log_close(void);

/* Log a message at the given level. printf-style format. */
void log_write(int level, const char *fmt, ...);

/* Convenience macros */
#define log_debug(...) log_write(LOG_DEBUG, __VA_ARGS__)
#define log_info(...)  log_write(LOG_INFO,  __VA_ARGS__)
#define log_warn(...)  log_write(LOG_WARN,  __VA_ARGS__)
#define log_error(...) log_write(LOG_ERROR, __VA_ARGS__)

#endif
