/* SPDX-License-Identifier: GPL-2.0-only */
/* /var/lib/clock, the certificate time's floor (DESIGN D21 8.3; the format and its parser are crypto/zc/zc_clock.c):
 * read and written by sntp and reboot, one copy of the file handling. */
#ifndef ZENIX_CLOCKFILE_H
#define ZENIX_CLOCKFILE_H
#include <stdint.h>

/* the file's time into *t: 0, or -1 (no file, or not the format: ignored) */
int clockfile_read(uint32_t *t);
/* /var/lib/clock := t (written beside, then renamed: never half a line; mode 644); 0 or a negative error */
int clockfile_write(uint32_t t);

#endif
