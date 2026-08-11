#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define PATH_SEP '\\'
#else
#include <unistd.h>
#define PATH_SEP '/'
#endif

/* ---- global config pointer ---- */
static Config *g_config = NULL;

Config *config_get(void) { return g_config; }

/* ---- wildcard pattern matching ---- */

/* Match a single pattern against a string. Returns 1 on match, 0 on no match.
 * Supports * (any sequence) and ? (any single char). */
static int wildcard_match(const char *pattern, const char *str) {
    while (*pattern) {
        if (*pattern == '*') {
            pattern++;
            if (!*pattern) return 1;           /* trailing * matches everything */
            while (*str) {
                if (wildcard_match(pattern, str)) return 1;
                str++;
            }
            return wildcard_match(pattern, str); /* try empty tail */
        } else if (*pattern == '?') {
            if (!*str) return 0;
            pattern++; str++;
        } else {
            if (*pattern != *str) return 0;
            pattern++; str++;
        }
    }
    return *str == '\0';
}

/* Check if an entry matches a pattern string.
 * Pattern syntax (gitignore-like):
 *   - Patterns without '/' match against the entry name only.
 *   - Patterns with '/' match against the relative path.
 *   - Trailing '/' means the pattern only matches directories. */
static int pattern_match(const char *pattern, const char *name,
                         const char *rel_path, int is_dir) {
    /* trailing '/' → match directories only */
    size_t plen = strlen(pattern);
    int dir_only = 0;
    const char *effective = pattern;

    if (plen > 0 && pattern[plen - 1] == '/') {
        if (!is_dir) return 0;
        dir_only = 1;
        /* strip trailing '/' for matching */
        char *stripped = malloc(plen);
        if (!stripped) return 0;
        memcpy(stripped, pattern, plen - 1);
        stripped[plen - 1] = '\0';
        effective = stripped;
    }

    int result = 0;
    if (strchr(effective, '/') || strchr(effective, '\\')) {
        /* pattern contains path separator → match against relative path */
        result = wildcard_match(effective, rel_path);
    } else {
        /* match against entry name only */
        result = wildcard_match(effective, name);
    }

    if (dir_only) free((void *)effective);
    return result;
}

/* Check if name/rel_path matches any pattern in the list. */
static int match_any(const char *name, const char *rel_path, int is_dir,
                     char **patterns, int count) {
    for (int i = 0; i < count; i++) {
        if (pattern_match(patterns[i], name, rel_path, is_dir))
            return 1;
    }
    return 0;
}

/* ---- config filtering logic ---- */

int config_should_include(Config *cfg, const char *name,
                          const char *rel_path, int is_dir, int is_hidden) {
    if (!cfg) {
        /* no config → hide hidden files, show everything else */
        return is_hidden ? 0 : 1;
    }

    if (is_hidden) {
        /* hidden file: only show if whitelisted */
        if (cfg->whitelist_count > 0 &&
            match_any(name, rel_path, is_dir, cfg->whitelist, cfg->whitelist_count)) {
            return 1;
        }
        return 0;
    }

    /* non-hidden file: hide if blacklisted */
    if (cfg->blacklist_count > 0 &&
        match_any(name, rel_path, is_dir, cfg->blacklist, cfg->blacklist_count)) {
        return 0;
    }
    return 1;
}

/* ---- default config JSON ---- */

static const char *DEFAULT_CONFIG_JSON =
    "{\n"
    "    // Maximum recursion depth for directory scanning\n"
    "    \"max_depth\": 4,\n"
    "\n"
    "    // How long changed entries stay highlighted green (seconds)\n"
    "    \"change_highlight_secs\": 3,\n"
    "\n"
    "    // Polling interval between scans (milliseconds)\n"
    "    \"scan_interval_ms\": 2000,\n"
    "\n"
    "    // Blacklist patterns - non-hidden entries matching these are excluded\n"
    "    // Patterns without / match filename; with / match relative path\n"
    "    // Trailing / matches directories only; * matches any sequence\n"
    "    \"blacklist\": [\n"
    "    ],\n"
    "\n"
    "    // Whitelist patterns - hidden entries matching these are shown\n"
    "    // Same syntax as blacklist\n"
    "    \"whitelist\": []\n"
    "}\n";

/* ---- get executable directory ---- */

static char *get_exe_dir(void) {
#ifdef _WIN32
    wchar_t wpath[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, wpath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return NULL;

    /* convert wide-char path to UTF-8 */
    int ulen = WideCharToMultiByte(CP_UTF8, 0, wpath, len, NULL, 0, NULL, NULL);
    char *path = malloc(ulen + 1);
    if (!path) return NULL;
    WideCharToMultiByte(CP_UTF8, 0, wpath, len, path, ulen, NULL, NULL);
    path[ulen] = '\0';

    /* strip filename, leave directory */
    char *last_sep = strrchr(path, '\\');
    if (last_sep) *last_sep = '\0';
    return path;
#else
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0) return NULL;
    buf[len] = '\0';
    char *last_sep = strrchr(buf, '/');
    if (last_sep) *last_sep = '\0';
    return strdup(buf);
#endif
}

