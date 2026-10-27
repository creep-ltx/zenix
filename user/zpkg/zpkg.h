/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
/*
 * zpkg's core (DESIGN D22, docs/d22-zpkg.md): the parts without system calls, built for Zenix and for the PC's host
 * tests (tools/host/zpkg-test.sh) alike.
 *   index.c    the package list (3.3) and latest, and the trust rules for a list (5.1-5.3)
 *   resolve.c  the resolver: what -S, -Syu, -Syuu and -R/-Rs change and in which order (4.3)
 *   ffsslot.c  the update slot's read-only FFS walk and its in-place data write (4.5)
 * The installed database (D15's manifests) is read through user/zpkgfmt.c's manifest parser (the main session's
 * step 1); the resolver takes the installed packages as struct zp_inst, filled from it.
 * Errors are negative errno numbers (Linux's, as Zenix's), with a reason string (static text) where one helps.
 */
#include <stddef.h>
#include <stdint.h>

typedef uint32_t zp_u32;

#define ZP_EINVAL  (-22)
#define ZP_ENOMEM  (-12)
#define ZP_E2BIG   (-7)
#define ZP_ENOENT  (-2)
#define ZP_EPERM   (-1)
#define ZP_EIO     (-5)
#define ZP_ELOOP   (-40)
#define ZP_ENOSPC  (-28)
/* the list's own refusals (5.1-5.3), so zpkg can say which: below every errno number */
#define ZP_ECHANNEL  (-1001)        /* not the channel asked for, or an unknown one */
#define ZP_ECLASS    (-1002)        /* the channel's class does not fit the key that signed it */
#define ZP_EREPLAY   (-1003)        /* the serial is not above the stored one (or below the floor) */
#define ZP_ESERIAL   (-1004)        /* the serial is above its own date's, or below a version the list names */
#define ZP_EEXPIRED  (-1005)        /* past its expiry: "the package list expired on E, and the clock says T" */

#define ZP_INDEX_MAX   (256u * 1024)    /* a bigger list is refused (3.3) */
#define ZP_LATEST_MAX  64
#define ZP_SIG_SIZE    80
#define ZP_NAMEMAX     24
#define ZP_VSTRMAX     15               /* the bundle header's version string, 16 bytes with its NUL */
#define ZP_DESCMAX     72
#define ZP_MAXDEP      8                /* as zup's manifests (user/zup.c MAXDEP) */
#define ZP_MAXTAKES    8
#define ZP_BUNDLE_MAX  (16u << 20)      /* zenixboot's and mkzup's ceiling */

#define ZP_KEY_TEST     1               /* the test key: the channel "test" */
#define ZP_KEY_RELEASE  2               /* the release keys: "testing" and "stable" */

struct zp_dep {
    const char *name;
    zp_u32 vnum;
};

struct zp_pkg {
    const char *name;
    const char *vstr;
    const char *file;               /* the pool name NAME-VNUM.zup; 0 for a base-group block without one */
    const char *sha;                /* 64 lowercase hex characters (not NUL-terminated at 64: use zp_hex32) */
    const char *desc;               /* 0: none */
    zp_u32 vnum, fsize, size;       /* version, the pool file's bytes, the installed bytes */
    unsigned char base;             /* "group base" */
    unsigned char ndep, ntakes;
    unsigned short dep0, takes0;    /* first entry in the list's dep[] and takes[] */
};

struct zp_system {
    zp_u32 vnum, fsize, min;
    const char *vstr, *file, *sha;
};

struct zp_index {
    const char *channel;
    zp_u32 serial, date, expires;
    struct zp_system sys;
    int npkg;
    struct zp_pkg *pkg;             /* sorted by name (zp_index_find is a binary search) */
    struct zp_dep *dep;             /* every block's dependencies, each block's in a row */
    const char **takes;
};

