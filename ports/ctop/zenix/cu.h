/* SPDX-License-Identifier: GPL-2.0-only */
/* placeholder */
#ifndef CU_H
#define CU_H
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
extern const char *cu_name;
extern int cu_rc;
extern int cu_argc;
extern char **cu_argv;
int cu_args(const char *usage);
int cu_opt(const char *spec, char **val);
long cu_num(const char *s, int *ok);
void cu_flush(void);
void cu_err(int rc, const char *what, const char *why);
/* ---- cu_tasks.c: every task, copied out under Disable() (ps, kill) ---- */
typedef struct {
    APTR addr;
    int process;                 /* a process (else a plain exec task) */
    int pri, state;              /* state: 0 running, 1 ready, 2 waiting */
    ULONG stack, sigwait;        /* stack size; the signals it waits for */
    LONG free;                   /* stack headroom in bytes, -1 unknown */
    LONG cli;                    /* CLI number, 0 when none */
    char name[48];
    char cmd[64];                /* the CLI's command in flight, "" if none */
} CuTask;
int cu_tasks(CuTask *v, int max);          /* how many were filled in */
#define CU_MAXTASKS 32

/* ---- cu_cpu.c: how busy the processor is, each task's share (top, ctop) -
 * Open timer.device and set TimerBase first. Exact through exec's task
 * hooks when this program got them (one at a time), else sampled on the
 * vertical blank. */
typedef struct {
    int pct;                     /* tenths of a percent since the last measure */
    int timed;                   /* exact (else pct is sampled, time 0) */
    unsigned long long time;     /* EClock ticks it ran since cu_cpuopen */
} CuShare;
int cu_cpuopen(void);                      /* 0: no memory */
void cu_cpuclose(void);                    /* everything out again: call on every exit */
void cu_cpudrain(void);                    /* the hooks' ring read: twice a second or more */
int cu_cpumeasure(const CuTask *t, int n, CuShare *s);  /* busy tenths; -1 nothing yet */
ULONG cu_cpuclock(void);                   /* vertical blanks counted: the pace */
int cu_cpurate(void);                      /* ... a second (50, 60 NTSC) */
int cu_cpuexact(void);                     /* the hooks are ours */
ULONG cu_cpufreq(void);                    /* EClock ticks a second (for time) */
int cu_emu68(void);                        /* a PiStorm with Emu68 */
ULONG cu_emu68mips(void);                  /* 68k MIPS since the last call, -1 unknown */

/* ---- cu_pi.c: the Pi under a PiStorm (Emu68), where there is one ----- */
typedef struct {
    char emu68[64];              /* "Emu68 1.1.0 (...)", "" unknown */
    char model[64];              /* "Raspberry Pi Compute Module 4 ..." */
    ULONG temp, tempmax;         /* millidegrees C, 0 unknown */
    ULONG armhz, armmax;         /* the ARM clock now and at most, 0 unknown */
    ULONG throttled;             /* the firmware's flags (bit 0 under-voltage, 2 throttled now, 3 too hot) */
    int hasthrottle;
} CuPi;
int cu_pi(CuPi *p);                        /* 0: no Pi to ask */
int cu_isemu68(void);                      /* devicetree.resource with /emu68 */
int cu_signal(APTR task, ULONG sigs);      /* 0: that task is gone */

#endif
