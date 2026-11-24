/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
/* The filesystem servers' calls to the disk server (fsd, zfsd; DESIGN D11 5.4). A request that was in service when
 * diskd ended answers -ECONNRESET; it is sent once more, to the restarted diskd, which kept every handle and claim
 * (its saved table): repeating a sector read or write, or a DISK_OPEN by the holder of the claim, is safe.
 * DESIGN D60 5.5 (stage 6): a sector read or write declares its buffer (SYS_callv, a3 for a2 sectors), so with full
 * memory protection the disk server is shown it at its receive, where its window can be the one kept from the last
 * request (d60 section 23), and diskd's own ask finds it declared; without protection SYS_callv is SYS_call. */
static int disk_call(int port, struct umsg *m)
{
    struct ucalldecl d;
    d.n = 0;
    if ((m->type == DISK_READ || m->type == DISK_WRITE) && m->a[2] < 0x800000u) {
        d.n = 1;
        d.b[0].addr = m->a[3];
        d.b[0].len = m->a[2] * 512;
        d.b[0].flags = m->type == DISK_READ ? CALLB_RD | CALLB_WR : CALLB_RD;
    }
    int r = sys_callv(port, m, &d);
    return r == -ECONNRESET ? sys_callv(port, m, &d) : r;
}
