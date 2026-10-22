#pragma once
#include <stdint.h>
#include <stdarg.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t s32;

#define PAGE_SIZE 4096u
#define KVA_BASE  0x80000000u   /* kernel virtual base (kernel.ld) */

/* user address space layout */
#define UVA_BASE   0x400000u
#define USTACK_TOP 0x480000u
#define USTACK_PAGES 4

/* bootinfo v2, written by the bootblock at 0x1000 */
#define MEMF_CHIP 2u
#define MEMF_FAST 4u
struct bootregion { u32 attrs, lower, upper; };
struct bootinfo {                       /* v3 */
    u16 attn;
    u16 nregions;
    u32 kernel_pa;
    u32 kernel_size;
    u32 font;                   /* GfxBase->DefaultFont (ROM Topaz), or 0 */
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

/* pmm.c */
void pmm_init(u32 lo, u32 hi);
void pmm_reserve(u32 pa, u32 len);
u32 pmm_alloc(void);
void pmm_free(u32 pa);
u32 pmm_free_pages(void);

/* kmalloc.c */
void *kmalloc(u32 n);
void kfree(void *p);

/* mmu030.c — page descriptor status bits (MC68030UM 9.5.1.1) */
#define PD_WP 0x04u
#define PD_CI 0x40u
void mmu_init(u32 kernel_pa);
void mmu_map_super(u32 lo, u32 hi, u32 flags);
void mmu_map_kernel(void);
void mmu_load_final(void);
void mmu_switch_crp(const u32 *crp);   /* pmove CRP + clear logical caches */
void cache_enable(void);
extern u32 crp_empty[2];

/* fpu.c */
struct proc;
int  fpu_probe(void);
void fpu_save(struct proc *p);
void fpu_restore(struct proc *p);
void fpu_reset(void);
extern volatile int fpu_probe_active, fpu_absent;

/* ide.c */
int ide_init(void);
int ide_read(u32 lba, u32 count, void *buf);
int ide_write(u32 lba, u32 count, const void *buf);

/* fat.c */
int fat_mount(void);
void fat_ls(void);
int fat_read_file(const char *name, void *buf, u32 max);
int fat_write_file(const char *name, const void *data, u32 len);
int fat_stat(const char *name, u32 *first, u32 *size);
int fat_read_at(u32 first, u32 size, u32 pos, void *buf, u32 n);
int fat_readdir(u32 index, char name[16], u32 *size);

/* umem.c — user address space helpers (tables/pages are pmm pages,
 * addressed by identity-mapped physical pointers) */
void umem_map(u32 *uroot, u32 va, u32 pa, u32 flags);
u32 uva_to_pa(const u32 *uroot, u32 va);
void umem_free_root(u32 *uroot);       /* free all pages + tables + root */
int copy_from_user(struct proc *p, void *dst, u32 uva, u32 n);
int copy_to_user(struct proc *p, u32 uva, const void *src, u32 n);
int strncpy_from_user(struct proc *p, char *dst, u32 uva, u32 max);

/* elf.c */
int elf_load(u32 *uroot, const u8 *img, u32 len, u32 *entry_out,
             u32 *brk_out);

/* readdir syscall fills this in user memory */
struct udirent {
    char name[16];
    u32 size;
};

/* file descriptors */
#define NFD 8
enum ftype { FD_NONE = 0, FD_CONS, FD_RFILE, FD_WFILE };
struct file {
    int type;
    u32 first, size, pos;       /* FD_RFILE */
    u8 *wbuf;                   /* FD_WFILE: buffered until close */
    u32 wlen;
    char name[13];
};
#define WFILE_MAX 4000

/* signals */
#define NSIG    16
#define SIGINT  2
#define SIGKILL 9
#define SIGSEGV 11
#define SIGTERM 15
#define SIG_DFL 0u
#define SIG_IGN 1u

/* proc.c */
#define NPROC 8
enum pstate { P_FREE = 0, P_READY, P_WAITING, P_ZOMBIE };
struct proc {                   /* asm relies on: */
    u32 *ksp;                   /*   offset 0 */
    u32 usp;                    /*   offset 4 */
    u32 crp[2];                 /*   offset 8, 8-aligned */
    u32 entry;                  /*   offset 16 */
    int state;
    int pid, ppid;
    int exit_code;
    int in_kernel;              /* defer preemption during syscalls */
    u32 sigpending;
    u32 sighandler[NSIG];       /* user fn ptr, SIG_DFL or SIG_IGN */
    u32 brk;                    /* user heap end (grows toward UBRK_MAX) */
    u32 *uroot;
    struct file files[NFD];
    u8 fpframe[224] __attribute__((aligned(4)));
    u8 fpregs[108];
    u32 kstack[1024];
} __attribute__((aligned(8)));
extern struct proc procs[NPROC];
extern struct proc *current;
void proc_init(void);
int proc_spawn_path(const char *name);  /* kernel-side: load ELF from FAT */
void schedule(void);
void console_wake(void);
void proc_exit_signal(int code) __attribute__((noreturn));

/* trap0 register frame: d0-d7,a0-a6 ascending, then sr.w pc.l fmt.w */
struct uregs { u32 d[8]; u32 a[7]; };
#define FRAME_SR(r) (*(u16 *)((u8 *)(r) + 60))
#define FRAME_PC(r) (*(u32 *)((u8 *)(r) + 62))

/* console.c */
void con_init(u32 fontptr);     /* framebuffer up if ROM font validates */
void con_putc(char c);          /* screen (if up) + serial mirror */
void con_input_push(u8 c);      /* from serial RX / keyboard ISRs */
int con_getc_buffered(void);

/* kbd.c */
void kbd_isr(void);
int kbd_set_keymap(const char *name);
const char *kbd_keymap_name(void);

/* signal.c */
extern int console_fg;          /* pid that Ctrl-C signals, 0 = none */
void sig_post(struct proc *p, int sig);     /* set pending + wake */
int sig_deliverable(struct proc *p);        /* pending non-ignored? */
u32 *sig_deliver(u32 *r);       /* at return-to-user; may move reg block */
long sys_signal(long sig, u32 handler);
long sys_kill(long pid, long sig);
u32 *sys_sigreturn(u32 *r);
void sig_on_exec(struct proc *p);

/* syscalls (TRAP #0: num d0, args d1-d3, result d0) */
#define SYS_exit   1
#define SYS_putc   2
#define SYS_yield  3
#define SYS_getpid 4
#define SYS_fork   5
#define SYS_exec   6
#define SYS_wait   7
#define SYS_open   8
#define SYS_close  9
#define SYS_read    10
#define SYS_write   11
#define SYS_lseek   12
#define SYS_readdir 13
#define SYS_brk     14
#define SYS_signal  15
#define SYS_kill    16
#define SYS_sigreturn 17
#define SYS_setfg   18
#define SYS_setkmap 19

#define UBRK_MAX 0x460000u      /* heap ceiling; gap below the stack */

/* main.c */
void panic(const char *why) __attribute__((noreturn));
void *memset(void *d, int c, unsigned long n);
void *memcpy(void *d, const void *s, unsigned long n);

/* start.S */
void set_vbr(void *table);
u16 irq_save(void);
void irq_restore(u16 sr);
void ints_on(void);
void ints_off(void);
void cpu_idle(void);
void switch_to(struct proc *old, struct proc *next);
void user_first_entry(void);
void fork_child_entry(void);
u32 get_usp(void);
void set_usp(u32 usp);
void exc_common(void);
void trap0_entry(void);
void irq2_entry(void);
void irq5_entry(void);
