/* SPDX-License-Identifier: GPL-2.0-only */
#include "kernel.h"
#include "hw.h"
#include "hwinfo.h"
#include "diag.h"

static u32 vector_table[256] __attribute__((aligned(4)));
volatile u32 ticks;
u32 time_base;                  /* seconds since 1970 at tick 0: set by date -s (an RTC will set it at boot) */

void *memset(void *d, int c, unsigned long n)
{
    u8 *p = d;
    if (n >= 8 && !((u32)p & 3)) {
        u32 v = (u32)(u8)c * 0x01010101u, *pl = (u32 *)p;
        for (unsigned long k = n >> 2; k; k--)
            *pl++ = v;
        p = (u8 *)pl;
        n &= 3;
    }
    while (n--)
        *p++ = (u8)c;
    return d;
}

void *memcpy(void *d, const void *s, unsigned long n)
{
    u8 *p = d;
    const u8 *q = s;
    if (n >= 8 && !(((u32)p | (u32)q) & 3)) {
        u32 *pl = (u32 *)p;
        const u32 *ql = (const u32 *)q;
        for (unsigned long k = n >> 2; k; k--)
            *pl++ = *ql++;
        p = (u8 *)pl;
        q = (const u8 *)ql;
        n &= 3;
    }
    while (n--)
        *p++ = *q++;
    return d;
}

/* ---- boot stage colours (audit A11, kernel/diag.h) ---- */
u16 diag_colour = DIAG_ORANGE;          /* start.S set stage 5 before kmain */

void diag_stage(int n, u16 colour)
{
    if (colour) {
        diag_colour = colour;
        if (!gfx_up())                  /* once the Copper runs, it owns COLOR00 */
            COLOR00 = colour;
    }
#ifdef DIAG_STOP
    if (n == DIAG_STOP) {
        kprintf("diag: stopped at stage %d (DIAG_STOP test build)\n", n);
        serial_flush();
        for (;;)
            ;
    }
#endif
#ifdef DIAG_PANIC
    if (n == DIAG_PANIC) {
        kprintf("diag: test panic at stage %d (DIAG_PANIC test build)\n", n);
        panic("test panic (DIAG_PANIC)");
    }
#endif
    (void)n;
}

/* The start of every kernel stop: interrupts off, the reason kept for the next boot's "previous boot" lines (A12),
 * and the message to the screen too once the display is up (after the boot it only goes to the log). A fault while
 * the panic text is being printed (the terminal itself failing) leaves the screen alone from then on. */
static int panic_printing, panic_noscreen;

static void panic_begin(void)
{
    ints_off();
    klog_set_reason(KLOG_END_PANIC);
    if (panic_printing) {
        klog_mirror_off();
        panic_noscreen = 1;
    } else if (gfx_up() && !panic_noscreen)
        klog_mirror_on();
    panic_printing = 1;
}

void panic(const char *why)
{
    panic_begin();
    kprintf("\n*** kernel panic: %s\n", why);
    kprintf("*** stopped. Photo, then Ctrl-Amiga-Amiga: the next boot shows this again\n");
    panic_printing = 0;
    serial_flush();             /* TX ring won't drain with ints off */
    if (gfx_up() && !panic_noscreen)
        gfx_panic();            /* the background turns dark red, the text stays */
    else
        diag_halt(diag_colour); /* no display: the stage colour over red */
    for (;;)
        ;
}

/* any exception without its own handler; register block protocol as in
 * start.S. M0: report and stop. */
void exc_c(u32 *rb)
{
    u8 *frame = (u8 *)rb + 60;
    u16 sr = *(u16 *)frame;
    u32 pc = *(u32 *)(frame + 2);
    u16 fv = *(u16 *)(frame + 6);
    u16 vec = (u16)((fv & 0xFFF) >> 2);
    u16 fmt = (u16)(fv >> 12);

    u32 faddr = (fmt == 0xA || fmt == 0xB) ? *(u32 *)(frame + 16) : 0;

    if (!(sr & 0x2000) && current->pid != 0) {
        /* user-mode fault: kill just this process, free what it owned */
        kprintf("\n[kernel] pid %d (%s) killed: vector %u pc=%p addr=%p\n",
                current->pid, current->name, vec, pc, faddr);
        proc_exit(139);
    }
    panic_begin();                      /* this text, too, must reach the log and the screen before the stop */
    kprintf("\n*** exception in the kernel: vector %u sr=%x pc=%p", vec, sr, pc);
    if (fmt == 0xA || fmt == 0xB)
        kprintf(" fault-addr=%p", faddr);
    panic_printing = 0;
    panic("unhandled exception");
}

