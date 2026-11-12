/* SPDX-License-Identifier: GPL-2.0-only */
/* ZINE's gadtools.library and keymap.library (docs/d29-zine.md 16.17), small: enough for a program that builds its
 * window's gadgets and menus with gadtools and reads its IDCMP through GT_GetIMsg (Efreet's deeppan). ZINE draws no
 * gadgets and no menus: the structures are made as the autodocs say (a context gadget, one Gadget per
 * CreateGadgetA linked behind the previous, Menu and MenuItem lists from a NewMenu array with GTMENU_USERDATA behind
 * each), the drawing and layout calls do nothing, GT_GetIMsg and GT_ReplyIMsg are exec's GetMsg and ReplyMsg.
 * keymap.library: the USA keymap of binput.c (MapRawKey, MapANSI). Written from the RKRM autodocs and the NDK 3.2's
 * include and .fd files (names and numbers only); never from AmigaOS code. */
#include "blob.h"

#define ZGAD 0x5A474144u                /* "ZGAD": a Gadget, Menu or MenuItem made here (FreeGadgets, FreeMenus) */
u32 c_GetMsg(u8 *mp);
void c_ReplyMsg(u8 *m);

static u8 *mk(u32 size)
{
    u8 *p = (u8 *)c_AllocVec(size + 4, MEMF_PUBLIC | MEMF_CLEAR);
    if (p)
        *(u32 *)(p + size) = ZGAD;
    return p;
}

/* Gadget (intuition/intuition.h): NextGadget 0, LeftEdge 4, TopEdge 6, Width 8, Height 10, Flags 12, Activation 14,
 * GadgetType 16, GadgetText 26, GadgetID 38, UserData 40; 44 bytes. NewGadget (libraries/gadtools.h): ng_LeftEdge 0,
 * ng_TopEdge 2, ng_Width 4, ng_Height 6, ng_GadgetText 8, ng_GadgetID 16, ng_UserData 26. */
#define GSIZE 44
#define GTYP_GADTOOLS 0x0100

/* CreateContext(glistptr a0) (-114): the context gadget, put at *glistptr */
u32 c_CreateContext(u32 *gl)
{
    u8 *g = mk(GSIZE);
    if (g) {
        *(u16 *)(g + 12) = 3;           /* GFLG_GADGHNONE */
        *(u16 *)(g + 16) = GTYP_GADTOOLS;
        if (gl)
            *gl = (u32)g;
    }
    return (u32)g;
}

/* CreateGadgetA(kind d0, previous a0, newgad a1, tags a2) (-30): 0 when previous is 0 (the autodoc's chaining) */
u32 c_CreateGadgetA(u32 kind, u8 *prev, const u8 *ng, const u32 *tags)
{
    (void)kind, (void)tags;
    if (!prev || !ng)
        return 0;
    u8 *g = mk(GSIZE);
    if (!g)
        return 0;
    for (int i = 0; i < 4; i++)
        *(u16 *)(g + 4 + 2 * i) = *(const u16 *)(ng + 2 * i);
    *(u32 *)(g + 26) = *(const u32 *)(ng + 8);
    *(u16 *)(g + 38) = *(const u16 *)(ng + 16);
    *(u32 *)(g + 40) = *(const u32 *)(ng + 26);
    *(u16 *)(g + 12) = 0;               /* GFLG_GADGHCOMP */
    *(u16 *)(g + 14) = 1;               /* GACT_RELVERIFY */
    *(u16 *)(g + 16) = GTYP_GADTOOLS | 1;   /* GTYP_BOOLGADGET */
    *(u32 *)(g + 0) = *(u32 *)prev;
    *(u32 *)prev = (u32)g;
    return (u32)g;
}

/* FreeGadgets(gad a0) (-36): the list's gadgets made here */
void c_FreeGadgets(u8 *g)
{
    for (int n = 0; g && n < 4096; n++) {
        u8 *next = *(u8 **)g;
        if (*(u32 *)(g + GSIZE) == ZGAD) {
            *(u32 *)(g + GSIZE) = 0;
            c_FreeVec((u32)g);
        }
        g = next;
    }
}

/* CreateMenusA(newmenu a0, tags a1) (-48). NewMenu: nm_Type 0 (NM_END 0, NM_TITLE 1, NM_ITEM 2, NM_SUB 3, IM_ 0x80
 * |, NM_IGNORE 0x40), nm_Label 2, nm_CommKey 6, nm_Flags 10, nm_MutualExclude 12, nm_UserData 16; 20 bytes. Menu:
 * NextMenu 0, Flags 12, MenuName 14, FirstItem 18, 30 bytes; MenuItem: NextItem 0, Flags 12, MutualExclude 14,
 * Command 26, SubItem 28, NextSelect 32, 34 bytes; each with its user data behind it. ItemFill (no IntuiText: ZINE
 * draws no menus) stays 0. */
