/* SPDX-License-Identifier: GPL-2.0-only */
/* The AmigaOS screen calls of Ahiru on Zenix (include/amigagui.h): none of them opens anything (Zenix's text console
 * has no screens or windows to give: CLAUDE.md, "Text console"), so the code that would draw on one is never reached;
 * Ahiru draws inside the terminal through the text-UI library instead. OpenFont gives a font all the same (upstream
 * reads its size before it tries the screen, which then fails). */
#include <amigagui.h>

struct Screen *LockPubScreen(CONST_STRPTR name) { (void)name; return NULL; }
void UnlockPubScreen(CONST_STRPTR name, struct Screen *s) { (void)name; (void)s; }
struct Screen *OpenScreenTags(const void *ns, ...) { (void)ns; return NULL; }
struct Window *OpenWindowTags(const void *nw, ...) { (void)nw; return NULL; }
LONG CloseScreen(struct Screen *s) { (void)s; return 1; }
void CloseWindow(struct Window *w) { (void)w; }
void ScreenToFront(struct Screen *s) { (void)s; }
void ScreenToBack(struct Screen *s) { (void)s; }
void ActivateWindow(struct Window *w) { (void)w; }
UWORD PubScreenStatus(struct Screen *s, UWORD flags) { (void)s; (void)flags; return 0; }
ULONG GetVPModeID(struct ViewPort *vp) { (void)vp; return INVALID_ID; }
void LoadRGB4(struct ViewPort *vp, const UWORD *colours, LONG n) { (void)vp; (void)colours; (void)n; }
void ReplyMsg(struct Message *m) { (void)m; }
struct TextFont *OpenFont(struct TextAttr *ta)
{
    static struct TextFont topaz8 = { 8, 8, 6, 0 };
    (void)ta;
    return &topaz8;
}
struct TextFont *OpenDiskFont(struct TextAttr *ta) { (void)ta; return NULL; }
void CloseFont(struct TextFont *f) { (void)f; }
void SetFont(struct RastPort *rp, struct TextFont *f) { (void)rp; (void)f; }
void SetAPen(struct RastPort *rp, ULONG pen) { (void)rp; (void)pen; }
void SetBPen(struct RastPort *rp, ULONG pen) { (void)rp; (void)pen; }
void SetDrMd(struct RastPort *rp, ULONG mode) { (void)rp; (void)mode; }
void Move(struct RastPort *rp, LONG x, LONG y) { (void)rp; (void)x; (void)y; }
LONG Text(struct RastPort *rp, CONST_STRPTR s, ULONG n) { (void)rp; (void)s; (void)n; return 0; }
void RectFill(struct RastPort *rp, LONG x0, LONG y0, LONG x1, LONG y1) { (void)rp; (void)x0; (void)y0; (void)x1; (void)y1; }
struct DiskObject *GetDiskObject(CONST_STRPTR name) { (void)name; return NULL; }
void FreeDiskObject(struct DiskObject *d) { (void)d; }
