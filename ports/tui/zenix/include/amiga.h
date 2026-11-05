/* SPDX-License-Identifier: GPL-2.0-only */
/* The AmigaOS names the text-UI library's upstream source (ports/tui/upstream, the owner's tui.library) uses, for its
 * Zenix build: the types, and the few calls its drawing half makes, over the C library (DESIGN D27 section 9's table:
 * Write is write, AllocVec malloc, CopyMem memmove, CurrentTime gettimeofday). A BPTR is a file descriptor inside the
 * toolkit; TuiOpen, TuiOpenIO and TuiPossible take handles, the descriptor + 1 (ports/tui/zenix/core.c).
 * The terminal half of the upstream file (packets to the console, signals, SetMode, the size by CSI 0 q) is replaced
 * as a whole by ports/tui/zenix/core.c; the calls only it makes are declared, so the upstream file compiles
 * unchanged, and defined nowhere: if a part the Zenix build keeps ever starts to use one, the link fails and says so.
 * Every header the upstream file includes (exec/..., dos/..., proto/...) is this one. */
#ifndef ZENIX_TUI_AMIGA_H
#define ZENIX_TUI_AMIGA_H
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>

typedef unsigned long ULONG;
typedef long LONG;
typedef unsigned char UBYTE;
typedef unsigned short UWORD;
typedef short WORD;
typedef short BOOL;
typedef void *APTR;
typedef long BPTR;                      /* a file descriptor */
typedef char *STRPTR;
typedef const char *CONST_STRPTR;
#define VOID void
#define CONST const
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

/* exec and dos memory and signals */
#define MEMF_ANY    0
#define MEMF_PUBLIC 1
#define MEMF_CLEAR  0x10000
#define SIGBREAKF_CTRL_C (1UL << 12)
#define SIGBREAKF_CTRL_D (1UL << 13)
#define SIGBREAKF_CTRL_E (1UL << 14)
#define SIGBREAKF_CTRL_F (1UL << 15)

static inline APTR AllocVec(ULONG n, ULONG flags) { return flags & MEMF_CLEAR ? calloc(1, n) : malloc(n); }
static inline void FreeVec(APTR p) { free(p); }
static inline void CopyMem(const void *s, void *d, ULONG n) { memmove(d, s, n); }
/* all of it: a write to a pty that waited for room and was cut by a signal's handler gives back what went (upstream
 * never writes the rest: a screen cut inside an escape sequence) */
static inline LONG Write(BPTR fd, const void *b, LONG n)
{
    LONG d = 0;
    while (d < n) {
        ssize_t k = write((int)fd, (const char *)b + d, (size_t)(n - d));
        if (k <= 0) {
            if (k < 0 && errno == EINTR) continue;
            return d ? d : (LONG)k;
        }
        d += (LONG)k;
    }
    return d;
}
static inline void CurrentTime(ULONG *secs, ULONG *mics)
{
    struct timeval tv;
    gettimeofday(&tv, 0);
    *secs = (ULONG)tv.tv_sec;
    *mics = (ULONG)tv.tv_usec;
}

/* only the upstream terminal half uses these (replaced on Zenix): declared, never defined */
struct Library { UWORD lib_Version, lib_Revision; };
struct MsgPort { UBYTE mp_SigBit; };
struct Message { int unused; };
struct DosPacket { LONG dp_Type, dp_Res1, dp_Res2, dp_Arg1; };
struct FileHandle { struct MsgPort *fh_Type; };
struct InfoData { LONG id_DiskType; };
#define BADDR(b)  ((void *)(b))
#define MKBADDR(p) ((BPTR)(p))
#define ACTION_WAIT_CHAR 20
#define ACTION_DISK_INFO 25
#define DOS_STDPKT 3
LONG Read(BPTR fh, APTR b, LONG n);
LONG WaitForChar(BPTR fh, LONG us);
LONG SetMode(BPTR fh, LONG mode);
LONG IsInteractive(BPTR fh);
/* the descriptor behind signal bit `bit` for TuiWait's extra signals, -1 none: the program's AmigaOS layer gives it
 * (ports/common/dos.c); ports/tui/zenix/sigfd.c is the library's own (none), linked only when nothing else has it */
int tui_zenix_sigfd(int bit);
BPTR Input(void);
BPTR Output(void);
void SendPkt(struct DosPacket *dp, struct MsgPort *port, struct MsgPort *reply);
LONG DoPkt(struct MsgPort *port, LONG action, LONG a1, LONG a2, LONG a3, LONG a4, LONG a5);
struct Message *GetMsg(struct MsgPort *port);
struct Message *WaitPort(struct MsgPort *port);
ULONG Wait(ULONG sigs);
ULONG SetSignal(ULONG on, ULONG mask);
struct MsgPort *CreateMsgPort(void);
void DeleteMsgPort(struct MsgPort *port);
APTR AllocDosObject(ULONG type, const void *tags);
void FreeDosObject(ULONG type, APTR p);
#ifdef ZENIX_TUI_UTF8
/* the widths switch (ports/tui/README; off until the console's UTF-8, DESIGN D28): the upstream file's own way to
 * the widths of utf8.library (its TUI_LIB build) leads to the C library's wcwidth (include/proto/utf8.h) */
static inline void Forbid(void) { }
static inline void Permit(void) { }
static inline struct Library *OpenLibrary(CONST_STRPTR name, ULONG version)
{
    static struct Library utf8;
    (void)version;
    return strcmp(name, "utf8.library") ? 0 : &utf8;
}
static inline void CloseLibrary(struct Library *lib) { (void)lib; }
#else
void Forbid(void);
void Permit(void);
struct Library *OpenLibrary(CONST_STRPTR name, ULONG version);
void CloseLibrary(struct Library *lib);
#endif
#endif
