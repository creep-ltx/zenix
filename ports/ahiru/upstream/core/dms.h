/* Zenix's copy: derived from other authors' code (lha: LHa for UNIX; lzx: unlzx 1.1; dms: xDMS 1.3, public
 * domain), through the owner's BBX Go readers. Its licence is NOT settled: not for a public release of Zenix
 * until it is (docs/audit-2026-10-29-block2.md, "Licences to settle"). */
/* dms - DiskMasher archives: unpack to an ADF disk image.
 *
 * A port of xDMS 1.3 (Andre Rodrigues de la Rocha, public domain) by
 * way of BBX's Go reader: the same track records, CRCs and sums, and
 * all seven packing modes (NOCOMP, SIMPLE, QUICK, MEDIUM, DEEP, HEAVY1,
 * HEAVY2) with the decoders' state carried from track to track as xDMS
 * does. Unlike xDMS it never reads outside its buffers and never loops
 * without end. Memory: two track buffers (about 12K each for a DD disk)
 * plus 34K of decoder state, the latter only once a packed track needs
 * it; all of it lent by arc_alloc and given back before returning.
 *
 * The file (big-endian): a 56-byte header "DMS!" (CRC-16 over bytes
 * 4..53 at 54), then 20-byte track records "TR" each followed by its
 * packed bytes. Track 80 is an attached FILE_ID.DIZ, track 0xFFFF a
 * banner, a track 0 of 1024 bytes a "fake boot block" advert; only
 * tracks 0..79 longer than 2048 bytes are the disk.
 */
#ifndef CF_DMS_H
#define CF_DMS_H

#include "arc.h"

/* packing modes, by the number in a track record */
enum { DMS_NOCOMP, DMS_SIMPLE, DMS_QUICK, DMS_MEDIUM, DMS_DEEP, DMS_HEAVY1, DMS_HEAVY2 };

/* general info flags in the header (they lie: BBS software appends a
 * DIZ without setting DMS_FILEID, so dms_text scans the tracks) */
#define DMS_NOZERO     1
#define DMS_ENCRYPTED  2
#define DMS_APPENDS    4
#define DMS_BANNER     8
#define DMS_HD         16
#define DMS_MSDOS      32
#define DMS_REGISTERED 0x80
#define DMS_FILEID     256

struct dmsinfo {
    int lowtrack, hightrack;    /* as the header says (wrong in some files) */
    unsigned long unpacked;     /* unpacked bytes the header claims (901120 for a DD disk) */
    int diskhd;                 /* 1 = HD (1760K) */
    unsigned long packed;       /* packed bytes the header claims */
    unsigned long date;         /* creation, seconds since 1.1.1970 */
    unsigned int geninfo;       /* DMS_* flags */
    unsigned int version;       /* DMS version that made it, e.g. 111 = 1.11 */
    unsigned int disktype;      /* 0/1 OFS, 2 FFS, 3/4 OFS/FFS intl, 5/6 OFS/FFS dircache, 7 FMS */
    unsigned int mode;          /* the packing mode mostly used */
    int encrypted;              /* password protected: dms_unpack says ARC_EUNSUP */
    int fms;                    /* an FMS file archive, not a disk: ARC_EUNSUP */
};

/* Reads and checks the header (its CRC included). ARC_OK, or
 * ARC_ECORRUPT (not DMS), ARC_ECRC (header CRC), ARC_EREAD. */
int dms_info(struct arcsrc *s, struct dmsinfo *i);

/* Unpacks the disk to out as an ADF: tracks 0..79 in order, a track
 * the archive lacks (before its first, between, after its last) written
 * as zeros, so a whole DD disk is 901120 bytes, HD 1802240. Every
 * track's record CRC, data CRC and unpacked sum is checked before its
 * bytes are written; the first bad one ends it (ARC_ECRC for a check
 * value, ARC_ECORRUPT for data that cannot be unpacked or a file cut
 * short). track() (may be NULL) is called after each of the 80 tracks
 * is written with its number; it returns 0 to stop (ARC_ESTOP).
 * Encrypted and FMS archives: ARC_EUNSUP. */
int dms_unpack(struct arcsrc *s, struct arcsink *out,
               int (*track)(void *ctx, int tracknum), void *tctx);

/* The attached texts: which = 80 for FILE_ID.DIZ, 0xFFFF for the
 * banner. Writes every such track's text to out. ARC_OK when one was
 * written, 1 when the archive has none, or an error. */
#define DMS_DIZ     80
#define DMS_BANNERT 0xFFFF
int dms_text(struct arcsrc *s, unsigned int which, struct arcsink *out);

#endif
