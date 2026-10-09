#!/usr/bin/env python3
"""Generate full-pinyin -> Xiaohe or Ziranma reverse maps for sime-spbuild.

The key tables encode both layouts. Natural Code zero-initial codes repeat
a/e/o, leave two-letter finals unchanged, and encode longer finals with their
first letter plus the final key.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
INC = os.path.join(HERE, "..", "src", "dict.inc")

INITIAL_TO_KEY = {"zh": "v", "ch": "i", "sh": "u"}
TWO_LETTER_INITIALS = ("zh", "ch", "sh")
SINGLE_INITIALS = set("bpmfdtnlgkh jqxrzcsyw".replace(" ", ""))
U_SET = {"g", "k", "h", "zh", "ch", "sh", "r", "z", "c", "s"}
IONG_SET = {"j", "q", "x"}
NL_SET = {"n", "l"}
O_SET = {"b", "p", "m", "f", "w"}
UAI_SET = {"g", "k", "h", "zh", "ch", "sh"}

# Fixed final -> key assignments, transcribed from InputScheme.swift.
FIXED = {
    "xiaohe": {
        "a": "a", "e": "e", "i": "i", "u": "u", "iu": "q",
        "ei": "w", "uan": "r", "un": "y", "ie": "p", "ai": "d",
        "en": "f", "eng": "g", "ang": "h", "an": "j", "ou": "z",
        "ao": "c", "in": "b", "iao": "n", "ian": "m",
    },
    "ziranma": {
        "a": "a", "e": "e", "i": "i", "u": "u", "ui": "v",
        "ai": "l", "ei": "z", "ao": "k", "ou": "b", "iu": "q",
        "an": "j", "en": "f", "ang": "h", "eng": "g", "ie": "x",
        "iao": "c", "ian": "m", "in": "n", "uan": "r", "un": "p",
    },
}

# The key used by each scheme's ambiguous final family.
AMBIG_KEYS = {
    "xiaohe": {"uoO": "o", "iaUa": "x", "iangUang": "l",
               "ongIong": "s", "uiV": "v", "ueVe": "t", "uaiIng": "k"},
    "ziranma": {"uoO": "o", "iaUa": "w", "iangUang": "d",
                "ongIong": "s", "uiV": "v", "ueVe": "t", "uaiIng": "y"},
}


def split_syllable(syllable):
    for initial in TWO_LETTER_INITIALS:
        if syllable.startswith(initial):
            return initial, syllable[len(initial):]
    if syllable[0] in SINGLE_INITIALS:
        return syllable[0], syllable[1:]
    return "", syllable


def final_key(scheme, final, initial):
    fixed = FIXED[scheme].get(final)
    if fixed is not None:
        return fixed

    family = None
    if final in {"uo", "o"}:
        family = "uoO"
        if final == "o" and initial not in O_SET:
            return None
        if final == "uo" and initial in O_SET:
            return None
    elif final in {"ia", "ua"}:
        family = "iaUa"
        if (final == "ua") != (initial in U_SET):
            return None
    elif final in {"iang", "uang"}:
        family = "iangUang"
        if (final == "uang") != (initial in U_SET):
            return None
    elif final in {"ong", "iong"}:
        family = "ongIong"
        if (final == "iong") != (initial in IONG_SET):
            return None
    elif final in {"ui", "v"}:
        family = "uiV"
        if (final == "v") != (initial in NL_SET):
            return None
    elif final in {"ue", "ve"}:
        family = "ueVe"
        if (final == "ve") != (initial in NL_SET):
            return None
    elif final in {"uai", "ing"}:
        family = "uaiIng"
        if (final == "uai") != (initial in UAI_SET):
            return None
    if family is None:
        return None
    return AMBIG_KEYS[scheme][family]


def to_code(scheme, syllable):
    if syllable == "er":
        return "er"
    initial, final = split_syllable(syllable)
    if not initial and syllable in {"a", "e", "o"}:
        return syllable + syllable
    if not initial and scheme == "ziranma" and len(syllable) == 2:
        return syllable
    key = final_key(scheme, final, initial)
    if key is None:
        raise ValueError(f"no {scheme} key for {syllable!r} ({initial!r}+{final!r})")
    if not initial:
        # Xiaohe/Ziranma enter zero-initial syllables using their vowel key.
        return syllable[0] + key
    initial_key = INITIAL_TO_KEY.get(initial, initial)
    return initial_key + key


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in FIXED:
        raise SystemExit(f"Usage: {sys.argv[0]} xiaohe|ziranma [out.map.txt]")
    scheme = sys.argv[1]
    examples = {
        "a": "aa", "e": "ee", "o": "oo", "ang": "ah", "eng": "eg", "er": "er",
    }
    examples.update({
        "xiaohe": {"ai": "ad", "an": "aj", "en": "ef", "ou": "oz"},
        "ziranma": {"ai": "ai", "an": "an", "en": "en", "ou": "ou"},
    }[scheme])
    for syllable, expected in examples.items():
        actual = to_code(scheme, syllable)
        if actual != expected:
            raise SystemExit(f"{scheme}: {syllable} mapped to {actual}, expected {expected}")
    output = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, f"{scheme}.map.txt")
    rows = []
    with open(INC, encoding="utf-8") as source:
        for line in source:
            match = re.search(r'\{\s*"([a-z:]+)"\s*,\s*0x([0-9a-fA-F]+)\s*\}', line)
            if not match:
                continue
            syllable, value = match.group(1), int(match.group(2), 16)
            if (value & 0xFFF) == 0:
                continue
            try:
                code = to_code(scheme, syllable)
            except ValueError:
                continue
            rows.append((syllable, code))
    rows.sort()
    with open(output, "w", encoding="utf-8") as target:
        target.write(f"# full-pinyin-syllable  {scheme}-shuangpin-code\n")
        target.write(f"# generated by gen_shuangpin_map.py for {scheme}\n")
        for syllable, code in rows:
            target.write(f"{syllable} {code}\n")
    print(f"wrote {len(rows)} syllable mappings to {output}")


if __name__ == "__main__":
    main()
