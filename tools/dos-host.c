/* SPDX-License-Identifier: GPL-2.0-only */
/* tools/dos-host-test.sh's driver: ports/common/dos.c on the PC. Zenix's own calls imitated: the mount table is
 * "/" and DIR/m2 (a second filesystem), the rest answers nothing. Run in DIR/m1 with DIR as its argument. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <amigados.h>
#include "usys.h"                       /* the test's stand-in (dos-host-test.sh) */

static char top[40];                    /* (the scratch directory: mktemp's, short) */
int zrealpath(const char *in, char *out, size_t cap)
{
    char r[4096];
    if (!realpath(in, r) || strlen(r) >= cap) return -1;
    strcpy(out, r);
    return 0;
}
int sys_mountinfo(u32 i, struct umountinfo *o)
{
    memset(o, 0, sizeof *o);
    if (i == 0) { strcpy(o->path, "/"); return 0; }
    if (i == 1) { snprintf(o->path, sizeof o->path, "%s/m2", top); return 0; }
    return -1;
}
int sys_statfs_mount(struct ustatfs *f, u32 m) { (void)m; memset(f, 0, sizeof *f); return 0; }
u32 sys_ticks(void) { return 0; }
int sys_meminfo(u32 out[4]) { memset(out, 0, 16); return 0; }
int sys_klog(u32 off, void *buf, u32 n) { (void)off; (void)buf; (void)n; return 0; }
int sys_utimes(const char *p, u32 t) { (void)p; (void)t; return 0; }
int tui_zenix_tfd = -1;                 /* (the text-UI library's, which Wait reads: not linked here) */
unsigned long tui_zenix_tsig;

static int fails;
static void check(const char *what, int ok)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) fails = 1;
}
static void put(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    fputs(text, f);
    fclose(f);
}
static int has(const char *path, const char *text)
{
    char b[64] = "";
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t n = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    b[n] = 0;
    return !strcmp(b, text);
}

int main(int argc, char **argv)
{
    char p[512], q[512];
    if (argc < 2) return 2;
    snprintf(top, sizeof top, "%s", argv[1]);

    /* B3-1: Rename */
    put("a", "A"); put("b", "B");
    check("Rename onto another existing file is refused (ERROR_OBJECT_EXISTS), both files kept",
          !Rename((STRPTR)"a", (STRPTR)"b") && IoErr() == ERROR_OBJECT_EXISTS && has("a", "A") && has("b", "B"));
    check("Rename to a new name works", Rename((STRPTR)"a", (STRPTR)"c") && has("c", "A") && access("a", F_OK) < 0);
    link("c", "c2");
    check("Rename of a file onto another name of the same file is not refused", Rename((STRPTR)"c", (STRPTR)"c2") != 0);

    /* B3-3: MODE_OLDFILE writes in place, and still opens a read-only file */
    put("img", "0123456789");
    BPTR fh = Open((STRPTR)"img", MODE_OLDFILE);
    check("MODE_OLDFILE opens for writing in place", fh && Seek(fh, 4, OFFSET_BEGINNING) >= 0 && Write(fh, "xy", 2) == 2);
    if (fh) Close(fh);
    check("the in-place write landed", has("img", "0123xy6789"));
    chmod("img", 0444);
    fh = Open((STRPTR)"img", MODE_OLDFILE);
    char b4[4] = "";
    check("a read-only file still opens with MODE_OLDFILE (read only)", geteuid() == 0 ||
          (fh && Read(fh, b4, 3) == 3 && !memcmp(b4, "012", 3)));
    if (fh) Close(fh);

    /* B3-4: SameLock across mounts */
    snprintf(p, sizeof p, "%s/m2/f", top); put(p, "x");
    BPTR l1 = Lock((STRPTR)"c2", SHARED_LOCK), l2 = Lock((STRPTR)p, SHARED_LOCK), l3 = Lock((STRPTR)"img", SHARED_LOCK);
    check("SameLock: two files on different mounts are LOCK_DIFFERENT", l1 && l2 && SameLock(l1, l2) == LOCK_DIFFERENT);
    check("SameLock: two files on one mount are LOCK_SAME_VOLUME", l1 && l3 && SameLock(l1, l3) == LOCK_SAME_VOLUME);
    if (l1) UnLock(l1);
    if (l2) UnLock(l2);
    if (l3) UnLock(l3);

    /* B3-8: ExNext lists a name whose path is too long */
    mkdir("d", 0755);
    getcwd(q, sizeof q);
    snprintf(p, sizeof p, "d/%.*s", 60, "nnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnn");
    put(p, "x");
    BPTR dl = Lock((STRPTR)"d", SHARED_LOCK);
    struct FileInfoBlock fib;
    int seen = 0;
    if (dl && Examine(dl, &fib))
        while (ExNext(dl, &fib))
            if (strlen(fib.fib_FileName) == 60 && (fib.fib_Protection & FIBF_READ)) seen = 1;
    check("ExNext lists a name whose path is too long (an empty file nobody may read)", seen);
    if (dl) UnLock(dl);

    /* B3-9 (the plain case): DeleteFile takes an empty folder */
    mkdir("e", 0755);
    check("DeleteFile removes an empty folder", DeleteFile((STRPTR)"e") && access("e", F_OK) < 0);

    /* B3-2: SystemTags: a name with $(...) or ` runs nothing */
    snprintf(p, sizeof p, "echo \"x$(touch hit1)`touch hit2`$HOME'q\" > out.txt");
    SystemTags((STRPTR)p, TAG_DONE);
    check("SystemTags: a quoted name holding $(...), `...`, $VAR and ' is passed as it is",
          access("hit1", F_OK) < 0 && access("hit2", F_OK) < 0 && has("out.txt", "x$(touch hit1)`touch hit2`$HOME'q\n"));

    /* B3-7: T: is the user's own folder; a planted one is refused */
    fh = Open((STRPTR)"T:Sumi-1.save", MODE_NEWFILE);
    struct stat st;
    snprintf(p, sizeof p, "%s/tmp/T-%u", top, (unsigned)getuid());
    check("T: is /tmp/T-UID, the user's own folder with mode 700", fh && lstat(p, &st) == 0 && S_ISDIR(st.st_mode) &&
          (st.st_mode & 0777) == 0700);
    if (fh) Close(fh);
    snprintf(q, sizeof q, "%s/T-gone", top);
    rename(p, q);
    symlink(q, p);
    fh = Open((STRPTR)"T:Sumi-2.save", MODE_NEWFILE);
    check("T: refused when /tmp/T-UID is a soft link someone planted", !fh);
    if (fh) Close(fh);

    printf(fails ? "dos-host-test: FAILED\n" : "dos-host-test: all passed\n");
    return fails;
}