void irq3_c(void)
{
    INTREQ = INTF_VERTB;
    gfx_vblank();
}

void irq5_c(void)
{
    serial_rx_isr();
}

/* Vectors 24-31 without a handler of their own (start.S irq_other_entry): a spurious interrupt (24), a level 1, 4 or
 * 6 request although none of their sources is enabled, or the NMI (31). A Paula level is switched off in INTENA so a
 * stuck source cannot keep interrupting (its INTREQ bits are left alone: the floppy and sound drivers poll them),
 * level 6 also reads CIA-B's ICR; then back to whatever ran, which is not touched. kwork logs each vector once (an
 * interrupt only takes note, audit B4). */
static volatile u8 irq_other_seen;      /* bit v - 24 */

void irq_other_c(u32 fv)
{
    u32 vec = (fv & 0xFFF) >> 2;
    if (vec < 24 || vec > 31)
        return;
    if (vec == 25)
        INTENA = 0x0007;                /* TBE, DSKBLK, SOFT */
    else if (vec == 28)
        INTENA = 0x0780;                /* AUD0-3 */
    else if (vec == 30) {
        INTENA = 0x2000;                /* EXTER: CIA-B and the expansion bus's INT6 */
        (void)CIAB(CIA_ICR);
    }
    irq_other_seen |= (u8)(1u << (vec - 24));
    kwork_pending = 1;
}

static void irq_other_report(void)
{
    static u8 told;
    u8 news = irq_other_seen & (u8)~told;
    if (!news)
        return;
    told |= news;
    for (u32 v = 24; v <= 31; v++)
        if (news & (1u << (v - 24)))
            kprintf("irq: unexpected %s (vector %u), ignored%s\n",
                    v == 24 ? "spurious interrupt" : v == 31 ? "NMI" : "interrupt", v,
                    (v == 25 || v == 28 || v == 30) ? "; its level is switched off" : "");
}

/* ---- deferred input work (audit B4/B5) ----
 * The keyboard and serial interrupts only queue raw codes and bytes (kbd.c, serial.c) and set kwork_pending; the
 * tick sets it for key repeat. kwork() does the real work - keymap, tty line discipline and echo, Ctrl-C/Ctrl-Z,
 * console switches, scrollback - in process context, with interrupts
 * enabled. It is called from four places only, each a point where no kernel code is half-way through anything:
 *   - the idle loop (pid 0, between schedule() calls),
 *   - the end of every system call (syscall_c, just before the return to user mode),
 *   - the level-2 interrupt (tick or keyboard) when it interrupted *user* mode: the kernel is never preempted, so
 *     then no kernel code is running at all; the mask is lowered to 0 first (it is "return to user mode" work),
 *   - between the 256-byte pieces of a long terminal write (tty_write), where the terminal is consistent and no
 *     check-then-block is pending, so a program printing a lot does not overflow the input rings.
 * An interrupt that cuts into kernel code (a system call, or kwork itself) never runs it, and `busy` keeps it from
 * being re-entered, so tty, terminal, heap and blitter state are only ever changed by one piece of code at a time.
 * This also closes the lost wakeup (B5): a reader that finds no line and then blocks (tty_read; init's proc_wait for a
 * console that wants a login) is kernel code with none of the four points between its check and its block, so kwork -
 * the only producer of input and of console requests - cannot run there; input that arrives meanwhile waits in the ring and its wake_io() comes
 * after the reader is P_IO. A new call of kwork() must keep it that way. */
volatile int kwork_pending;

void kwork(void)
{
    static int busy;
    if (busy)
        return;
    busy = 1;
    while (kwork_pending) {
        kwork_pending = 0;              /* cleared before the work: input queued during it asks for another round */
        serial_work();
        kbd_work();
        irq_other_report();
    }
    busy = 0;
}

