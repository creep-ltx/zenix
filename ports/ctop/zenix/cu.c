/* SPDX-License-Identifier: GPL-2.0-only */
/* ctop's layer on Zenix (ports/ctop/README): the part of CUtils' library ctop calls (cu.h here: its command line,
 * messages, the task list, the CPU, the Pi) over Zenix's system calls, and the AmigaOS calls only ctop makes
 * (amigados.h's last part): the volumes (the mount table), SetTaskPri (the nice value) and bsdsocket.library's four
 * calls ctop makes through its library base (the interfaces' byte counters, netd's SOCK_IFLIST; sockbase.S). What
 * Zenix cannot tell (the CPU time of each process, how busy the processor is, the stack's free part, CLI numbers)
 * stays "not known" in ctop's own way: no graph, "-" and "?". */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include "cu.h"
#define ZENIX_LIBC 1                    /* usys.h: the C library's headers give the constants */
#include "../../../user/usys.h"
#include "../../../kernel/netproto.h"

int cu_rc;
int cu_argc;
char **cu_argv;
extern char **_zargv;                   /* the C library's start (libc/zenix/start.c) */
static struct Library *openlib(CONST_STRPTR name, ULONG version);

/* ---- the command line: the words after the name, Unix flags (cu.h's rules) ---- */
static int optw = 1, optc = 0;
static const char *usage_text;
int cu_args(const char *usage)
{
    int n = 0;
    zenix_openlib = openlib;            /* (ctop asks for bsdsocket.library after its command line) */
    usage_text = usage;
    while (_zargv && _zargv[n]) n++;
    cu_argc = n > 1 ? n - 1 : 0;
    cu_argv = _zargv ? _zargv + (n ? 1 : 0) : 0;
    if (cu_argc == 1 && !strcmp(cu_argv[0], "?")) {
        dprintf(1, "usage: %s\n", usage);
        cu_rc = 0;
        return -1;
    }
    optw = 0;
    optc = 0;
    return cu_argc;
}
int cu_opt(const char *spec, char **val)
{
    for (;;) {
        if (optw >= cu_argc) break;
        char *w = cu_argv[optw];
        if (!optc) {
            if (w[0] != '-' || !w[1]) break;            /* a word, or a lone - */
            if (!strcmp(w, "--")) { optw++; break; }
            optc = 1;
        }
        int c = (unsigned char)w[optc++];
        if (!c) { optw++; optc = 0; continue; }
        const char *s = strchr(spec, c);
        if (!s || c == ':') {
            char f[3] = { '-', (char)c, 0 };
            cu_err(10, f, "unknown option");
            if (usage_text) dprintf(2, "usage: %s\n", usage_text);
            optw++; optc = 0;
            return '?';
        }
        if (s[1] == ':') {
            if (w[optc]) *val = w + optc;
            else if (optw + 1 < cu_argc) *val = cu_argv[++optw];
            else { char f[3] = { '-', (char)c, 0 }; cu_err(10, f, "needs a value"); optw++; optc = 0; return '?'; }
            optw++; optc = 0;
        }
        return c;
    }
    cu_argv += optw;
    cu_argc -= optw;
    optw = 0;
    return -1;
}
long cu_num(const char *s, int *ok)
{
    long v = 0;
    int any = 0;
    *ok = 0;
    for (; *s >= '0' && *s <= '9'; s++, any = 1) {
        if (v > 0x7fffffffL / 10 - 1) return 0;
        v = v * 10 + (*s - '0');
    }
    if (*s == 'k' || *s == 'K') { v *= 1024; s++; }
    else if (*s == 'M') { v *= 1024 * 1024; s++; }
    else if (*s == 'G') { v *= 1024L * 1024 * 1024; s++; }
    *ok = any && !*s;
    return v;
}
void cu_flush(void) { }
void cu_err(int rc, const char *what, const char *why)
{
    if (what) dprintf(2, "%s: %s: %s\n", cu_name, what, why);
    else dprintf(2, "%s: %s\n", cu_name, why);
    if (rc > cu_rc) cu_rc = rc;
}

/* ---- the processes: one node each (its address is ctop's key for a task), listed in SysBase->TaskWait so that
 * ctop's own look for a task before SetTaskPri finds it ---- */
