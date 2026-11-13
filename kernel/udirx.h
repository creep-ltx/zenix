/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
/* DESIGN D49 (docs/d49-readdir.md 3.2, frozen): one record of a batch listing, SYS_readdirs (99) and FS_READDIRS (27).
 * Records follow each other in the caller's buffer, each starting at a multiple of 4 from it. Shared by the kernel
 * (kernel/fsproto.h), the servers and the programs (user/usys.h); every field is big-endian as the machine is.
 *   size   this record's bytes: UDIRX_HEAD + the name + its 0, rounded up to 4 (44 .. UDIRX_MAX today; the rule below)
 *   type   udirent's type numbers (ext2's file types: 1 file, 2 directory, 7 symbolic link, 3-6 the others; 0 unknown)
 *   flags  UDX_STAT: st is the entry's stat as the server's FS_LSTAT gives it for this name, as Linux's fstatat(dirfd,
 *          NAME, AT_SYMLINK_NOFOLLOW): the caller's search right on this directory checked (the ones above were, at the
 *          open); never for ".", "..", a name the server had to cut, or one lstat cannot reach. A name that is a mount
 *          point keeps the directory underneath's (as ino always did): libz's zdir takes it out. Other bits 0
 *   ino    as udirent's
 *   next   the directory's position after this record (what the descriptor holds once it is read; 0 in libz's old mode)
 *   st     struct ustat's first 28 bytes (ino .. links); zeros without UDX_STAT
 *   name   1 .. 63 bytes and a 0, then zeros up to size */
struct udirx {
    unsigned short size;
    unsigned char type, flags;
    unsigned int ino, next;
    struct { unsigned int ino, mode, size, mtime, uid, gid, links; } st;
    char name[64];                          /* at most; a record holds only its own name's bytes and padding */
};
#define UDIRX_HEAD 40                       /* the bytes before the name */
#define UDIRX_MAX  104                      /* the longest record (a 63-byte name): the smallest buffer SYS_readdirs takes */
#define UDX_STAT   1
#define UDIRX_SIZE(namelen) ((UDIRX_HEAD + (namelen) + 1 + 3) & ~3u)
/* The growth rule (frozen): a reader steps from record to record by size (at least UDIRX_HEAD + 4, a multiple of 4, not
 * past the answer) and ignores flags it does not know and any bytes after the name's 0. Today's records are at most
 * UDIRX_MAX; a longer form comes only when a request asks for it (SYS_readdirs' size bits 16-31, 0 today).
 * udirx_step: the record at X (LEFT bytes from it to the answer's end) can be stepped over: its size, else 0 (stop).
 * udirx_named: and its name is one a program can use (1-63 bytes ended by a 0 inside the record) */
static inline unsigned udirx_step(const struct udirx *x, unsigned left)
{
    unsigned s = left >= UDIRX_HEAD + 4 ? x->size : 0;
    return s >= UDIRX_HEAD + 4 && s <= left && !(s & 3) ? s : 0;
}
static inline int udirx_named(const struct udirx *x)
{
    unsigned i, n = x->size - UDIRX_HEAD;
    if (n > 64) n = 64;
    for (i = 0; i < n && x->name[i]; i++) {}
    return i > 0 && i < n;
}
/* the record's type for a file of MODE (S_IF* in bits 12-15): ext2's numbers, 0 for none */
static inline unsigned udirx_type(unsigned mode)
{
    static const unsigned char t[16] = { 0, 5, 3, 0, 2, 0, 4, 0, 1, 0, 7, 0, 6, 0, 0, 0 };
    return t[(mode >> 12) & 15];
}
/* a server's record for NAME (at most 63 bytes are taken) at BUF + *USED of ROOM bytes: the record (flags 0, st zeros:
 * the server fills them), *USED moved past it; 0 when it does not fit */
static inline struct udirx *udirx_put(char *buf, unsigned room, unsigned *used, const char *name, unsigned ino,
                                      unsigned type, unsigned next)
{
    unsigned n = 0, s;
    while (n < 63 && name[n]) n++;
    s = UDIRX_SIZE(n);
    if (*used > room || room - *used < s) return 0;          /* (a server skips an empty name before: never listed) */
    struct udirx *x = (struct udirx *)(buf + *used);
    __builtin_memset(x, 0, s);
    __builtin_memcpy(x->name, name, n);
    x->size = (unsigned short)s; x->type = (unsigned char)type; x->ino = ino; x->next = next;
    *used += s;
    return x;
}
