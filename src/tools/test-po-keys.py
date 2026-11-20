#!/usr/bin/env python3
"""Check that translated strings keep the key shortcuts named in msgid.

Prevents stale translations after shortcut updates (e.g. msgid updated from
'v' to 'Ctrl+V' while msgstr still says '按 v' or 'c:'). Excludes obsolete
entries (#~) and untranslated empty msgstrs.
"""
import pathlib
import re
import sys

MOD = r"(?:Ctrl|Alt|Shift|Super|Meta)"
COMBO_RE = re.compile(
    r"(?:%s\+)+(?:F\d{1,2}|[A-Za-z0-9]+|[.,=+\-/\\\[\]`;']|PgUp|PgDn|Home|End|Esc|Enter|Return|Tab|Space|Del|Delete|Ins|Insert|Print|Up|Down|Left|Right)"
    % MOD
)


def parse_po(text):
    entries = []
    cur = {}
    key = None
    line_no = 0
    for idx, line in enumerate(text.splitlines(), 1):
        if not line.strip():
            if cur:
                entries.append(cur)
                cur = {}
                key = None
            continue
        if line.startswith("#~"):
            cur = {}
            key = None
            continue
        if line.startswith("#"):
            continue
        m = re.match(r'(msgctxt|msgid_plural|msgid|msgstr(?:\[\d+\])?)\s+(".*")$', line)
        if m:
            if not cur:
                line_no = idx
            key = m.group(1)
            cur["line"] = line_no or idx
            cur[key] = _unquote(m.group(2))
            continue
        if line.startswith('"') and key:
            cur[key] += _unquote(line)
    if cur:
        entries.append(cur)
    return entries


def _unquote(quoted):
    s = quoted[1:-1]
    out = []
    i = 0
    while i < len(s):
        if s[i] == "\\" and i + 1 < len(s):
            c = s[i + 1]
            out.append({ "n": "\n", "t": "\t", "r": "\r", "\\": "\\", '"': '"' }.get(c, c))
            i += 2
        else:
            out.append(s[i])
            i += 1
    return "".join(out)


def check_file(path):
    entries = parse_po(path.read_text(encoding="utf-8"))
    errors = []
    for e in entries:
        mid = e.get("msgid", "")
        if not mid:
            continue
        srcs = [mid] + ([e["msgid_plural"]] if "msgid_plural" in e else [])
        want = sorted(set(t.lower() for t in COMBO_RE.findall(" ".join(srcs))))
        if not want:
            continue
        for k, s in sorted(e.items()):
            if not k.startswith("msgstr") or not s:
                continue
            have = set(t.lower() for t in COMBO_RE.findall(s))
            missing = [t for t in want if t not in have]
            if missing:
                errors.append(
                    f"{path}:{e['line']}: missing {','.join(missing)} in {k} ({s!r}) for msgid {mid!r}"
                )
    return errors


def main():
    if len(sys.argv) < 2:
        src = pathlib.Path(__file__).resolve().parent.parent
        paths = [src / "po" / f"{lang}.po" for lang in (src / "po/LINGUAS").read_text().split() if lang.strip()]
    else:
        paths = [pathlib.Path(p) for p in sys.argv[1:]]
    all_errors = []
    for p in paths:
        all_errors.extend(check_file(p))
    if all_errors:
        for err in all_errors:
            print("FAIL", err, file=sys.stderr)
        return 1
    print(f"PASS test-po-keys: {len(paths)} catalog(s) keep all key combinations")
    return 0


if __name__ == "__main__":
    sys.exit(main())
