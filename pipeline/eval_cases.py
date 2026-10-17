#!/usr/bin/env python3
"""Sentence and char accuracy of the top candidate on the case files.

Char accuracy is 1 - edit distance / gold chars. Files with "num" in their
name are decoded as T9. --out writes file, input, gold, top per line, for
diffing two builds.

Usage: eval_cases.py [--out tops.tsv] <sime-bin> <model-dir> <cases...>
"""

import re
import subprocess
import sys


def edit_distance(a, b):
    prev = list(range(len(b) + 1))
    for i, x in enumerate(a, 1):
        cur = [i]
        for j, y in enumerate(b, 1):
            cur.append(min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (x != y)))
        prev = cur
    return prev[-1]


def main():
    args = sys.argv[1:]
    out = None
    if args[:1] == ["--out"]:
        out = open(args[1], "w", encoding="utf-8")
        args = args[2:]
    binary, root = args[0], args[1]
    for cases in args[2:]:
        rows = [l.rstrip("\n").split("\t") for l in open(cases, encoding="utf-8") if "\t" in l]
        pairs = [(r[0], r[-1]) for r in rows]
        cmd = [binary, "-d", f"{root}/sime.dict", "-c", f"{root}/sime.cnt", "-s", "-n", "1"]
        if "num" in cases:
            cmd.append("--num")
        stdout = subprocess.run(cmd, input="\n".join(p for p, _ in pairs) + "\n:quit\n",
                                capture_output=True, text=True).stdout
        tops = re.findall(r"\[0\] (.*?) \[", stdout)
        assert len(tops) == len(pairs), (cases, len(tops), len(pairs))
        name = cases.split("/")[-1]
        ok = sum(gold == top for (_, gold), top in zip(pairs, tops))
        chars = sum(len(gold) for _, gold in pairs)
        errors = sum(edit_distance(gold, top) for (_, gold), top in zip(pairs, tops))
        print(f"{name}\t{ok}/{len(pairs)}\tsent {100 * ok / len(pairs):.2f}%\t"
              f"char {100 * (1 - errors / chars):.2f}%")
        if out:
            for (inp, gold), top in zip(pairs, tops):
                out.write(f"{name}\t{inp}\t{gold}\t{top}\n")


if __name__ == "__main__":
    main()
