# DirectoryScan

Terminal directory tree viewer with live change detection. Scans a directory recursively, displays its structure with file metadata (modification time, line count, size), and watches for changes — re-rendering only when something changes and highlighting modified entries in green.

```
CLAUDE.md             11:18         89        4KiB
config.c              11:16        495       14KiB
LICENSE               11:50         16        1KiB
README.md             11:50         30        1KiB
src/                  11:18
├─display.c           11:49        216        7KiB
├─display.h           09:55         16        1KiB
├─fileutil.c          10:28         81        2KiB
├─fileutil.h          09:57         20        1KiB
├─main.c              11:15        145        4KiB
├─scanner.c           11:15        406       11KiB
├─scanner.h           11:12         33        1KiB
├─watcher.c           11:14        131        4KiB
╰─watcher.h           09:54         33        1KiB
```

## Features

- Recursive directory tree with Unicode box-drawing characters
- Live monitoring — re-renders only on file changes (mtime/size)
- Changed entries highlighted in green (duration configurable)
- Line counting for text files, binary file detection
- Adaptive file sizes (KiB / MiB)
- Configurable recursion depth (default: 4), shows `MAX DEPTH` in red
- Configurable blacklist/whitelist with wildcard patterns
- Hidden file filtering (Windows attribute + dot-prefix), with whitelist overrides
- Cross-platform: Windows (UTF-8, GBK path support) and Unix

## Build

```sh
# One-shot compile
gcc -Wall -Wextra -std=gnu11 -O2 -o DirectoryScan *.c

# Or with mingw32-make
mingw32-make
```

## Install

```sh
mingw32-make install                    # → ~/bin (Windows), /usr/local/bin (Unix)
mingw32-make install PREFIX=/opt/tools  # custom location
```

## Usage

```sh
DirectoryScan                  # scan current directory
DirectoryScan /path/to/scan    # scan specific path
```

## Configuration

`DSConfig.jsonc` (JSON with comments) is auto-generated in the executable's directory on first run:

```jsonc
{
    "max_depth": 4,                // recursion depth
    "change_highlight_secs": 3,    // green highlight duration
    "scan_interval_ms": 2000,      // polling interval
    "blacklist": [                 // hide non-hidden entries
        "node_modules/",
        "*.o"
    ],
    "whitelist": [                 // show hidden entries
        ".gitignore"
    ]
}
```

**Pattern syntax:** `*` matches any sequence. Patterns without `/` match filename; with `/` match relative path. Trailing `/` matches directories only.

## License

MIT — see [LICENSE](LICENSE)
