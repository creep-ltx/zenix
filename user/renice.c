// SPDX-License-Identifier: GPL-2.0-only
#include "libz.h"
#include "pw.h"
/* renice [-n] N [-p] pid... | renice [-n] N -u user...: set the niceness of running processes to N (as on Linux: the
 * value, not an increment), on the scale -20 (most favoured) .. 19 (DESIGN D10). Anyone may lower the priority of
 * their own processes; only root may raise one (a lower N) or touch another user's. */
static int num(const char *s, int *out)
{
    char *end;
    long v = strtol(s, &end, 10);
    if (!*s || *end) return 0;
    *out = v < -40 ? -40 : v > 40 ? 40 : (int)v;
    return 1;
}

/* a process or user id: the whole number, never clamped as a niceness is (a pid of 46 was taken for 40: FA-46) */
static int idnum(const char *s, int *out)
{
    char *end;
    long v = strtol(s, &end, 10);
    if (!*s || *end || v < 0 || v > 0x7FFFFFFFL) return 0;
    *out = (int)v;
    return 1;
}

static const char USAGE[] = "usage: renice [-n] N [-p] pid... | renice [-n] N -u user...\n";
int main(int argc, char **argv)
{
    zopt(argc, argv, USAGE);
    int i = 1, n, which = PRIO_PROCESS, rc = 0;
    if (i < argc && !strcmp(argv[i], "-n")) i++;
    if (i < argc && argv[i][0] == '-' && !num(argv[i], &n)) zbadopt(argv[i], USAGE);
    if (i >= argc || !num(argv[i], &n)) zusage(USAGE);
    i++;
    if (i >= argc) { dprintf(2, "renice: no process or user given\n"); return 2; }
    n = n < -20 ? -20 : n > 19 ? 19 : n;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "-p")) { which = PRIO_PROCESS; continue; }
        if (!strcmp(argv[i], "-u")) { which = PRIO_USER; continue; }
        int id;
        struct pw pw;
        if (which == PRIO_USER) {
            if (!idnum(argv[i], &id)) {
                if (pw_byname(argv[i], &pw) < 0) { dprintf(2, "renice: %s: no such user\n", argv[i]); rc = 1; continue; }
                id = (int)pw.uid;
            }
            if (id < 0 || (id == 0 && sys_getuid() != 0)) {      /* who 0 would mean "my own" to the kernel */
                dprintf(2, "renice: %s: permission denied\n", argv[i]); rc = 1; continue;
            }
        } else if (!idnum(argv[i], &id) || id <= 0) {
            dprintf(2, "renice: %s: not a process number\n", argv[i]); rc = 1; continue;
        }
        zerrno = 0;
        int old = getpriority(which, id);
        if (old == -1 && zerrno) { dprintf(2, "renice: %s: %s\n", argv[i], zwhy("no such process")); rc = 1; continue; }
        if (setpriority(which, id, n) < 0) { dprintf(2, "renice: %s: %s\n", argv[i], zwhy("refused")); rc = 1; continue; }
        dprintf(1, "%d (%s) old priority %d, new priority %d\n", id, which == PRIO_USER ? "user ID" : "process ID", old, n);
    }
    return rc;
}
