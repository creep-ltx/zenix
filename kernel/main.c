#include "kernel.h"
#include "hw.h"

static u32 vector_table[256] __attribute__((aligned(4)));
volatile u32 ticks;

void *memset(void *d, int c, unsigned long n)
{
    u8 *p = d;
    while (n--)
        *p++ = (u8)c;
    return d;
}

void *memcpy(void *d, const void *s, unsigned long n)
{
    u8 *p = d;
    const u8 *q = s;
    while (n--)
        *p++ = *q++;
    return d;
}

void panic(const char *why)
{
    ints_off();
    kprintf("\n*** kernel panic: %s\n", why);
    serial_flush();             /* TX ring won't drain with ints off */
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
    kprintf("\n*** exception in the kernel: vector %u sr=%x pc=%p", vec, sr, pc);
    if (fmt == 0xA || fmt == 0xB)
        kprintf(" fault-addr=%p", faddr);
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

void irq2_c(u32 sr)
{
    u8 icr = CIAA(CIA_ICR);             /* read acknowledges the CIA */
    INTREQ = INTF_PORTS;
    if (icr & 8)
        kbd_isr();
    if (icr & 1) {
        ticks++;
        if (ticks % 100 == 0)
            bar_draw();
        sched_tick((u16)sr);
    }
}

static void vectors_init(void)
{
    for (int i = 0; i < 256; i++)
        vector_table[i] = (u32)exc_common;
    vector_table[26] = (u32)irq2_entry;     /* level 2: CIA-A tick */
    vector_table[27] = (u32)irq3_entry;     /* level 3: vertical blank */
    vector_table[29] = (u32)irq5_entry;     /* level 5: serial receive */
    vector_table[32] = (u32)trap0_entry;    /* TRAP #0: system calls */
    set_vbr(vector_table);
}

static void timer_init(void)
{
    CIAA(CIA_CRA) = 0x00;
    CIAA(CIA_ICR) = 0x7F;
    (void)CIAA(CIA_ICR);
    CIAA(CIA_TALO) = CIA_TICKS_100HZ & 0xFF;
    CIAA(CIA_TAHI) = CIA_TICKS_100HZ >> 8;
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

    int font_ok = gfx_font_init(bi->font) == 0;     /* before mem_init: OS leftovers */
    vectors_init();
    mem_init(bi, bi->kernel_pa, *(u32 *)(bi->kernel_pa + 12));
    mem_selftest();
    cache_enable();
    kprintf("cache: instruction cache on\n");
    if (font_ok && gfx_init() == 0)
        kprintf("display: 640x256, 2 bitplanes, Copper split bar + text, ROM Topaz 8\n");
    else
        kprintf("display: not available (%s), serial only\n",
                font_ok ? "out of chip RAM" : "no ROM font");

    if (ext2_mount())
        panic("cannot mount the root filesystem");
    proc_init();
    tty_init();
    char *iargv[] = { "/bin/init", 0 };
    int pid = proc_spawn("/bin/init", iargv, 1, 0, 0, 1);
    if (pid < 0) {
        kprintf("cannot start init (%d)\n", pid);
        panic("no init");
    }
    kprintf("proc: init is pid %d\n", pid);

    timer_init();
    ints_on();
    bar_draw();
    for (;;) {
        proc_reap_orphans();
        for (int w = 1; w <= 9; w++)
            if (ws_spawn_pending[w]) {
                ws_spawn_pending[w] = 0;
                char *a[] = { "/bin/sh", 0 };
                proc_spawn("/bin/sh", a, 1, 0, 0, w);
                bar_draw();
            }
        schedule();             /* run anything ready; returns when idle */
        serial_tx_drain();
        if (!serial_tx_pending())
            cpu_idle();
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
