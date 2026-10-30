#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# ZINE's AmigaOS tables (docs/d29-zine.md section 5 and 16 step 1), from the public RKRM "Includes and Autodocs"
# (3rd edition, the 2.04 = V37 interface) as plain text: its "Function Offset Reference" (the .fd files) and its
# "Structure Reference" (every structure field's offset). The text is OCR'd, so every number is read in the forms the
# page prints it (decimal, $hex, -$hex) and must agree, and every function name is spelt as the manuals spell it most
# often (all manual texts in the folder vote). Names and numbers only: nothing else is taken.
#   tools/zine-tables.py lvo MANUALS OUT.c OUT.h   write user/zine/lvo.c and lvo.h (the library offset name table)
#   tools/zine-tables.py check MANUALS [OFS.h]     the committed lvo.c/lvo.h are what lvo makes, and every offset in
#                                                  user/zine/amigaofs.h is the Structure Reference's
#   tools/zine-tables.py structs MANUALS STRUCT... print the Structure Reference's fields of these structures
# MANUALS is the folder of the manuals' text files (the knowledge base's manuals/text; ZENIX_MANUALS in the tests).
import collections, difflib, os, re, sys

RKRM = "Amiga_ROM_Kernal_Reference_Includes_and_Autodocs_3rd_Edition.txt"
LIBS = [("exec", ["exec.library"]), ("dos", ["dos.library"]), ("graphics", ["graphics.library"]),
        ("intuition", ["intuition.library"]), ("cia", ["ciaa.resource", "ciab.resource"]),
        ("potgo", ["potgo.resource"])]
# OCR slips the vote does not settle (a letter read as a digit or the other way round, a capital read small), each
# the printed name on the page; the key is the spelling the vote gave
FIX = {
    "Exitlntr": "ExitIntr", "switch": "Switch", "exception": "Exception", "signal": "Signal", "CheckiO": "CheckIO",
    "WaitiO": "WaitIO", "AbortiO": "AbortIO", "RawIOlnit": "RawIOInit", "CreateiORequest": "CreateIORequest",
    "DeleteiORequest": "DeleteIORequest", "CreatePrivatePoo1": "CreatePrivatePool",
    "De1etePrivatePool": "DeletePrivatePool", "ExecReservedOO": "ExecReserved00", "Chi1d0rphan": "ChildOrphan",
    "ExecReservedOl": "ExecReserved01",
    "exit": "Exit", "UnLoadSeq": "UnLoadSeg", "NoReqLoadSeq": "NoReqLoadSeg", "Islnteractive": "IsInteractive",
    "Selectlnput": "SelectInput", "ExAII": "ExAll", "SetloErr": "SetIoErr", "GetArqStr": "GetArgStr",
    "SetArqStr": "SetArgStr", "format": "Format", "Readltem": "ReadItem", "Clilnit": "CliInit",
    "ClilnitNewcli": "CliInitNewcli", "ClilnitRun": "CliInitRun",
    "drawellipse": "DrawEllipse", "CMOVE": "CMove", "Ci'lait": "CWait", "UCopperListlnit": "UCopperListInit",
    "NextDisplayinfo": "NextDisplayInfo", "AddDisplayinfo": "AddDisplayInfo", "AddDisplayinfoData": "AddDisplayInfoData",
    "SetDisplayinfoData": "SetDisplayInfoData", "WritePixelLineS": "WritePixelLine8",
    "WritePixelArrayS": "WritePixelArray8",
    "Openintuition": "OpenIntuition", "WindowL~ts": "WindowLimits", "LockPubScreenL~st": "LockPubScreenList",
    "MoveWindowinFrontOf": "MoveWindowInFrontOf", "SetiPrefs": "SetIPrefs", "EasyRequestAigs": "EasyRequestArgs",
    "BuildEasyRequestAigs": "BuildEasyRequestArgs", "DrawimageState": "DrawImageState",
    "Pointinimage": "PointInImage", "Eraseimage": "EraseImage", "Of£Menu": "OffMenu",
    "AddiCRVector": "AddICRVector", "RemiCRVector": "RemICRVector", "AbleiCR": "AbleICR", "SetiCR": "SetICR",
}
# lines the text layer lost, read from the page image (PDF page 913, "Function Offset Reference" page 12)
MISSING = {"graphics": [(612, "OrRegionRegion"), (618, "XorRegionRegion"), (624, "AndRegionRegion"),
                        (630, "SetRGB4CM"), (636, "BltMaskBitMapRastPort"), (654, "AttemptLockLayerRom")]}