#define MSIZE 34
#define ISIZE 38
void c_FreeMenus(u8 *m);
u32 c_CreateMenusA(const u8 *nm, const u32 *tags)
{
    (void)tags;
    u8 *first = 0, *menu = 0, *item = 0, *sub = 0;
    for (int i = 0; nm && i < 1024; i++, nm += 20) {
        u8 t = nm[0];
        if (t == 0)
            break;
        if (t & 0x40)
            continue;
        u8 *p = mk((t & 3) == 1 ? MSIZE : ISIZE);
        if (!p) {
            c_FreeMenus(first);
            return 0;
        }
        u32 label = *(const u32 *)(nm + 2), ud = *(const u32 *)(nm + 16);
        (void)label;
        u16 fl = *(const u16 *)(nm + 10);
        if ((t & 3) == 1) {
            *(u16 *)(p + 12) = (u16)(fl & 1 ? 0 : 1);      /* MENUENABLED unless NM_MENUDISABLED */
            *(u32 *)(p + 14) = label;
            *(u32 *)(p + 30) = ud;
            if (menu)
                *(u32 *)menu = (u32)p;
            else
                first = p;
            menu = p, item = sub = 0;
            continue;
        }
        if (!menu) {                    /* an item before any title: the array is wrong */
            c_FreeVec((u32)p);
            c_FreeMenus(first);
            return 0;
        }
        /* ITEMTEXT 2, ITEMENABLED 0x10 (NM_ITEMDISABLED 0x10 clears it), HIGHCOMP 0x40; CHECKIT, CHECKED, MENUTOGGLE
         * and COMMSEQ as the NewMenu says */
        u16 f = (u16)(2 | 0x40 | (fl & 0x10 ? 0 : 0x10) | (fl & (0x1 | 0x100 | 0x8)));
        u32 ck = *(const u32 *)(nm + 6);
        if (ck && c_TypeOfMem(ck))
            p[26] = *(const u8 *)ck, f |= 4;    /* COMMSEQ */
        *(u16 *)(p + 12) = f;
        *(u32 *)(p + 14) = *(const u32 *)(nm + 12);
        *(u16 *)(p + 32) = 0xFFFF;      /* MENUNULL */
        *(u32 *)(p + 34) = ud;
        if ((t & 3) == 2) {
            if (item)
                *(u32 *)item = (u32)p;
            else
                *(u32 *)(menu + 18) = (u32)p;
            item = p, sub = 0;
        } else {
            if (!item) {
                c_FreeVec((u32)p);
                c_FreeMenus(first);
                return 0;
            }
            if (sub)
                *(u32 *)sub = (u32)p;
            else
                *(u32 *)(item + 28) = (u32)p;
            sub = p;
        }
    }
    return (u32)first;
}

static void free_items(u8 *it, int depth)
{
    for (int n = 0; it && n < 1024; n++) {
        u8 *next = *(u8 **)it;
        if (*(u32 *)(it + ISIZE) != ZGAD)
            return;
        if (depth == 0)
            free_items(*(u8 **)(it + 28), 1);
        *(u32 *)(it + ISIZE) = 0;
        c_FreeVec((u32)it);
        it = next;
    }
}

/* FreeMenus(menu a0) (-54) */
void c_FreeMenus(u8 *m)
{
    for (int n = 0; m && n < 256; n++) {
        u8 *next = *(u8 **)m;
        if (*(u32 *)(m + MSIZE) != ZGAD)
            return;
        free_items(*(u8 **)(m + 18), 0);
        *(u32 *)(m + MSIZE) = 0;
        c_FreeVec((u32)m);
        m = next;
    }
}

/* GetVisualInfoA(screen a0, tags a1) (-126): a handle of ZINE's; FreeVisualInfo (-132) */
u32 c_GetVisualInfoA(u32 scr) { u8 *v = mk(16); if (v) *(u32 *)v = scr; return (u32)v; }
void c_FreeVisualInfo(u8 *v)
{
    if (v && *(u32 *)(v + 16) == ZGAD) {
        *(u32 *)(v + 16) = 0;
        c_FreeVec((u32)v);
    }
}

u32 c_GT_GetIMsg(u8 *port) { return port ? c_GetMsg(port) : 0; }
void c_GT_ReplyIMsg(u8 *m) { if (m) c_ReplyMsg(m); }

/* the library tables: gadtools_lib.fd (-30 .. -174, four private slots) and keymap_lib.fd (-30 .. -48) */
#define F(x) extern void zs_##x(void)
F(CreateGadgetA); F(FreeGadgets); F(CreateMenusA); F(FreeMenus); F(GT_GetIMsg); F(GT_ReplyIMsg); F(CreateContext);
F(GetVisualInfoA); F(FreeVisualInfo); F(RetA1); F(rts); F(Ret0); F(Ret1); F(RawKeyConvert); F(MapANSI);
F(AskKeyMapDefault);
static const u16 gt_lvo[] = { 30, 36, 42, 48, 54, 60, 66, 72, 78, 84, 90, 96, 102, 108, 114, 120, 126, 132, 138, 144,
                              174 };
static void (*const gt_fn[])(void) = { zs_CreateGadgetA, zs_FreeGadgets, zs_Ret0, zs_CreateMenusA, zs_FreeMenus,
    zs_Ret1, zs_Ret1, zs_GT_GetIMsg, zs_GT_ReplyIMsg, zs_rts, zs_rts, zs_rts, zs_RetA1, zs_RetA1, zs_CreateContext,
    zs_rts, zs_GetVisualInfoA, zs_FreeVisualInfo, zs_rts, zs_rts, zs_Ret0 };
_Static_assert(sizeof gt_lvo / 2 == sizeof gt_fn / sizeof gt_fn[0], "gadtools table");
const u16 *const zb_gt_lvo = gt_lvo;
void (*const *const zb_gt_fn)(void) = gt_fn;
const u16 zb_gt_n = sizeof gt_lvo / 2;

static const u16 km_lvo[] = { 30, 36, 42, 48 };
static void (*const km_fn[])(void) = { zs_rts, zs_AskKeyMapDefault, zs_RawKeyConvert, zs_MapANSI };
const u16 *const zb_km_lvo = km_lvo;
void (*const *const zb_km_fn)(void) = km_fn;
const u16 zb_km_n = sizeof km_lvo / 2;

/* AskKeyMapDefault (-36): a KeyMap whose tables are empty (ZINE converts with its own USA one) */
u32 c_AskKeyMapDefault(void)
{
    static u32 km[8];
    return (u32)km;
}
