/* SPDX-License-Identifier: GPL-2.0-only */
/* The AmigaOS names the owner's terminal programs use (Sumi, Maki, Markdown: ports/PROG/upstream), for their Zenix
 * build: the types, the structures they read, and the calls, over the C library (DESIGN D27 section 9's table). The
 * calls are ports/common/dos.c; the few that are one line are here. What each one does on Zenix, in short:
 *  - a file handle (BPTR from Open, Input, Output) is the descriptor + 1, so 0 stays "failed" as AmigaOS has it
 *    (the text-UI library's layer takes the same handles); "*" is the terminal (/dev/tty); a lock (Lock, CreateDir)
 *    is a small block holding the path and the file's device and inode (SameLock compares those);
 *  - the program's own settings names are mapped (nothing else; the Makefile's TT_NAMES_PROG): "ENVARC:NAME" is
 *    $HOME/.config/NAME (the settings, the keys and where each file was left), "ENV:NAME" is no file at all
 *    (AmigaOS's RAM copy of ENVARC:; one place on Zenix, so the second copy is never written and never read),
 *    "T:PROG-" is /tmp/PROG- (an unnamed file kept when the terminal goes away);
 *  - a file's protection (fib_Protection) carries its Unix mode in bits 8-19 with bit 31 set, and AmigaOS's r, w
 *    and e bits (set when NOT allowed) from what this user may do (upstream then says "read-only" and refuses to
 *    save over a file it may not write); SetProtection gives the mode back (chmod) and, for root, the owner Examine
 *    saw (a save is a new file renamed over the old one); a soft link is shown as what it points to;
 *  - DateStamp is the modification time (whole seconds since 1970; CompareDates as AmigaOS's: < 0 when the first is
 *    later), DateToStr writes it as AmigaOS's FORMAT_DOS (dd-mmm-yy, hh:mm:ss) in local time;
 *  - IoErr is the last call's errno (ENOENT as ERROR_OBJECT_NOT_FOUND), Fault its text (strerror);
 *  - GetVar is getenv; SystemTags runs the command with the shell (system()) in the terminal, its SYS_Input and
 *    SYS_Output as the command's descriptors 0 and 1;
 *  - a DOS packet (SendPkt of ACTION_READ, Maki reading a pipe while it shows it) is a read done when the
 *    descriptor has data: GetMsg on the reply port polls it; the port's signal bit names that descriptor to the
 *    text-UI library's wait (tui_zenix_sigfd), so the wait ends when the pipe has more; other packets fail;
 *  - the clipboard device does not open (no shared clipboard on Zenix yet): upstream's clip.c then keeps what was
 *    copied inside the program; DoubleClick only matters with a mouse, which the console has not; no library opens
 *    (OpenLibrary is 0: utf8.library's composing, openurl.library's browser are not there);
 *  - FindTask(NULL) answers a task whose stack is large enough that upstream's main() never swaps stacks: the stack
 *    is the program's own (STACK_PROG in the Makefile, D27 9 rule 5); SetSignal never has Ctrl-C (in raw mode
 *    ^C is a key). */
#ifndef ZENIX_TT_AMIGADOS_H
#define ZENIX_TT_AMIGADOS_H
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

typedef unsigned long ULONG;
typedef long LONG;
typedef unsigned char UBYTE;
typedef unsigned short UWORD;
typedef short WORD;
typedef short BOOL;
typedef void *APTR;
typedef long BPTR;
typedef char *STRPTR;
typedef const char *CONST_STRPTR;
typedef ULONG Tag;
#define VOID void
#define CONST const
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
#define DOSTRUE (-1L)
#define DOSFALSE 0L

