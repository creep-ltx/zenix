/* SPDX-License-Identifier: GPL-2.0-only */
/* The owner's terminal programs on Zenix (roadmap block 3; DESIGN D27 section 9: Sumi, Maki, Markdown): the AmigaOS
 * calls they make (ports/PROG/upstream, compiled unchanged), over the C library. What each one means here is said in
 * include/amigados.h; the reasons and the choices in ports/common/README. Compiled once per program, with the
 * settings names it maps (ZENIX_TT_NAMES, the Makefile's TT_NAMES_PROG). */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <poll.h>
#include <time.h>
#include <amigados.h>
#include <zenix.h>                      /* zrealpath */
#define ZENIX_LIBC                      /* usys.h: the C library's errno and constants */
#include "../../user/usys.h"   /* SYS_meminfo, SYS_statfs, SYS_mountinfo, SYS_utimes: no C library calls */
#include "../tui/upstream/src/version.h" /* the linked text-UI library's version, for OpenLibrary("tui.library") */

static LONG lasterr;                    /* IoErr: the last failure's errno (0 none) */

static int fail(void)
{
    lasterr = errno ? errno : EIO;
    return 0;
}

/* ---- names: the program's own settings names and its T: prefix, nothing else (amigados.h) ---- */

#define ZPATH 256                       /* PATH_MAX: the kernel's 255 bytes and the NUL */

#ifndef ZENIX_TT_NAMES
#error "ZENIX_TT_NAMES: the program's settings names (the Makefile's TT_NAMES_PROG)"
#endif
static const char *const envnames[] = { ZENIX_TT_NAMES, 0 };   /* the first is the program's own */

/* the length of "PREFIX" + one of the program's names at the start of name (followed by nothing, '/' or '.' for
 * "ENVARC:Sumi/..."; '-' for "T:Sumi-1.save"), 0 if none */
static size_t mapped(const char *name, const char *prefix, int only_first, int sep)
{
    size_t p = strlen(prefix);
    if (strncmp(name, prefix, p))
        return 0;
    for (int i = 0; envnames[i] && (!i || !only_first); i++) {
        size_t n = strlen(envnames[i]);
        char c = name[p + n];
        if (!strncmp(name + p, envnames[i], n) && (sep ? c == sep : !c || c == '/' || c == '.'))
            return p + n;
    }
    return 0;
}

/* the AmigaOS places the owner's programs name in their own literals (amigados.h, ports/common/README) */
static const struct { const char *amiga, *zenix; } places[] = {
    { "T:", 0 }, { "RAM:", 0 }, { "SYS:", "/" }, { "NIL:", "/dev/null" }, { 0, 0 }
};

/* T: and RAM: (0 above): a folder of this user's own in /tmp, /tmp/T-UID/, made with mode 700 (audit B3-7: the
 * programs' fixed names there, Sumi-N.save, CFileV..., Ahiru.log, in the shared /tmp could be another user's file or
 * a soft link planted by one). A folder of that name that is not this user's own, or is a link, is refused (EACCES) */
#ifndef ZENIX_TMPDIR
#define ZENIX_TMPDIR "/tmp"
#endif
static const char *usertmp(void)
{
    static char d[32];
    struct stat st;
    snprintf(d, sizeof d, ZENIX_TMPDIR "/T-%u/", (unsigned)getuid());
    d[strlen(d) - 1] = 0;
    mkdir(d, 0700);
    if (lstat(d, &st) < 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 077)) {
        errno = EACCES;
        return 0;
    }
    strcat(d, "/");
    return d;
}

/* $HOME/.config/ + rest into out (the program's settings), 0 (errno set) without a HOME or room */
static int confpath(const char *rest, const char *sub, char *out)
{
    const char *home = getenv("HOME");
    if (!home || !home[0]) {
        errno = ENOENT;
        return 0;
    }
    if (strlen(home) + 9 + (sub ? strlen(sub) + 1 : 0) + strlen(rest) >= ZPATH) {
        errno = ENAMETOOLONG;
        return 0;
    }
    strcpy(out, home);
    strcat(out, "/.config/");
    if (sub) {
        strcat(out, sub);
        strcat(out, "/");
    }
    strcat(out, rest);
    return 1;
}

/* The one place for settings (docs/audit-2026-11-23-one-system.md OS-29, section 3 "Settings"): $HOME/.config/NAME/,
 * lowercase, one folder per program, the file named for what it holds: "ENVARC:Sumi/Sumi.keys" is sumi/keys,
 * "ENVARC:Sumi" the folder sumi, "ENVARC:ctop.keys" ctop/keys, "ENVARC:FileColours" filecolours, "PROGDIR:Ahiru.keys"
 * ahiru/keys (dir: the program's name, for PROGDIR:). Into out; 0 (errno set) without a HOME or room. */
static int newconf(const char *rest, const char *dir, char *out)
{
    char r[ZPATH];
    const char *s = dir ? dir : rest, *t;
    size_t i = 0, k;
    for (k = 0; s[k] && s[k] != '/' && s[k] != '.'; k++)     /* the program's name, lowercase */
        r[i++] = (char)tolower((unsigned char)s[k]);
    t = dir ? rest : rest + k;                                  /* what follows it */
    if (!dir && *t)
        t++;                                                    /* its '/' or '.' */
    if (dir || rest[k] == '/') {                                /* "Sumi/Sumi.keys", PROGDIR's "Ahiru.keys": the ext */
        if (!strncmp(t, s, k) && t[k] == '.')
            t += k + 1;
    }
    if (*t || rest[k])
        r[i++] = '/';
    if (i + strlen(t) >= sizeof r) {
        errno = ENAMETOOLONG;
        return 0;
    }
    strcpy(r + i, t);
    return confpath(r, 0, out);
}
/* a settings file still at its old place (the AmigaOS names under $HOME/.config, before 2026-11-23) is moved to the
 * new one the first time it is asked for, with a line saying so; its old folder goes when it is left empty. Only
 * files are moved (a folder of the old names would keep them). */
