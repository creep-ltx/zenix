/* SPDX-License-Identifier: GPL-2.0-only */
/* Sumi on Zenix (roadmap block 3; DESIGN D27 section 9): the AmigaOS calls the owner's editor makes
 * (ports/sumi/upstream, compiled unchanged), over the C library. What each one means here is said in
 * include/amigados.h; the reasons and the choices in ports/sumi/README. */
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <amigados.h>

static LONG lasterr;                    /* IoErr: the last failure's errno (0 none) */

static int fail(void)
{
    lasterr = errno ? errno : EIO;
    return 0;
}

/* ---- names: three of upstream's own, nothing else (amigados.h) ---- */

#define ZPATH 64                        /* PATH_MAX: the kernel's 63 bytes and the NUL */

/* name as a Zenix path into out; 0 (errno set) when it has none */
static int zpath(const char *name, char *out)
{
    const char *rest = 0, *pre = 0, *home;
    size_t n;
    if (!strncmp(name, "ENVARC:Sumi", 11)) {
        home = getenv("HOME");
        if (!home || !home[0]) {
            errno = ENOENT;
            return 0;
        }
        pre = home;
        rest = name + 7;                /* "Sumi..." after "/.config/" */
        if (strlen(pre) + 9 + strlen(rest) >= ZPATH) {
            errno = ENAMETOOLONG;
            return 0;
        }
        strcpy(out, pre);
        strcat(out, "/.config/");
        strcat(out, rest);
        return 1;
    }
    if (!strncmp(name, "ENV:Sumi", 8)) {
        errno = ENOENT;
        return 0;
    }
    if (!strncmp(name, "T:Sumi-", 7)) {
        pre = "/tmp/";
        rest = name + 2;
    }
    if (!pre) {
        pre = "";
        rest = name[0] ? name : ".";    /* AmigaOS's "" is the current directory */
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

/* ---- files: a handle is the descriptor + 1 ---- */

static struct { int fd, pos, len; char b[256]; } fg = { -1, 0, 0, { 0 } };   /* FGets' buffer, one file at a time */

BPTR Open(CONST_STRPTR name, LONG mode)
{
    char p[ZPATH];
    int fd;
    errno = 0;
    if (!strcmp(name, "*"))             /* the console: the terminal Sumi runs in */
        fd = dup(mode == MODE_NEWFILE ? 1 : 0);
    else if (!zpath(name, p))
        return fail();
    else if (mode == MODE_NEWFILE)
        fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    else
        fd = open(p, O_RDONLY);
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

BPTR Lock(CONST_STRPTR name, LONG mode)
{
    struct zlock *l;
    struct stat st;
    char p[ZPATH];
    (void)mode;
    errno = 0;
    if (!zpath(name, p) || stat(p, &st) < 0)
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
    fib->fib_Size = (LONG)st->st_size;
    fib->fib_Date.ds_Days = (LONG)(st->st_mtime / 86400);
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
        if (strlen(l->path) + 1 + strlen(d->d_name) >= ZPATH)
            continue;                   /* a name no call could open: not offered */
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

LONG SameLock(BPTR a, BPTR b)
{
    struct zlock *x = (struct zlock *)a, *y = (struct zlock *)b;
    if (x->dev != y->dev)
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
    if (unlink(p) < 0 && (errno != EISDIR || rmdir(p) < 0))
        return fail();
    return DOSTRUE;
}

LONG Rename(CONST_STRPTR from, CONST_STRPTR to)
{
    char a[ZPATH], b[ZPATH];
    errno = 0;
    if (!zpath(from, a) || !zpath(to, b) || rename(a, b) < 0)
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
    return type == DOS_FIB ? calloc(1, sizeof(struct FileInfoBlock)) : NULL;
}

void FreeDosObject(ULONG type, APTR p)
{
    (void)type;
    free(p);
}

LONG IoErr(void)
{
    return lasterr == ENOENT ? ERROR_OBJECT_NOT_FOUND : lasterr;
}

LONG Fault(LONG code, CONST_STRPTR header, STRPTR buf, LONG len)
{
    const char *t;
    if (len <= 0)
        return 0;
    if (code == ERROR_OBJECT_NOT_FOUND)
        code = ENOENT;
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

/* upstream's "run": the command through the shell in the terminal; its exit status, -1 if it could not run */
LONG SystemTags(CONST_STRPTR command, ...)
{
    int st = system(command);
    return st < 0 ? -1 : WEXITSTATUS(st);
}

BOOL DoubleClick(ULONG s1, ULONG m1, ULONG s2, ULONG m2)
{
    long ms = (long)(s2 - s1) * 1000 + ((long)m2 - (long)m1) / 1000;
    return ms >= 0 && ms <= 500;
}

/* exec: the task (its stack large enough that upstream's main() never swaps: the stack is the program's own) */
struct Task *FindTask(CONST_STRPTR name)
{
    static struct Task me = { 0, (APTR)0x100000 };
    (void)name;
    return &me;
}

void StackSwap(struct StackSwapStruct *s)
{
    (void)s;
}

/* the clipboard device: none on Zenix (upstream keeps the copied text itself, amigados.h) */
struct MsgPort *CreateMsgPort(void) { return NULL; }
void DeleteMsgPort(struct MsgPort *p) { (void)p; }
struct IORequest *CreateIORequest(struct MsgPort *p, ULONG size) { (void)p; (void)size; return NULL; }
void DeleteIORequest(struct IORequest *r) { (void)r; }
LONG OpenDevice(CONST_STRPTR name, ULONG unit, struct IORequest *r, ULONG flags)
{
    (void)name; (void)unit; (void)r; (void)flags;
    return -1;
}
void CloseDevice(struct IORequest *r) { (void)r; }
LONG DoIO(struct IORequest *r) { (void)r; return -1; }