/* exec */
#define MEMF_ANY    0
#define MEMF_PUBLIC 1
#define MEMF_CLEAR  0x10000
static inline APTR AllocVec(ULONG n, ULONG flags) { return flags & MEMF_CLEAR ? calloc(1, n) : malloc(n); }
static inline void FreeVec(APTR p) { free(p); }
static inline void CopyMem(const void *s, void *d, ULONG n) { memmove(d, s, n); }
struct Task { APTR tc_SPLower, tc_SPUpper; };
struct Process { struct Task pr_Task; APTR pr_WindowPtr; BPTR pr_CES; };
struct Library { UWORD lib_Version, lib_Revision; };
struct Library *OpenLibrary(CONST_STRPTR name, ULONG version);
void CloseLibrary(struct Library *lib);
#define SIGBREAKF_CTRL_C (1UL << 12)
ULONG SetSignal(ULONG on, ULONG mask);
struct StackSwapStruct { APTR stk_Lower; ULONG stk_Upper; APTR stk_Pointer; };
struct Task *FindTask(CONST_STRPTR name);
struct ExecBase { struct Task *ThisTask; };
extern struct ExecBase *SysBase;       /* ThisTask is FindTask(NULL)'s (dos.c) */
void StackSwap(struct StackSwapStruct *s);
struct Message { int unused; };
struct Node { struct Node *ln_Succ, *ln_Pred; char *ln_Name; };
struct List { struct Node *lh_Head, *lh_Tail, *lh_TailPred; };
struct DosPacket;
struct MsgPort {                        /* a reply port: one packet out at a time (dos.c) */
    UBYTE mp_Flags, mp_SigBit;
    struct DosPacket *pending;
    int fd;
    struct List mp_MsgList;             /* (Ahiru's window code reads it: never reached, amigagui.h) */
};
#define PA_IGNORE 2
struct Message *GetMsg(struct MsgPort *p);
struct IORequest { UWORD io_Command; UBYTE io_Error; };
struct IOClipReq {
    UWORD io_Command;
    UBYTE io_Error;
    ULONG io_Actual, io_Length;
    STRPTR io_Data;
    ULONG io_Offset;
    LONG io_ClipID;
};
#define CMD_READ   2
#define CMD_WRITE  3
#define CMD_UPDATE 4
struct MsgPort *CreateMsgPort(void);
void DeleteMsgPort(struct MsgPort *p);
struct IORequest *CreateIORequest(struct MsgPort *p, ULONG size);
void DeleteIORequest(struct IORequest *r);
LONG OpenDevice(CONST_STRPTR name, ULONG unit, struct IORequest *r, ULONG flags);
void CloseDevice(struct IORequest *r);
LONG DoIO(struct IORequest *r);

/* dos */
struct DateStamp { LONG ds_Days, ds_Minute, ds_Tick; };
#define AMIGA_EPOCH_DAYS 2922           /* a DateStamp's day 0, 1978-01-01, is day 2922 from 1970 */
struct DateTime {
    struct DateStamp dat_Stamp;
    UBYTE dat_Format, dat_Flags;
    STRPTR dat_StrDay, dat_StrDate, dat_StrTime;
};
#define FORMAT_DOS 0
LONG DateToStr(struct DateTime *dt);
struct DosPacket {
    struct Message *dp_Link;
    struct MsgPort *dp_Port;
    LONG dp_Type, dp_Res1, dp_Res2, dp_Arg1, dp_Arg2, dp_Arg3, dp_Arg4, dp_Arg5, dp_Arg6, dp_Arg7;
};
struct FileHandle {                     /* what BADDR gives for a handle (dos.c): fh_Arg1 the descriptor */
    struct Message *fh_Link;
    struct MsgPort *fh_Type;            /* not 0: a real file (0: NIL:, the handle 0) */
    BPTR fh_Buf;
    LONG fh_Pos, fh_End, fh_Arg1;
};
struct InfoData { LONG id_NumSoftErrors, id_UnitNumber, id_DiskState, id_NumBlocks, id_NumBlocksUsed,
                  id_BytesPerBlock, id_DiskType; BPTR id_VolumeNode; LONG id_InUse; };
