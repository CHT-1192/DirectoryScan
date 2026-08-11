#ifndef HASH_H
#define HASH_H

/* SHA-1 digest of file content as 40-char hex string (caller frees).
 * Returns NULL if file doesn't exist, can't be read, or exceeds max_size. */
char *hash_file_sha1(const char *path, long max_size);

/* ---- thread pool for parallel hashing ---- */

typedef struct HashJob {
    const char *path;
    long        max_size;
    char       *sha1;   /* result, malloc'd by worker */
    int         done;   /* 1 when complete */
    void       *ctx;    /* caller context (e.g. Entry pointer) */
} HashJob;

/* Start thread pool with n worker threads (0 = shutdown). */
void hash_pool_start(int n_threads);

/* Submit jobs. Returns immediately; call hash_pool_wait() to block. */
void hash_pool_submit(HashJob *jobs, int count);

/* Block until all submitted jobs complete. */
void hash_pool_wait(void);

/* Shut down pool, free resources. */
void hash_pool_stop(void);

#endif
