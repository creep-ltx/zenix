#include "kernel.h"

/* 68881/68882 support. This file is compiled with -m68881 (see Makefile)
 * so the assembler accepts FPU instructions; the kernel proper stays
 * FP-free.
 *
 * State frames (MC68881UM 6.4): FSAVE writes a null frame (4 bytes,
 * format word $0000) if the FPU is in the reset state, else an idle/busy
 * frame of 28/60/184/216 bytes depending on chip and state. FRESTORE of
 * a null frame resets the FPU. So context switch cost is 4 bytes for
 * processes that never touched the FPU.
 */

volatile int fpu_probe_active;
volatile int fpu_absent;

/* Probe by executing FNOP with a flag set; if there is no FPU, the
 * coprocessor interface raises an F-line exception (vector 11) and
 * exc_c() sets fpu_absent and skips the instruction. */
int fpu_probe(void)
{
    fpu_absent = 0;
    fpu_probe_active = 1;
    asm volatile("fnop");
    fpu_probe_active = 0;
    return !fpu_absent;
}

void fpu_reset(void)
{
    static const u32 nullframe = 0;
    asm volatile("frestore (%0)" :: "a"(&nullframe));
}

void fpu_save(struct proc *p)
{
    asm volatile("fsave (%0)" :: "a"(p->fpframe) : "memory");
    if (*(u16 *)p->fpframe != 0) {          /* not a null frame */
        asm volatile(
            "fmovem.x %%fp0-%%fp7,(%0)\n\t"
            "fmovem.l %%fpcr/%%fpsr/%%fpiar,(%1)"
            :: "a"(p->fpregs), "a"(p->fpregs + 96) : "memory");
    }
}

void fpu_restore(struct proc *p)
{
    if (*(u16 *)p->fpframe != 0) {
        asm volatile(
            "fmovem.l (%1),%%fpcr/%%fpsr/%%fpiar\n\t"
            "fmovem.x (%0),%%fp0-%%fp7"
            :: "a"(p->fpregs), "a"(p->fpregs + 96) : "memory");
    }
    asm volatile("frestore (%0)" :: "a"(p->fpframe));
}
