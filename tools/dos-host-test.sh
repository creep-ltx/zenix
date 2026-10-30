#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The owner's programs' AmigaOS layer on the PC (ports/common/dos.c, audit 2026-10-30 block 3): dos.c built for a
# 32-bit PC over the PC's C library, Zenix's few own calls imitated (tools/dos-host.c: the mount table is two folders
# of a scratch directory). Checked: Rename never replaces another file (B3-1) but renames the same one; MODE_OLDFILE
# writes in place (B3-3) and still opens a read-only file; SameLock tells two mounts apart (B3-4); ExNext lists a name
# whose path is too long instead of leaving it out (B3-8); DeleteFile takes a folder; SystemTags hands sh single
# quotes, so a name holding $(...) or ` runs nothing (B3-2); T: is the user's own folder, mode 700, and one planted
# by someone else (here: a soft link) is refused (B3-7).
cd "$(dirname "$0")/.." || exit 1
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
mkdir -p "$T/ports/common" "$T/ports/tui/upstream/src" "$T/user" "$T/inc"
cp ports/common/dos.c "$T/ports/common/"
cp ports/tui/upstream/src/version.h "$T/ports/tui/upstream/src/"
cat > "$T/user/usys.h" <<'EOF'
#pragma once
#include <stdint.h>
typedef uint32_t u32;
struct ustatfs { unsigned bsize, blocks, bfree, files, ffree; };
struct umountinfo { char path[48], port[16], dev[16], type[8]; };
int sys_statfs_mount(struct ustatfs *f, u32 mount);
int sys_mountinfo(u32 index, struct umountinfo *out);
u32 sys_ticks(void);
int sys_meminfo(u32 out[4]);
int sys_klog(u32 off, void *buf, u32 n);
int sys_utimes(const char *p, u32 t);
EOF
cat > "$T/inc/zenix.h" <<'EOF'
#pragma once
#include <stddef.h>
int zrealpath(const char *in, char *out, size_t cap);
EOF
if ! gcc -m32 -O1 -w -D_GNU_SOURCE -DZENIX_TT_NAMES='"Sumi"' -DZENIX_TMPDIR="\"$T/tmp\"" -I"$T/inc" -Iports/common/include \
        -c "$T/ports/common/dos.c" -o "$T/dos.o" 2>"$T/cc.log" ||
   ! gcc -m32 -O1 -Wall -Iports/common/include -I"$T/inc" -I"$T/user" -c tools/dos-host.c -o "$T/h.o" 2>>"$T/cc.log" ||
   ! gcc -m32 "$T/h.o" "$T/dos.o" -o "$T/dh" 2>>"$T/cc.log"; then
    cat "$T/cc.log"; echo "FAIL the AmigaOS layer does not build for the PC (gcc -m32)"; exit 1
fi
mkdir -p "$T/tmp" "$T/m1" "$T/m2"
(cd "$T/m1" && "$T/dh" "$T")