/* ---- minimal JSONC tokeniser / parser ---- */

typedef enum {
    TOK_EOF, TOK_LBRACE, TOK_RBRACE, TOK_LBRACKET, TOK_RBRACKET,
    TOK_STRING, TOK_NUMBER, TOK_COLON, TOK_COMMA, TOK_ERROR
} JsonTokenType;

typedef struct {
    JsonTokenType type;
    char *sval;    /* owned; valid for TOK_STRING */
    long  ival;    /* valid for TOK_NUMBER */
} Token;

typedef struct {
    const char *json;
    int pos;
    int len;
    Token lookahead;
    int has_lookahead;
} Parser;

static void token_free(Token *t) {
    if (t->type == TOK_STRING) free(t->sval);
    t->sval = NULL;
}

/* skip whitespace and comments, return next character position */
static int skip_ws_comments(Parser *p) {
    while (p->pos < p->len) {
        char c = p->json[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            p->pos++;
            continue;
        }
        if (c == '/') {
            if (p->pos + 1 < p->len) {
                char nxt = p->json[p->pos + 1];
                if (nxt == '/') {
                    /* line comment → skip to end of line */
                    p->pos += 2;
                    while (p->pos < p->len && p->json[p->pos] != '\n') p->pos++;
                    continue;
                }
                if (nxt == '*') {
                    /* block comment → skip to *​/ */
                    p->pos += 2;
                    while (p->pos + 1 < p->len) {
                        if (p->json[p->pos] == '*' && p->json[p->pos + 1] == '/') {
                            p->pos += 2;
                            break;
                        }
                        p->pos++;
                    }
                    continue;
                }
            }
        }
        break;
    }
    return p->pos;
}

static Token make_token(JsonTokenType type) {
    Token t = {type, NULL, 0};
    return t;
}

static Token make_error(void) {
    return make_token(TOK_ERROR);
}

/* lexical analysis: read next token */
static Token lex(Parser *p) {
    skip_ws_comments(p);
    if (p->pos >= p->len) return make_token(TOK_EOF);

    char c = p->json[p->pos];

    switch (c) {
    case '{': p->pos++; return make_token(TOK_LBRACE);
    case '}': p->pos++; return make_token(TOK_RBRACE);
    case '[': p->pos++; return make_token(TOK_LBRACKET);
    case ']': p->pos++; return make_token(TOK_RBRACKET);
    case ':': p->pos++; return make_token(TOK_COLON);
    case ',': p->pos++; return make_token(TOK_COMMA);

    case '"': {
        /* parse string with escape sequences */
        p->pos++; /* skip opening " */
        int cap = 64;
        char *buf = malloc(cap);
        if (!buf) return make_error();
        int len = 0;
        while (p->pos < p->len) {
            c = p->json[p->pos++];
            if (c == '"') {
                buf[len] = '\0';
                Token t = {TOK_STRING, buf, 0};
                return t;
            }
            if (c == '\\' && p->pos < p->len) {
                char esc = p->json[p->pos++];
                switch (esc) {
                case '"':  c = '"'; break;
                case '\\': c = '\\'; break;
                case '/':  c = '/'; break;
                case 'n':  c = '\n'; break;
                case 't':  c = '\t'; break;
                case 'r':  c = '\r'; break;
                default:   c = esc; break; /* keep as-is */
                }
            }
            if (len + 1 >= cap) {
                cap *= 2;
                buf = realloc(buf, cap);
                if (!buf) return make_error();
            }
            buf[len++] = c;
        }
        free(buf);
        return make_error(); /* unterminated string */
    }

    default:
        /* number or error */
        if ((c >= '0' && c <= '9') || c == '-') {
            char *end;
            long val = strtol(p->json + p->pos, &end, 10);
            if (end == p->json + p->pos) return make_error();
            p->pos = (int)(end - p->json);
            Token t = {TOK_NUMBER, NULL, val};
            return t;
        }
        /* unknown character → skip and continue */
        p->pos++;
        return lex(p);
    }
}

static Token peek(Parser *p) {
    if (!p->has_lookahead) {
        p->lookahead = lex(p);
        p->has_lookahead = 1;
    }
    return p->lookahead;
}

static Token next(Parser *p) {
    if (p->has_lookahead) {
        p->has_lookahead = 0;
        return p->lookahead;
    }
    return lex(p);
}

/* forward declarations */
static int parse_array(Parser *p, char ***list, int *count);

