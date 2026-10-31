/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
/* ZINE (DESIGN D29, docs/d29-zine.md): SYS_zine's structs and the kernel-blob interface. Included by the kernel
 * (kernel/zine.c), by /bin/zine (user/zine.c) and by the blob (user/zine/blob*.c). Needs u32/u16/u8 (kernel.h, or
 * the blob's own typedefs).
 *
 * Frozen (docs/frozen-interfaces.md): SYS_zine's ops and struct uzine_info / struct uzine_go below (they only grow
 * at the end, the caller passes its size). NOT frozen: everything after "the kernel-blob interface": the kernel,
 * the blob and /bin/zine are built from one tree and shipped together; the kernel refuses a blob whose layout
 * number is not its own (-ENOEXEC), and zine explains that as an update not yet finished (section 3). */

#define ZINE_INFO 0                     /* a: struct uzine_info *, b: its size -> 0 */
#define ZINE_GO   1                     /* a: struct uzine_go * -> the blob's answer (0: a program ran; see the block) */

struct uzine_info {                     /* what this machine has (frozen; UZINE_INFO_FIRST bytes) */
    u32 size;                           /* the bytes the kernel filled */
    u32 layout;                         /* the kernel-blob layout this kernel accepts (ZINE_LAYOUT) */
    u32 attn;                           /* exec AttnFlags as AmigaOS reported them at boot (bootinfo) */
    u32 chip_total, chip_free;          /* Zenix's chip pools: bytes in them, bytes free */
    u32 fast_total, fast_free;          /* the same for fast RAM (0, 0: a chip-only machine) */
};
#define UZINE_INFO_FIRST 28

struct uzine_go {                       /* ZINE_GO's request (frozen; UZINE_GO_FIRST bytes) */
    u32 size;                           /* sizeof, at least UZINE_GO_FIRST */
    u32 blob, blob_size;                /* the relocated blob, in the caller's own memory (SYS_memalloc) */
    u32 vol, vol_size;                  /* the volume block (user/zine/zvol.h) */
    u32 blk, blk_size;                  /* the ZINE block (struct zine_blk at its start, the rest the blob's) */
    u32 flags;                          /* 0 */
};
#define UZINE_GO_FIRST 32

/* ---- the kernel-blob interface (one build; not frozen) ---- */
#define ZINB_MAGIC  0x5A494E42u         /* "ZINB": the blob's header, at its image offset ZINB_OFF */
#define ZINB_OFF    32u                 /* after the ZEXE header (32 bytes): magic, layout, CRC32 of the file */
#define ZINE_LAYOUT 1u

struct zine_range { u32 lo, hi; int chip; };   /* kernel/mem.c's pool ranges, as they are in its table */

/* given by the kernel to the blob's entry (on the caller's kernel stack: the blob copies what it needs first) */
struct zine_args {
    u32 layout;
    u32 resume;                         /* the kernel's resume routine: jump there in supervisor mode, d0 the answer,
                                         * d1 the session's seconds, with the hardware as 9.2 leaves it */
    u32 vol, vol_size, blk, blk_size;
    const struct zine_range *ranges;    /* Zenix's pools (walked by the blob with interrupts masked) */
    u32 nranges;
    u32 kernel_pa, kernel_mem;          /* the kernel image and its BSS */
    u32 klog_pa, klog_size;             /* the log area at the top of chip RAM (0: none) */
    u32 ram_pa, ram_size;               /* a RAM disk outside the pools (0: none) */
    /* Zenix's records of the registers the way back sets (write-only ones cannot be read back) */
    u32 cop;                            /* the console's Copper list (COP1LC) */
    u16 diwstrt, diwstop, serper, potgo;
    u16 adkcon;                         /* ADKCONR at the hand-over */
    u16 pad;
    u32 eclock_hz;                      /* CIA E-clock rate: the tick latch is (eclock_hz + 50) / 100 - 1 */
    u32 hr_at;                          /* hr_time() at the hand-over: CIA-B's counter is set back to it */
};

/* The ZINE block's start (zine fills the request, the blob the answer; zine reads it after SYS_zine) */
#define ZB_MAGIC   0x5A424C4Bu          /* "ZBLK" */
#define ZB_OUTMAX  16384u               /* the program's output kept (the ring) */
#define ZB_ARGMAX  256u
enum {                                  /* how the program ended (struct zine_blk how) */
    ZH_NONE = 0, ZH_RETURN, ZH_EXIT, ZH_REBOOT, ZH_UNSUP, ZH_CRASH, ZH_ALERT, ZH_STOP, ZH_LOAD
};
enum {                                  /* why it stopped (ZH_STOP) or could not be loaded (ZH_LOAD) */
    ZS_WAIT = 1, ZS_VIEW, ZS_NOMEM, ZS_NOFILE, ZS_BADFILE
};
struct zine_blk {
    u32 magic, size;
    char name[32];                      /* the program's name (the volume's entry) */
    char args[ZB_ARGMAX];               /* its arguments, ending in "\n" (AmigaDOS's form) */
    u32 arglen, stack;                  /* the arguments' length; the stack's bytes */
    /* the answer */
    u32 how, why;                       /* ZH_*, ZS_* */
    s32 code;                           /* the return code (ZH_RETURN, ZH_EXIT) */
    u32 vec, pc, addr;                  /* ZH_CRASH: vector, PC, access address; ZH_ALERT: code in addr */
    u32 lib, lvo;                       /* ZH_UNSUP: the library (0 exec, 1 graphics, 2 dos, 3 intuition), offset */
    u32 secs;                           /* the session's length */
    u32 need;                           /* -ENOMEM before the hand-over: the bytes this block would need */
    u32 chip_free, fast_free, fast_largest;   /* what the program was given */
    u32 outlen, outlost;                /* bytes in out, bytes that did not fit */
    char out[ZB_OUTMAX];
};
