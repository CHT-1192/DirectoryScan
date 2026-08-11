#ifndef FILEUTIL_H
#define FILEUTIL_H

#include <stddef.h>

/* Check if a file appears to be binary.
 * Reads up to 8192 bytes. Returns 1 if binary, 0 if text. */
int is_binary(const char *path);

/* Count lines in a text file. Returns line count, or -1 on error. */
long count_lines(const char *path);

/* Format size in bytes to human-readable string (KiB/MiB adaptive).
 * Writes to buf (must be at least 16 bytes).
 * < 1 KiB  -> "1KiB"
 * >= 1 KiB -> "XiB" (integer)
 * >= 1024 KiB -> "X.XMiB" (one decimal) */
void format_size(long size, char *buf, size_t bufsize);

#endif