/* text: the list's bytes (len of them, len <= ZP_INDEX_MAX) in a buffer of at least len + 1 bytes; parsed in place
 * (the strings point into it, so it must outlive ix). The signature is checked BEFORE this is called (5.1). */
int zp_index_parse(struct zp_index *ix, char *text, size_t len, const char **why);
void zp_index_free(struct zp_index *ix);
const struct zp_pkg *zp_index_find(const struct zp_index *ix, const char *name);
static inline const struct zp_dep *zp_pkg_dep(const struct zp_index *ix, const struct zp_pkg *p, int i)
{
    return &ix->dep[p->dep0 + i];
}
/* hex: 64 hex characters -> out; 0, or ZP_EINVAL */
int zp_hex32(unsigned char out[32], const char *hex);

/* latest: "zpkglatest 1 SERIAL" with an optional newline, at most 64 bytes */
int zp_latest_parse(const char *buf, size_t n, zp_u32 *serial);
/* what -Sy does with a latest serial against the stored one (4.3) */
#define ZP_LATEST_SAME   0          /* use the stored list (its expiry is still checked) */
#define ZP_LATEST_NEWER  1          /* fetch index-SERIAL and its .sig */
#define ZP_LATEST_BEHIND 2          /* "this mirror is behind": the next mirror */
int zp_latest_decide(zp_u32 latest, zp_u32 stored);

/* the class of a channel name: ZP_KEY_TEST, ZP_KEY_RELEASE, 0 unknown */
int zp_channel_class(const char *channel);

/* ---- the trust rules for a parsed list (5.1-5.3) ---- */
#define ZP_T_SNAPSHOT      1u       /* --snapshot: class checked, serial and expiry not */
#define ZP_T_IGNORE_EXPIRY 2u       /* --ignore-expiry (root, once) */
#define ZP_W_CLOCK_BEHIND  1u       /* the clock is more than a day behind the list's signed date */
#define ZP_W_EXPIRY_IGNORED 2u
#define ZP_W_UNCHECKED     4u       /* a snapshot: serial and expiry not checked (said on the screen) */
struct zp_trust {
    const char *channel;            /* the channel asked for (a snapshot: 0, any channel of the key's class) */
    int keyclass;                   /* the class of the key the list's .sig verified with */
    zp_u32 stored;                  /* the stored list's serial for this channel, 0 none */
    zp_u32 floor;                   /* a fresh machine's floor: zpkg's build date as YYYYMMDD00 */
    zp_u32 clock;                   /* the clock's date YYYYMMDD, 0 when there is no clock */
    zp_u32 clockfloor;              /* D21's time floor (/var/lib/clock, the build date) as YYYYMMDD */
    unsigned flags;
};
/* "today" (5.3): the newer of the clock and the floor */
zp_u32 zp_today(const struct zp_trust *t);
/* a list just fetched and verified: every rule; 0 or a refusal; *warn gets ZP_W_* bits */
int zp_index_check(const struct zp_index *ix, const struct zp_trust *t, unsigned *warn, const char **why);
/* the stored list at every use (5.3: also when latest says nothing is new): its expiry only */
int zp_index_usable(const struct zp_index *ix, const struct zp_trust *t, unsigned *warn, const char **why);
/* a downloaded package or system bundle against the list (5.1): its header's version and channel; name is the
 * package name its manifest gave (zpkgfmt.c), 0 for the system bundle */
int zp_bundle_fits(const struct zp_index *ix, const struct zp_pkg *p, const char *name, zp_u32 hdr_vnum,
                   const char *hdr_channel, int keyclass, const char **why);

/* dates: YYYYMMDD -> days since 1970-01-01 (-1: not a date) */
long zp_days(zp_u32 ymd);

