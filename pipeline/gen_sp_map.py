#!/usr/bin/env python3
"""Emit the build-time Sogou/Microsoft key map for the offline index builder.

It skips bare initials and interjections without Shuangpin codes. Runtime input
is decoded from raw keys by the selected index.
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
    f.write("# build-time pinyin-syllable to raw-key map for the decoder index\n")
    for syl, code in rows:
        f.write(f"{syl} {code}\n")
print(f"wrote {len(rows)} syllable mappings to {OUT}")
