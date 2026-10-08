#!/usr/bin/env python3
"""Generate the Sogou(=Microsoft layout) reverse map: full-pinyin syllable -> 2-key shuangpin code.

Transcribed verbatim from iOS/Shared/InputScheme.swift `ShuangpinLayout.microsoft`.
This is pure data derived from the single source of truth; no guessing.

Forward (key->final) is 1-to-many via initial-dependent ambiguous finals.
Reverse (syllable->code) is 1-to-1: the syllable already fixes initial+final.
"""

# --- transcribed from InputScheme.swift ---

# commonInitials: v->zh, i->ch, u->sh  (reverse: zh->v, ch->i, sh->u)
INITIAL_TO_KEY = {"zh": "v", "ch": "i", "sh": "u"}
# all other initials are their own letter.
ALL_INITIALS = ["b","p","m","f","d","t","n","l","g","k","h",
                "j","q","x","zh","ch","sh","r","z","c","s","y","w"]

# finals table from `microsoft`. Each entry: key -> Final.
# We invert to final-string -> key. Ambiguous finals contribute BOTH products.
# resolve rules (from InputScheme.swift):
#   uoO:      oSet(b,p,m,f,w)->o  else uo       (key o)
#   iaUa:     uSet(g,k,h,zh,ch,sh,r,z,c,s)->ua else ia   (key w)
#   iangUang: uSet->uang else iang              (key d)
#   ongIong:  iongSet(j,q,x)->iong else ong     (key s)
#   uaiV:     nlSet(n,l)->v(ü) else uai         (key y)
#   ueVe:     nlSet->ve(üe) else ue             (key t)
# fixed finals map their literal string.

FIXED_FINALS = {
    "a": "a", "e": "e", "i": "i", "u": "u",
    "ui": "v", "ai": "l", "ei": "z", "ao": "k", "ou": "b",
    "iu": "q", "an": "j", "en": "f", "ang": "h", "eng": "g",
    "ie": "x", "iao": "c", "ian": "m", "in": "n",
    "ing": ";", "uan": "r", "un": "p",
}
# ambiguous finals: final-string -> key (both products of each ambiguous key)
AMBIG_FINALS = {
    "uo": "o", "o": "o",        # uoO on key o
    "ia": "w", "ua": "w",       # iaUa on key w
    "iang": "d", "uang": "d",   # iangUang on key d
    "ong": "s", "iong": "s",    # ongIong on key s
    "uai": "y", "v": "y",       # uaiV on key y  (v = ü in Sime notation)
    "ue": "t", "ve": "t",       # ueVe on key t  (ve = üe)
}
FINAL_TO_KEY = {**FIXED_FINALS, **AMBIG_FINALS}

# zero-initial: marker 'o'. syllable with no initial -> 'o' + final-key.
# (oa=a, ol=ai, oo=o, or=er ...). 'er' is a zero-initial syllable; its final
# is 'er' which isn't in FINAL_TO_KEY, so handle below.
ZERO_MARKER = "o"

# Finals that can appear standalone as a zero-initial syllable.
# These are the vowel-initial finals; code = marker + final-key.
# 'er' is special (no final key in the table) — Microsoft maps er -> or.
ZERO_SPECIAL = {"er": ZERO_MARKER + "r"}  # 而/二 = or

def split_syllable(syl):
    """Split a full-pinyin syllable into (initial, final).
    initial may be '' (zero-initial). Longest-initial-match first."""
    # zero-initial special (er)
    if syl in ZERO_SPECIAL:
        return ("", syl)   # final == whole syllable, handled by caller
    # two-letter initials first
    for ini in ("zh", "ch", "sh"):
        if syl.startswith(ini):
            return (ini, syl[len(ini):])
    # single-letter initials
    first = syl[0]
    single = {"b","p","m","f","d","t","n","l","g","k","h",
              "j","q","x","r","z","c","s","y","w"}
    if first in single:
        return (first, syl[1:])
    # zero-initial: starts with a vowel
    return ("", syl)

def initial_key(ini):
    if ini == "":
        return None  # zero-initial handled separately
    return INITIAL_TO_KEY.get(ini, ini)

def final_key(fin):
    return FINAL_TO_KEY.get(fin)

def to_code(syl):
    """Return 2-char shuangpin code or raise with reason."""
    # whole-syllable zero-initial specials (er)
    if syl in ZERO_SPECIAL:
        return ZERO_SPECIAL[syl]
    ini, fin = split_syllable(syl)
    if ini == "":
        # zero-initial vowel syllable: marker + final-key
        fk = final_key(fin)
        if fk is None:
            raise ValueError(f"zero-initial final not in map: {syl!r} (final={fin!r})")
        return ZERO_MARKER + fk
    ik = initial_key(ini)
    fk = final_key(fin)
    if fk is None:
        raise ValueError(f"final not in map: {syl!r} (initial={ini!r} final={fin!r})")
    return ik + fk

if __name__ == "__main__":
    import sys
    # quick self-check against known test cases
    checks = {
        "hao": "hk", "ma": "ma", "ni": "ni", "nv": "ny", "lve": "lt",
        "gui": "gv", "guai": "gy", "zui": "zv", "zhuang": "vd",
        "a": "oa", "ai": "ol", "o": "oo", "er": "or",
        "jiong": "js", "yong": "ys", "guang": "gd", "men": "mf",
    }
    ok = True
    for syl, want in checks.items():
        try:
            got = to_code(syl)
        except ValueError as e:
            print(f"FAIL {syl}: {e}"); ok = False; continue
        mark = "ok" if got == want else "FAIL"
        if got != want: ok = False
        print(f"{mark:4} {syl:8} -> {got:4} (want {want})")
    sys.exit(0 if ok else 1)