void irq2_c(u32 sr)
{
    u8 icr = CIAA(CIA_ICR);             /* read acknowledges the CIA */
    INTREQ = INTF_PORTS;
    if (!(icr & 0x1F))
        ide_stray_irq();                /* not the CIA: Gayle's IDE latch (it shares level 2) */
    if (icr & 8)
        kbd_isr();                      /* only queues the code */
    if (icr & 1) {
        ticks++;
        kbd_tick();
        audio_tick();
        input_poll();
        fd_tick();
    }
    if (!(sr & 0x2000)) {               /* we interrupted user mode: no kernel code is running */
        if (kwork_pending) {
            irq_restore(0x2000);        /* interrupts on: this is return-to-user work, not interrupt work */
            kwork();
            if (current->killed || current->stop_req) {
                schedule();             /* Ctrl-C / Ctrl-Z of the running program: it ends or stops now */
                return;
            }
        }
        if (icr & 1)
            sched_tick((u16)sr);
    }
}

/* The E-clock (crystal / 40) of this machine: 709379 Hz on PAL, 715909 on NTSC, from the Agnus/Alice id's NTSC bit
 * (VPOSR bit 12: OCS $10, ECS $30, AGA $32/$33 are NTSC chips). It drives the 100 Hz tick (latch below) and is what
 * hr_time() counts. */
u32 eclock_hz = ECLOCK_PAL;

/* Time in E-clock ticks (eclock_hz: 709379 Hz on PAL, 715909 on NTSC), the CIA's own clock, read from hardware only: CIA-B
 * timer A counts E-clock pulses down from 0xFFFF, timer B counts timer A's underflows (CRB INMODE 10), so the two
 * are one free-running 32-bit counter (wraps after about 1.7 hours). It does not depend on the tick interrupt
 * being taken, so it never reads early (the old form, ticks * 7095 + timer A's position, read about 10 ms low
 * in the moment between timer A wrapping and the tick interrupt running - always the case inside irq2 itself).
 * Timer B only moves when timer A wraps (every 92 ms), so re-reading it after timer A catches that moment. */
u32 hr_time(void)
{
    u32 bhi, blo, ahi, alo, ahi2, bhi2, blo2;
    do {
        bhi = CIAB(CIA_TBHI);
        blo = CIAB(CIA_TBLO);
        ahi = CIAB(CIA_TAHI);
        alo = CIAB(CIA_TALO);
        ahi2 = CIAB(CIA_TAHI);          /* alo belongs to ahi only if the high byte did not move around it */
        bhi2 = CIAB(CIA_TBHI);
        blo2 = CIAB(CIA_TBLO);
    } while (ahi != ahi2 || bhi != bhi2 || blo != blo2);
    return ~(((bhi << 8 | blo) << 16) | (ahi << 8 | alo));
}

static void vectors_init(void)
{
    for (int i = 0; i < 256; i++)
        vector_table[i] = (u32)exc_common;
    for (int i = 24; i <= 31; i++)
        vector_table[i] = (u32)irq_other_entry;     /* spurious, levels 1-7: logged once and returned from */
    vector_table[26] = (u32)irq2_entry;     /* level 2: CIA-A tick */
    vector_table[27] = (u32)irq3_entry;     /* level 3: vertical blank */
    vector_table[29] = (u32)irq5_entry;     /* level 5: serial receive */
    vector_table[32] = (u32)trap0_entry;    /* TRAP #0: system calls */
    set_vbr(vector_table);
}

/* CIA-B timers A and B chained as the free-running E-clock counter behind hr_time(): started early in kmain, because
 * gfx_init times its frame measurement with it */
static void eclock_init(void)
{
    eclock_hz = (VPOSR & 0x1000) ? ECLOCK_NTSC : ECLOCK_PAL;
    CIAB(CIA_CRA) = 0x00;
    CIAB(CIA_CRB) = 0x00;
    CIAB(CIA_ICR) = 0x7F;               /* no CIA-B interrupts */
    (void)CIAB(CIA_ICR);
    CIAB(CIA_TALO) = 0xFF;
    CIAB(CIA_TAHI) = 0xFF;
    CIAB(CIA_TBLO) = 0xFF;
    CIAB(CIA_TBHI) = 0xFF;
    CIAB(CIA_CRB) = 0x51;               /* timer B: count timer A underflows, continuous, force load, start */
    CIAB(CIA_CRA) = 0x11;               /* timer A: E-clock, continuous, force load, start */
}