#define ACTION_READ      'R'
#define ACTION_DISK_INFO 25
#define DOS_STDPKT 3
void *zenix_baddr(BPTR b);
#define BADDR(b)  zenix_baddr((BPTR)(b))
#define MKBADDR(p) ((BPTR)(p))
void SendPkt(struct DosPacket *pkt, struct MsgPort *handler, struct MsgPort *reply);
LONG DoPkt(struct MsgPort *handler, LONG action, LONG a1, LONG a2, LONG a3, LONG a4, LONG a5);
BPTR Input(void);
BPTR Output(void);
LONG IsInteractive(BPTR fh);
LONG GetVar(CONST_STRPTR name, STRPTR buf, LONG size, ULONG flags);
void Delay(LONG ticks);
#define ST_USERDIR  2
#define ST_SOFTLINK 3
#define ST_LINKDIR  4
#define ST_FILE     (-3)
#define ST_LINKFILE (-4)
struct FileInfoBlock {
    LONG fib_DiskKey;
    LONG fib_DirEntryType;              /* > 0 a directory, < 0 a file */
    char fib_FileName[108];
    LONG fib_Protection;                /* (mode << 8) | bit 31, and r w e set when not allowed (above) */
    LONG fib_EntryType;
    LONG fib_Size;
    LONG fib_NumBlocks;
    struct DateStamp fib_Date;
    char fib_Comment[80];
    UWORD fib_OwnerUID, fib_OwnerGID;
};
#define FIBF_DELETE  (1 << 0)
#define FIBF_EXECUTE (1 << 1)
#define FIBF_WRITE   (1 << 2)
#define FIBF_READ    (1 << 3)
#define FIBF_SCRIPT  (1 << 6)
#define MODE_OLDFILE 1005
#define MODE_NEWFILE 1006
#define SHARED_LOCK  (-2)
#define ACCESS_READ  SHARED_LOCK
#define OFFSET_BEGINNING (-1)
#define OFFSET_CURRENT   0
#define OFFSET_END       1
#define ERROR_OBJECT_NOT_FOUND 205
#define ERROR_NO_MORE_ENTRIES  232
#define DOS_FIB   2
#define LOCK_SAME 0
#define LOCK_SAME_VOLUME 1
#define LOCK_DIFFERENT (-1)
#define TAG_DONE 0
#define TAG_END  0
#define SYS_Input    0x80000020
#define SYS_Output   0x80000021
#define NP_StackSize 0x800003EB
BPTR Open(CONST_STRPTR name, LONG mode);
LONG Close(BPTR fh);
LONG Read(BPTR fh, APTR b, LONG n);
LONG Write(BPTR fh, const void *b, LONG n);
LONG Seek(BPTR fh, LONG pos, LONG how);
STRPTR FGets(BPTR fh, STRPTR buf, ULONG len);
LONG PutStr(CONST_STRPTR s);
BPTR Lock(CONST_STRPTR name, LONG mode);
void UnLock(BPTR lock);
LONG Examine(BPTR lock, struct FileInfoBlock *fib);
LONG ExNext(BPTR lock, struct FileInfoBlock *fib);
LONG SameLock(BPTR a, BPTR b);
BPTR CreateDir(CONST_STRPTR name);
LONG DeleteFile(CONST_STRPTR name);
LONG Rename(CONST_STRPTR from, CONST_STRPTR to);
LONG SetProtection(CONST_STRPTR name, LONG prot);
LONG SetComment(CONST_STRPTR name, CONST_STRPTR comment);
APTR AllocDosObject(ULONG type, const void *tags);
void FreeDosObject(ULONG type, APTR p);
LONG IoErr(void);
LONG Fault(LONG code, CONST_STRPTR header, STRPTR buf, LONG len);
LONG CompareDates(const struct DateStamp *a, const struct DateStamp *b);
LONG SystemTags(CONST_STRPTR command, ...);

