"""Regenerate the Unicode tables at the end of deelx.h.

usage: python gen_unicode_tables.py <UCD directory> [deelx.h]

The UCD directory must hold UnicodeData.txt and CaseFolding.txt, e.g. from
https://www.unicode.org/Public/16.0.0/ucd/ . With --check, nothing is written;
the tables in deelx.h are compared with the UCD instead.
Update the version in the comment above the tables when the UCD changes.
"""
import os
import re
import sys

CATS = ["Lu", "Ll", "Lt", "Lm", "Lo", "Mn", "Mc", "Me", "Nd", "Nl", "No",
        "Pc", "Pd", "Ps", "Pe", "Pi", "Pf", "Po", "Sm", "Sc", "Sk", "So",
        "Zs", "Zl", "Zp", "Cc", "Cf", "Cs", "Co", "Cn"]


def read_fields(path):
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if line:
                yield [x.strip() for x in line.split(";")]


def load(ucd):
    gc = ["Cn"] * 0x110000
    first = None
    for f in read_fields(os.path.join(ucd, "UnicodeData.txt")):
        cp, name, cat = int(f[0], 16), f[1], f[2]
        if name.endswith(", First>"):
            first = cp
        elif name.endswith(", Last>"):
            for c in range(first, cp + 1):
                gc[c] = cat
        else:
            gc[cp] = cat

    fold = {}
    for f in read_fields(os.path.join(ucd, "CaseFolding.txt")):
        if f[1] in ("C", "S"):
            fold[int(f[0], 16)] = int(f[2], 16)

    return gc, fold


def category_runs(gc):
    return [(cp << 5) | CATS.index(gc[cp]) for cp in range(0x110000) if cp == 0 or gc[cp] != gc[cp - 1]]


def fold_ranges(fold):
    # (first, last, delta, step); a step-2 range must not span another folded
    # char, or the binary search in deelx_unicode_fold() would miss that one
    items = sorted(fold.items())
    ranges = []
    i = 0
    while i < len(items):
        cp, to = items[i]
        delta = to - cp
        last, step, j = cp, 1, i + 1
        if j < len(items) and items[j][1] - items[j][0] == delta and items[j][0] - cp in (1, 2):
            step = items[j][0] - cp
            while (j < len(items) and items[j][1] - items[j][0] == delta and items[j][0] == last + step
                   and (step == 1 or last + 1 not in fold)):
                last = items[j][0]
                j += 1
        ranges.append((cp, last, delta, step))
        i = j
    return ranges


def body(values, per_line, fmt):
    lines = []
    for k in range(0, len(values), per_line):
        lines.append("\t\t" + ", ".join(fmt(v) for v in values[k:k + per_line]) + ",")
    return "\n".join(lines) + "\n"


def replace_array(text, decl, new_body):
    pattern = re.compile(r"(" + re.escape(decl) + r"\n\t\{\n)(.*?)(\t\};)", re.S)
    m = pattern.search(text)
    if m is None:
        sys.exit("cannot find " + decl)
    return text[:m.start(2)] + new_body + text[m.end(2):], m.group(2)


def main():
    args = [a for a in sys.argv[1:] if a != "--check"]
    check = "--check" in sys.argv
    if not args:
        sys.exit(__doc__)
    ucd = args[0]
    header = args[1] if len(args) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "deelx.h")

    gc, fold = load(ucd)
    run_list, fold_list = category_runs(gc), fold_ranges(fold)
    runs = body(run_list, 8, lambda v: "0x%07X" % v)
    folds = body(fold_list, 4, lambda r: "0x%05X, 0x%05X, %6d, %d" % r)

    with open(header, encoding="latin-1", newline="") as f:
        text = f.read()
    crlf = "\r\n" in text
    text = text.replace("\r\n", "\n")

    text, old_runs = replace_array(text, "\tstatic const unsigned int runs[] =", runs)
    text, old_folds = replace_array(text, "\tstatic const int ranges[] =", folds)

    if check:
        same = old_runs == runs and old_folds == folds
        print("tables are up to date" if same else "tables differ from the UCD")
        sys.exit(0 if same else 1)

    if crlf:
        text = text.replace("\n", "\r\n")
    with open(header, "w", encoding="latin-1", newline="") as f:
        f.write(text)
    print("%d category runs, %d fold ranges" % (len(run_list), len(fold_list)))


main()
