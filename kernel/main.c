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

    kprintf("\n*** exception: vector %u sr=%x pc=%p", vec, sr, pc);
    if (fmt == 0xA || fmt == 0xB)
        kprintf(" fault-addr=%p", *(u32 *)(frame + 16));
    panic("unhandled exception");
}

void irq2_c(void)
{
    u8 icr = CIAA(CIA_ICR);             /* read acknowledges the CIA */
    INTREQ = INTF_PORTS;
    if (icr & 1)
        ticks++;
}

static void vectors_init(void)
{
    for (int i = 0; i < 256; i++)
        vector_table[i] = (u32)exc_common;
    vector_table[26] = (u32)irq2_entry;     /* level 2: CIA-A tick */
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
    INTENA = INTF_SETCLR | INTF_INTEN | INTF_PORTS;
    CIAA(CIA_CRA) = 0x11;               /* start, continuous */
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
    kprintf("\nZenix M0 - 68020+, flat, no MMU\n");
    kprintf("cpu: %s (attnflags %x)\n", cpu_name(bi->attn), bi->attn);
    kprintf("kernel: %u bytes at %p\n", bi->kernel_size, bi->kernel_pa);
    for (int i = 0; i < bi->nregions; i++) {
        struct bootregion *r = &bi->r[i];
        kprintf("  ram %p..%p %s\n", r->lower, r->upper,
                (r->attrs & MEMF_FAST) ? "fast" :
                (r->attrs & MEMF_CHIP) ? "chip" : "?");
    }

    vectors_init();
    cache_enable();
    kprintf("cache: instruction cache on\n");

    timer_init();
    ints_on();
    u32 t0 = ticks;
    while (ticks - t0 < 100)
        cpu_idle();
    kprintf("timer: 100 ticks counted, interrupts work\n");
    kprintf("M0 ok\n");
    serial_flush();
    for (;;)
        cpu_idle();
}
