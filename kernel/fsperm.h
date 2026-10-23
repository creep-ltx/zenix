// SPDX-License-Identifier: GPL-2.0-only
#pragma once
/* The permission rules of Zenix, for every filesystem server (ext2 today, ZenixFS next). Header only, no
 * dependencies: everything is passed as plain values, so a server on any on-disk format can use it.
 *
 * Who is asking comes from the request message (struct umsg: uid, gid, umask), stamped by the kernel. The server
 * reads the owner (uid, gid) and mode from its own inode. "want" is a mask of FSP_R / FSP_W / FSP_X.
 *
 *  1. Owner class if uid == owner uid, else group class if gid == owner gid, else other. Root (uid 0) passes every
 *     check except executing a regular file that has no x bit at all.
 *  2. Walking a path needs x on every directory searched (not on the last component).
 *  3. Adding, removing, renaming or linking an entry needs w+x on the directory (fsp_dir_modify).
 *  4. Opening needs r and/or w for the access mode, and x with FSP_O_EXEC, which the kernel adds when it loads a
 *     program (fsp_open_want).
 *  5. chmod: the owner or root; a non-root owner cannot set set-group-id unless the file's group is their group (the
 *     bit is dropped, as Linux does: fsp_chmod_mode). utimes: an explicit time only the owner or root; "now" also
 *     anyone with w access (fsp_utimes_ok). chown/chgrp: root only (fsp_is_root).
 *  6. A sticky directory (mode 01000, like /tmp) lets only the owner of the entry, the owner of the directory or
 *     root remove or rename entries (fsp_sticky_ok).
 *  7. New files belong to the caller: uid/gid from the message, mode 0666 & ~umask; directories 0777 & ~umask
 *     (fsp_new_file_mode, fsp_new_dir_mode). In a set-group-id directory (02000) new entries get the directory's
 *     group instead, and new directories inherit the 02000 bit (fsp_new_gid, fsp_new_dir_bits). chown clears the
 *     set-user-id and set-group-id bits.
 *  8. Set-user-id (04000) on a program makes it run as its owner: the kernel reads the mode and owner from the
 *     stat that FS_OPEN returns, so servers must report uid, gid and the full mode there.
 *  9. A write (or a truncate) by anyone but the owner and root clears set-user-id, and set-group-id when the group
 *     has x (fsp_write_clear): a changed program must not keep running as its owner.
 * 10. Moving a directory to another parent rewrites its "..": that needs w on the directory itself as well
 *     (fsp_access FSP_W), as on Linux.
 * A refusal is returned to the caller as -13 (-EACCES). */

#define FSP_R 4
#define FSP_W 2
#define FSP_X 1
#define FSP_O_EXEC   0x8000          /* same value as O_EXEC in fsproto.h */
#define FSP_O_WRONLY 1
#define FSP_O_RDWR   2

#define FSP_IFMT 0170000
#define FSP_IFDIR 0040000

/* may uid/gid do "want" on an object with this mode and owner? (1 yes, 0 no) */
static inline int fsp_access(unsigned uid, unsigned gid, unsigned mode, unsigned owner_uid, unsigned owner_gid, int want)
{
    if (uid == 0)
        return !(want & FSP_X) || (mode & FSP_IFMT) == FSP_IFDIR || (mode & 0111);
    int sh = uid == owner_uid ? 6 : gid == owner_gid ? 3 : 0;
    return (int)(((mode >> sh) & 7) & (unsigned)want) == want;
}

static inline int fsp_is_root(unsigned uid) { return uid == 0; }
static inline int fsp_owner_or_root(unsigned uid, unsigned owner_uid) { return uid == 0 || uid == owner_uid; }

/* the access bits an open with these flags needs */
static inline int fsp_open_want(unsigned flags)
{
    int want = (flags & FSP_O_WRONLY) ? FSP_W : (flags & FSP_O_RDWR) ? FSP_R | FSP_W : FSP_R;
    if (flags & FSP_O_EXEC)
        want |= FSP_R | FSP_X;
    return want;
}

/* may uid add/remove/rename an entry in a directory with this mode and owner? */
static inline int fsp_dir_modify(unsigned uid, unsigned gid, unsigned dir_mode, unsigned dir_uid, unsigned dir_gid)
{
    return fsp_access(uid, gid, dir_mode, dir_uid, dir_gid, FSP_W | FSP_X);
}

/* sticky directory rule: may uid remove/rename the entry owned by entry_uid? */
static inline int fsp_sticky_ok(unsigned uid, unsigned dir_mode, unsigned dir_uid, unsigned entry_uid)
{
    return uid == 0 || !(dir_mode & 01000) || uid == dir_uid || uid == entry_uid;
}

static inline unsigned fsp_new_file_mode(unsigned umask) { return 0666 & ~umask & 07777; }
static inline unsigned fsp_new_dir_mode(unsigned umask) { return 0777 & ~umask & 07777; }

/* the group of a new entry in a directory: the directory's in a set-group-id directory, else the caller's */
static inline unsigned fsp_new_gid(unsigned gid, unsigned dir_mode, unsigned dir_gid)
{
    return (dir_mode & 02000) ? dir_gid : gid;
}
/* extra mode bits of a new directory: set-group-id is inherited */
static inline unsigned fsp_new_dir_bits(unsigned dir_mode) { return dir_mode & 02000; }

/* the mode bits chmod stores: a non-root owner may not make a file set-group-id for a group that is not theirs */
static inline unsigned fsp_chmod_mode(unsigned uid, unsigned gid, unsigned owner_gid, unsigned mode)
{
    mode &= 07777;
    if (uid != 0 && gid != owner_gid)
        mode &= ~02000u;
    return mode;
}

/* may uid set the time of a file it owns or not? explicit: a given time (only the owner, root); otherwise "now"
 * (also anyone who may write it: can_write) */
static inline int fsp_utimes_ok(unsigned uid, unsigned owner_uid, int explicit_time, int can_write)
{
    return uid == 0 || uid == owner_uid || (!explicit_time && can_write);
}

/* the mode after uid writes to (or truncates) the file: the bits a non-owner write clears (rule 9) */
static inline unsigned fsp_write_clear(unsigned uid, unsigned owner_uid, unsigned mode)
{
    if (uid == 0 || uid == owner_uid)
        return mode;
    mode &= ~04000u;
    if (mode & 0010)
        mode &= ~02000u;
    return mode;
}
