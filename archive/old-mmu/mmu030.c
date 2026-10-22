#include "kernel.h"

/* 68030 MMU — final translation tables (the boot stub's early tables get
 * us into virtual space; these replace them).
 *
 * TC: 4K pages, TIA=7/TIB=7/TIC=6, SRE=1 — supervisor translates through
 * SRP, user through per-process CRP, so the kernel simply doesn't exist
 * in user address spaces. TC is already enabled by the boot stub; we only
 * swap SRP here (PMOVE to SRP/CRP flushes the ATC, MC68030UM 9.5.4).
 *
 * Supervisor map:
 *  - identity map of chip RAM/IO/ROM/fast regions via 256KB
 *    early-termination descriptors at the B level, with CI (cache
 *    inhibit) on chip+IO — chip is the DMA arena, IO is IO
 *  - kernel at VA 0x80000000 with 4K pages: text/rodata write-protected
 */

#define DT_INVALID 0u
#define DT_PAGE    1u
#define DT_TABLE   2u

extern char __ro_end[], __bss_end[];

static u32 rootA[128] __attribute__((aligned(16)));
static u32 btabs[8][128] __attribute__((aligned(16)));
static u32 nbtabs;
static u32 kernB[128] __attribute__((aligned(16)));
static u32 kernC[8][64] __attribute__((aligned(16)));
static u32 nkernc;

static u32 uroot_empty[128] __attribute__((aligned(16)));
u32 crp_empty[2] __attribute__((aligned(8)));

/* VA->PA for kernel statics (identity for everything else we touch) */
static u32 kpa_of(const void *kva, u32 kernel_pa)
{
    return (u32)kva - KVA_BASE + kernel_pa;
}

static u32 kernel_pa_saved;

void mmu_init(u32 kernel_pa)
{
    kernel_pa_saved = kernel_pa;
}

void mmu_map_super(u32 lo, u32 hi, u32 flags)
{
    lo &= ~0x3FFFFu;
    hi = (hi + 0x3FFFFu) & ~0x3FFFFu;
    for (u32 addr = lo; addr < hi && addr >= lo; addr += 0x40000u) {
        u32 ai = addr >> 25;
        u32 *bt;
        if ((rootA[ai] & 3) == DT_TABLE) {
            bt = (u32 *)(KVA_BASE + ((rootA[ai] & ~0xFu) - kernel_pa_saved));
        } else {
            if (nbtabs >= 8)
                panic("mmu: B-table pool exhausted");
            bt = btabs[nbtabs++];
            rootA[ai] = kpa_of(bt, kernel_pa_saved) | DT_TABLE;
        }
        bt[(addr >> 18) & 0x7F] = addr | flags | DT_PAGE;
    }
}

void mmu_map_kernel(void)
{
    u32 pa = kernel_pa_saved;
    u32 span = (u32)__bss_end - KVA_BASE;
    u32 ai = KVA_BASE >> 25;                     /* 64 */
    rootA[ai] = kpa_of(kernB, pa) | DT_TABLE;
    for (u32 off = 0; off < span; off += PAGE_SIZE) {
        u32 va = KVA_BASE + off;
        u32 bi = (va >> 18) & 0x7F, ci = (va >> 12) & 0x3F;
        u32 *ct;
        if ((kernB[bi] & 3) != DT_TABLE) {
            if (nkernc >= 8)
                panic("mmu: kernel C-table pool exhausted");
            ct = kernC[nkernc++];
            kernB[bi] = kpa_of(ct, pa) | DT_TABLE;
        } else
            ct = (u32 *)(KVA_BASE + ((kernB[bi] & ~0xFu) - pa));
        u32 flags = (va < (u32)__ro_end) ? PD_WP : 0;
        ct[ci] = (pa + off) | flags | DT_PAGE;
    }
}

static inline void pmove_srp(const u32 *rp)
{
    asm volatile("pmove (%0),%%srp" :: "a"(rp) : "memory");
}
static inline void pmove_crp(const u32 *rp)
{
    asm volatile("pmove (%0),%%crp" :: "a"(rp) : "memory");
}

void mmu_load_final(void)
{
    static u32 srp[2] __attribute__((aligned(8)));

    srp[0] = 0x7FFF0002u;
    srp[1] = kpa_of(rootA, kernel_pa_saved);
    crp_empty[0] = 0x7FFF0002u;
    crp_empty[1] = kpa_of(uroot_empty, kernel_pa_saved);

    pmove_srp(srp);
    pmove_crp(crp_empty);
}

/* Switch the user address space mid-syscall (exec): the logical caches
 * must be cleared just like in switch_to. */
void mmu_switch_crp(const u32 *crp)
{
    pmove_crp(crp);
    cache_enable();             /* 0x3919: clear both + keep enabled */
}

/* CACR (MC68030UM 6.3.1): WA=13 DBE=12 CD=11 FD=9 ED=8 IBE=4 CI=3 EI=0.
 * 0x3919 clears both caches and enables them with burst + write-allocate. */
void cache_enable(void)
{
    asm volatile("movec %0,%%cacr" :: "d"(0x3919) : "memory");
}