# slots the listing leaves empty on purpose ("function slot(s) reserved here"); any other hole is an error
RESERVED = {"dos": [486, 864, 960], "graphics": [642, 648]}

DIG = str.maketrans("lIiOoSsB|£", "1110055810")


def num(s):
    s = s.replace(" ", "").replace(".", "").translate(DIG)
    return int(s) if s.isdigit() else None


def hexnum(s):
    s = s.lower().replace("o", "0").replace("l", "1").replace("i", "1").replace("s", "5").replace("£", "f")
    try:
        return int(s, 16)
    except ValueError:
        return None


def key(name):
    return name.lower().translate(str.maketrans("1i|!0o5", "lllloos"))


def pages(text, title):
    return [p for p in text.split("\f") if title in p]


ENTRY = re.compile(r"(?<![\w$])([\dlI][\dlIoOB. ]{0,5}?)\s+\$([\w?£]{4})\s+-+\$([\w?£]{4})\s+([A-Za-z0-9_~'£]+)\s*[({]")
MARK = re.compile(r"##\s*(base|bias)\s+(\S+)|\*[*\-]{8,}\s*(\w+)")


def read_fd(text):
    """{lib: [(offset, ocrname)]}, offsets checked"""
    out, errs = {}, []
    for pg in pages(text, "Function Offset Reference"):
        cols = ([], [])
        for line in pg.split("\n"):
            items = []
            for m in MARK.finditer(line):
                items.append((m.start(), ("lib", m.group(3)) if m.group(3) else (m.group(1), m.group(2))))
            for m in ENTRY.finditer(line):
                items.append((m.start(), ("fn", m.groups())))
            for x, it in sorted(items, key=lambda t: t[0]):
                cols[x >= 45].append(it)
        for col in cols:
            for it in col:
                yield it


def lvo_table(text):
    libs, errs = {}, []
    cur, expect = None, None
    for kind, v in read_fd(text):
        if kind == "lib":
            cur = v if v in dict(LIBS) else None
            expect = None
            if cur:
                libs.setdefault(cur, [])
        elif kind == "bias" and cur:
            expect = num(v)
        elif kind == "fn" and cur:
            d, h, nh, name = v
            c = [num(d), (65536 - hexnum(h)) if hexnum(h) is not None else None, hexnum(nh)]
            if expect is not None and expect in c:
                off = expect
            else:
                agree = [x for x in c if x is not None and c.count(x) >= 2]
                if not agree:
                    errs.append("%s: no reading of %r agrees (%s)" % (cur, name, c))
                    continue
                off = agree[0]
                if expect is not None and off < expect:
                    errs.append("%s: %s at %d goes back (expected %d)" % (cur, name, off, expect))
            if off % 6 or any(o == off for o, _ in libs[cur]):
                errs.append("%s: %s at %d is not a new slot" % (cur, name, off))
                continue
            libs[cur].append((off, name))
            expect = off + 6
    return libs, errs


def spell(names, folder):
    """the most frequent spelling of each name's key over every manual's text"""
    want = {key(n) for n in names}
    cnt = collections.defaultdict(collections.Counter)
    # only where a name stands as a function: NAME( or library/NAME (prose has words like "open" and "wait")
    tok = re.compile(r"([A-Za-z][A-Za-z0-9_]{2,}) ?\(|\.(?:library|resource)/ ?([A-Za-z][A-Za-z0-9_]{2,})")
    for f in sorted(os.listdir(folder)):
        if f.endswith(".txt"):
            with open(os.path.join(folder, f), encoding="utf-8", errors="replace") as fh:
                for a, b in tok.findall(fh.read()):
                    t = a or b
                    k = key(t)
                    if k in want:
                        cnt[k][t] += 1
    best = {}
    for n in names:
        c = cnt[key(n)]
        # most votes; a tie goes to the spelling with fewer digits (OCR turns l into 1, never the other way round)
        best[n] = max(c.items(), key=lambda kv: (kv[1], -sum(ch.isdigit() for ch in kv[0]), kv[0]))[0] if c else n
    return best


