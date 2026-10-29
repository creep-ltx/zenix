/* SPDX-License-Identifier: GPL-2.0-only */
/* The owner's terminal programs on Zenix (roadmap block 3; DESIGN D27 section 9: Sumi, Maki, Markdown): the AmigaOS
 * calls they make (ports/PROG/upstream, compiled unchanged), over the C library. What each one means here is said in
 * include/amigados.h; the reasons and the choices in ports/common/README. Compiled once per program, with the
 * settings names it maps (ZENIX_TT_NAMES, the Makefile's TT_NAMES_PROG). */
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

static LONG lasterr;                    /* IoErr: the last failure's errno (0 none) */

static int fail(void)
{
    lasterr = errno ? errno : EIO;
    return 0;
}

/* ---- names: the program's own settings names and its T: prefix, nothing else (amigados.h) ---- */

#define ZPATH 64                        /* PATH_MAX: the kernel's 63 bytes and the NUL */

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

/* name as a Zenix path into out; 0 (errno set) when it has none */
static int zpath(const char *name, char *out)
{
    const char *rest = 0, *pre = 0, *home;
    size_t n;
    if (mapped(name, "ENVARC:", 0, 0)) {
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
    if (mapped(name, "ENV:", 0, 0)) {
        errno = ENOENT;
        return 0;
    }
    if (mapped(name, "T:", 1, '-')) {
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
    if (!strcmp(name, "*")) {           /* the console: the terminal the program runs in, even when its input is a pipe */
        fd = open("/dev/tty", mode == MODE_NEWFILE ? O_WRONLY : O_RDONLY);
        if (fd < 0)
            fd = dup(mode == MODE_NEWFILE ? 1 : 0);
    }
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
    if (access(path, R_OK) < 0)
        fib->fib_Protection |= FIBF_READ;
    if (!dir && !(st->st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)))
        fib->fib_Protection |= FIBF_EXECUTE;    /* AmigaOS's e bit: a program (Maki's colours) */
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
    return type == DOS_FIB ? calloc(1, sizeof(struct FileInfoBlock)) :
           type == DOS_STDPKT ? calloc(1, sizeof(struct DosPacket)) : NULL;
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

/* upstream's "run" and Maki's editor: the command through the shell in the terminal, SYS_Input and SYS_Output (handles)
 * as its descriptors 0 and 1 (put back after); its exit status, -1 if it could not run. Upstream's default editor
 * "Sumi" is Zenix's sumi. */
LONG SystemTags(CONST_STRPTR command, ...)
{
    va_list ap;
    ULONG tag;
    BPTR h[2] = { 0, 0 };
    int keep[2] = { -1, -1 }, st, i;
    char *cmd = NULL;
    va_start(ap, command);
    while ((tag = va_arg(ap, ULONG)) != TAG_DONE) {
        LONG v = va_arg(ap, LONG);
        if (tag == SYS_Input) h[0] = v;
        else if (tag == SYS_Output) h[1] = v;
    }
    va_end(ap);
    if (!strncmp(command, "Sumi ", 5) && (cmd = strdup(command)) != NULL) {
        cmd[0] = 's';
        command = cmd;
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
    return st < 0 ? -1 : WEXITSTATUS(st);
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
    time_t t = (time_t)dt->dat_Stamp.ds_Days * 86400 + dt->dat_Stamp.ds_Minute * 60 + dt->dat_Stamp.ds_Tick / 50;
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

struct Library *OpenLibrary(CONST_STRPTR name, ULONG version)
{
    (void)name;
    (void)version;
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
struct Task *FindTask(CONST_STRPTR name)
{
    static struct Process me = { { 0, (APTR)0x100000 }, 0, 0 };
    (void)name;
    return &me.pr_Task;
}

void StackSwap(struct StackSwapStruct *s)
{
    (void)s;
}

/* the clipboard device: none on Zenix (upstream keeps the copied text itself, amigados.h) */
struct IORequest *CreateIORequest(struct MsgPort *p, ULONG size) { (void)p; (void)size; return NULL; }
void DeleteIORequest(struct IORequest *r) { (void)r; }
LONG OpenDevice(CONST_STRPTR name, ULONG unit, struct IORequest *r, ULONG flags)
{
    (void)name; (void)unit; (void)r; (void)flags;
    return -1;
}
void CloseDevice(struct IORequest *r) { (void)r; }
LONG DoIO(struct IORequest *r) { (void)r; return -1; }