/* ---- Ahiru, the owner's file manager, and the CFile core under it (ports/ahiru): what they call beyond the three
 * terminal programs. The same rules as above; besides:
 *  - more names (amigados.h's head and ports/common/README): "PROGDIR:NAME..." of the program's own names is
 *    $HOME/.config/NAME/NAME... (its folder made, mode 700, when a file there is written); "SYS:" is "/"; "T:" and
 *    "RAM:" are /tmp; "NIL:" is /dev/null;
 *  - a lock holds an absolute path (made from the current directory when the name is relative); NameFromLock gives it
 *    with every soft link on the way resolved; ParentDir the folder above ("/" has none); CurrentDir changes the
 *    process's directory (chdir) and gives back the lock it had before (0: where the program started); Info gives the
 *    filesystem's blocks (SYS_statfs of the mount the path is on);
 *  - a DateStamp counts days from 1978 as AmigaOS's does (the core turns them into dates itself): Examine fills it
 *    so, DateStamp gives now, SetFileDate sets a file's time (SYS_utimes, whole seconds);
 *  - IoErr gives AmigaOS's numbers for what the core tests (not found, in use, disk full, protected, exists, not
 *    empty, across devices), else the errno; Fault gives the text of either;
 *  - AllocMem is malloc (MEMF_CLEAR calloc; FreeMem's size unused); AvailMem the free memory (SYS_meminfo: chip,
 *    fast or both; MEMF_LARGEST half of it, the kernel tells no largest block); Forbid/Permit nothing (one process);
 *  - Wait(signals) waits on the descriptors behind them: the text-UI library's terminal (TuiSignal's bit) and the
 *    reply ports' (tui_zenix_sigfd); Ctrl-C never comes (in raw mode ^C is a key); Signal does nothing;
 *  - SystemTags with SYS_Asynch runs the command in the background through the shell (its input and output the
 *    handles given, else /dev/null) and does not wait; Execute runs a command through the shell, "" the user's shell
 *    reading from the input given ($SHELL, else /bin/sh). */
struct MinNode { struct MinNode *mln_Succ, *mln_Pred; };
#define MEMF_CHIP    2
#define MEMF_FAST    4
#define MEMF_LARGEST 0x20000
APTR AllocMem(ULONG n, ULONG flags);
void FreeMem(APTR p, ULONG n);
ULONG AvailMem(ULONG flags);
static inline void Forbid(void) { }
static inline void Permit(void) { }
static inline void Signal(struct Task *t, ULONG sigs) { (void)t; (void)sigs; }
ULONG Wait(ULONG signals);
#define MODE_READWRITE 1004
#define EXCLUSIVE_LOCK (-1)
#define ACCESS_WRITE   EXCLUSIVE_LOCK
#define ERROR_OBJECT_IN_USE        202
#define ERROR_OBJECT_EXISTS        203
#define ERROR_DIR_NOT_FOUND        204
#define ERROR_OBJECT_WRONG_TYPE    212
#define ERROR_RENAME_ACROSS_DEVICES 215
#define ERROR_DIRECTORY_NOT_EMPTY  216
#define ERROR_DISK_FULL            221
#define ERROR_DELETE_PROTECTED     222
#define ERROR_WRITE_PROTECTED      223
#define ERROR_READ_PROTECTED       224
#define FIBF_ARCHIVE (1 << 4)
#define FIBF_PURE    (1 << 5)
#define ST_ROOT      1
#define SYS_Asynch   0x80000022
#define NP_Name      0x800003F4
void DateStamp(struct DateStamp *ds);
LONG SetFileDate(CONST_STRPTR name, const struct DateStamp *ds);
static inline void CurrentTime(ULONG *secs, ULONG *mics)
{
    struct timeval tv;
    gettimeofday(&tv, 0);
    *secs = (ULONG)tv.tv_sec;
    *mics = (ULONG)tv.tv_usec;
}
LONG FPuts(BPTR fh, CONST_STRPTR s);
LONG FPutC(BPTR fh, LONG c);
static inline LONG Flush(BPTR fh) { (void)fh; return DOSTRUE; }   /* handles are not buffered */
BPTR CurrentDir(BPTR lock);
BPTR ParentDir(BPTR lock);
BPTR DupLock(BPTR lock);
LONG NameFromLock(BPTR lock, STRPTR buf, LONG len);
LONG Info(BPTR lock, struct InfoData *id);
LONG Execute(CONST_STRPTR cmd, BPTR in, BPTR out);

/* intuition */
BOOL DoubleClick(ULONG s1, ULONG m1, ULONG s2, ULONG m2);
#endif
