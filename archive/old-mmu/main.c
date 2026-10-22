#include "kernel.h"
#include "hw.h"

extern char __ro_end[], __bss_end[];

static u32 vector_table[256] __attribute__((aligned(4)));

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

/* any exception that isn't the syscall gate or an interrupt ends up
 * here; register block protocol as in start.S */
u32 *exc_c(u32 *rb)
{
    u8 *frame = (u8 *)rb + 60;
    u16 sr = *(u16 *)frame;
    u32 pc = *(u32 *)(frame + 2);
    u16 fv = *(u16 *)(frame + 6);
    u16 vec = (u16)((fv & 0xFFF) >> 2);
    u16 fmt = (u16)(fv >> 12);
    u32 faddr = (fmt == 0xA || fmt == 0xB) ? *(u32 *)(frame + 16) : 0;

    /* FPU presence probe: F-line with the probe flag set means no FPU;
     * record that and skip the 4-byte FNOP. */
    if (fpu_probe_active && vec == 11) {
        fpu_absent = 1;
        *(u32 *)(frame + 2) = pc + 4;
        return rb;
    }

    if (!(sr & 0x2000) && current && current->pid != 0) {
        /* user-mode fault: post SIGSEGV. Default action kills (exit
         * 139); a handler gets the whole fault frame preserved in its
         * sigframe so sigreturn can rerun the access. */
        if (current->sighandler[SIGSEGV] == SIG_DFL)
            kprintf("\n[kernel] pid %d fault: vector %u pc=%p addr=%p\n",
                    current->pid, vec, pc, faddr);
        sig_post(current, SIGSEGV);
        return sig_deliver(rb);
    }
    kprintf("\n*** exception in supervisor mode: vector %u sr=%x pc=%p",
            vec, sr, pc);
    if (fmt == 0xA || fmt == 0xB)
        kprintf(" fault-addr=%p", faddr);
    panic("unhandled exception");
}

static void vectors_init(void)
{
    for (int i = 0; i < 256; i++)
        vector_table[i] = (u32)exc_common;
    vector_table[26] = (u32)irq2_entry;     /* level 2: CIA-A tick */
    vector_table[29] = (u32)irq5_entry;     /* level 5: serial RBF */
    vector_table[32] = (u32)trap0_entry;    /* TRAP #0 */
    set_vbr(vector_table);
}

static void timer_init(void)
{
    CIAA(CIA_CRA) = 0x00;
    CIAA(CIA_ICR) = 0x7F;
    (void)CIAA(CIA_ICR);
    CIAA(CIA_TALO) = CIA_TICKS_100HZ & 0xFF;
    CIAA(CIA_TAHI) = CIA_TICKS_100HZ >> 8;
    CIAA(CIA_ICR) = 0x89;               /* enable timer A + keyboard SP */
    INTREQ = INTF_PORTS | INTF_RBF;
    INTENA = INTF_SETCLR | INTF_INTEN | INTF_PORTS | INTF_RBF;
    CIAA(CIA_CRA) = 0x11;
}

/* BogoMIPS-style speed probe: time a cached busy loop against CIA-A
 * timer B, which counts E-clock pulses (PAL 709379 Hz) in real time no
 * matter how the CPU is clocked (or throttled by an emulator). The
 * loop costs ~7 CPU clocks per iteration on a 68030 from I-cache. */
static void speed_probe(void)
{
    u32 loops = 60000;
    CIAA(CIA_CRB) = 0x00;
    CIAA(CIA_TBLO) = 0xFF;
    CIAA(CIA_TBHI) = 0xFF;
    CIAA(CIA_CRB) = 0x11;               /* force-load $FFFF, start */
    __asm__ volatile("0: subq.l #1,%0\n\tbne 0b" : "+d"(loops) :: "cc");
    CIAA(CIA_CRB) = 0x00;
    u32 e = 0xFFFFu - (((u32)CIAA(CIA_TBHI) << 8) | CIAA(CIA_TBLO));
    if (e == 0 || e > 0xF000)           /* wrapped: CPU below ~5 MHz */
        return;
    u32 khz = 60u * 7u * 709379u / e;
    kprintf("cpu: ~%u.%u MHz effective\n", khz / 1000, (khz % 1000) / 100);
}

static void kmalloc_smoketest(void)
{
    void *a = kmalloc(100), *b = kmalloc(2000), *c = kmalloc(48);
    if (!a || !b || !c || a == b || b == c)
        panic("kmalloc smoketest");
    kfree(b);
    void *d = kmalloc(1000);    /* should reuse b's block */
    kfree(a); kfree(c); kfree(d);
}