static void moveold(const char *old, const char *now)
{
    struct stat st;
    char d[ZPATH], *sl;
    if (!strcmp(old, now) || lstat(now, &st) == 0 || errno != ENOENT || lstat(old, &st) < 0 || !S_ISREG(st.st_mode))
        return;
    strcpy(d, now);
    if ((sl = strrchr(d, '/')) && sl > d) {
        *sl = 0;
        mkdir(d, 0700);                 /* the program's folder: this user's only */
    }
    if (rename(old, now) == 0) {
        const char *w[] = { envnames[0], ": settings moved from ", old, " to ", now, " (one place for every program's)\n" };
        for (int i = 0; i < 6; i++)     /* (no formatter: the programs that print nothing link none) */
            if (write(2, w[i], strlen(w[i])) < 0)
                break;
        strcpy(d, old);
        if ((sl = strrchr(d, '/')) && sl > d) {
            *sl = 0;
            rmdir(d);                   /* the old folder, once it is empty */
        }
    }
}

/* name as a Zenix path into out; 0 (errno set) when it has none */
static int zpath(const char *name, char *out)
{
    const char *rest = 0, *pre = 0;
    size_t n;
    int i;
    char old[ZPATH];
    if (mapped(name, "ENVARC:", 0, 0)) {
        if (!newconf(name + 7, 0, out))
            return 0;
        if (confpath(name + 7, 0, old))         /* the old place: "Sumi/Sumi.keys" after "/.config/" */
            moveold(old, out);
        return 1;
    }
    if (mapped(name, "ENV:", 0, 0)) {
        errno = ENOENT;
        return 0;
    }
    if (mapped(name, "PROGDIR:", 0, 0)) {
        if (!newconf(name + 8, envnames[0], out))
            return 0;
        if (confpath(name + 8, envnames[0], old))   /* the old place: "Ahiru.keys" after "/.config/Ahiru/" */
            moveold(old, out);
        return 1;
    }
    for (i = 0; places[i].amiga && !pre; i++)
        if (!strncmp(name, places[i].amiga, strlen(places[i].amiga))) {
            pre = places[i].zenix;
            if (!pre && !(pre = usertmp()))
                return 0;
            rest = name + strlen(places[i].amiga);
            if (pre[1] && pre[strlen(pre) - 1] == '/' && *rest == '/')
                rest++;                 /* "T:/x" as "T:x" */
        }
    if (!pre) {
        pre = "";
        rest = name[0] ? name : ".";    /* AmigaOS's "" is the current directory */
        n = strlen(name);
        if (name[0] == '/' && n > 1 && name[n - 1] == ':' && strchr(name, ':') == name + n - 1) {
            if (n > ZPATH) {            /* "/mnt/nas:", a mount named as a volume (ctop): the path */
                errno = ENAMETOOLONG;
                return 0;
            }
            memcpy(out, name, n - 1);
            out[n - 1] = 0;
            return 1;
        }
    }
    n = strlen(pre) + strlen(rest);
    if (n >= ZPATH) {
        errno = ENAMETOOLONG;
        return 0;
    }
    strcpy(out, pre);
    strcat(out, rest);
    return 1;
}

/* a file written into the program's settings ("ENVARC:NAME": $HOME/.config made first; "PROGDIR:": $HOME/.config and the
 * program's folder) */
static void mkconf(const char *name)
{
    char p[ZPATH], *slash;
    if ((!mapped(name, "ENVARC:", 0, 0) && !mapped(name, "PROGDIR:", 0, 0)) || !confpath("", 0, p))
        return;
    p[strlen(p) - 1] = 0;
    mkdir(p, 0755);                     /* $HOME/.config, as AmigaOS's ENVARC: is always there */
    if (!zpath(name, p) || !(slash = strrchr(p, '/')))
        return;
    *slash = 0;
    if (strrchr(p, '/') && strcmp(strrchr(p, '/'), "/.config"))
        mkdir(p, 0700);                 /* the program's own folder (ctop/, ahiru/): this user's only */
}

/* ---- files: a handle is the descriptor + 1 ---- */

static struct { int fd, pos, len; char b[256]; } fg = { -1, 0, 0, { 0 } };   /* FGets' buffer, one file at a time */

BPTR Open(CONST_STRPTR name, LONG mode)
{
    char p[ZPATH];
    int fd;
    errno = 0;
    if (!strcmp(name, "*")) {           /* the console: the terminal the program runs in, even when its input is a pipe */
        fd = open("/dev/tty", mode == MODE_NEWFILE ? O_WRONLY : O_RDONLY);
        if (fd < 0)
            fd = dup(mode == MODE_NEWFILE ? 1 : 0);
    }
    else if (!zpath(name, p))
        return fail();
    else if (mode == MODE_NEWFILE) {
        mkconf(name);
        fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    } else if (mode == MODE_READWRITE) {     /* (AmigaOS's: the file kept, made when missing) */
        mkconf(name);
        fd = open(p, O_RDWR | O_CREAT, 0666);
    } else {                            /* MODE_OLDFILE: AmigaOS's is read and write (Ahiru writes disk images in place,
                                         * audit B3-3); read only when writing is not allowed */
        fd = open(p, O_RDWR);
        if (fd < 0 && errno != ENOENT && errno != ENOTDIR && errno != ENAMETOOLONG)
            fd = open(p, O_RDONLY);
    }
    if (fd < 0)
        return fail();
    return fd + 1;
}

LONG Close(BPTR fh)
{
    if (fh <= 0)
        return DOSTRUE;
    if (fg.fd == fh - 1)
        fg.fd = -1;
    errno = 0;
    return close((int)fh - 1) < 0 ? fail() : DOSTRUE;
}

LONG Read(BPTR fh, APTR b, LONG n)
{
    long got = 0;
    errno = 0;
    while (got < n) {                   /* AmigaOS's Read of a file gives all it asks for, short only at the end */
        long r = read((int)fh - 1, (char *)b + got, (size_t)(n - got));
        if (r < 0) {
            fail();
            return -1;
        }
        if (!r)
            break;
        got += r;
    }
    return got;
}

LONG Write(BPTR fh, const void *b, LONG n)
{
    long put = 0;
    errno = 0;
    while (put < n) {
        long r = write((int)fh - 1, (const char *)b + put, (size_t)(n - put));
        if (r <= 0) {
            fail();
            return -1;
        }
        put += r;
    }
    return put;
}

