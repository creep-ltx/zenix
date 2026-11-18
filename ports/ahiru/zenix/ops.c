/* SPDX-License-Identifier: GPL-2.0-only */
/* Ahiru's copy and move on Zenix (ports/ahiru/README): upstream's fs/ops.c compiled unchanged, but for a check before
 * a paste. On AmigaDOS ".." is an ordinary name (the parent is "/", which upstream already turns into "_"); on Zenix
 * "TARGET/.." is the target's parent. So an archive member or a disk image's folder named "..", "." or "" picked for a
 * paste would land beside the target or on it (the full audit FA-11, B3-6). Inside a folder that is pasted, the core's
 * safename (fs/arcfs.c) leaves such names out already; here a picked one refuses the whole paste before anything is
 * written, with a message. The same for a delete from an lha, lzx or zip archive (its rebuild stages the members as
 * files: ".." there would be the stage's parent; a disk image's own writer deletes inside the image and is left as it
 * is). Upstream's fs_copy and fs_delete are renamed by the define and declared static first, as list.c does. */
#include <string.h>
#define fs_copy up_fs_copy
#define fs_delete up_fs_delete
struct place;
static void up_fs_copy(struct place *p, struct place *q, int cur, int ismove, int force);
static void up_fs_delete(struct place *p, int cur);
#include "../upstream/fs/ops.c"
#undef fs_copy
#undef fs_delete

/* a name that is no name in a folder of Zenix's: "", "." or ".." */
static int noname(const char *s)
{
    return !s[0] || !strcmp(s, ".") || !strcmp(s, "..");
}

/* 1: a picked entry of P is no name here (said, with WHAT: "pasted" or "deleted"); -1: nothing picked; 0: go on */
static int picked_noname(struct place *p, int cur, const char *what)
{
    int n, s, *pick = NULL;
    n = place_pick(p, cur, &pick);
    for (s = 0; s < n; s++)
        if (noname(p->ent[pick[s]].name)) {
            char mb[96];
            sfmt(mb, sizeof(mb), "a member named \"%s\" names no file here - nothing %s", p->ent[pick[s]].name, what);
            xfree(pick);
            showmsg(mb);
            return 1;
        }
    xfree(pick);
    return n ? 0 : -1;                  /* nothing picked (or no memory: said) */
}

void fs_copy(struct place *p, struct place *q, int cur, int ismove, int force)
{
    if (pl_inarc(p) && !pl_involume(p) && !p->fail && picked_noname(p, cur, "pasted")) return;
    up_fs_copy(p, q, cur, ismove, force);
}

void fs_delete(struct place *p, int cur)
{
    if (pl_inarc(p) && !arcfs_isadf(p) && !p->fail && picked_noname(p, cur, "deleted")) return;
    up_fs_delete(p, cur);
}
