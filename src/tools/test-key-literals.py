#!/usr/bin/env python3
"""No program decides quit or the clipboard by raw key values (KEYS.md).

Ctrl with a letter reaches a program as its control character (Ctrl+Q is 17),
so 'q' tested with the Ctrl flag never matches, and 'q' alone quit some
programs but not others. The meaning of these keys lives in one table,
base/key-action.c; programs ask key_action() or key_list_action(). This test
fails on the old forms in progs/:
  - q tested as a key (quit or back): key_list_action gives q as Esc;
  - Ctrl+Q, Ctrl+C, Ctrl+X, Ctrl+V, Ctrl+Z, Ctrl+A, Ctrl+F, Ctrl+S, Ctrl+O,
    Ctrl+N, Ctrl+T, Ctrl+W tested by control code or by letter with Ctrl.
The terminal (it hands keys to the shell) and the extended display (it hands
them to the other computer) forward raw keys on purpose and are not checked.
"""
import pathlib
import re
import sys

TABLE_LETTERS = "acfnoqstvwxz"
CODES = {ord(c) & 0x1F: c for c in TABLE_LETTERS}
EXEMPT = {"term.c", "extdisplay.c", "keytable.c"}

PATTERNS = [
    (re.compile(r"\bkey\s*==\s*'[qQ]'"), "q tested as a key: use key_list_action (q is Esc in a list)"),
    (re.compile(r"\bkey\s*==\s*\(\s*'[%s]'\s*&\s*0x1[fF]\s*\)" % TABLE_LETTERS),
     "a Ctrl+letter of the table by control code: use key_action"),
    (re.compile(r"\bkey\s*==\s*(%s)\b(?!\s*[.)]?\s*[*+/-])" % "|".join(str(n) for n in sorted(CODES))),
     "a Ctrl+letter of the table by its number: use key_action"),
    (re.compile(r"(?:\bctrl\b|KEYS_CTRL\))[^;{}\n]{0,48}\bkey\s*==\s*'[%s]'" % TABLE_LETTERS),
     "a table letter with the Ctrl flag (never what the key layer gives): use key_action"),
    (re.compile(r"\bkey\s*==\s*'[%s]'[^;{}\n]{0,48}(?:\bctrl\b|KEYS_CTRL\))" % TABLE_LETTERS),
     "a table letter with the Ctrl flag (never what the key layer gives): use key_action"),
]


def main(src):
    progs = pathlib.Path(src) / "progs"
    hits = []
    for path in sorted(list(progs.glob("*.c")) + list(progs.glob("*.inc"))):
        if path.name in EXEMPT:
            continue
        for number, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            stripped = line.strip()
            if stripped.startswith(("/*", "*", "//")):
                continue
            for pattern, why in PATTERNS:
                if pattern.search(line):
                    hits.append(f"{path.name}:{number}: {why}: {stripped}")
                    break
    for hit in hits:
        print(f"FAIL {hit}")
    if hits:
        print(f"FAIL key literals: {len(hits)} raw key tests left in progs/")
        return 1
    print("PASS key literals: quit and the clipboard come from key_action in every program")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else str(pathlib.Path(__file__).resolve().parents[1])))