/* the old position, as AmigaOS's Seek (-1 on failure) */
LONG Seek(BPTR fh, LONG pos, LONG how)
{
    off_t old = lseek((int)fh - 1, 0, SEEK_CUR);
    int w = how == OFFSET_BEGINNING ? SEEK_SET : how == OFFSET_END ? SEEK_END : SEEK_CUR;
    if (old < 0 || lseek((int)fh - 1, pos, w) < 0) {
        fail();
        return -1;
    }
    if (fg.fd == fh - 1)
        fg.fd = -1;
    return (LONG)old;
}

/* a line with its new line (at most len - 1 bytes), or NULL at the end */
STRPTR FGets(BPTR fh, STRPTR buf, ULONG len)
{
    ULONG n = 0;
    if (fg.fd != fh - 1) {
        fg.fd = (int)fh - 1;
        fg.pos = fg.len = 0;
    }
    while (n + 1 < len) {
        if (fg.pos == fg.len) {
            long r = read(fg.fd, fg.b, sizeof fg.b);
            if (r <= 0)
                break;
            fg.pos = 0;
            fg.len = (int)r;
        }
        buf[n] = fg.b[fg.pos++];
        if (buf[n++] == '\n')
            break;
    }
    buf[n] = 0;
    return n ? buf : NULL;
}

LONG PutStr(CONST_STRPTR s)
{
    return write(1, s, strlen(s)) < 0;
}

/* ---- locks: the path, its device and inode, and a directory being listed ---- */

struct zlock {
    char path[ZPATH];
    dev_t dev;
    ino_t ino;
    DIR *dir;
};

/* p made absolute (from the current directory) and plain: no "." or ".." parts, no doubled '/' (the parts as
 * written: a ".." after a soft link goes up from the link's folder, as the shell's cd does); 0 when it will not fit */
static int zabsolute(char *p)
{
    char in[ZPATH], *s, *o = p;
    if (p[0] != '/') {
        if (!getcwd(in, sizeof in) || strlen(in) + 1 + strlen(p) >= ZPATH) {
            errno = ENAMETOOLONG;
            return 0;
        }
        strcat(in, "/");
        strcat(in, p);
    } else
        strcpy(in, p);
    *o = 0;
    for (s = in; *s; ) {
        char *e;
        size_t n;
        while (*s == '/') s++;
        for (e = s; *e && *e != '/'; e++)
            ;
        n = (size_t)(e - s);
        if (n == 1 && s[0] == '.')
            ;
        else if (n == 2 && s[0] == '.' && s[1] == '.') {
            char *up = strrchr(p, '/');
            if (up) *up = 0;
        } else if (n) {
            size_t l = strlen(p);
            p[l] = '/';
            memcpy(p + l + 1, s, n);
            p[l + 1 + n] = 0;
        }
        s = e;
    }
    if (!p[0])
        strcpy(p, "/");
    return 1;
}

BPTR Lock(CONST_STRPTR name, LONG mode)
{
    struct zlock *l;
    struct stat st;
    char p[ZPATH];
    (void)mode;
    errno = 0;
    if (!zpath(name, p) || !zabsolute(p) || stat(p, &st) < 0)
        return fail();
    if (!(l = calloc(1, sizeof *l))) {
        errno = ENOMEM;
        return fail();
    }
    strcpy(l->path, p);
    l->dev = st.st_dev;
    l->ino = st.st_ino;
    return (BPTR)l;
}

void UnLock(BPTR lock)
{
    struct zlock *l = (struct zlock *)lock;
    if (!l)
        return;
    if (l->dir)
        closedir(l->dir);
    free(l);
}

/* the owners Examine saw, for SetProtection to give back when root saves (a save makes a new file) */
#define NOWNERS 8
static struct { char path[ZPATH]; uid_t uid; gid_t gid; } owners[NOWNERS];
static int nextowner;

static void fill(struct FileInfoBlock *fib, const char *path, const char *name, const struct stat *st)
{
    int dir = S_ISDIR(st->st_mode);
    memset(fib, 0, sizeof *fib);
    fib->fib_DirEntryType = fib->fib_EntryType = dir ? 2 : -3;
    strncpy(fib->fib_FileName, name, sizeof fib->fib_FileName - 1);
    fib->fib_Protection = (LONG)(0x80000000UL | (unsigned long)(st->st_mode & 07777) << 8);
    if (access(path, W_OK) < 0)
        fib->fib_Protection |= FIBF_WRITE;
    if (access(path, R_OK) < 0)
        fib->fib_Protection |= FIBF_READ;
    if (!dir && !(st->st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)))
        fib->fib_Protection |= FIBF_EXECUTE;    /* AmigaOS's e bit: a program (Maki's colours) */
    fib->fib_Size = (LONG)st->st_size;
    fib->fib_Date.ds_Days = (LONG)(st->st_mtime / 86400) - AMIGA_EPOCH_DAYS;
    fib->fib_Date.ds_Minute = (LONG)(st->st_mtime % 86400 / 60);
    fib->fib_Date.ds_Tick = (LONG)(st->st_mtime % 60 * 50);
    fib->fib_OwnerUID = (UWORD)st->st_uid;
    fib->fib_OwnerGID = (UWORD)st->st_gid;
}

LONG Examine(BPTR lock, struct FileInfoBlock *fib)
{
    struct zlock *l = (struct zlock *)lock;
    struct stat st;
    const char *name;
    errno = 0;
    if (stat(l->path, &st) < 0)
        return fail();
    name = strrchr(l->path, '/');
    fill(fib, l->path, name && name[1] ? name + 1 : l->path, &st);
    if (S_ISREG(st.st_mode)) {
        int i;
        for (i = 0; i < NOWNERS && strcmp(owners[i].path, l->path); i++)
            ;
        if (i == NOWNERS)
            i = nextowner++ % NOWNERS;
        strcpy(owners[i].path, l->path);
        owners[i].uid = st.st_uid;
        owners[i].gid = st.st_gid;
    }
    if (S_ISDIR(st.st_mode)) {          /* ExNext lists it */
        if (l->dir)
            closedir(l->dir);
        l->dir = opendir(l->path);
    }
    return DOSTRUE;
}

