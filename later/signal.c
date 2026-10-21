#include "kernel.h"

/* Minimal POSIX signals.
 *
 * Delivery happens at return-to-user: every kernel entry (syscall,
 * interrupt, exception) saves the full user register block and its C
 * handler returns the — possibly relocated — block pointer that the asm
 * restores from. To run a handler we push a sigframe onto the USER
 * stack containing the register block, the CPU exception frame (bus
 * fault frames included, copied verbatim so sigreturn can resume the
 * faulted access exactly), and a tiny trampoline that issues
 * SYS_sigreturn; then the kernel-stack frame is rewritten as a plain
 * format-0 frame aimed at the handler.
 *
 * A handler returning from SIGSEGV resumes the faulting access (and
 * faults again if nothing changed); longjmp out of the handler simply
 * abandons the frames — both POSIX-consistent. No sigprocmask yet, and
 * a signal arriving while its handler runs will nest. */

int console_fg;

#define FRAME_BYTES(fmt) ((fmt) == 0xA ? 32 : (fmt) == 0xB ? 92 : 8)

struct sigframe {
    u32 ret;                    /* -> tramp below */
    u32 signum;
    u32 regs[15];
    u16 frame[46];              /* original CPU frame (framesize bytes) */
    u16 framesize;
    u16 tramp[3];               /* moveq #SYS_sigreturn,d0; trap #0 */
    u32 old_usp;
};

void sig_post(struct proc *p, int sig)
{
    if (sig <= 0 || sig >= NSIG)
        return;
    p->sigpending |= 1u << sig;
    if (p->state == P_WAITING)
        p->state = P_READY;
}

int sig_deliverable(struct proc *p)
{
    for (int s = 1; s < NSIG; s++)
        if ((p->sigpending & (1u << s)) && p->sighandler[s] != SIG_IGN)
            return 1;
    return 0;
}

void sig_on_exec(struct proc *p)
{
    p->sigpending = 0;
    for (int s = 0; s < NSIG; s++)
        p->sighandler[s] = SIG_DFL;
}

long sys_signal(long sig, u32 handler)
{
    if (sig <= 0 || sig >= NSIG || sig == SIGKILL)
        return -1;
    u32 old = current->sighandler[sig];
    current->sighandler[sig] = handler;
    return (long)old;
}

long sys_kill(long pid, long sig)
{
    for (int i = 0; i < NPROC; i++)
        if (procs[i].state != P_FREE && procs[i].state != P_ZOMBIE &&
            procs[i].pid == pid) {
            sig_post(&procs[i], (int)sig);
            return 0;
        }
    return -1;
}

/* Deliver one pending signal, if any. r points at the 15-longword user
 * register block; the CPU frame follows at r+60. Returns the (possibly
 * moved) block pointer for the restore path. */
u32 *sig_deliver(u32 *r)
{
    u8 *fp = (u8 *)r + 60;
    if (*(u16 *)fp & 0x2000)            /* returning to supervisor: skip */
        return r;
    int sig = 0;
    for (int s = 1; s < NSIG; s++)
        if (current->sigpending & (1u << s)) {
            sig = s;
            break;
        }
    if (!sig)
        return r;
    current->sigpending &= ~(1u << sig);

    u32 handler = current->sighandler[sig];
    if (sig == SIGKILL || handler == SIG_DFL)
        proc_exit_signal(128 + sig);    /* default action: die */
    if (handler == SIG_IGN)
        return r;

    u16 fmt = (u16)(*(u16 *)(fp + 6) >> 12);
    u32 fbytes = FRAME_BYTES(fmt);

    struct sigframe sf;
    memset(&sf, 0, sizeof sf);
    sf.signum = (u32)sig;
    memcpy(sf.regs, r, 60);
    memcpy(sf.frame, fp, fbytes);
    sf.framesize = (u16)fbytes;
    sf.tramp[0] = (u16)(0x7000 | SYS_sigreturn);    /* moveq #n,d0 */
    sf.tramp[1] = 0x4E40;                           /* trap #0 */
    sf.old_usp = get_usp();
    u32 usp = (sf.old_usp - sizeof sf) & ~3u;
    sf.ret = usp + 4 + 4 + 60 + 92 + 2;             /* &tramp in user space */
    if (copy_to_user(current, usp, &sf, sizeof sf)) {
        proc_exit_signal(128 + sig);    /* unwritable stack: die */
    }
    set_usp(usp);
    cache_enable();                     /* tramp is code: clear stale I-cache */

    /* rewrite kernel frame: plain format 0 aimed at the handler */
    u32 *nr = (u32 *)((u8 *)r + fbytes - 8);
    for (int i = 14; i >= 0; i--)
        nr[i] = r[i];
    u8 *nfp = (u8 *)nr + 60;
    *(u16 *)nfp = 0;                    /* SR: user, ints on */
    *(u32 *)(nfp + 2) = handler;
    *(u16 *)(nfp + 6) = 0;              /* format 0 */
    return nr;
}

/* SYS_sigreturn: rebuild the original register block + CPU frame from
 * the user sigframe. Called from the trap0 path (format 0, 8 bytes);
 * the rebuilt frame may be a bigger bus-fault frame, so the block moves
 * down accordingly. */
u32 *sys_sigreturn(u32 *r)
{
    struct sigframe sf;
    u32 base = get_usp() - 4;           /* usp points at signum after rts */
    if (copy_from_user(current, &sf, base, sizeof sf))
        proc_exit_signal(128 + SIGSEGV);
    u32 fbytes = sf.framesize;
    if (fbytes != 8 && fbytes != 32 && fbytes != 92)
        proc_exit_signal(128 + SIGSEGV);
    if (fbytes > 8 && (sf.frame[3] & 0x2000))
        proc_exit_signal(128 + SIGSEGV);        /* forged supervisor SR */
    if (sf.frame[0] & 0x2000)
        proc_exit_signal(128 + SIGSEGV);

    u32 *nr = (u32 *)((u8 *)r + 8 - fbytes);
    memcpy(nr, sf.regs, 60);
    memcpy((u8 *)nr + 60, sf.frame, fbytes);
    set_usp(sf.old_usp);
    return nr;
}
