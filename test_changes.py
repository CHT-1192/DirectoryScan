#!/usr/bin/env python3
"""DirectoryScan change-detection test suite.
Run this alongside DirectoryScan to verify three-color highlighting:
  green  = created,  cyan = modified,  red = deleted

Usage:  python test_changes.py <target_dir>
"""

import os, sys, time, random, shutil, string

TARGET = sys.argv[1] if len(sys.argv) > 1 else "./test_scan"
INTERVAL = 2.5  # slightly longer than scan_interval_ms to guarantee detection


def ensure_dir(path):
    os.makedirs(path, exist_ok=True)


def rand_name(prefix="f", ext=".txt"):
    return f"{prefix}_{random.randint(1000,9999)}{ext}"


def log(msg):
    print(f"  [{time.strftime('%H:%M:%S')}] {msg}")


# ── phase 1: create files ──────────────────────────────────────────

def phase_create_files():
    log("=== PHASE 1: Create files ===")

    # flat files
    for _ in range(4):
        p = os.path.join(TARGET, rand_name("file"))
        with open(p, "w") as f:
            f.write("\n".join(f"line {i}" for i in range(random.randint(3, 10))))
        log(f"CREATE  {p}")
    time.sleep(INTERVAL)

    # nested directory + files
    d1 = os.path.join(TARGET, rand_name("dir"))
    os.makedirs(d1, exist_ok=True)
    log(f"CREATE  {d1}/")

    for _ in range(3):
        p = os.path.join(d1, rand_name("nested"))
        with open(p, "w") as f:
            f.write("\n".join(f"nested line {i}" for i in range(5)))
        log(f"CREATE  {p}")
    time.sleep(INTERVAL)

    # deeper: dir/subdir/files
    d2 = os.path.join(d1, rand_name("subdir"))
    os.makedirs(d2, exist_ok=True)
    log(f"CREATE  {d2}/")

    for _ in range(2):
        p = os.path.join(d2, rand_name("deep"))
        with open(p, "w") as f:
            f.write("deep content\n" * 3)
        log(f"CREATE  {p}")
    time.sleep(INTERVAL)


# ── phase 2: modify files ──────────────────────────────────────────

def phase_modify_files():
    log("=== PHASE 2: Modify files ===")

    # collect all files in target tree
    all_files = []
    for root, dirs, files in os.walk(TARGET):
        for fn in files:
            all_files.append(os.path.join(root, fn))

    if not all_files:
        log("(no files to modify)")
        return

    # add lines to some files
    for p in random.sample(all_files, min(3, len(all_files))):
        with open(p, "a") as f:
            f.write("\n".join(f"appended {i}" for i in range(random.randint(1, 4))) + "\n")
        log(f"MODIFY  {p}  (+lines)")

    time.sleep(INTERVAL)

    # modify inline content in some files
    for p in random.sample(all_files, min(3, len(all_files))):
        try:
            with open(p, "r") as f:
                lines = f.readlines()
            if lines:
                idx = random.randint(0, len(lines) - 1)
                lines[idx] = f"[MODIFIED at {time.strftime('%H:%M:%S')}] {lines[idx].strip()}\n"
            with open(p, "w") as f:
                f.writelines(lines)
            log(f"MODIFY  {p}  (inline)")
        except Exception:
            pass

    time.sleep(INTERVAL)

    # delete lines from some files
    for p in random.sample(all_files, min(3, len(all_files))):
        try:
            with open(p, "r") as f:
                lines = f.readlines()
            if len(lines) > 2:
                cut = random.randint(1, len(lines) - 1)
                keep = lines[:cut]
                with open(p, "w") as f:
                    f.writelines(keep)
                log(f"MODIFY  {p}  (-lines, {len(lines)} -> {len(keep)})")
        except Exception:
            pass

    time.sleep(INTERVAL)


# ── phase 3: rename files / dirs ───────────────────────────────────

def phase_rename():
    log("=== PHASE 3: Rename files/dirs ===")

    all_items = []
    for root, dirs, files in os.walk(TARGET):
        for d in dirs:
            all_items.append((root, d, True))
        for fn in files:
            all_items.append((root, fn, False))

    if not all_items:
        log("(no items to rename)")
        return

    # rename a few items
    for _ in range(min(3, len(all_items))):
        root, name, is_dir = random.choice(all_items)
        old = os.path.join(root, name)
        new_name = rand_name("renamed", "/" if is_dir else ".txt").rstrip("/")
        new = os.path.join(root, new_name)
        try:
            os.rename(old, new)
            what = "dir" if is_dir else "file"
            log(f"RENAME  {old}  ->  {new_name}  ({what})")
        except OSError as e:
            log(f"RENAME FAIL  {old}: {e}")

    time.sleep(INTERVAL)


# ── phase 4: delete files ──────────────────────────────────────────

def phase_delete():
    log("=== PHASE 4: Delete files ===")

    all_items = []
    for root, dirs, files in os.walk(TARGET):
        for d in dirs:
            all_items.append((root, d, True))
        for fn in files:
            all_items.append((root, fn, False))

    if not all_items:
        log("(no items to delete)")
        return

    # delete files first (so dirs still have contents to delete later)
    files_only = [(r, n, False) for r, n, d in all_items if not d]
    dirs_only = [(r, n, True) for r, n, d in all_items if d]

    for root, name, _ in random.sample(files_only, min(4, len(files_only))):
        p = os.path.join(root, name)
        os.remove(p)
        log(f"DELETE  {p}")

    time.sleep(INTERVAL)

    # delete directories (with contents)
    for root, name, _ in random.sample(dirs_only, min(2, len(dirs_only))):
        p = os.path.join(root, name)
        shutil.rmtree(p)
        log(f"DELETE  {p}/  (+ contents)")

    time.sleep(INTERVAL)


# ── main ────────────────────────────────────────────────────────────

def main():
    print(f"DirectoryScan test runner")
    print(f"Target: {os.path.abspath(TARGET)}")
    print(f"Interval: {INTERVAL}s")
    print(f"Run DirectoryScan {os.path.abspath(TARGET)} in another terminal")
    print(f"Press Ctrl+C to stop\n")

    ensure_dir(TARGET)

    phases = [
        phase_create_files,
        phase_modify_files,
        phase_rename,
        phase_delete,
        phase_create_files,   # create again — should show green
        phase_modify_files,
        phase_delete,
    ]

    try:
        while True:
            for phase in phases:
                phase()
                time.sleep(INTERVAL)
    except KeyboardInterrupt:
        log("Stopped. Cleaning up...")
        shutil.rmtree(TARGET, ignore_errors=True)
        log("Done.")


if __name__ == "__main__":
    main()