LONG ExNext(BPTR lock, struct FileInfoBlock *fib)
{
    struct zlock *l = (struct zlock *)lock;
    struct dirent *d;
    while (l->dir && (d = readdir(l->dir)) != NULL) {
        char p[ZPATH];
        struct stat st;
        if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, ".."))
            continue;
        if (strlen(l->path) + 1 + strlen(d->d_name) >= ZPATH) {
            memset(&st, 0, sizeof st);  /* a path no call can open: listed (an empty file nobody may read), so a copy
                                         * of its folder fails on it with "too long" instead of leaving it out unsaid
                                         * (audit B3-8) */
            fill(fib, "", d->d_name, &st);
            return DOSTRUE;
        }
        strcpy(p, l->path);
        if (strcmp(p, "/"))
            strcat(p, "/");
        strcat(p, d->d_name);
        if (stat(p, &st) < 0)
            memset(&st, 0, sizeof st);  /* a dangling link: listed as a file */
        fill(fib, p, d->d_name, &st);
        return DOSTRUE;
    }
    lasterr = ERROR_NO_MORE_ENTRIES;
    return DOSFALSE;
}

/* the mount a path is on (1 + its index: the mount whose path is the longest at the start of the path, links
 * resolved), 0 when it cannot be told. Zenix's stat gives no device (audit B3-4): this is SameLock's and Rename's */
static u32 zmount(const char *path)
{
    struct umountinfo mi;
    char r[ZPATH];
    u32 i, best = 0;
    size_t bl = 0;
    if (zrealpath(path, r, sizeof r) < 0)
        return 0;
    for (i = 0; sys_mountinfo(i, &mi) == 0; i++) {
        size_t n = strnlen(mi.path, sizeof mi.path);
        if (n > bl && !strncmp(r, mi.path, n) && (n == 1 || r[n] == '/' || !r[n])) {
            best = i + 1;
            bl = n;
        }
    }
    return best;
}

LONG SameLock(BPTR a, BPTR b)
{
    struct zlock *x = (struct zlock *)a, *y = (struct zlock *)b;
    if (x->dev != y->dev || zmount(x->path) != zmount(y->path))
        return LOCK_DIFFERENT;
    return x->ino == y->ino ? LOCK_SAME : LOCK_SAME_VOLUME;
}

BPTR CreateDir(CONST_STRPTR name)
{
    char p[ZPATH];
    errno = 0;
    if (!zpath(name, p))
        return fail();
    if (!strncmp(name, "ENVARC:", 7)) { /* $HOME/.config first; Sumi's own folder only for this user */
        char *s = strrchr(p, '/');
        *s = 0;
        mkdir(p, 0755);
        *s = '/';
        if (mkdir(p, 0700) < 0 && errno != EEXIST)
            return fail();
    } else if (mkdir(p, 0755) < 0)
        return fail();
    return Lock(name, SHARED_LOCK);
}

LONG DeleteFile(CONST_STRPTR name)
{
    char p[ZPATH];
    errno = 0;
    if (!zpath(name, p))
        return fail();
    if (unlink(p) < 0 && ((errno != EISDIR && errno != EPERM) || rmdir(p) < 0))   /* (EPERM: POSIX's other answer
                                         * for a folder, an NFS server's: B3-9) */
        return fail();
    return DOSTRUE;
}

/* AmigaOS's Rename never replaces: an existing other object at the new name is refused (ERROR_OBJECT_EXISTS), as
 * upstream expects (its saves move the old file aside first; Ahiru's r and :rename ask nothing). Zenix's rename(2)
 * replaces a file silently, which lost the old one (audit B3-1). The same object under another name (a name that
 * differs only in case, a hard link) may be renamed. */
LONG Rename(CONST_STRPTR from, CONST_STRPTR to)
{
    char a[ZPATH], b[ZPATH];
    struct stat sa, sb;
    errno = 0;
    if (!zpath(from, a) || !zpath(to, b) || lstat(a, &sa) < 0)
        return fail();
    if (lstat(b, &sb) == 0 && (sa.st_ino != sb.st_ino || zmount(a) != zmount(b))) {
        errno = EEXIST;
        return fail();
    }
    if (rename(a, b) < 0)
        return fail();
    return DOSTRUE;
}

/* the mode Examine read (bit 31 set: else nothing is changed), and for root the owner it saw */
LONG SetProtection(CONST_STRPTR name, LONG prot)
{
    char p[ZPATH];
    int i;
    errno = 0;
    if (!((ULONG)prot & 0x80000000UL))
        return DOSTRUE;
    if (!zpath(name, p))
        return fail();
    if (geteuid() == 0)
        for (i = 0; i < NOWNERS; i++)
            if (!strcmp(owners[i].path, p)) {
                chown(p, owners[i].uid, owners[i].gid);
                break;
            }
    if (chmod(p, (mode_t)(((ULONG)prot >> 8) & 07777)) < 0)
        return fail();
    return DOSTRUE;
}

LONG SetComment(CONST_STRPTR name, CONST_STRPTR comment)
{
    (void)name;
    (void)comment;
    return DOSTRUE;                     /* Zenix files have no comment (Examine gives none, so this is never asked) */
}

APTR AllocDosObject(ULONG type, const void *tags)
{
    (void)tags;
    return type == DOS_FIB ? calloc(1, sizeof(struct FileInfoBlock)) :
           type == DOS_STDPKT ? calloc(1, sizeof(struct DosPacket)) : NULL;
}

void FreeDosObject(ULONG type, APTR p)
{
    (void)type;
    free(p);
}

/* the errno numbers that have an AmigaOS number upstream tests for (IoErr), and back (Fault) */
static const struct { short amiga, err; } amigaerr[] = {
    { ERROR_OBJECT_NOT_FOUND, ENOENT }, { ERROR_OBJECT_IN_USE, EBUSY }, { ERROR_OBJECT_EXISTS, EEXIST },
    { ERROR_OBJECT_WRONG_TYPE, ENOTDIR }, { ERROR_RENAME_ACROSS_DEVICES, EXDEV },
    { ERROR_DIRECTORY_NOT_EMPTY, ENOTEMPTY }, { ERROR_DISK_FULL, ENOSPC }, { ERROR_WRITE_PROTECTED, EACCES },
    { 0, 0 }
};

