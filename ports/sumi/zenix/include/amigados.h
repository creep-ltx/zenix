/* SPDX-License-Identifier: GPL-2.0-only */
/* The AmigaOS names Sumi's upstream source (ports/sumi/upstream, the owner's editor) uses, for its Zenix build: the
 * types, the structures it reads, and the calls, over the C library (DESIGN D27 section 9's table). The calls are
 * ports/sumi/zenix/dos.c; the few that are one line are here. What each one does on Zenix, in short:
 *  - a file handle (BPTR from Open) is the descriptor + 1, so 0 stays "failed" as AmigaOS has it; a lock (Lock,
 *    CreateDir) is a small block holding the path and the file's device and inode (SameLock compares those);
 *  - three of upstream's own names are mapped (nothing else): "ENVARC:Sumi" is $HOME/.config/Sumi (the settings, the
 *    keys and where each file was left), "ENV:Sumi" is no file at all (AmigaOS's RAM copy of ENVARC:; one place on
 *    Zenix, so the second copy is never written and never read), "T:Sumi-" is /tmp/Sumi- (an unnamed file kept when
 *    the terminal goes away);
 *  - a file's protection (fib_Protection) carries its Unix mode in bits 8-19 with bit 31 set, and FIBF_WRITE when this
 *    user may not write it (upstream then says "read-only" and refuses to save over it); SetProtection gives the mode
 *    back (chmod) and, for root, the owner Examine saw (a save is a new file renamed over the old one);
 *  - DateStamp is the modification time (whole seconds; CompareDates as AmigaOS's: < 0 when the first is later);
 *  - IoErr is the last call's errno (ENOENT as ERROR_OBJECT_NOT_FOUND), Fault its text (strerror);
 *  - SystemTags runs the command with the shell (system()), in the terminal: upstream's "run" command;
 *  - the clipboard device does not open (no shared clipboard on Zenix yet): upstream's clip.c then keeps what was
 *    copied inside Sumi, shared by its tabs; DoubleClick only matters with a mouse, which the console has not;
 *  - FindTask(NULL) answers a task whose stack is large enough that upstream's main() never swaps stacks: the stack
 *    is the program's own (STACK_sumi in the Makefile, D27 9 rule 5). */
#ifndef ZENIX_SUMI_AMIGADOS_H
#define ZENIX_SUMI_AMIGADOS_H
#include <stdlib.h>
#include <string.h>

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
struct StackSwapStruct { APTR stk_Lower; ULONG stk_Upper; APTR stk_Pointer; };
struct Task *FindTask(CONST_STRPTR name);
void StackSwap(struct StackSwapStruct *s);
struct MsgPort { int unused; };
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
struct FileInfoBlock {
    LONG fib_DiskKey;
    LONG fib_DirEntryType;              /* > 0 a directory, < 0 a file */
    char fib_FileName[108];
    LONG fib_Protection;                /* (mode << 8) | bit 31, FIBF_WRITE when not writable (above) */
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

/* intuition */
BOOL DoubleClick(ULONG s1, ULONG m1, ULONG s2, ULONG m2);
#endif
