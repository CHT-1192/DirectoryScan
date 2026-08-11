# DirectoryScan

Terminal directory tree viewer with live change detection. Scans a directory recursively, displays file metadata (time, line count, size), and watches for changes — re-rendering with diff-based partial updates and three-color highlighting.

```
CLAUDE.md             11:18         89        4KiB
src/                  11:18
├─display.c           11:49        216        7KiB
├─scanner.c           11:15        406       11KiB
╰─watcher.c           11:14        131        4KiB
```

## Features

- Recursive directory tree with Unicode box-drawing characters (`├─` `│ ` `╰─`)
- **Diff-based rendering** — only changed lines are redrawn, no flicker
- **Alternate screen buffer** — scrollback is preserved on exit
- **Three-color change highlighting** (configurable duration):
  - <span style="color:green">●</span> Green — created
  - <span style="color:cyan">●</span> Cyan — modified
  - <span style="color:red">●</span> Red — deleted (stays in-place)
  - <span style="color:yellow">●</span> Yellow — renamed
- **NTFS USN Journal** (Windows, Admin) — exact rename detection, skip scan on idle
- Line counting for text files, binary file detection
- Adaptive file sizes (KiB / MiB)
- Configurable recursion depth, `MAX DEPTH` in red at limit
- Blacklist / whitelist with gitignore-style wildcard patterns
- Hidden file filtering (Windows attribute + dot-prefix)
- Full UTF-8 support (GBK path conversion on Chinese Windows)
- Cross-platform: Windows and Unix

## Build

```sh
gcc -Wall -Wextra -std=gnu11 -O2 -o DirectoryScan *.c
mingw32-make
```

## Install

```sh
mingw32-make install                     # → ~/bin (Windows), /usr/local/bin (Unix)
mingw32-make install PREFIX=/opt/tools
```

## Usage

```sh
DirectoryScan                         # current directory
DirectoryScan C:\path\to\scan         # specific path

# USN Journal mode (exact rename detection, Admin required)
DirectoryScan C:\path                 # run as Administrator

# With NSudo (TrustedInstaller)
NSudoLC.exe -U:T -P:E DirectoryScan C:\path
```

## Configuration

`DSConfig.jsonc` — auto-generated next to the executable on first run:

```jsonc
{
    "max_depth": 4,
    "change_highlight_secs": 3,
    "scan_interval_ms": 2000,
    "blacklist": [],
    "whitelist": []
}
```

| Key | Description |
|---|---|
| `max_depth` | Recursion depth |
| `change_highlight_secs` | Highlight duration (seconds) |
| `scan_interval_ms` | Polling interval (milliseconds) |
| `blacklist` | Patterns to hide (non-hidden entries) |
| `whitelist` | Patterns to show (hidden entries only) |

**Pattern syntax** (gitignore-like): `*` wildcard. No `/` → match filename; with `/` → match relative path. Trailing `/` → directories only.

## Testing

```sh
# Terminal 1
DirectoryScan ./test/

# Terminal 2 — random stress test (create/modify/delete/rename)
python test.py ./test/
```

## Logs

`DirectoryScan.log` is written to the executable's directory. Includes startup/config info and USN Journal status.

## License

MIT — see [LICENSE](LICENSE)