struct znode { struct Node n; int pid; char name[16]; };
static struct znode nodes[CU_MAXTASKS];
static struct znode *nodefor(int pid)
{
    struct znode *fr = 0;
    for (int i = 0; i < CU_MAXTASKS; i++) {
        if (nodes[i].pid == pid && nodes[i].n.ln_Name) return &nodes[i];
        if (!nodes[i].n.ln_Name && !fr) fr = &nodes[i];
    }
    if (fr) { fr->pid = pid; fr->n.ln_Name = fr->name; }
    return fr;
}
int cu_tasks(CuTask *v, int max)
{
    struct uproc p;
    int n = 0, me = sys_getpid();
    struct List *l = &SysBase->TaskWait;
    static char live[CU_MAXTASKS];
    memset(live, 0, sizeof live);
    l->lh_Head = (struct Node *)&l->lh_Tail;
    l->lh_Tail = 0;
    l->lh_TailPred = (struct Node *)&l->lh_Head;
    for (u32 i = 0; n < max && sys_procinfo(i, &p) == 0; i++) {
        if (p.state == 0) continue;
        struct znode *z = nodefor((int)p.pid);
        if (!z) break;
        live[z - nodes] = 1;
        memcpy(z->name, p.name, 15);
        z->name[15] = 0;
        z->n.ln_Pred = l->lh_TailPred;          /* at the end of TaskWait */
        z->n.ln_Succ = (struct Node *)&l->lh_Tail;
        l->lh_TailPred->ln_Succ = &z->n;
        l->lh_TailPred = &z->n;
        CuTask *t = &v[n++];
        memset(t, 0, sizeof *t);
        t->addr = z;
        t->process = 1;
        t->pri = -p.nice;                       /* AmigaOS's way round: higher is more favoured */
        t->state = (int)p.pid == me ? 0 : p.state == 1 ? 1 : 2;   /* running (ctop), ready, else waiting */
        t->free = -1;                           /* not known */
        strcpy(t->name, z->name);
        strcpy(t->cmd, (int)p.pid == 0 ? "" : z->name);   /* the kernel's idle loop as a task, [idle] */
    }
    for (int i = 0; i < CU_MAXTASKS; i++)
        if (!live[i]) nodes[i].n.ln_Name = 0;
    return n;
}
void SetTaskPri(struct Task *t, LONG pri)
{
    struct znode *z = (struct znode *)t;
    if (z < nodes || z >= nodes + CU_MAXTASKS) return;      /* (ctop's own: FindTask's) */
    long nice = -pri < -20 ? -20 : -pri > 19 ? 19 : -pri;
    sys_setpriority(0, z->pid, (int)nice);
}
int cu_signal(APTR task, ULONG sigs)
{
    struct znode *z = task;
    int sig = sigs & SIGBREAKF_CTRL_C ? SIGINT : sigs & SIGBREAKF_CTRL_D ? SIGTERM :
              sigs & SIGBREAKF_CTRL_E ? SIGUSR1 : SIGUSR2;
    if (z < nodes || z >= nodes + CU_MAXTASKS || !z->n.ln_Name) return 0;
    return sys_killsig(z->pid, sig) == 0;
}

/* ---- the CPU: no CPU time per process, no idle count on Zenix (docs/future-ideas.md item 4) ---- */
int cu_cpuopen(void) { FindTask(0); return 1; }        /* (FindTask reads the CPU's line from the log) */
void cu_cpuclose(void) { }
void cu_cpudrain(void) { }
int cu_cpumeasure(const CuTask *t, int n, CuShare *s)
{
    (void)t;
    memset(s, 0, (size_t)n * sizeof *s);
    return -1;                                  /* not known: ctop draws no graph and no figure */
}
ULONG cu_cpuclock(void) { return (ULONG)sys_ticks(); }  /* the 100 Hz tick */
int cu_cpurate(void) { return 100; }
int cu_cpuexact(void) { return 0; }
ULONG cu_cpufreq(void) { return 0; }                     /* no CPU time: TIME+ shows "-" */
int cu_emu68(void)                      /* the kernel knows (ZINE_INFO's machine, UZI_EMU68) */
{
    unsigned long zi[8] = { 0 };
    return sys3(93, 0, (unsigned long)zi, sizeof zi) == 0 && zi[0] >= 32 && (zi[7] & 1);
}
ULONG cu_emu68mips(void) { return (ULONG)-1; }
int cu_pi(CuPi *p) { memset(p, 0, sizeof *p); return 0; }
int cu_isemu68(void) { return 0; }

/* ---- the volumes: the mount table, each named by its path ("/", "/mnt/nas"); a Lock of "PATH:" is PATH (dos.c) ---- */
#define NVOL 8
static struct DosList vols[NVOL];
static unsigned char volnames[NVOL][50];
struct DosList *LockDosList(ULONG flags)
{
    struct umountinfo mi;
    int n = 0;
    (void)flags;
    for (u32 i = 0; n < NVOL && sys_mountinfo(i, &mi) == 0; i++) {
        size_t k = strnlen(mi.path, sizeof mi.path);
        if (!k || k > 47) continue;
        volnames[n][0] = (unsigned char)k;              /* a BSTR: the length, then the name */
        memcpy(volnames[n] + 1, mi.path, k);
        vols[n].dol_Name = (BPTR)volnames[n];
        vols[n].dol_Task = (struct MsgPort *)1;         /* mounted */
        vols[n].dol_Next = 0;
        if (n) vols[n - 1].dol_Next = &vols[n];
        n++;
    }
    static struct DosList head;
    head.dol_Next = n ? &vols[0] : 0;
    return &head;
}
struct DosList *NextDosEntry(struct DosList *dl, ULONG flags) { (void)flags; return dl ? dl->dol_Next : 0; }
void UnLockDosList(ULONG flags) { (void)flags; }

