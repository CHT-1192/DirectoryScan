#include "hash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
/* use Windows threads + condition variables (Vista+) */
#else
#include <pthread.h>
#endif

/* ==================================================================
 * SHA-1  (RFC 3174)
 * ================================================================== */

typedef struct {
    unsigned state[5];
    unsigned count[2];
    unsigned char buffer[64];
} SHA1_CTX;

#define SHA1_ROTL(v,n) (((v) << (n)) | ((v) >> (32 - (n))))

static void sha1_transform(unsigned state[5], const unsigned char block[64]) {
    unsigned w[80], a, b, c, d, e, t;
    for (int i = 0; i < 16; i++)
        w[i] = ((unsigned)block[i*4] << 24) | ((unsigned)block[i*4+1] << 16) |
               ((unsigned)block[i*4+2] << 8) | block[i*4+3];
    for (int i = 16; i < 80; i++)
        w[i] = SHA1_ROTL(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    a = state[0]; b = state[1]; c = state[2]; d = state[3]; e = state[4];
    for (int i = 0; i < 80; i++) {
        if (i < 20) t = (b & c) | ((~b) & d);
        else if (i < 40) t = b ^ c ^ d;
        else if (i < 60) t = (b & c) | (b & d) | (c & d);
        else t = b ^ c ^ d;
        t += SHA1_ROTL(a, 5) + e + w[i];
        if (i < 20) t += 0x5A827999;
        else if (i < 40) t += 0x6ED9EBA1;
        else if (i < 60) t += 0x8F1BBCDC;
        else t += 0xCA62C1D6;
        e = d; d = c; c = SHA1_ROTL(b, 30); b = a; a = t;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}

static void sha1_init(SHA1_CTX *ctx) {
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xEFCDAB89;
    ctx->state[2] = 0x98BADCFE;
    ctx->state[3] = 0x10325476;
    ctx->state[4] = 0xC3D2E1F0;
    ctx->count[0] = ctx->count[1] = 0;
}

static void sha1_update(SHA1_CTX *ctx, const unsigned char *data, size_t len) {
    size_t i = (ctx->count[0] >> 3) & 63;
    ctx->count[0] += (unsigned)(len << 3);
    if (ctx->count[0] < (unsigned)(len << 3)) ctx->count[1]++;
    ctx->count[1] += (unsigned)(len >> 29);
    if (len >= 64 - i) {
        memcpy(ctx->buffer + i, data, 64 - i);
        sha1_transform(ctx->state, ctx->buffer);
        for (i = 64 - i; i + 63 < len; i += 64)
            sha1_transform(ctx->state, data + i);
    } else i = 0;
    if (i < len) memcpy(ctx->buffer + i, data + (len - (len - i)), len - i);
}

static void sha1_final(unsigned char digest[20], SHA1_CTX *ctx) {
    unsigned char finalcount[8];
    for (int i = 0; i < 8; i++)
        finalcount[i] = (unsigned char)((ctx->count[(i >= 4 ? 0 : 1)]
                         >> ((3 - (i & 3)) * 8)) & 255);
    unsigned char pad = 0x80;
    sha1_update(ctx, &pad, 1);
    while ((ctx->count[0] & 504) != 448) { pad = 0; sha1_update(ctx, &pad, 1); }
    sha1_update(ctx, finalcount, 8);
    for (int i = 0; i < 20; i++)
        digest[i] = (unsigned char)((ctx->state[i >> 2] >> ((3 - (i & 3)) * 8)) & 255);
}

/* ==================================================================
 * public: sha1 of file
 * ================================================================== */

char *hash_file_sha1(const char *path, long max_size) {
#ifdef _WIN32
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (wlen <= 0) return NULL;
    wchar_t *wpath = malloc(wlen * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, wlen);
    FILE *f = _wfopen(wpath, L"rb");
    free(wpath);
#else
    FILE *f = fopen(path, "rb");
#endif
    if (!f) return NULL;

    SHA1_CTX ctx;
    sha1_init(&ctx);
    unsigned char buf[8192];
    long total = 0;
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        total += (long)n;
        if (max_size > 0 && total > max_size) { fclose(f); return NULL; }
        sha1_update(&ctx, buf, n);
    }
    fclose(f);

    unsigned char digest[20];
    sha1_final(digest, &ctx);
    char *hex = malloc(41);
    for (int i = 0; i < 20; i++)
        sprintf(hex + i * 2, "%02x", digest[i]);
    hex[40] = '\0';
    return hex;
}

/* ==================================================================
 * Thread pool (2 workers, portable)
 * ================================================================== */

#define MAX_JOBS 4096

static HashJob *g_jobs = NULL;
static int      g_job_count = 0;
static int      g_job_head = 0;
static int      g_job_done = 0;
static int      g_running = 1;

#ifdef _WIN32
static HANDLE g_workers[2];
static CRITICAL_SECTION g_lock;
static CONDITION_VARIABLE g_cond;
#define MUTEX_LOCK   EnterCriticalSection(&g_lock)
#define MUTEX_UNLOCK LeaveCriticalSection(&g_lock)
#define COND_WAIT    SleepConditionVariableCS(&g_cond, &g_lock, INFINITE)
#define COND_WAKE    WakeConditionVariable(&g_cond)
#define THREAD_FUNC  DWORD WINAPI
#define THREAD_RET   return 0
#else
static pthread_t g_workers[2];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cond = PTHREAD_COND_INITIALIZER;
#define MUTEX_LOCK   pthread_mutex_lock(&g_lock)
#define MUTEX_UNLOCK pthread_mutex_unlock(&g_lock)
#define COND_WAIT    pthread_cond_wait(&g_cond, &g_lock)
#define COND_WAKE    pthread_cond_signal(&g_cond)
#define THREAD_FUNC  void *
#define THREAD_RET   return NULL
#endif

static THREAD_FUNC worker_thread(void *arg) {
    (void)arg;
    while (1) {
        MUTEX_LOCK;
        while (g_job_head >= g_job_count && g_running)
            COND_WAIT;
        if (!g_running && g_job_head >= g_job_count) {
            MUTEX_UNLOCK;
            THREAD_RET;
        }
        int idx = g_job_head++;
        MUTEX_UNLOCK;

        HashJob *job = &g_jobs[idx];
        job->sha1 = hash_file_sha1(job->path, job->max_size);
        job->done = 1;

        MUTEX_LOCK;
        g_job_done++;
        if (g_job_done == g_job_count) COND_WAKE;
        MUTEX_UNLOCK;
    }
}

void hash_pool_start(int n_threads) {
    if (n_threads < 1) n_threads = 1;
    if (n_threads > 2) n_threads = 2;
    g_jobs = malloc(MAX_JOBS * sizeof(HashJob));
    g_job_count = 0;
    g_job_head = 0;
    g_job_done = 0;
    g_running = 1;
#ifdef _WIN32
    InitializeCriticalSection(&g_lock);
    InitializeConditionVariable(&g_cond);
    for (int i = 0; i < n_threads; i++)
        g_workers[i] = CreateThread(NULL, 0, worker_thread, NULL, 0, NULL);
#else
    for (int i = 0; i < n_threads; i++)
        pthread_create(&g_workers[i], NULL, worker_thread, NULL);
#endif
}

void hash_pool_submit(HashJob *jobs, int count) {
    MUTEX_LOCK;
    for (int i = 0; i < count && g_job_count < MAX_JOBS; i++) {
        g_jobs[g_job_count++] = jobs[i];
    }
    g_job_done = g_job_head; /* reset done counter to current head */
    MUTEX_UNLOCK;
    COND_WAKE; /* wake one worker */
    /* wake both if 2 workers */
    COND_WAKE;
}

void hash_pool_wait(void) {
    MUTEX_LOCK;
    while (g_job_done < g_job_count)
        COND_WAIT;
    MUTEX_UNLOCK;
}

void hash_pool_stop(void) {
    MUTEX_LOCK;
    g_running = 0;
    MUTEX_UNLOCK;
    COND_WAKE; COND_WAKE;
#ifdef _WIN32
    for (int i = 0; i < 2; i++) {
        if (g_workers[i]) {
            WaitForSingleObject(g_workers[i], INFINITE);
            CloseHandle(g_workers[i]);
            g_workers[i] = NULL;
        }
    }
    DeleteCriticalSection(&g_lock);
#else
    for (int i = 0; i < 2; i++) {
        if (g_workers[i]) {
            pthread_join(g_workers[i], NULL);
            g_workers[i] = 0;
        }
    }
#endif
    free(g_jobs);
    g_jobs = NULL;
}
