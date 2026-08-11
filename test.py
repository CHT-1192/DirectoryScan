#!/usr/bin/env python3
"""DirectoryScan change-detection stress test.
Randomly interleaves create / modify / delete / rename operations.

Usage:  python test_changes.py <target_dir>
"""

import os, sys, time, random, shutil

TARGET = sys.argv[1] if len(sys.argv) > 1 else "./test/"

def rand_interval():
    return random.choice([1.0, 1.5, 2.0, 2.5, 3.0, 3.5, 4.0, 4.5, 5.0])


def ensure_dir(path):
    os.makedirs(path, exist_ok=True)


def rand_name(prefix="f", ext=".txt"):
    return f"{prefix}_{random.randint(1000,9999)}{ext}"


def log(msg):
    print(f"  [{time.strftime('%H:%M:%S')}] {msg}")


def all_items(types="both"):
    """Walk target tree, yield (root, name, is_dir)."""
    for root, dirs, files in os.walk(TARGET):
        if types in ("both", "dirs"):
            for d in dirs:
                yield (root, d, True)
        if types in ("both", "files"):
            for fn in files:
                yield (root, fn, False)


def action_create_file():
    d = random.choice([TARGET] + [os.path.join(TARGET, r[1]) for r in all_items("dirs")]) if random.random() < 0.4 else TARGET
    ensure_dir(d)
    p = os.path.join(d, rand_name("file"))
    lines = random.randint(1, 10)
    with open(p, "w") as f:
        f.write("\n".join(f"line {i}" for i in range(lines)) + "\n")
    log(f"CREATE  {p}  ({lines} lines)")


def action_create_dir():
    parent = random.choice([TARGET] + [os.path.join(TARGET, r[1]) for r in all_items("dirs")]) if random.random() < 0.3 else TARGET
    ensure_dir(parent)
    p = os.path.join(parent, rand_name("dir"))
    os.makedirs(p, exist_ok=True)
    log(f"CREATE  {p}/")
    # maybe add a file inside
    if random.random() < 0.6:
        fp = os.path.join(p, rand_name("nested"))
        with open(fp, "w") as f:
            f.write("nested content\n" * random.randint(1, 3))
        log(f"CREATE  {fp}")


def action_delete():
    items = list(all_items())
    if not items:
        return action_create_file()
    root, name, is_dir = random.choice(items)
    p = os.path.join(root, name)
    try:
        if is_dir:
            shutil.rmtree(p)
            log(f"DELETE  {p}/")
        else:
            os.remove(p)
            log(f"DELETE  {p}")
    except OSError:
        pass


def action_modify():
    items = list(all_items("files"))
    if not items:
        return action_create_file()
    root, name, _ = random.choice(items)
    p = os.path.join(root, name)

    kind = random.choice(["append", "inline", "delete_line"])
    try:
        if kind == "append":
            with open(p, "a") as f:
                f.write("\n".join(f"appended {i}" for i in range(random.randint(1, 4))) + "\n")
            log(f"MODIFY  {p}  (+lines)")

        elif kind == "inline":
            with open(p, "r") as f:
                lines = f.readlines()
            if lines:
                idx = random.randint(0, len(lines) - 1)
                lines[idx] = f"[MOD {time.strftime('%H:%M:%S')}] {lines[idx].strip()}\n"
            with open(p, "w") as f:
                f.writelines(lines)
            log(f"MODIFY  {p}  (inline)")

        else:  # delete_line
            with open(p, "r") as f:
                lines = f.readlines()
            if len(lines) > 1:
                del lines[random.randint(0, len(lines) - 1)]
                with open(p, "w") as f:
                    f.writelines(lines)
                log(f"MODIFY  {p}  (-line)")

    except OSError:
        pass


def action_rename():
    items = list(all_items())
    if not items:
        return action_create_file()
    root, name, is_dir = random.choice(items)
    old = os.path.join(root, name)
    new_name = rand_name("renamed", "" if is_dir else ".txt")
    new = os.path.join(root, new_name)
    try:
        os.rename(old, new)
        log(f"RENAME  {name}  ->  {new_name}")
    except OSError:
        pass


# ── weighted action table ───────────────────────────────────────────

ACTIONS = [
    (action_create_file,  18),
    (action_create_dir,   12),
    (action_modify,       35),
    (action_delete,       12),
    (action_rename,        8),
]


def weighted_choice():
    total = sum(w for _, w in ACTIONS)
    r = random.randint(1, total)
    for fn, w in ACTIONS:
        r -= w
        if r <= 0:
            return fn
    return ACTIONS[-1][0]


# ── main ────────────────────────────────────────────────────────────

def main():
    print(f"DirectoryScan random stress test")
    print(f"Target: {os.path.abspath(TARGET)}")
    print(f"Interval: 1.0–5.0s (0.5s steps)")
    print(f"Run DirectoryScan {os.path.abspath(TARGET)} in another terminal")
    print(f"Press Ctrl+C to stop\n")

    ensure_dir(TARGET)
    count = 0

    try:
        while True:
            fn = weighted_choice()
            fn()
            count += 1

            # batch 1-3 actions per interval for more visual impact
            for _ in range(random.randint(0, 2)):
                weighted_choice()()

            time.sleep(rand_interval())
    except KeyboardInterrupt:
        log(f"Stopped after {count} actions. Cleaning up...")
        shutil.rmtree(TARGET, ignore_errors=True)
        log("Done.")


if __name__ == "__main__":
    main()