/* ---- the network: bsdsocket.library's base (sockbase.S) opens while netd runs (opening a socket would start it) ---- */
extern struct Library zenix_sockbase;
static int sock = -1;
static struct unetif ifs[8];
static int nifs;
static int netd_runs(void)
{
    struct uproc p;
    for (u32 i = 0; sys_procinfo(i, &p) == 0; i++)
        if (p.state && !strncmp(p.name, "netd", 16)) return 1;
    return 0;
}
static int iflist(void)
{
    struct usockreq r;
    memset(&r, 0, sizeof r);
    r.buf = (u32)ifs;
    r.len = sizeof ifs;
    r.flags = sizeof ifs[0];
    nifs = sys_sock(SOCK_IFLIST | (u32)sizeof r << 16, sock, &r);
    return nifs >= 0;
}
static struct Library *openlib(CONST_STRPTR name, ULONG version)
{
    (void)version;
    if (!name || strcmp(name, "bsdsocket.library") || !netd_runs()) return 0;
    if (sock < 0) {
        struct usockreq r;
        memset(&r, 0, sizeof r);
        r.level = 2;                            /* AF_INET */
        r.flags = SOCK_DGRAM;
        sock = sys_sock(SOCK_SOCKET | (u32)sizeof r << 16, 0, &r);
        if (sock < 0) return 0;
    }
    return &zenix_sockbase;
}
/* SocketBaseTagList: SBTC_GET_BYTES_RECEIVED (64) and _SENT (65) as references to 64-bit counts, the interfaces'
 * (not the loopback's) added up; 0 when every tag was understood */
#define SBTM_GETREF(code) (TAG_USER | 0x8000UL | ((ULONG)(code) << 1))
LONG zenix_sbtags(struct TagItem *t)
{
    u32 rx = 0, tx = 0;
    LONG bad = 0;
    if (!iflist()) return -1;
    for (int i = 0; i < nifs; i++)
        if (!(ifs[i].flags & NETIF_LOOPBACK)) { rx += ifs[i].rx_bytes; tx += ifs[i].tx_bytes; }
    for (; t && t->ti_Tag != TAG_END; t++) {
        ULONG *q = (ULONG *)t->ti_Data;
        if (t->ti_Tag == SBTM_GETREF(64)) { q[0] = 0; q[1] = rx; }
        else if (t->ti_Tag == SBTM_GETREF(65)) { q[0] = 0; q[1] = tx; }
        else bad = -1;
    }
    return bad;
}
/* ObtainInterfaceList / ReleaseInterfaceList / QueryInterfaceTagList (IFQ_Address: a sockaddr_in) */
static struct List ilist;
static struct Node inodes[8];
static char inames[8][9];
struct List *zenix_obtainifs(void)
{
    if (!iflist()) return 0;
    ilist.lh_Head = (struct Node *)&ilist.lh_Tail;
    ilist.lh_Tail = 0;
    ilist.lh_TailPred = (struct Node *)&ilist.lh_Head;
    for (int i = 0; i < nifs && i < 8; i++) {
        memcpy(inames[i], ifs[i].name, 8);
        inames[i][8] = 0;
        inodes[i].ln_Name = inames[i];
        inodes[i].ln_Pred = ilist.lh_TailPred;
        inodes[i].ln_Succ = (struct Node *)&ilist.lh_Tail;
        ilist.lh_TailPred->ln_Succ = &inodes[i];
        ilist.lh_TailPred = &inodes[i];
    }
    return &ilist;
}
void zenix_releaseifs(struct List *l) { (void)l; }
#define IFQ_Address (TAG_USER + 1900 + 14)
LONG zenix_queryif(const char *name, struct TagItem *t)
{
    int k;
    for (k = 0; k < nifs && strncmp(ifs[k].name, name, 8); k++);
    if (k == nifs) return -1;
    for (; t && t->ti_Tag != TAG_END; t++)
        if (t->ti_Tag == IFQ_Address) {
            unsigned char *sa = (unsigned char *)t->ti_Data;
            memset(sa, 0, 16);
            sa[0] = 16;
            sa[1] = 2;                          /* AF_INET */
            memcpy(sa + 4, &ifs[k].addr, 4);    /* network order, as netd keeps it */
        } else
            return -1;
    return 0;
}
LONG zenix_sockbad(void) { return -1; }
