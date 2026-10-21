#pragma once
#include <stdint.h>
#include <stdarg.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t s32;

/* bootinfo v3, written by the bootblock at 0x1000 */
#define MEMF_CHIP 2u
#define MEMF_FAST 4u
struct bootregion { u32 attrs, lower, upper; };
struct bootinfo {
    u16 attn;                   /* exec AttnFlags: bit1 68020, bit2 68030, ... */
    u16 nregions;
    u32 kernel_pa;
    u32 kernel_size;
    u32 font;                   /* unused for now */
    struct bootregion r[8];
};

/* serial.c */
void serial_init(void);
void serial_putc(char c);
void serial_flush(void);
int serial_tx_pending(void);
void serial_tx_drain(void);
void serial_rx_isr(void);
void serial_puts(const char *s);
void kprintf(const char *fmt, ...);

/* main.c */
void panic(const char *why) __attribute__((noreturn));
void *memset(void *d, int c, unsigned long n);
void *memcpy(void *d, const void *s, unsigned long n);
extern volatile u32 ticks;      /* 100 Hz, CIA-A timer A */

/* start.S */
void set_vbr(void *table);
u16 irq_save(void);
void irq_restore(u16 sr);
void ints_on(void);
void ints_off(void);
void cpu_idle(void);
void cache_enable(void);
void exc_common(void);
void irq2_entry(void);

/* mem.c */
#define MEM_CHIP     1u         /* must be chip RAM (DMA: graphics, audio, disk) */
#define MEM_FASTONLY 2u         /* fast RAM only, fail rather than use chip */
void mem_init(const struct bootinfo *bi, u32 kernel_pa, u32 kernel_mem);
void *mem_alloc(u32 n, u32 flags, u32 owner);
void mem_free(void *p);
u32 mem_free_owner(u32 owner);
void mem_stats(u32 *chip_free, u32 *fast_free);
void *kmalloc(u32 n);
void kfree(void *p);

/* ide.c */
int ide_init(void);
int ide_read(u32 lba, u32 count, void *buf);
int ide_write(u32 lba, u32 count, const void *buf);

/* ZEXE header, see tools/mkzexe.py */
struct zexe {
    u16 bra, bra_disp;
    u32 magic;                  /* 'ZEXE' */
    u32 image_size, mem_size, nreloc, reloc_off, stack_size, reserved;
};
#define ZEXE_MAGIC 0x5a455845u

/* proc.c */
#define NPROC 16
#define SLICE 5                 /* ticks (100 Hz) */
enum pstate { P_FREE = 0, P_READY, P_SLEEP, P_WAIT, P_ZOMBIE };
struct proc {                   /* start.S relies on the first three: */
    u32 *ksp;                   /*   offset 0 */
    u32 usp;                    /*   offset 4 */
    u32 entry;                  /*   offset 8 */
    int state;
    int pid, ppid;
    int exit_code;
    int wait_for;               /* P_WAIT: child pid, -1 = any */
    u32 wake;                   /* P_SLEEP: tick to wake at */
    u32 base, span;             /* the process's own memory (image+bss+stack) */
    char name[16];
};
extern struct proc procs[NPROC];
extern struct proc *current;
void proc_init(void);
int proc_spawn(const char *name, int ppid);
void schedule(void);
void proc_exit(int code) __attribute__((noreturn));
void proc_reap_orphans(void);
void sched_tick(u16 sr);
int user_ok(const struct proc *p, u32 ptr, u32 len);

/* boot disk table (raw, until the filesystem arrives): sector 0 */
struct zdirent { char name[20]; u32 lba, bytes; };
struct zdir { u32 magic, count; struct zdirent e[15]; u8 pad[512 - 8 - 15 * 28]; };
#define ZDIR_MAGIC 0x5a444952u  /* "ZDIR" */
int bootdisk_init(void);
void *bootdisk_load(const char *name, u32 *bytes);   /* kmalloc'd, caller frees */

/* syscalls: TRAP #0, number in d0, args d1-d3, result in d0 */
#define SYS_exit   1
#define SYS_write  2
#define SYS_yield  3
#define SYS_getpid 4
#define SYS_spawn  5
#define SYS_wait   6
#define SYS_sleep  7
void syscall_c(u32 *rb);
void trap0_entry(void);
void switch_to(struct proc *old, struct proc *next);
void user_first_entry(void);