LONG IoErr(void)
{
    int i;
    for (i = 0; amigaerr[i].amiga; i++)
        if (amigaerr[i].err == lasterr)
            return amigaerr[i].amiga;
    return lasterr;
}

LONG Fault(LONG code, CONST_STRPTR header, STRPTR buf, LONG len)
{
    const char *t;
    int i;
    if (len <= 0)
        return 0;
    for (i = 0; amigaerr[i].amiga; i++)
        if (amigaerr[i].amiga == code)
            code = amigaerr[i].err;
    t = code == ERROR_NO_MORE_ENTRIES ? "no more entries" : strerror((int)code);
    snprintf(buf, (size_t)len, "%s%s%s", header ? header : "", header ? ": " : "", t);
    return (LONG)strlen(buf);
}

LONG CompareDates(const struct DateStamp *a, const struct DateStamp *b)
{
    if (a->ds_Days != b->ds_Days)
        return a->ds_Days > b->ds_Days ? -1 : 1;
    if (a->ds_Minute != b->ds_Minute)
        return a->ds_Minute > b->ds_Minute ? -1 : 1;
    if (a->ds_Tick != b->ds_Tick)
        return a->ds_Tick > b->ds_Tick ? -1 : 1;
    return 0;
}

/* upstream's "run", Maki's editor and Ahiru's commands: the command through the shell in the terminal, SYS_Input and
 * SYS_Output (handles) as its descriptors 0 and 1 (put back after); its exit status, -1 if it could not run. With
 * SYS_Asynch it runs in the background (sh's '&': not waited for; its input and output /dev/null unless given) and the
 * answer is 0 when it started. Upstream's editors by their AmigaOS names are Zenix's sumi: "Sumi" (Maki's, without
 * $EDITOR) and "C:Ed" (Ahiru's, without $EDITOR: the owner's editor, as on his A1200). */
static const char *const editors[] = { "Sumi ", "C:Ed ", 0 };

/* upstream quotes names AmigaDOS's way ("NAME", the name as it is: upstream escapes no star or quote in it), and
 * AmigaDOS expands nothing inside the quotes; Zenix's sh expands $, ` and \ inside "..." (audit B3-2: a file named x$(cmd) ran cmd when it was
 * edited or opened). Each quoted part becomes a single-quoted one for sh (' as '\''); the rest is left as written (the
 * user's own command text). malloc'd; 0 when out of memory */
static char *amiquote(const char *c)
{
    char *o = malloc(strlen(c) * 4 + 3), *w = o;
    int in = 0;
    if (!o)
        return 0;
    for (; *c; c++) {
        if (!in) {
            if (*c == '"') { in = 1; *w++ = '\''; }
            else *w++ = *c;
            continue;
        }
        char ch = *c;
        if (ch == '"') { in = 0; *w++ = '\''; continue; }
        if (ch == '\'') { memcpy(w, "'\\''", 4); w += 4; }
        else *w++ = ch;
    }
    if (in)
        *w++ = '\'';
    *w = 0;
    return o;
}

LONG SystemTags(CONST_STRPTR command, ...)
{
    va_list ap;
    ULONG tag;
    BPTR h[2] = { 0, 0 };
    int keep[2] = { -1, -1 }, st, i, async = 0;
    char *cmd = NULL;
    va_start(ap, command);
    while ((tag = va_arg(ap, ULONG)) != TAG_DONE) {
        LONG v = va_arg(ap, LONG);
        if (tag == SYS_Input) h[0] = v;
        else if (tag == SYS_Output) h[1] = v;
        else if (tag == SYS_Asynch) async = v != 0;
    }
    va_end(ap);
    for (i = 0; editors[i]; i++)
        if (!strncmp(command, editors[i], strlen(editors[i]))) {
            const char *rest = command + strlen(editors[i]);
            if ((cmd = malloc(strlen(rest) + 6)) != NULL) {
                strcpy(cmd, "sumi ");
                strcat(cmd, rest);
                command = cmd;
            }
            break;
        }
    {
        char *q = amiquote(command);
        if (!q) {
            free(cmd);
            return -1;
        }
        free(cmd);
        command = cmd = q;
    }
    if (async) {                        /* "cmd &": the shell starts it and ends, the command is nobody's child */
        char *bg = malloc(strlen(command) + 40);
        if (!bg) {
            free(cmd);
            return -1;
        }
        strcpy(bg, command);
        strcat(bg, h[0] > 0 || h[1] > 0 ? " &" : " </dev/null >/dev/null 2>&1 &");
        free(cmd);
        command = cmd = bg;
    }
    for (i = 0; i < 2; i++)
        if (h[i] > 0 && h[i] - 1 != i && (keep[i] = dup(i)) >= 0)
            dup2((int)h[i] - 1, i);
    st = system(command);
    for (i = 0; i < 2; i++)
        if (keep[i] >= 0) {
            dup2(keep[i], i);
            close(keep[i]);
        }
    free(cmd);
    if (st < 0) return -1;
    if (async) return WEXITSTATUS(st) ? -1 : 0;
    return WEXITSTATUS(st);
}

/* ---- the standard handles, variables, dates, time ---- */

BPTR Input(void) { return 1; }
BPTR Output(void) { return 2; }
LONG IsInteractive(BPTR fh) { return fh > 0 && isatty((int)fh - 1) ? DOSTRUE : DOSFALSE; }

/* a variable's length (copied, cut to size - 1), -1 when it is not set */
LONG GetVar(CONST_STRPTR name, STRPTR buf, LONG size, ULONG flags)
{
    const char *v = getenv(name);
    size_t n;
    (void)flags;
    if (!v || size <= 0) {
        lasterr = ERROR_OBJECT_NOT_FOUND;
        return -1;
    }
    n = strlen(v);
    if (n > (size_t)size - 1)
        n = (size_t)size - 1;
    memcpy(buf, v, n);
    buf[n] = 0;
    return (LONG)n;
}

