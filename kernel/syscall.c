#include "kernel.h"

int proc_wait(int pid);

/* TRAP #0 handler. rb[0..7] = d0-d7, rb[8..14] = a0-a6. d0 = number,
 * d1-d3 = arguments; the result goes back into rb[0]. */
void syscall_c(u32 *rb)
{
    u32 a1 = rb[1], a2 = rb[2];
    s32 r = -1;

    switch (rb[0]) {
    case SYS_exit:
        proc_exit((int)a1);
    case SYS_write:
        if (a1 == 1 || a1 == 2) {
            u32 n = rb[3];
            if (!user_ok(current, a2, n))
                break;                  /* bad pointer: -1, process lives */
            const char *s = (const char *)a2;
            for (u32 i = 0; i < n; i++)
                serial_putc(s[i]);
            r = (s32)n;
        }
        break;
    case SYS_yield:
        schedule();
        r = 0;
        break;
    case SYS_getpid:
        r = current->pid;
        break;
    case SYS_spawn: {
        char name[20];
        int i = 0;
        if (!user_ok(current, a1, 1))
            break;
        while (i < 19 && user_ok(current, a1 + (u32)i, 1) &&
               ((const char *)a1)[i]) {
            name[i] = ((const char *)a1)[i];
            i++;
        }
        name[i] = 0;
        r = proc_spawn(name, current->pid);
        break;
    }
    case SYS_wait:
        r = proc_wait((int)a1);
        break;
    case SYS_sleep:
        current->wake = ticks + a1;
        current->state = P_SLEEP;
        schedule();
        r = 0;
        break;
    }
    rb[0] = (u32)r;
}