def make_lvo(folder):
    text = open(os.path.join(folder, RKRM), encoding="utf-8", errors="replace").read()
    libs, errs = lvo_table(text)
    for lib, _ in LIBS:
        if not libs.get(lib):
            errs.append("%s: not found in the Function Offset Reference" % lib)
            continue
        have = {o for o, _ in libs[lib]}
        for o, n in MISSING.get(lib, []):
            if o in have:
                errs.append("%s: %s at %d is in the text now: drop it from MISSING" % (lib, n, o))
            libs[lib].append((o, n))
            have.add(o)
        holes = [o for o in range(min(have), max(have), 6) if o not in have]
        if holes != RESERVED.get(lib, []):
            errs.append("%s: no function read at %s" % (lib, holes))
    if errs:
        sys.exit("zine-tables: " + "\n  ".join(errs))
    sp = spell({n for l in libs.values() for _, n in l}, folder)
    sp.update({n: n for l in MISSING.values() for _, n in l})
    used = set(sp.values())
    stale = [k for k in FIX if k not in used]
    if stale:
        sys.exit("zine-tables: FIX entries no name needs any more: %s" % stale)
    hdr = ("/* SPDX-License-Identifier: GPL-2.0-only */\n"
           "/* Made by tools/zine-tables.py lvo from the RKRM Includes and Autodocs, 3rd edition (the 2.04 = V37\n"
           " * interface): its Function Offset Reference (the .fd files). Names and offsets only. Do not edit: run the\n"
           " * tool (docs/d29-zine.md, step 1 as built). V39/V40 functions are not here: needs the NDK (owner question 4). */\n")
    c = [hdr, '#include "lvo.h"\n\n']
    h = [hdr, "#pragma once\n\n",
         "struct zl_lib {\n    const char *name;                   /* \"exec.library\" */\n"
         "    unsigned short first, n;            /* the first offset (as a positive number) and the slots, 6 bytes each */\n"
         "    const char *names;                  /* n names, each ended by a 0 byte; an empty one: no function there */\n};\n"
         "extern const struct zl_lib zl_libs[];\n#define ZL_NLIBS %d\n"
         "/* the function at -OFFSET of the library LIB (\"exec.library\"), or 0 if the table has none there */\n"
         "const char *zl_name(const char *lib, unsigned offset);\n\n" % sum(len(f) for _, f in LIBS)]
    tables = []
    for lib, full in LIBS:
        fns = sorted(libs[lib])
        first, last = fns[0][0], fns[-1][0]
        byoff = {o: FIX.get(sp[n], sp[n]) for o, n in fns}
        s = "".join(byoff.get(o, "") + "\\0" for o in range(first, last + 6, 6))
        c.append("static const char n_%s[] =\n" % lib)
        line = '    "'
        for part in re.findall(r"[^\\]*\\0", s):
            if len(line) + len(part) > 116:
                c.append(line + '"\n')
                line = '    "'
            line += part
        c.append(line + '";\n')
        for f in full:
            tables.append('    { "%s", %d, %d, n_%s },\n' % (f, first, (last - first) // 6 + 1, lib))
        for o, n in fns:
            nm = byoff[o]
            if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*", nm):
                sys.exit("zine-tables: %s: %r at %d is no name: add it to FIX" % (lib, nm, o))
            h.append("#define ZL_%s_%s %d\n" % (lib, nm, o))
        h.append("\n")
    c.append("\nconst struct zl_lib zl_libs[] = {\n" + "".join(tables) + "};\n\n")
    c.append("const char *zl_name(const char *lib, unsigned offset)\n{\n"
             "    for (int i = 0; i < ZL_NLIBS; i++) {\n"
             "        const struct zl_lib *l = &zl_libs[i];\n"
             "        const char *a = l->name, *b = lib;\n"
             "        while (*a && *a == *b)\n            a++, b++;\n"
             "        if (*a || *b)\n            continue;\n"
             "        if (offset < l->first || (offset - l->first) % 6 || (offset - l->first) / 6 >= l->n)\n"
             "            return 0;\n"
             "        const char *p = l->names;\n"
             "        for (unsigned k = (offset - l->first) / 6; k; k--)\n"
             "            while (*p++)\n                ;\n"
             "        return *p ? p : 0;\n    }\n    return 0;\n}\n")
    return "".join(c), "".join(h).rstrip("\n") + "\n"


# ---- the Structure Reference
SENT = re.compile(r"\$(\w{4})\s+([\dB][\d ]{0,4})\s+(\S(?:[^\s$]| (?! ))*)")
HEAD = re.compile(r"(?<![\w$])([A-Za-z][A-Za-z0-9_]{2,}):")
# fields the text layer lost or garbled past reading, read from the page image (PDF page, Structure Reference page)
IMAGE = {("ExecBase", "KickTagPtr"): 550,                                         # 922, page 7
         ("CommandLineInterface", "cli_CommandName"): 16, ("CommandLineInterface", "cli_FailLevel"): 20,
         ("CommandLineInterface", "cli_CurrentInput"): 32, ("CommandLineInterface", "cli_Interactive"): 40,
         ("CommandLineInterface", "cli_DefaultStack"): 52, ("CommandLineInterface", "cli_Module"): 60}  # 920, page 3


def read_structs(text):
    """{struct: [(offset, field)]}, the fields as printed (OCR)"""
    structs, order = {}, []
    stream = []
    for pg in pages(text, "Structure Reference"):
        lines = pg.split("\n")
        xs = sorted(m.start() for ln in lines for m in SENT.finditer(ln))
        cent = []
        for x in xs:
            if cent and x - cent[-1][-1] <= 12:
                cent[-1].append(x)
            else:
                cent.append([x])
        cent = [sum(c) / len(c) for c in cent if len(c) > 3]
        if not cent:
            continue
        cols = [[] for _ in cent]
        def col(x):
            return min(range(len(cent)), key=lambda i: abs(cent[i] - x))
        for ln in lines:
            for m in HEAD.finditer(ln):
                if not SENT.match(ln, m.start()):
                    cols[col(m.start() + 2)].append(("s", m.group(1)))
            for m in SENT.finditer(ln):
                cols[col(m.start())].append(("f", m.groups()))
        for c in cols:
            stream += c
    cur = None
    for kind, v in stream:
        if kind == "s":
            cur = v
            structs.setdefault(key(cur), [])
        elif cur:
            h, d, name = v
            a, b = hexnum(h), num(d)
            structs[key(cur)].append((a, b, name.strip()))
    return structs


def fkey(name):
    return re.sub(r"[^a-z0-9]", "", key(name))


def check(folder, ofs=None):
    bad = 0
    want_c, want_h = make_lvo(folder)
    here = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "user", "zine")
    for f, want in (("lvo.c", want_c), ("lvo.h", want_h)):
        if open(os.path.join(here, f)).read() != want:
            print("FAIL user/zine/%s is not what tools/zine-tables.py lvo makes from the manual" % f)
            bad += 1
    if not bad:
        print("PASS the library offset tables are the RKRM's Function Offset Reference (%d names)" % want_h.count("#define ZL_"))
    text = open(os.path.join(folder, RKRM), encoding="utf-8", errors="replace").read()
    st = read_structs(text)
    n = 0
    for ln in open(ofs or os.path.join(here, "amigaofs.h")):
        m = re.match(r"#define ZO_(\w+?)_(\w+)\s+(\d+)\b", ln)
        if not m:
            continue
        s, fld, off = m.group(1), m.group(2), int(m.group(3))
        fl = st.get(key(s), [])
        target = "sizeof%s" % s if fld == "SIZEOF" else fld
        if IMAGE.get((s, fld)) == off:
            n += 1
            continue
        def match(entries):
            t = fkey(target)
            return [e for e in entries if off in (e[0], e[1]) and (e[0] == e[1] or None in (e[0], e[1])) and
                    difflib.SequenceMatcher(None, fkey(e[2])[:len(t) + 3], t).ratio() >= 0.75]
        good = match(fl)
        if not good and ("_" in fld or fld == "SIZEOF"):    # a prefixed name is unique: a heading the text lost
            good = match([e for v in st.values() for e in v])
        if not good:
            print("FAIL amigaofs.h: %s.%s is %d here; the Structure Reference does not say so" % (s, fld, off))
            bad += 1
        n += 1
    if not bad:
        print("PASS every structure offset in amigaofs.h is the RKRM's Structure Reference (%d)" % n)
    return bad


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == "lvo" and len(sys.argv) == 5:
        c, h = make_lvo(sys.argv[2])
        open(sys.argv[3], "w").write(c)
        open(sys.argv[4], "w").write(h)
    elif len(sys.argv) in (3, 4) and sys.argv[1] == "check":
        sys.exit(1 if check(sys.argv[2], sys.argv[3] if len(sys.argv) == 4 else None) else 0)
    elif len(sys.argv) >= 4 and sys.argv[1] == "structs":
        st = read_structs(open(os.path.join(sys.argv[2], RKRM), encoding="utf-8", errors="replace").read())
        for s in sys.argv[3:]:
            print("%s:" % s)
            for e in st.get(key(s), []):
                print("  %s %s %s" % e)
    else:
        sys.exit(__doc__ if __doc__ else "usage: see the head of tools/zine-tables.py")


main()
