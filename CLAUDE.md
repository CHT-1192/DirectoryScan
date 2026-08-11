# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

DirectoryScan is a C terminal application that recursively scans a directory and displays its structure in a tree format. It watches for file changes (mtime/size) and re-renders only when changes are detected, highlighting modified entries in green for 3 seconds.

## Build and run

```sh
# Build (primary — one-shot compile)
gcc -Wall -Wextra -std=gnu11 -O2 -o DirectoryScan *.c

# Or with mingw32-make (Windows)
mingw32-make

# Install to ~/bin (Windows) or /usr/local/bin (Unix)
mingw32-make install                    # default PREFIX
mingw32-make install PREFIX=/some/path  # custom location

# Run (defaults to current directory)
./DirectoryScan

# Run with specific path
./DirectoryScan "C:\path\to\scan"
```

Uses `-std=gnu11` for POSIX extensions (`strdup`, etc.).

## Architecture

Eight C modules, each with a `.h` / `.c` pair plus `main.c`:

| Module | Responsibility |
|---|---|
| `config` | JSONC parser, wildcard pattern matching, blacklist/whitelist filtering. Reads `DSConfig.jsonc` from exe directory; auto-generates default on first run. |
| `scanner` | Recursive directory traversal, tree building (`Entry` struct), sorting (dirs before files, case-insensitive alpha). Uses runtime `max_depth` from config. |
| `fileutil` | Binary file detection (null byte + non-printable ratio), line counting, human-readable size formatting (KiB/MiB adaptive). |
| `display` | Terminal rendering with Unicode box-drawing chars (`├─`, `│ `, `╰─`), ANSI color codes (green for changes, red for MAX DEPTH), column alignment. |
| `watcher` | Flat snapshot of `(path, mtime, size)` per entry. Compares snapshots to detect changes, marks tree entries with `change_time`. |
| `main` | Argument parsing, ANSI + UTF-8 enable on Windows, main loop: scan → detect changes → display if needed. |

## Configuration (`DSConfig.jsonc`)

JSON with comments, located alongside the executable. Default values (from `config.h`):

| Key | Default | Description |
|---|---|---|
| `max_depth` | `4` | Maximum recursion depth |
| `change_highlight_secs` | `3` | How long changed entries stay green |
| `scan_interval_ms` | `2000` | Polling interval between scans |
| `blacklist` | `[]` | Non-hidden entries matching these patterns are excluded |
| `whitelist` | `[]` | Hidden entries matching these patterns are shown |

**Pattern syntax** (gitignore-like, in `config.c`):
- Patterns without `/` match against filename only
- Patterns with `/` match against relative path from scan root
- `*` matches any sequence of characters
- Trailing `/` matches directories only

**Filtering logic** (`config_should_include`):
1. If entry is hidden (Windows `FILE_ATTRIBUTE_HIDDEN` or dot-prefix) → check whitelist: show if matched, else hide
2. If entry is not hidden → check blacklist: hide if matched, else show

## Output format

```
<line#>\t<tree-prefix><name><pad>  <HH:MM>  <lines>    <size>
```

- Line numbers are right-aligned, tab-separated from content
- Name column is left-aligned with dynamic width; directories get `/` suffix
- Time is `HH:MM` (24h) from file mtime
- Line count is right-aligned; binary files skip it; max-depth dirs show `MAX DEPTH` in red
- Size is KiB (integer) or MiB (one decimal), adaptive
- Tree characters: `├─` (branch), `╰─` (last), `│ ` (continuation indent)

## Sentinel values

- `Entry.line_count = -1` (LINES_BINARY) → binary file, omit line count
- `Entry.line_count = -2` (LINES_MAXDEPTH) → directory at max depth, show red "MAX DEPTH"

## Platform support

Uses `#ifdef _WIN32` for platform-specific code:
- Directory traversal: `FindFirstFileW`/`FindNextFileW` on Windows, `opendir`/`readdir` on Unix
- Sleep: `Sleep()` on Windows, `usleep()` on Unix
- ANSI: calls `SetConsoleMode` with `ENABLE_VIRTUAL_TERMINAL_PROCESSING` on Windows
