#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Host test of tools/mkzexe.py's ZEX2 path (DESIGN D13, docs/d13-shared-code.md section 11): small programs built the
# way a program with shared code is (tools/purecc.sh, link/pure.ld, crt0 -DZEX2, libz.po), then
#   - the good one: a .data pointer into constants is in the to-code list, a .data pointer into BSS and the table slot
#     of a BSS array in the to-data list, the slot of a constant in the to-code list, the header adds up;
#   - refused, each with its reason and the symbol named: an absolute reference from code to data (shared-to-data), a
#     PC-relative one, a value equal to text_size, an undefined weak symbol, an absolute symbol.
#   tools/mkzexe-test.sh            (BUILD=dir to build in another directory; default a temporary one)
cd "$(dirname "$0")/.." || exit 1
OUT=$(mktemp -d /tmp/zenix-mkzexe.XXXXXX)
B=${BUILD:-$OUT/build}
trap 'rm -rf "$OUT"' EXIT
make -s BUILD="$B" "$B/crt0-pure.o" "$B/libz.po" >"$OUT/make.log" 2>&1 || { cat "$OUT/make.log"; echo "FAIL build"; exit 1; }
# the Makefile's default compiler (TOOLPREFIX, or m68k-elf-gcc on PATH, else the default install place)
PFX=${TOOLPREFIX:-$(command -v m68k-elf-gcc >/dev/null 2>&1 && echo m68k-elf- || echo "$HOME/opt/m68k-elf/bin/m68k-elf-")}
CC=${PFX}gcc
LIBGCC=$("$CC" -mcpu=68060 -msoft-float -print-libgcc-file-name)
F=(-m68020-60 -msoft-float -ffreestanding -fno-builtin -nostdlib -O2 -Wall -Wextra -fno-strict-aliasing -fno-common
   -ffunction-sections -fdata-sections)
pass=1
ok()  { echo "PASS $1"; }
bad() { echo "FAIL $1"; pass=0; }

prog() {   # prog NAME SRCFILE [ld flags]: compile (C through purecc.sh, .S assembled as is) and link
    local n=$1 src=$2; shift 2
    case "$src" in
    *.c) tools/purecc.sh "$CC" "${F[@]}" -c "$src" -o "$OUT/$n.o" ;;
    *.S) "$CC" -m68020-60 -c "$src" -o "$OUT/$n.o" ;;
    esac || return 1
    "$CC" "${F[@]}" -nostartfiles -T link/pure.ld -Wl,--build-id=none -Wl,-q -Wl,--gc-sections "$@" \
        "$B/crt0-pure.o" "$OUT/$n.o" "$B/libz.po" "$LIBGCC" -o "$OUT/$n.elf" 2>"$OUT/$n.ld.log"
}
refused() {   # refused NAME WHAT PATTERN: mkzexe fails on NAME and its message matches PATTERN
    if TOOLPREFIX=$PFX python3 tools/mkzexe.py "$OUT/$1.elf" "$OUT/$1.zx" >"$OUT/$1.log" 2>&1; then
        bad "$2 (mkzexe accepted it)"
    elif grep -qE -- "$3" "$OUT/$1.log"; then
        ok "$2: $(grep -E -- "$3" "$OUT/$1.log" | head -1 | sed 's/^[^:]*: //')"
    else
        bad "$2 (message: $(head -1 "$OUT/$1.log"))"
    fi
}

# ---- the good one ----
cat > "$OUT/good.c" <<'EOF'
const char msg[] = "constant";
const char *pmsg = msg;                 /* .data -> .rodata: to code */
static int buf[16];
int *pbuf = buf + 2;                    /* .data -> .bss: to data */
int dat = 5;
int get(int i) { return buf[i] + dat; }         /* buf and dat through table slots: to data */
const char *m(void) { return msg; }             /* msg through a slot: to code */
int main(void) { buf[1] = 3; return get(1) + *m() + *pmsg + *pbuf; }
EOF
if prog good "$OUT/good.c" && TOOLPREFIX=$PFX python3 tools/mkzexe.py "$OUT/good.elf" "$OUT/good.zx" 16384 >"$OUT/good.log" 2>&1; then
    ok "a program with shared code is accepted ($(sed 's/^[^:]*: //' "$OUT/good.log"))"
    python3 - "$OUT/good.elf" "$OUT/good.zx" "${PFX}nm" <<'EOF' || pass=0
