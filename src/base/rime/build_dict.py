#!/usr/bin/env python3
import sys

words_file = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/dict/words"
out_file = sys.argv[2] if len(sys.argv) > 2 else "easy_en.dict.yaml"

words = [l.strip() for l in open(words_file) if l.strip().isalpha() and len(l.strip()) >= 2]
seen = set()
entries = []

for w in words:
    wl = w.lower()
    if wl in seen:
        continue
    seen.add(wl)
    # Shorter common words get higher base weight
    weight = max(100, 100000 - len(wl) * 1500)
    if w.islower():
        weight += 5000
    entries.append(f"{wl}\t{w}\t{weight}")

with open(out_file, "w", encoding="utf-8") as f:
    f.write("---\nname: easy_en\nversion: \"0.1\"\nsort: by_weight\nuse_preset_vocabulary: false\n...\n")
    f.write("\n".join(entries) + "\n")