/* ---- the resolver (4.3) ---- */
struct zp_inst {                    /* an installed package, from /var/lib/zpkg/NAME.manifest and the deps file */
    const char *name;
    zp_u32 vnum;
    unsigned char base;             /* in the group base */
    unsigned char asdep;            /* named in /var/lib/zpkg/deps: installed only as a dependency */
    int ndep;
    const struct zp_dep *dep;
};
struct zp_world {
    const struct zp_index *ix;      /* the stored, usable list */
    const struct zp_inst *inst;
    int ninst;
    zp_u32 effbase;                 /* the smaller of the base manifest's version and the running kernel's (8.4.2) */
    const char *const *hold;        /* hold NAME lines of /etc/zpkg.conf ("base" holds the system) */
    int nhold;
};
#define ZP_STEP_INSTALL   1         /* not installed: install the list's version */
#define ZP_STEP_UPGRADE   2
#define ZP_STEP_DOWNGRADE 3         /* -Syuu only */
#define ZP_STEP_WAIT      4         /* needs a newer base: downloaded, installed after the system update (4.4 step 3) */
#define ZP_STEP_HELD      5         /* not changed: held, or needs a held package's newer version (said) */
#define ZP_STEP_REMOVE    6
struct zp_step {
    int kind;
    const struct zp_pkg *pkg;       /* the list's block (0 for REMOVE) */
    const struct zp_inst *from;     /* the installed one, 0 for INSTALL */
    unsigned char asdep;            /* INSTALL: brought in only as a dependency (the deps file after the commit) */
    const char *why;                /* WAIT/HELD: what it waits for */
};
struct zp_plan {
    int n;
    struct zp_step *step;           /* in order: dependencies first; removals dependents first */
    int system;                     /* 1: the list's system bundle is newer than the installed base (and not held) */
    const char *bad;                /* on an error: the package it is about */
    const char *bad2;               /* ... and the other one (a dependency, a dependent) */
};
#define ZP_SYNC_U    1u             /* -Syu: every installed package with a newer version */
#define ZP_SYNC_UU   2u             /* -Syuu: also down to the list's version */
#define ZP_RM_S      1u             /* -Rs: also the dependencies only installed as such that nothing left needs */
int zp_plan_sync(const struct zp_world *w, const char *const *names, int nnames, unsigned flags, struct zp_plan *pl,
                 const char **why);
int zp_plan_remove(const struct zp_world *w, const char *const *names, int nnames, unsigned flags,
                   struct zp_plan *pl, const char **why);
void zp_plan_free(struct zp_plan *pl);

/* ---- the update slot on BOOT: (4.5): the read-only FFS walk, every time ---- */
#define ZP_DOS1 0x444F5301u
#define ZP_DOS3 0x444F5303u
typedef int (*zp_blkio)(void *u, zp_u32 blk, unsigned char *buf);      /* one 512-byte block; 0 or <0 */
struct zp_ffs {
    zp_u32 blocksize;               /* from the RDB partition's DosEnvec (SizeBlock x 4) */
    zp_u32 reserved;                /* DosEnvec Reserved */
    zp_u32 dostype;                 /* DosEnvec DosType */
    zp_u32 nblocks;                 /* the partition's blocks */
    int canflush;                   /* the device has DISKCAP_FLUSH */
    zp_blkio read, write;           /* partition-relative block numbers */
    void *u;
};
struct zp_slot {
    zp_u32 size;                    /* the file's bytes (its header's byte size) */
    zp_u32 nblk;
    zp_u32 *blk;                    /* the data blocks in file order (malloc'd; never kept between runs) */
    zp_u32 header;                  /* the file header's block (for the tests) */
};
/* find DIR/NAME (one directory below the root) with every check of 4.5; 0, or a refusal with the reason */
int zp_slot_find(const struct zp_ffs *f, const char *dir, const char *name, struct zp_slot *s, const char **why);
/* write n bytes at off into the slot's data blocks only (whole blocks written, partial ones read first) */
int zp_slot_pwrite(const struct zp_ffs *f, const struct zp_slot *s, zp_u32 off, const unsigned char *data, zp_u32 n);
void zp_slot_free(struct zp_slot *s);
