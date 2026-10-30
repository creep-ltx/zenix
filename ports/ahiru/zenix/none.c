/* SPDX-License-Identifier: GPL-2.0-only */
/* Ahiru on Zenix (roadmap block 3; ports/ahiru/README): the parts of Ahiru and the CFile core that have no place on
 * Zenix's text console yet, each answering as upstream's own code does when its library or device is missing. These
 * upstream files are not compiled (each one's calls are here instead):
 *  - src/paula.c, core/ptplay.c, core/svx.c, core/ilbm.c, ahiru/src/media.c: sound straight on Paula and pictures;
 *    a program on Zenix never touches the chips (the sound server is roadmap block 6): playing says so;
 *  - ahiru/src/thumb.c: picture previews, drawn as pixels on Ahiru's screen or as sixels in CTerm; the console has
 *    no pixels of its own (the text-UI library's CellW is 0): no thumbnail, the preview card says what the file is;
 *  - ahiru/src/rexx.c: the ARexx port (no ARexx on Zenix);
 *  - fs/cx.c: the commodity (a hotkey that pops Ahiru up from Workbench): no Workbench;
 *  - fs/mount.c: an ADF image in a trackfile.device drive (OS 3.2): no such device; the image still opens as a folder
 *    with Enter (the core's arcfs, as on an Amiga before 3.2);
 *  - fs/modload.c, fs/modwrap.c: loading the archive code only when used (AmigaOS hunks inside Ahiru's file): on
 *    Zenix Ahiru is one program with shared code (ZEX2), the archive code always in;
 *  - ahiru/src/jobs.c: a paste in a second process while Ahiru goes on (CreateNewProc): Zenix has no threads and no
 *    fork (DESIGN D27 9, rule 3), so a paste runs in the front, with its progress bar and Esc, as one into an archive
 *    does upstream (job_start answers 0: ahiru/src/ops.c then copies here and now). */
#include <string.h>
#include "ahiru.h"
#include "core/util.h"
#include "fs/fe.h"
#include "fs/cx.h"
#include "fs/mount.h"
#include "fs/modload.h"
#include "paula.h"

/* ---- sound ---- */
int snd_load(const char *path, int kind, char *why, int whysize)
{
    (void)path;
    (void)kind;
    sfmt(why, whysize, "no sound on Zenix's console yet");
    return 0;
}
int snd_start(char *why, int whysize) { sfmt(why, whysize, "no sound on Zenix's console yet"); return 0; }
void snd_status(struct sndstat *s) { memset(s, 0, sizeof(*s)); s->kind = SND_NONE; }
void snd_free(void) { }

/* ---- pictures (thumb.c) ---- */
unsigned char *thumb_make(const char *path, int maxw, int maxh, int *tw, int *th, int *sw, int *sh, int *stop)
{
    (void)path; (void)maxw; (void)maxh;
    *tw = *th = *sw = *sh = 0;
    *stop = 0;
    return NULL;
}
void thumb_show(const unsigned char *rgb, int tw, int th, int x, int y)
{
    (void)rgb; (void)tw; (void)th; (void)x; (void)y;
}
void thumb_drop(void) { }
void thumb_exit(void) { }
/* a key (or a played one) waiting: stop and let the keyboard win (upstream's, in thumb.c) */
int keywaiting(void)
{
    if (inject && *inject) return 1;
    return ui_keywaiting();
}

/* ---- ARexx ---- */
int rexx_open(void) { return 0; }
ULONG rexx_sig(void) { return 0; }
int rexx_handle(void) { return 0; }
int rexx_busy(int hidden) { (void)hidden; return 0; }
void rexx_close(void) { }
const char *rexx_name(void) { return ""; }

/* ---- the commodity and Workbench ---- */
int cx_open(const char *name, const char *title, const char *descr, const char *hotkey, int *dup)
{
    (void)name; (void)title; (void)descr; (void)hotkey;
    *dup = 0;
    return 0;
}
int cx_hotkeyok(void) { return 1; }
ULONG cx_sig(void) { return 0; }
int cx_poll(void) { return CX_NONE; }
void cx_close(void) { }
void cx_tooltypes(const void *wbstartup, int *popup, char *hotkey, int size) { (void)wbstartup; (void)popup; (void)hotkey; (void)size; }
int wb_activedir(char *dst, int size) { (void)dst; (void)size; return 0; }

/* ---- disk images in drives ---- */
int mnt_have(void) { return 0; }
void mnt_exit(void) { }
int mnt_find(const char *file, char *dev, int size) { (void)file; (void)dev; (void)size; return 0; }
int mnt_imageof(const char *path, char *file, int size) { (void)path; (void)file; (void)size; return 0; }
int mnt_release(const char *file) { (void)file; return 0; }
void mnt_unmap(char *path, int size) { (void)path; (void)size; }
int fs_mount(struct place *p, int cur, const char *other, char *go, int size)
{
    (void)p; (void)cur; (void)other; (void)go; (void)size;
    showmsg("no drives for disk images on Zenix - Enter opens an ADF as a folder");
    return 0;
}

/* ---- code loaded when used ---- */
void mod_self(int argc, char **argv) { (void)argc; (void)argv; }
void mod_idle(void) { }
void mod_exit(void) { }

/* ---- the background paste ---- */
int job_mine(void) { return 0; }
int job_busy(void) { return 0; }
ULONG job_sig(void) { return 0; }
void job_say(const char *s) { (void)s; }
int job_ask(const char *q) { (void)q; return 27; }
int job_line(const char *prompt, char *buf, int max) { (void)prompt; (void)buf; (void)max; return 0; }
void job_progshow(ULONG total) { (void)total; }
void job_progadd(ULONG n) { (void)n; }
int job_cancelled(void) { return 0; }
void job_changed(void) { }
int job_start(struct place *src, struct place *dst, int ismove, int force, int n)
{
    (void)src; (void)dst; (void)ismove; (void)force; (void)n;
    return 0;
}
int job_poll(void) { return 0; }
void job_label(char *dst) { dst[0] = 0; }
void job_show(void) { say("no job runs (a paste runs in the front on Zenix)", 0); }
void job_end(void) { }
