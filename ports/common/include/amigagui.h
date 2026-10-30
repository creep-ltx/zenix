/* SPDX-License-Identifier: GPL-2.0-only */
/* The AmigaOS screen calls of Ahiru (ports/ahiru), for its Zenix build: Intuition, graphics, diskfont, icon and
 * Workbench. Ahiru draws on a screen of its own, in a Workbench window, or inside the terminal it was started from;
 * Zenix's text console has only the last (CLAUDE.md: no desktop features in the text console), so none of these ever
 * opens: OpenScreenTags, OpenWindowTags and LockPubScreen give NULL, OpenFont and OpenDiskFont NULL, GetDiskObject
 * NULL; the rest are never reached once those fail (ports/common/gui.c). Only the structures' fields upstream's code
 * reads are here, so that it compiles unchanged. */
#ifndef ZENIX_TT_AMIGAGUI_H
#define ZENIX_TT_AMIGAGUI_H
#include <amigados.h>

struct TextAttr { STRPTR ta_Name; UWORD ta_YSize; UBYTE ta_Style, ta_Flags; };
struct TextFont { UWORD tf_YSize, tf_XSize, tf_Baseline; UBYTE tf_Flags; };
#define FPF_PROPORTIONAL 0x20
struct RastPort { struct TextFont *Font; };
struct ViewPort { WORD DWidth, DHeight; };
struct Window;
struct Screen {
    struct Screen *NextScreen;
    struct Window *FirstWindow;
    WORD LeftEdge, TopEdge, Width, Height;
    signed char BarHeight;
    struct ViewPort ViewPort;
};
struct Window {
    struct Window *NextWindow;
    WORD LeftEdge, TopEdge, Width, Height;
    signed char BorderLeft, BorderTop, BorderRight, BorderBottom;
    struct RastPort *RPort;
    struct MsgPort *UserPort;
    WORD GZZWidth, GZZHeight;
};
struct IntuiMessage {
    struct Message ExecMessage;
    ULONG Class;
    UWORD Code, Qualifier;
    WORD MouseX, MouseY;
    ULONG Seconds, Micros;
};
#define IDCMP_NEWSIZE       0x2
#define IDCMP_MOUSEBUTTONS  0x8
#define IDCMP_CLOSEWINDOW   0x200
#define IDCMP_RAWKEY        0x400
#define IDCMP_VANILLAKEY    0x200000
#define SELECTDOWN          0x68
#define IEQUALIFIER_LSHIFT  0x1
#define IEQUALIFIER_RSHIFT  0x2
#define IEQUALIFIER_CONTROL 0x8
#define TAG_IGNORE          1
#define TAG_USER            0x80000000UL
#define WA_Left          (TAG_USER + 0x64)
#define WA_Top           (TAG_USER + 0x65)
#define WA_Width         (TAG_USER + 0x66)
#define WA_Height        (TAG_USER + 0x67)
#define WA_IDCMP         (TAG_USER + 0x6A)
#define WA_Title         (TAG_USER + 0x6E)
#define WA_CustomScreen  (TAG_USER + 0x70)
#define WA_MinWidth      (TAG_USER + 0x74)
#define WA_MinHeight     (TAG_USER + 0x75)
#define WA_MaxWidth      (TAG_USER + 0x76)
#define WA_MaxHeight     (TAG_USER + 0x77)
#define WA_ScreenTitle   (TAG_USER + 0x7A)
#define WA_PubScreen     (TAG_USER + 0x7B)
#define WA_SizeGadget    (TAG_USER + 0x83)
#define WA_DragBar       (TAG_USER + 0x84)
#define WA_DepthGadget   (TAG_USER + 0x85)
#define WA_CloseGadget   (TAG_USER + 0x86)
#define WA_Backdrop      (TAG_USER + 0x87)
#define WA_SizeBBottom   (TAG_USER + 0x8A)
#define WA_SimpleRefresh (TAG_USER + 0x8C)
#define WA_GimmeZeroZero (TAG_USER + 0x8F)
#define WA_Borderless    (TAG_USER + 0x88)
#define WA_Activate      (TAG_USER + 0x89)
#define WA_RMBTrap       (TAG_USER + 0x8D)
#define SA_Width         (TAG_USER + 0x23)
#define SA_Height        (TAG_USER + 0x24)
#define SA_Depth         (TAG_USER + 0x25)
#define SA_Title         (TAG_USER + 0x28)
#define SA_Font          (TAG_USER + 0x2B)
#define SA_Type          (TAG_USER + 0x2D)
#define SA_DisplayID     (TAG_USER + 0x32)
#define SA_ShowTitle     (TAG_USER + 0x36)
#define SA_Quiet         (TAG_USER + 0x38)
#define SA_Pens          (TAG_USER + 0x3A)
#define SA_PubName       (TAG_USER + 0x2F)
#define CUSTOMSCREEN     0xF
#define PSNF_PRIVATE     1
#define INVALID_ID       (~0UL)
#define HIRES_KEY        0x8000
#define JAM2             1
struct Screen *LockPubScreen(CONST_STRPTR name);
void UnlockPubScreen(CONST_STRPTR name, struct Screen *s);
struct Screen *OpenScreenTags(const void *ns, ...);
struct Window *OpenWindowTags(const void *nw, ...);
LONG CloseScreen(struct Screen *s);
void CloseWindow(struct Window *w);
void ScreenToFront(struct Screen *s);
void ScreenToBack(struct Screen *s);
void ActivateWindow(struct Window *w);
UWORD PubScreenStatus(struct Screen *s, UWORD flags);
ULONG GetVPModeID(struct ViewPort *vp);
void LoadRGB4(struct ViewPort *vp, const UWORD *colours, LONG n);
void ReplyMsg(struct Message *m);
struct TextFont *OpenFont(struct TextAttr *ta);
struct TextFont *OpenDiskFont(struct TextAttr *ta);
void CloseFont(struct TextFont *f);
void SetFont(struct RastPort *rp, struct TextFont *f);
void SetAPen(struct RastPort *rp, ULONG pen);
void SetBPen(struct RastPort *rp, ULONG pen);
void SetDrMd(struct RastPort *rp, ULONG mode);
void Move(struct RastPort *rp, LONG x, LONG y);
LONG Text(struct RastPort *rp, CONST_STRPTR s, ULONG n);
void RectFill(struct RastPort *rp, LONG x0, LONG y0, LONG x1, LONG y1);
struct DiskObject { STRPTR do_DefaultTool; STRPTR *do_ToolTypes; };
struct DiskObject *GetDiskObject(CONST_STRPTR name);
void FreeDiskObject(struct DiskObject *d);
#endif
