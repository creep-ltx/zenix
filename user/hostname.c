/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
/* hostname: $HOSTNAME (login sets it from /etc/hostname), else /etc/hostname's first line, else "zenix" */
int main(void)
{
    const char *u = getenv("HOSTNAME");
    if (u && *u) { dprintf(1, "%s\n", u); return 0; }
    char b[64];
    int fd = sys_open("/etc/hostname", O_RDONLY), n = fd >= 0 ? sys_read(fd, b, sizeof b - 1) : -1;
    if (fd >= 0) sys_close(fd);
    if (n > 0) { b[n] = 0; char *e = strchr(b, '\n'); if (e) *e = 0; if (b[0]) { dprintf(1, "%s\n", b); return 0; } }
    dprintf(1, "zenix\n");
    return 0;
}