/* the 100 Hz tick: CIA-A timer A. Its period is latch + 1 E-clock counts: the latch nearest to eclock_hz / 100 */
static void timer_init(void)
{
    u32 latch = (eclock_hz + 50) / 100 - 1;     /* 7093 PAL (99.997 Hz), 7158 NTSC (100.001 Hz) */
    kprintf("clock: E-clock %u Hz (%s), tick latch %u\n", eclock_hz, eclock_hz == ECLOCK_NTSC ? "NTSC" : "PAL", latch);
    CIAA(CIA_CRA) = 0x00;
    CIAA(CIA_ICR) = 0x7F;
    (void)CIAA(CIA_ICR);
    CIAA(CIA_TALO) = (u8)(latch & 0xFF);
    CIAA(CIA_TAHI) = (u8)(latch >> 8);
    CIAA(CIA_ICR) = 0x81;               /* timer A interrupt only */
    INTREQ = INTF_PORTS;
    INTENA = INTF_SETCLR | INTF_INTEN | INTF_PORTS | INTF_RBF;
    CIAA(CIA_CRA) = 0x11;               /* start, continuous */
}

static void mem_selftest(void)
{
    u32 c0, f0, c1, f1;
    mem_stats(&c0, &f0);
    kprintf("mem: %u KB chip, %u KB fast free\n", c0 / 1024, f0 / 1024);
    void *a = mem_alloc(100, 0, 1), *b = mem_alloc(40000, 0, 1);
    void *c = mem_alloc(5000, MEM_CHIP, 2), *d = mem_alloc(300, 0, 1);
    if (!a || !b || !c || !d)
        panic("mem selftest: alloc failed");
    if ((u32)c >= 0x200000 || (f0 && (u32)a < 0x200000))
        panic("mem selftest: wrong pool");
    mem_free(b);
    mem_free(a);
    u32 got = mem_free_owner(1);        /* d only */
    mem_free(c);
    mem_stats(&c1, &f1);
    if (got < 300 || c1 != c0 || f1 != f0)
        panic("mem selftest: memory not fully returned");
    kprintf("mem: self-test ok (alloc, free, merge, owner free)\n");
}

static const char *cpu_name(u16 attn)
{
    if (attn & 0x80)  return "68060";
    if (attn & 0x08)  return "68040";
    if (attn & 0x04)  return "68030";
    if (attn & 0x02)  return "68020";
    return "68000/68010";
}