void Delay(LONG ticks)
{
    if (ticks > 0)
        usleep((useconds_t)ticks * 20000);
}

/* FORMAT_DOS: dd-mmm-yy and hh:mm:ss, local time */
LONG DateToStr(struct DateTime *dt)
{
    static const char mon[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    time_t t = ((time_t)dt->dat_Stamp.ds_Days + AMIGA_EPOCH_DAYS) * 86400 + dt->dat_Stamp.ds_Minute * 60 +
               dt->dat_Stamp.ds_Tick / 50;
    struct tm tm;
    if (!localtime_r(&t, &tm))
        return DOSFALSE;
    if (dt->dat_StrDate)
        snprintf(dt->dat_StrDate, 10, "%02d-%.3s-%02d", tm.tm_mday, mon + 3 * tm.tm_mon, tm.tm_year % 100);
    if (dt->dat_StrTime)
        snprintf(dt->dat_StrTime, 9, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    if (dt->dat_StrDay)
        dt->dat_StrDay[0] = 0;
    return DOSTRUE;
}

/* ---- handles as AmigaOS's FileHandle, and DOS packets: a read done when the descriptor has data ---- */

#define NFH 16                          /* the kernel's descriptors a process (NFD) */
static struct FileHandle fhs[NFH + 1];  /* [0]: the handle 0 (NIL:, fh_Type 0) */
static struct MsgPort dosport;          /* what a real handle's fh_Type points to (never waited on) */

void *zenix_baddr(BPTR b)
{
    if (b > 0x1000)
        return (void *)b;               /* an address (MKBADDR's, ctop's volume names): above any handle */
    if (b <= 0 || b > NFH)
        return &fhs[0];
    fhs[b].fh_Type = &dosport;
    fhs[b].fh_Arg1 = b - 1;
    return &fhs[b];
}

/* a packet out on its reply port; only ACTION_READ is done (its descriptor in dp_Arg1, as fh_Arg1 gives it) */
void SendPkt(struct DosPacket *pkt, struct MsgPort *handler, struct MsgPort *reply)
{
    (void)handler;
    pkt->dp_Port = reply;
    pkt->dp_Res1 = -1;
    pkt->dp_Res2 = EBADF;
    reply->pending = pkt;
    reply->fd = pkt->dp_Type == ACTION_READ ? (int)pkt->dp_Arg1 : -1;
}

LONG DoPkt(struct MsgPort *handler, LONG action, LONG a1, LONG a2, LONG a3, LONG a4, LONG a5)
{
    (void)handler; (void)action; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    lasterr = EINVAL;
    return DOSFALSE;                    /* no handler answers packets on Zenix (CTerm's disk type: not CTerm) */
}

/* the packet back once its read is done: the descriptor has data or its end (now, never waiting) */
struct Message *GetMsg(struct MsgPort *p)
{
    struct DosPacket *k = p ? p->pending : NULL;
    if (!k)
        return NULL;
    if (p->fd >= 0) {
        struct pollfd f;
        f.fd = p->fd;
        f.events = POLLIN;
        f.revents = 0;
        if (poll(&f, 1, 0) <= 0)
            return NULL;
        k->dp_Res1 = (LONG)read(p->fd, (void *)k->dp_Arg2, (size_t)k->dp_Arg3);
        k->dp_Res2 = k->dp_Res1 < 0 ? errno : 0;
    }
    p->pending = NULL;
    return (struct Message *)k;
}

/* the reply ports by signal bit (16-31, as AmigaOS gives a program's own), for the text-UI library's wait */
static struct MsgPort *ports[32];

/* the descriptor a wait for signal bit `bit` waits on, -1 none (ports/tui/zenix/core.c, TuiWait's extra signals) */
int tui_zenix_sigfd(int bit)
{
    struct MsgPort *p = bit >= 0 && bit < 32 ? ports[bit] : NULL;
    return p && p->pending && p->fd >= 0 ? p->fd : -1;
}

struct MsgPort *CreateMsgPort(void)
{
    struct MsgPort *p;
    int b;
    for (b = 16; b < 32 && ports[b]; b++)
        ;
    if (b == 32 || !(p = calloc(1, sizeof *p)))
        return NULL;
    p->mp_SigBit = (UBYTE)b;
    p->fd = -1;
    ports[b] = p;
    return p;
}

void DeleteMsgPort(struct MsgPort *p)
{
    if (!p)
        return;
    ports[p->mp_SigBit] = NULL;
    free(p);
}

ULONG SetSignal(ULONG on, ULONG mask)
{
    (void)on;
    (void)mask;
    return 0;                           /* no Ctrl-C signal: in raw mode ^C is a key (the text-UI library's) */
}

struct Library *(*zenix_openlib)(CONST_STRPTR name, ULONG version);
struct Library *OpenLibrary(CONST_STRPTR name, ULONG version)
{
    static struct Library tui = { TUI_VERSION, TUI_REVISION };
    (void)version;
    if (name && !strcmp(name, "tui.library"))
        return &tui;                    /* the text-UI library: linked into the program (ports/tui) */
    if (zenix_openlib)
        return zenix_openlib(name, version);    /* a program's own layer (ctop's bsdsocket.library) */
    return NULL;                        /* no AmigaOS libraries on Zenix */
}

void CloseLibrary(struct Library *lib)
{
    (void)lib;
}

BOOL DoubleClick(ULONG s1, ULONG m1, ULONG s2, ULONG m2)
{
    long ms = (long)(s2 - s1) * 1000 + ((long)m2 - (long)m1) / 1000;
    return ms >= 0 && ms <= 500;
}

/* exec: the task (its stack large enough that upstream's main() never swaps: the stack is the program's own), a
 * process for the fields upstream sets (pr_WindowPtr: no requesters on Zenix anyway) */
static struct Process me = { { 0, (APTR)0x100000 }, 0, 0 };
static struct ExecBase execbase = { &me.pr_Task, 0,
    { (struct Node *)&execbase.TaskReady.lh_Tail, 0, (struct Node *)&execbase.TaskReady.lh_Head },
    { (struct Node *)&execbase.TaskWait.lh_Tail, 0, (struct Node *)&execbase.TaskWait.lh_Head } };
struct ExecBase *SysBase = &execbase;

/* the first line of this boot's kernel log that starts with pre, into out (n bytes); 0 when the log no longer
 * holds it (the CPU and the memory at boot: what the kernel tells nowhere else) */
static int klogline(const char *pre, char *out, u32 n)
{
    char chunk[256], line[120];
    u32 ln = 0, k = (u32)strlen(pre), off = (u32)sys_klog(0x40000000u, chunk, 1);   /* where the log held starts */
    int got;
    while ((got = sys_klog(off, chunk, sizeof chunk)) > 0) {
        off += (u32)got;
        for (int i = 0; i < got; i++) {
            if (chunk[i] != '\n') {
                if (ln + 1 < sizeof line) line[ln++] = chunk[i];
                continue;
            }
            line[ln] = 0;
            if (ln >= k && !strncmp(line, pre, k)) {
                strncpy(out, line + k, n - 1);
                out[n - 1] = 0;
                return 1;
            }
            ln = 0;
        }
    }
    return 0;
}
static u32 hexafter(const char *s, const char *key)
{
    const char *p = strstr(s, key);
    return p ? (u32)strtoul(p + strlen(key), 0, 16) : 0;
}
/* the CPU's AttnFlags, once */
static void attnflags(void)
{
    static int done;
    char l[100];
    if (!done++ && klogline("cpu: ", l, sizeof l))
        execbase.AttnFlags = (UWORD)hexafter(l, "(attnflags ");
}

struct Task *FindTask(CONST_STRPTR name)
{
    attnflags();
    return name ? NULL : &me.pr_Task;   /* a task by name (Ahiru's archiver's): none on Zenix */
}

void StackSwap(struct StackSwapStruct *s)
{
    (void)s;
}

/* the clipboard device: none on Zenix (upstream keeps the copied text itself, amigados.h) */
struct IORequest *CreateIORequest(struct MsgPort *p, ULONG size) { (void)p; (void)size; return NULL; }
void DeleteIORequest(struct IORequest *r) { (void)r; }
static struct Device timerdev;
LONG OpenDevice(CONST_STRPTR name, ULONG unit, struct IORequest *r, ULONG flags)
{
    (void)unit; (void)flags;
    if (name && r && !strcmp(name, TIMERNAME)) {        /* only ReadEClock (ctop's clock) */
        r->io_Device = &timerdev;
        attnflags();
        return 0;
    }
    return -1;
}
ULONG ReadEClock(struct EClockVal *ev)
{
    ev->ev_hi = 0;
    ev->ev_lo = (ULONG)sys_ticks();     /* the 100 Hz tick since boot (wraps after 497 days) */
    return 100;
}
void CloseDevice(struct IORequest *r) { (void)r; }
LONG DoIO(struct IORequest *r) { (void)r; return -1; }

/* ---- Ahiru and the CFile core (amigados.h, "Ahiru, the owner's file manager"): memory, more of the files and
 * locks, dates, the shell, waiting ---- */

APTR AllocMem(ULONG n, ULONG flags) { return flags & MEMF_CLEAR ? calloc(1, n) : malloc(n); }
void FreeMem(APTR p, ULONG n) { (void)n; free(p); }

/* the free memory of the kind asked (chip, fast, either); "the largest block" is half of it: the kernel says only
 * how much is free, and upstream sizes its buffers from this (never all of it) */
ULONG AvailMem(ULONG flags)
{
    u32 m[4] = { 0, 0, 0, 0 };
    ULONG n;
    if (flags & MEMF_TOTAL) {           /* what the kernel had to give at boot: its log's "mem:" line */
        static long chip = -1, fast;
        char l[80];
        if (chip < 0) {
            chip = fast = 0;
            if (klogline("mem: ", l, sizeof l)) {
                char *f = strstr(l, "KB chip, ");
                chip = strtol(l, 0, 10) * 1024;
                fast = f ? strtol(f + 9, 0, 10) * 1024 : 0;
            }
        }
        return (ULONG)(flags & MEMF_CHIP ? chip : flags & MEMF_FAST ? fast : chip + fast);
    }
    sys_meminfo(m);
    n = flags & MEMF_CHIP ? m[0] : flags & MEMF_FAST ? m[1] : m[0] + m[1];
    return flags & MEMF_LARGEST ? n / 2 : n;
}

LONG FPuts(BPTR fh, CONST_STRPTR s)
{
    LONG n = (LONG)strlen(s);
    return Write(fh, s, n) == n ? 0 : -1;       /* AmigaOS's: 0, or -1 on failure */
}

LONG FPutC(BPTR fh, LONG c)
{
    char b = (char)c;
    return Write(fh, &b, 1) == 1 ? c : -1;
}

/* now, in AmigaOS's days from 1978 */
void DateStamp(struct DateStamp *ds)
{
    time_t t = time(0);
    ds->ds_Days = (LONG)(t / 86400) - AMIGA_EPOCH_DAYS;
    ds->ds_Minute = (LONG)(t % 86400 / 60);
    ds->ds_Tick = (LONG)(t % 60 * 50);
}

LONG SetFileDate(CONST_STRPTR name, const struct DateStamp *ds)
{
    char p[ZPATH];
    long t = ((long)ds->ds_Days + AMIGA_EPOCH_DAYS) * 86400 + ds->ds_Minute * 60 + ds->ds_Tick / 50;
    errno = 0;
    if (!zpath(name, p))
        return fail();
    if (t < 0)
        t = 0;
    if (sys_utimes(p, (u32)t) < 0) {
        lasterr = EPERM;
        return DOSFALSE;
    }
    return DOSTRUE;
}

/* the lock's path with every soft link on the way resolved (at most 8), into buf; FALSE when it will not fit */
LONG NameFromLock(BPTR lock, STRPTR buf, LONG len)
{
    struct zlock *l = (struct zlock *)lock;
    char cur[ZPATH], link[ZPATH], rest[ZPATH];
    int hops = 0;
    size_t at = 1;
    if (!l)
        return DOSFALSE;
    strcpy(cur, l->path);
    while (at < strlen(cur) + 1 && hops <= 8) {
        struct stat st;
        char *e = strchr(cur + at, '/'), save = 0;
        ssize_t n;
        if (e) save = *e, *e = 0;
        if (lstat(cur, &st) == 0 && S_ISLNK(st.st_mode) && (n = readlink(cur, link, sizeof link - 1)) > 0) {
            link[n] = 0;
            strcpy(rest, e ? e + 1 : "");
            if (link[0] != '/') {       /* relative to the link's folder */
                char *s = strrchr(cur, '/');
                s[1] = 0;
                if (strlen(cur) + strlen(link) >= sizeof cur) return DOSFALSE;
                strcat(cur, link);
            } else
                strcpy(cur, link);
            if (rest[0]) {
                if (strlen(cur) + 1 + strlen(rest) >= sizeof cur) return DOSFALSE;
                strcat(cur, "/");
                strcat(cur, rest);
            }
            if (!zabsolute(cur)) return DOSFALSE;
            hops++;
            at = 1;                     /* from the top: the target may hold links of its own */
            continue;
        }
        if (!e) break;
        *e = save;
        at = (size_t)(e - cur) + 1;
    }
    if (hops > 8 || (LONG)strlen(cur) >= len) {
        lasterr = hops > 8 ? ELOOP : ENAMETOOLONG;
        return DOSFALSE;
    }
    strcpy(buf, cur);
    return DOSTRUE;
}

BPTR DupLock(BPTR lock)
{
    struct zlock *l = (struct zlock *)lock, *d;
    if (!l || !(d = malloc(sizeof *d)))
        return 0;
    *d = *l;
    d->dir = NULL;
    return (BPTR)d;
}

/* the folder above the lock's (resolved) path; 0 at "/" */
BPTR ParentDir(BPTR lock)
{
    char p[ZPATH], *s;
    if (!NameFromLock(lock, p, sizeof p) || !strcmp(p, "/"))
        return 0;
    s = strrchr(p, '/');
    if (s == p) s[1] = 0;
    else *s = 0;
    return Lock(p, SHARED_LOCK);
}

/* the process's directory: chdir to the lock's path (0: where the program started); the lock it had before back */
static BPTR curdir;
static char startdir[ZPATH];
BPTR CurrentDir(BPTR lock)
{
    BPTR old = curdir;
    if (!startdir[0] && !getcwd(startdir, sizeof startdir))
        strcpy(startdir, "/");
    if (chdir(lock ? ((struct zlock *)lock)->path : startdir) == 0)
        curdir = lock;
    return old;
}

/* the filesystem the lock is on (the mount whose path is the longest at the start of the lock's): its blocks */
LONG Info(BPTR lock, struct InfoData *id)
{
    struct zlock *l = (struct zlock *)lock;
    struct umountinfo mi;
    struct ustatfs f;
    u32 i, best = 0;
    size_t bl = 0;
    char p[ZPATH];
    if (!l || !NameFromLock(lock, p, sizeof p))
        return DOSFALSE;
    for (i = 0; sys_mountinfo(i, &mi) == 0; i++) {
        size_t n = strnlen(mi.path, sizeof mi.path);
        if (n > bl && !strncmp(p, mi.path, n) && (n == 1 || p[n] == '/' || !p[n])) {
            best = i;
            bl = n;
        }
    }
    if (!bl || sys_statfs_mount(&f, best) < 0) {
        lasterr = EIO;
        return DOSFALSE;
    }
    memset(id, 0, sizeof *id);
    id->id_NumBlocks = (LONG)f.blocks;
    id->id_NumBlocksUsed = (LONG)(f.blocks - f.bfree);
    id->id_BytesPerBlock = (LONG)f.bsize;
    id->id_DiskState = 82;              /* ID_VALIDATED */
    return DOSTRUE;
}

/* a command through the shell with in and out (handles) as its descriptors 0 and 1, waited for; "" is the user's
 * shell, reading its commands from in. TRUE when it ran */
LONG Execute(CONST_STRPTR cmd, BPTR in, BPTR out)
{
    const char *sh = getenv("SHELL");
    return SystemTags(cmd && cmd[0] ? cmd : sh && sh[0] ? sh : "/bin/sh", SYS_Input, in, SYS_Output, out,
                      TAG_DONE) != -1;
}

/* the descriptor behind the text-UI library's signal (TuiSignal's bit: its terminal), set by ports/tui/zenix */
extern ULONG tui_zenix_tsig;
extern int tui_zenix_tfd;
extern volatile int tui_zenix_winch;    /* a SIGWINCH the library has not answered yet */

/* the signals that came of those asked for: the terminal's (TuiSignal's) when it has input, a reply port's when its
 * read has data (tui_zenix_sigfd); waits until one does. Ctrl-C never comes (^C is a key in raw mode). A signal
 * handler that ran ends the wait with nothing; a SIGWINCH with the terminal's signal (the library's TE_RESIZE). */
ULONG Wait(ULONG signals)
{
    struct pollfd f[9];
    ULONG bit[9], got = 0;
    int n = 0, i;
    if ((signals & tui_zenix_tsig) && tui_zenix_tfd >= 0) {
        f[n].fd = tui_zenix_tfd;
        f[n].events = POLLIN;
        f[n].revents = 0;
        bit[n++] = tui_zenix_tsig;
    }
    for (i = 0; i < 32 && n < 9; i++)
        if ((signals >> i & 1) && (1UL << i) != tui_zenix_tsig && tui_zenix_sigfd(i) >= 0) {
            f[n].fd = tui_zenix_sigfd(i);
            f[n].events = POLLIN;
            f[n].revents = 0;
            bit[n++] = 1UL << i;
        }
    if (!n)
        return 0;                       /* nothing that could ever come */
    /* a resize (SIGWINCH), before the wait or ending it: the terminal's signal, so TuiPoll gives TE_RESIZE */
    if ((signals & tui_zenix_tsig) && tui_zenix_winch)
        return tui_zenix_tsig;
    if (poll(f, (nfds_t)n, -1) <= 0)
        return (signals & tui_zenix_tsig) && tui_zenix_winch ? tui_zenix_tsig : 0;
    for (i = 0; i < n; i++)
        if (f[i].revents) got |= bit[i];
    return got;
}
