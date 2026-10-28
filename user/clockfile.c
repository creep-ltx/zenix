/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
#include "clockfile.h"
#include "../crypto/zc/zc_clock.h"

int clockfile_read(uint32_t *t)
{
    char b[ZC_CLOCK_LINE];
    int fd = sys_open(ZC_CLOCK_PATH, O_RDONLY), n;
    if (fd < 0)
        return -1;
    n = sys_read(fd, b, sizeof b);
    sys_close(fd);
    return n > 0 && zc_clock_parse(b, (size_t)n, t) == 0 ? 0 : -1;
}

int clockfile_write(uint32_t t)
{
    static const char tmp[] = ZC_CLOCK_PATH ".new";
    char b[ZC_CLOCK_LINE];
    int n = zc_clock_line(b, t), fd = sys_open(tmp, O_WRONLY | O_CREAT | O_TRUNC), w;
    if (fd < 0)
        return fd;
    w = sys_write(fd, b, (u32)n);
    sys_close(fd);
    if (w != n || (w = sys_chmod(tmp, 0644)) < 0 || (w = sys_rename(tmp, ZC_CLOCK_PATH)) < 0) {
        sys_unlink(tmp);
        return w < 0 ? w : -EIO;
    }
    return 0;
}
