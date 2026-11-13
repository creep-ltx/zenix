/* SPDX-License-Identifier: GPL-2.0-only */
#pragma GCC optimize("Os")              /* table walks at a spawn, an exit, an open for writing: size first */
#include "kernel.h"

/* Shared code segments (DESIGN D13, docs/d13-shared-code.md sections 6-7). A ZEX2 program's code and constants are
 * loaded once and run by every process that starts the same unchanged file. A segment belongs to no process: its block
 * carries SEG_OWNER, so mem_free_owner() of a sharer never frees it; the processes running it hold references (taken
 * at lookup, before anything can block; dropped in proc_exit), and the last one frees it at once.
 * A segment is found (listed) only while its key - mount slot, the mount's generation, inode, size, modification
 * time - still names the file it was loaded from; every change the kernel sees takes it off the list (vfs.c: a write
 * open, unlink, rename, link, symlink; a remount or a restarted filesystem server changes the generation), and an
 * open for writing of a listed one's file is refused (-ETXTBSY). Its processes keep running it either way.
 * The checksum (add and rotate over every long word) catches damage to the copy in memory: checked at every reuse and
 * when one of its processes faults. */

static struct seg segs[NSEG];

__attribute__((optimize("O2"))) u32 seg_sum(u32 code, u32 size)     /* the one loop over the whole code */
{
    const u32 *w = (const u32 *)code, *e = w + size / 4;
    u32 s = 0;
    while (w < e) {
        u32 v = *w++;
        s = ((s << 1) | (s >> 31)) + v;
    }
    return s;
}

static int key_eq(const struct seg *s, const struct exec_src *x, u32 size)
{
    return s->code && s->listed && s->m == (u8)x->m && s->gen == x->gen && s->ino == (u32)x->ino &&
           s->fsize == x->st.size && s->mtime == x->st.mtime && s->size == size;
}

/* a listed segment of this file, intact: a reference to it (0: none, the caller loads its own copy) */
struct seg *seg_find(const struct exec_src *x, u32 size, const char *path)
{
    if (x->stale || x->wopen || x->gen != vfs_mount_gen(x->m))
        return 0;
    for (int i = 0; i < NSEG; i++) {
        struct seg *s = &segs[i];
        if (!key_eq(s, x, size))
            continue;
        if (seg_sum(s->code, s->size) != s->sum) {
            kprintf("exec: %s: its shared code in memory is damaged (checksum): loading a fresh copy\n", path);
            s->listed = 0;
            return 0;
        }
        s->refs++;
        return s;
    }
    return 0;
}

/* the caller loaded and checksummed its own copy at CODE: list it (the segment owns the block from now on, *made 1),
 * or take the one another spawn listed meanwhile (*made 0: the caller frees its copy). 0: it may not be listed (the
 * file may have changed, it was open for writing, or the table is full): a private copy. */
struct seg *seg_enter(const struct exec_src *x, u32 code, u32 size, u32 sum, const char *path, int *made)
{
    struct seg *s = seg_find(x, size, path);
    *made = 0;
    if (s || x->stale || x->wopen || x->gen != vfs_mount_gen(x->m))
        return s;
    for (int i = 0; i < NSEG; i++) {
        s = &segs[i];
        if (s->code)
            continue;
        s->code = code;
        s->size = size;
        s->sum = sum;
        s->refs = 1;
        s->listed = 1;
        s->m = (u8)x->m;
        s->gen = x->gen;
        s->ino = (u32)x->ino;
        s->fsize = x->st.size;
        s->mtime = x->st.mtime;
        mem_set_owner((void *)code, SEG_OWNER);
        *made = 1;
        return s;
    }
    return 0;
}

void seg_put(struct seg *s)
{
    if (s && !--s->refs) {
        mem_free((void *)s->code);
        s->code = 0;
        s->listed = 0;
    }
}

/* DESIGN D54 section 10: p is the only process running its segment (refs 1): the code becomes p's own copy (owned
 * by its pid, freed with it), the slot is freed, and the same program started later loads a fresh shared copy */
void seg_disown(struct proc *p)
{
    struct seg *s = p->seg;
    mem_set_owner((void *)s->code, (u32)p->pid);
    s->code = 0;
    s->listed = 0;
    s->refs = 0;
    p->seg = 0;
}

void seg_unlist(int m, long ino)
{
    for (int i = 0; i < NSEG; i++)
        if (segs[i].code && segs[i].m == (u8)m && (ino < 0 || segs[i].ino == (u32)ino))
            segs[i].listed = 0;
}

/* an open for writing of the file with this status would change a segment's file (-ETXTBSY): one is in memory with
 * the same key (also one no longer listed: its processes still run it; Unix says "text file busy" too) */
int seg_busy(int m, const struct ustat *st)
{
    u32 gen = vfs_mount_gen(m);
    for (int i = 0; i < NSEG; i++) {
        const struct seg *s = &segs[i];
        if (s->code && s->m == (u8)m && s->gen == gen && s->ino == st->ino && s->fsize == st->size &&
            s->mtime == st->mtime)
            return 1;
    }
    return 0;
}

int seg_any(void)
{
    for (int i = 0; i < NSEG; i++)
        if (segs[i].code)
            return 1;
    return 0;
}

void seg_stats(u32 *bytes, u32 *saved)
{
    u32 b = 0, v = 0;
    for (int i = 0; i < NSEG; i++)
        if (segs[i].code) {
            b += segs[i].size;
            v += (segs[i].refs - 1u) * segs[i].size;
        }
    *bytes = b;
    *saved = v;
}

/* p faulted at pc while running shared code (exc_c, before proc_exit drops its reference): was the code damaged? Then
 * it is never used again, and the processes still running the damaged copy are named (without an MMU nothing can stop
 * them; D13 section 7) */
void seg_crash(struct proc *p, u32 pc)
{
    struct seg *s = p->seg;
    (void)pc;
    if (!s)
        return;
    if (seg_sum(s->code, s->size) == s->sum) {
        kprintf("[kernel] pid %d (%s): its shared code is intact\n", p->pid, p->name);
        return;
    }
    s->listed = 0;
    kprintf("[kernel] pid %d (%s): its shared code is DAMAGED (checksum): not used again; still running it:",
            p->pid, p->name);
    int n = 0;
    for (int i = 1; i < NPROC; i++)
        if (&procs[i] != p && procs[i].seg == s && procs[i].state != P_FREE && procs[i].state != P_ZOMBIE) {
            kprintf(" %d", procs[i].pid);
            n++;
        }
    kprintf("%s\n", n ? "" : " none");
}