void kmain(struct bootinfo *bi)
{
    serial_init();
    kprintf("\nZenix 0.1.5 - Zenith kernel (68030+68882)\n");
    kprintf("kernel: %u+%uK at pa %p, va %p, ro up to %p\n",
            bi->kernel_size / 1024,
            ((u32)__bss_end - KVA_BASE - bi->kernel_size) / 1024,
            bi->kernel_pa, KVA_BASE, (u32)__ro_end);

    vectors_init();

    if (!fpu_probe())
        panic("no FPU found - Zenix requires a 68881/68882");
    kprintf("fpu: present\n");

    struct bootregion *fast = 0, *chip = 0;
    kprintf("attnflags=%x, %u memory regions:\n", bi->attn, bi->nregions);
    for (int i = 0; i < bi->nregions; i++) {
        struct bootregion *r = &bi->r[i];
        kprintf("  %p..%p attrs=%x%s\n", r->lower, r->upper, r->attrs,
                (r->attrs & MEMF_FAST) ? " (fast)" :
                (r->attrs & MEMF_CHIP) ? " (chip)" : "");
        if ((r->attrs & MEMF_FAST) &&
            (!fast || r->upper - r->lower > fast->upper - fast->lower))
            fast = r;
        if (r->attrs & MEMF_CHIP)
            chip = r;
    }

    u32 kernel_span = (u32)__bss_end - KVA_BASE;
    if (fast) {
        pmm_init(fast->lower, fast->upper);
    } else if (chip) {
        kprintf("no fast RAM - falling back to chip (slow!)\n");
        pmm_init(chip->lower, chip->upper);
    } else {
        panic("no usable memory region");
    }
    pmm_reserve(bi->kernel_pa, kernel_span);

    /* final supervisor map: chip+IO cache-inhibited, RAM cacheable,
     * ROM write-protected, kernel 4K-paged with RO text */
    mmu_init(bi->kernel_pa);
    mmu_map_super(0x000000, 0x200000, PD_CI);       /* chip: DMA arena */
    for (int i = 0; i < bi->nregions; i++) {
        struct bootregion *r = &bi->r[i];
        mmu_map_super(r->lower, r->upper,
                      (r->attrs & MEMF_CHIP) ? PD_CI : 0);
    }
    mmu_map_super(0xBC0000, 0xC00000, PD_CI);       /* CIAs */
    mmu_map_super(0xD80000, 0xE00000, PD_CI);       /* Gayle/IDE + custom */
    mmu_map_super(0xF00000, 0x1000000, PD_WP);      /* ROM */
    mmu_map_kernel();
    mmu_load_final();
    kprintf("mmu: final tables - kernel text RO, chip/IO cache-inhibited\n");

    cache_enable();
    kprintf("cache: I+D on (burst, write-allocate)\n");

    con_init(bi->font);
    speed_probe();      /* after con_init so it shows on screen */


    kmalloc_smoketest();

    if (ide_init() || fat_mount())
        panic("no boot disk");
    fat_ls();

    /* boot counter: ZENITH.LOG holds a decimal number */
    static char fbuf[64];
    u32 boots = 0;
    int n = fat_read_file("ZENITH.LOG", fbuf, sizeof fbuf - 1);
    for (int i = 0; i < n; i++)
        if (fbuf[i] >= '0' && fbuf[i] <= '9')
            boots = boots * 10 + (u32)(fbuf[i] - '0');
    boots++;
    int len = 0;
    u32 v = boots;
    do {
        fbuf[len++] = '0' + v % 10;
        v /= 10;
    } while (v);
    for (int i = 0; i < len / 2; i++) {
        char t = fbuf[i];
        fbuf[i] = fbuf[len - 1 - i];
        fbuf[len - 1 - i] = t;
    }
    fbuf[len++] = '\n';
    if (fat_write_file("ZENITH.LOG", fbuf, (u32)len) == 0)
        kprintf("zenith.log: boot #%u on this disk\n", boots);

    /* keyboard layout: KEYMAP.CFG holds e.g. "sv" */
    n = fat_read_file("KEYMAP.CFG", fbuf, 3);
    if (n >= 2) {
        fbuf[2] = 0;
        if (kbd_set_keymap(fbuf) == 0)
            kprintf("kbd: keymap %s\n", fbuf);
    }

    proc_init();
    if (proc_spawn_path("INIT.ELF") < 0)
        panic("cannot load INIT.ELF");
    kprintf("proc: %u pages free\n\n", pmm_free_pages());

    timer_init();
    ints_on();
    for (;;) {
        serial_tx_drain();      /* hot-drain the TX backlog when idle */
        if (!serial_tx_pending())
            cpu_idle();
    }
}