import struct, subprocess, sys
elf, zx, nm = sys.argv[1:4]
sym = {}
for l in subprocess.run([nm, elf], capture_output=True, text=True).stdout.splitlines():
    p = l.split()
    if len(p) == 3:
        sym[p[2]] = int(p[0], 16)
f = open(zx, "rb").read()
assert f[4:8] == b"ZEX2"
ts, ds, ms, nc, nd, ro, stack, res = struct.unpack(">8I", f[8:40])
code = set(struct.unpack(f">{nc}I", f[ro:ro + 4 * nc]))
data = set(struct.unpack(f">{nd}I", f[ro + 4 * nc:ro + 4 * (nc + nd)]))
word = lambda a: struct.unpack(">I", f[a:a + 4])[0]
slot = lambda v: next(a for a in range(ts, ts + ds, 4) if word(a) == v)
checks = [
    ("the header: text_size 16-aligned, the data image and the lists fill the file, stack and reserved as given",
     ts % 16 == 0 and ro == ts + ds and len(f) == ro + 4 * (nc + nd) and ms >= ds and stack == 16384 and res == 0),
    ("text_size is where the table starts (__text_end)", ts == sym["__text_end"]),
    ("a .data pointer into constants is relocated to code", sym["pmsg"] in code and sym["pmsg"] not in data),
    ("a .data pointer into BSS is relocated to data", sym["pbuf"] in data and word(sym["pbuf"]) == sym["buf"] + 8),
    ("the table slot of a BSS array is relocated to data", slot(sym["buf"]) in data),
    ("the table slot of a .data word is relocated to data", slot(sym["dat"]) in data),
    ("the table slot of a constant is relocated to code", slot(sym["msg"]) in code),
    ("every relocation lies in the data image, on an even address",
     all(ts <= r <= ts + ds - 4 and not r & 1 for r in code | data)),
    ("every table slot is relocated once", all((a in code) != (a in data) for a in range(ts, sym["_GLOBAL_OFFSET_TABLE_"], 4))),
]
bad = 0
for what, good in checks:
    print(("PASS " if good else "FAIL ") + what)
    bad |= not good
sys.exit(bad)
EOF
else
    bad "a program with shared code is accepted ($(cat "$OUT/good.ld.log" "$OUT/good.log" 2>/dev/null | head -3))"
fi

# ---- refused ----
cat > "$OUT/s2d.S" <<'EOF'
    .text
    .globl main
main:
    move.l  #counter,%d0        /* an absolute address of data, placed in the shared code */
    rts
    .data
counter: .long 0
EOF
prog s2d "$OUT/s2d.S" && refused s2d "an absolute reference from code to data" "shared part \(counter; shared-to-data\)"

cat > "$OUT/pcrel.S" <<'EOF'
    .text
    .globl main
main:
    lea     counter2(%pc),%a0   /* PC-relative from the code into the data part */
    move.l  (%a0),%d0
    rts
    .data
counter2: .long 0
EOF
prog pcrel "$OUT/pcrel.S" && refused pcrel "a PC-relative reference from code to data" "to counter2(\+0x[0-9a-f]+)? in the data part \(shared-to-data\)"

cat > "$OUT/ts.S" <<'EOF'
    .text
    .globl main
main:
    moveq   #0,%d0
    rts
    .data
    .globl edge
edge: .long __text_end          /* the value is exactly text_size: start of data or end of code? */
EOF
prog ts "$OUT/ts.S" -Wl,--undefined=edge && refused ts "a value equal to text_size" "equals text_size"

cat > "$OUT/weak.c" <<'EOF'
extern int maybe __attribute__((weak));
int main(void) { return &maybe ? maybe : 0; }
EOF
prog weak "$OUT/weak.c" && refused weak "an undefined weak symbol" "undefined \(weak\) symbol maybe"

cat > "$OUT/abs.c" <<'EOF'
extern char hwreg[];
int main(void) { return hwreg[0]; }
EOF
prog abs "$OUT/abs.c" -Wl,--defsym=hwreg=0xDFF000 && refused abs "an absolute symbol" "absolute symbol hwreg"

[ $pass = 1 ] && { echo "mkzexe-test: all passed"; exit 0; }
echo "mkzexe-test: FAILED"; exit 1
