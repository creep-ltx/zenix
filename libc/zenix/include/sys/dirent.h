/* SPDX-License-Identifier: GPL-2.0-only */
/* Directories for the C library (DESIGN D27 section 5), over SYS_readdir's entries (user/usys.h's udirent): a name
 * of at most 63 bytes, its inode and type. picolibc's own sys/dirent.h is an #error for systems without one. */
#ifndef _SYS_DIRENT_H_
#define _SYS_DIRENT_H_
#include <sys/types.h>
#define MAXNAMLEN 63
struct dirent {
    ino_t d_ino;
    unsigned char d_type;               /* always DT_UNKNOWN (portable programs then stat the name) */
    char d_name[MAXNAMLEN + 1];
};
#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK     10
#define DT_SOCK    12
typedef struct _zenix_dir DIR;
#endif