void kmain(struct bootinfo *bi)
{
    klog_init(bi);                      /* before anything prints: the previous boot's log is read from there */
    diag_stage(6, DIAG_GREEN);
    serial_init();
    kprintf("\nZenix - 68020+, flat, no MMU\n");
    kprintf("cpu: %s (attnflags %x)\n", cpu_name(bi->attn), bi->attn);
    kprintf("kernel: %u bytes at %p\n", bi->kernel_size, bi->kernel_pa);
    for (int i = 0; i < bi->nregions; i++) {
        struct bootregion *r = &bi->r[i];
        kprintf("  ram %p..%p %s\n", r->lower, r->upper,
                (r->attrs & MEMF_FAST) ? "fast" :
                (r->attrs & MEMF_CHIP) ? "chip" : "?");
    }

    const struct hwinfo *hwi = (const struct hwinfo *)HWINFO_ADDR;
    if (hwi->magic != HWINFO_MAGIC)
        kprintf("pi: no hardware table (old bootblock)\n");
    else if (!(hwi->flags & HW_DT))
        kprintf("pi: not running under Emu68 (no device tree)\n");
    else {
        kprintf("pi: Emu68%s%s, peripherals at %p\n", (hwi->flags & HW_BCM2711) ? ", BCM2711 (Pi 4/CM4)" : "",
                (hwi->flags & HW_RANGEOPS) ? ", cache range opcodes" : "", hwi->soc_base);
        kprintf("pi: emmc %p mailbox %p gpio %p genet %p gic %p/%p\n", hwi->emmc, hwi->mailbox, hwi->gpio,
                hwi->genet, hwi->gic_dist, hwi->gic_cpu);
    }

    if (hwi->magic == HWINFO_MAGIC)
        for (u32 i = 0; i < hwi->nboards && i < HW_MAXBOARDS; i++) {
            const struct hwboard *b = &hwi->board[i];
            kprintf("zorro: %u: manufacturer %x product %u at %p, %u KB%s%s\n", i, b->manufacturer, b->product,
                    b->addr, b->size / 1024, (b->type & 0x20) ? ", RAM" : "", (b->type & 0xC0) == 0x80 ? ", Zorro III" : "");
        }

    int font_ok = gfx_font_init(bi->font) == 0;     /* before mem_init: OS leftovers */
    diag_stage(7, DIAG_BROWN);
    vectors_init();
    mem_init(bi, bi->kernel_pa, *(u32 *)(bi->kernel_pa + 12));
    mem_selftest();
    diag_stage(8, DIAG_PINK);
    cache_enable((bi->attn & 0x88) != 0);       /* 68040 or 68060 */
    kprintf("cache: instruction cache on (data cache off: no MMU to keep DMA coherent)\n");
    diag_stage(9, DIAG_WHITE);
    eclock_init();                              /* hr_time() from here on (gfx_init measures the frame with it) */
    if (font_ok && gfx_init() == 0) {
        klog_to_screen();                       /* the boot messages so far, then the rest as they come */
        kprintf("display: 640x%d, 1 bitplane, %dx%d text, ROM Topaz 8\n", trows * 8, COLS, trows);
    } else {
        diag_stage(0, DIAG_GREY);
        kprintf("display: not available (%s), serial only\n",
                font_ok ? "out of chip RAM" : "no ROM font");
    }
    klog_show_prev();                           /* how the previous boot ended (A12), on the screen if it is up */
    diag_stage(DIAG_DISPLAY_UP, 0);

    { u32 t = rtc_read(); if (t) time_base = t - ticks / 100; }
    if (blk_init() && !bi->ram_size)
        panic("no disk and no ramdisk");
    proc_init();
    tty_init();
    /* the filesystem server and the starter are built into the kernel */
    extern const char fsd_blob[], fsd_blob_end[], boot_blob[], boot_blob_end[],
                      diskd_blob[], diskd_blob_end[], zfsd_blob[], zfsd_blob_end[];
    int dpid = proc_spawn_image(diskd_blob, (u32)(diskd_blob_end - diskd_blob), "diskd", 1, 1);
    int fpid = proc_spawn_image(fsd_blob, (u32)(fsd_blob_end - fsd_blob), "fsd", 1, 0);
    int pid = proc_spawn_image(boot_blob, (u32)(boot_blob_end - boot_blob), "boot", 1, 1);
    if (dpid < 0 || fpid < 0 || pid < 0)
        panic("cannot start the filesystem server");
    /* the second root server: it serves the root if the root is ZenixFS and leaves at once otherwise (user/rootpick.h). Started last
     * so that the process numbers above stay as they were. If it cannot be started the ext2 root still works. */
    proc_spawn_image(zfsd_blob, (u32)(zfsd_blob_end - zfsd_blob), "zfsd", 1, 0);
    kprintf("proc: diskd %d, fsd %d, boot %d (it starts /bin/init)\n", dpid, fpid, pid);

    input_init();
    klog_mirror_off();                          /* from here the screen belongs to the programs */
    diag_stage(DIAG_BOOTED, 0);
    timer_init();
    ints_on();
    for (;;) {
        proc_reap_orphans();
        kwork();                /* queued input first: it may wake a reader */
        schedule();             /* run anything ready; returns when idle */
        serial_tx_drain();
        u16 s = irq_save();     /* checked with interrupts masked: STOP enables them and waits in one instruction,
                                 * so input queued after the check still ends the wait at once */
        if (!kwork_pending && !serial_tx_pending())
            cpu_idle();
        irq_restore(s);
    }
}

void *memmove(void *d, const void *s, unsigned long n)
{
    u8 *p = d;
    const u8 *q = s;
    if (p < q) {
        while (n--)
            *p++ = *q++;
    } else {
        p += n;
        q += n;
        while (n--)
            *--p = *--q;
    }
    return d;
}
