#ifndef CONFIG_H
#define CONFIG_H

/* Default values when config is missing or a field is unset */
#define DEFAULT_MAX_DEPTH            4
#define DEFAULT_CHANGE_HIGHLIGHT_SECS 3
#define DEFAULT_SCAN_INTERVAL_MS     500

typedef struct {
    int max_depth;
    int change_highlight_secs;
    int scan_interval_ms;
    int hash_enabled;
    int hash_max_size_mib;
    int hash_max_files;
    int hash_threads;
    char **blacklist;
    int blacklist_count;
    char **whitelist;
    int whitelist_count;
} Config;

/* Load config from <exe_directory>/DSConfig.jsonc.
 * If the file does not exist, create it with default values.
 * Returns NULL on critical failure (caller should fall back to defaults). */
Config *config_load(void);

/* Free a Config and all its owned memory. */
void config_free(Config *cfg);

/* Get the global config pointer (set by config_load).
 * Returns NULL if config has not been loaded yet. */
Config *config_get(void);

/* Check whether a file/directory entry should be included in output.
 *  name         - entry name only (not path)
 *  rel_path     - relative path from scan root (used for patterns with /)
 *  is_dir       - non-zero if the entry is a directory
 *  is_hidden    - non-zero if the entry is hidden (Windows attr or dot-prefix)
 * Returns 1 to include, 0 to exclude. */
int config_should_include(Config *cfg, const char *name,
                          const char *rel_path, int is_dir, int is_hidden);

#endif