/* parse: { "key": value, ... } */
static int parse_object(Parser *p, Config *cfg) {
    Token t = next(p);
    if (t.type != TOK_LBRACE) return 0;

    while (1) {
        t = peek(p);
        if (t.type == TOK_RBRACE) { next(p); return 1; }
        if (t.type == TOK_EOF) return 0;

        /* key must be a string */
        Token key = next(p);
        if (key.type != TOK_STRING) { token_free(&key); return 0; }

        /* colon */
        t = next(p);
        if (t.type != TOK_COLON) { token_free(&key); return 0; }

        /* value */
        t = peek(p);
        if (t.type == TOK_NUMBER) {
            t = next(p);
            if (strcmp(key.sval, "max_depth") == 0)
                cfg->max_depth = (int)t.ival;
            else if (strcmp(key.sval, "change_highlight_secs") == 0)
                cfg->change_highlight_secs = (int)t.ival;
            else if (strcmp(key.sval, "scan_interval_ms") == 0)
                cfg->scan_interval_ms = (int)t.ival;
        } else if (t.type == TOK_LBRACKET) {
            if (strcmp(key.sval, "blacklist") == 0)
                parse_array(p, &cfg->blacklist, &cfg->blacklist_count);
            else if (strcmp(key.sval, "whitelist") == 0)
                parse_array(p, &cfg->whitelist, &cfg->whitelist_count);
            else
                parse_array(p, NULL, NULL); /* skip unknown array */
        } else {
            /* skip unknown value */
            next(p);
        }

        token_free(&key);

        /* optional comma */
        t = peek(p);
        if (t.type == TOK_COMMA) next(p);
    }
}

/* parse: ["str", "str", ...] */
static int parse_array(Parser *p, char ***list, int *count) {
    Token t = next(p);
    if (t.type != TOK_LBRACKET) return 0;

    /* temporary dynamic array */
    char **items = NULL;
    int n = 0, cap = 0;

    while (1) {
        t = peek(p);
        if (t.type == TOK_RBRACKET) { next(p); break; }
        if (t.type == TOK_EOF) { free(items); return 0; }

        if (t.type == TOK_STRING) {
            t = next(p);
            if (list && count) {
                if (n >= cap) {
                    cap = cap ? cap * 2 : 8;
                    items = realloc(items, cap * sizeof(char *));
                }
                items[n++] = t.sval; /* transfer ownership */
            } else {
                token_free(&t); /* skip */
            }
        } else {
            next(p); /* skip non-string array elements */
        }

        t = peek(p);
        if (t.type == TOK_COMMA) next(p);
    }

    if (list && count) {
        *list = items;
        *count = n;
    }
    return 1;
}

/* ---- save default config to disk ---- */

static int save_default_config(const char *filepath) {
    FILE *f = fopen(filepath, "w");
    if (!f) return 0;
    fputs(DEFAULT_CONFIG_JSON, f);
    fclose(f);
    return 1;
}

/* ---- public API ---- */

Config *config_load(void) {
    char *exe_dir = get_exe_dir();
    if (!exe_dir) return NULL;

    size_t dlen = strlen(exe_dir);
    char *cfg_path = malloc(dlen + 32);
    if (!cfg_path) { free(exe_dir); return NULL; }
    snprintf(cfg_path, dlen + 32, "%s%cDSConfig.jsonc", exe_dir, PATH_SEP);
    free(exe_dir);

    /* initialise with defaults */
    Config *cfg = calloc(1, sizeof(Config));
    if (!cfg) { free(cfg_path); return NULL; }
    cfg->max_depth = DEFAULT_MAX_DEPTH;
    cfg->change_highlight_secs = DEFAULT_CHANGE_HIGHLIGHT_SECS;
    cfg->scan_interval_ms = DEFAULT_SCAN_INTERVAL_MS;
    cfg->blacklist = NULL;
    cfg->blacklist_count = 0;
    cfg->whitelist = NULL;
    cfg->whitelist_count = 0;

    /* try to read config file */
    FILE *f = fopen(cfg_path, "rb");
    if (!f) {
        /* config doesn't exist → create with defaults */
        save_default_config(cfg_path);
        free(cfg_path);
        g_config = cfg;
        return cfg;
    }

    /* read entire file */
    fseek(f, 0, SEEK_END);
    long flen = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (flen > 0 && flen < 1024 * 1024) { /* max 1 MB */
        char *json = malloc(flen + 1);
        if (json) {
            size_t nread = fread(json, 1, flen, f);
            json[nread] = '\0';

            Parser p = {json, 0, (int)nread, {TOK_EOF, NULL, 0}, 0};
            parse_object(&p, cfg);
            free(json);
        }
    }
    fclose(f);

    free(cfg_path);
    g_config = cfg;
    return cfg;
}

void config_free(Config *cfg) {
    if (!cfg) return;
    if (cfg->blacklist) {
        for (int i = 0; i < cfg->blacklist_count; i++)
            free(cfg->blacklist[i]);
        free(cfg->blacklist);
    }
    if (cfg->whitelist) {
        for (int i = 0; i < cfg->whitelist_count; i++)
            free(cfg->whitelist[i]);
        free(cfg->whitelist);
    }
    free(cfg);
}
