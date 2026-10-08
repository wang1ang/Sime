#!/usr/bin/env python3
"""Emit sogou.map.txt: full-pinyin syllable -> Sogou(Microsoft) shuangpin code.

Source of syllables: engine dict.inc (the set the engine actually recognizes).
Skips bare initials (b/p/m...) and interjections (ng/hm) that have no
shuangpin code. Mapping logic is in gen_sogou_map.to_code (transcribed from
InputScheme.swift).
"""
import os, sys, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_sogou_map import to_code

HERE = os.path.dirname(os.path.abspath(__file__))
INC = os.path.join(HERE, "..", "src", "dict.inc")
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "sogou.map.txt")

FinalMask = 0xFFF
rows = []
with open(INC) as f:
    for line in f:
        m = re.search(r'\{\s*"([a-z:]+)"\s*,\s*0x([0-9a-fA-F]+)\s*\}', line)
        if not m:
            continue
        syl, val = m.group(1), int(m.group(2), 16)
        if (val & FinalMask) == 0:
            continue  # bare initial, not a full syllable
        try:
            code = to_code(syl)
        except ValueError:
            continue  # ng/hm interjections: tool skips these entries
        rows.append((syl, code))

rows.sort()
with open(OUT, "w") as f:
    f.write("# full-pinyin-syllable  sogou-shuangpin-code\n")
    f.write("# generated from InputScheme.swift microsoft layout\n")
    for syl, code in rows:
        f.write(f"{syl} {code}\n")
print(f"wrote {len(rows)} syllable mappings to {OUT}")
