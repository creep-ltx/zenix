/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
/* The filesystem servers' calls to the disk server (fsd, zfsd; DESIGN D11 5.4). A request that was in service when
 * diskd ended answers -ECONNRESET; it is sent once more, to the restarted diskd, which kept every handle and claim
 * (its saved table): repeating a sector read or write, or a DISK_OPEN by the holder of the claim, is safe. */
static int disk_call(int port, struct umsg *m)
{
    int r = sys_call(port, m);
    return r == -ECONNRESET ? sys_call(port, m) : r;
}
