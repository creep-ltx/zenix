/* SPDX-License-Identifier: GPL-2.0-only */
/* Made by tools/zine-ndk-names.py from the NDK 3.2's .fd files: function names and offsets only (owner answer
 * 4). Do not edit: run the tool. Asked by `zine` when lvo.c (the RKRM's V37 tables) has no name. */
#include "lvo.h"

static const char n_exec[] =
    "ObtainQuickVector\0execPrivate14\0execPrivate15\0execPrivate16\0execPrivate17\0execPrivate18\0"
    "execPrivate19\0NewMinList\0execPrivate20\0execPrivate21\0execPrivate22\0execPrivate23\0execPrivate24\0"
    "execPrivate25\0execPrivate26\0execPrivate27\0execPrivate28\0execPrivate29\0execPrivate30\0"
    "execPrivate31\0execPrivate32\0execPrivate33\0";
static const char n_dos[] =
    "ExAllEnd\0SetOwner\0\0\0VolumeRequestHook\0dosPrivate7\0GetCurrentDir\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
    "PutErrStr\0ErrorOutput\0SelectError\0\0DoShellMethodTagList\0ScanStackToken\0";
static const char n_graphics[] =
    "CalcIVG\0AttachPalExtra\0ObtainBestPenA\0graphicsPrivate6\0SetRGB32\0GetAPen\0GetBPen\0GetDrMd\0"
    "GetOutlinePen\0LoadRGB32\0SetChipRev\0SetABPenDrMd\0GetRGB32\0graphicsPrivate7\0graphicsPrivate8\0"
    "AllocBitMap\0FreeBitMap\0GetExtSpriteA\0CoerceMode\0ChangeVPBitMap\0ReleasePen\0ObtainPen\0"
    "GetBitMapAttr\0AllocDBufInfo\0FreeDBufInfo\0SetOutlinePen\0SetWriteMask\0SetMaxPen\0SetRGB32CM\0"
    "ScrollRasterBF\0FindColor\0graphicsPrivate9\0AllocSpriteDataA\0ChangeExtSpriteA\0FreeSpriteData\0"
    "SetRPAttrsA\0GetRPAttrsA\0BestModeIDA\0WriteChunkyPixels\0";
static const char n_intuition[] =
    "\0\0\0\0\0\0AllocScreenBuffer\0FreeScreenBuffer\0ChangeScreenBuffer\0ScreenDepth\0ScreenPosition\0"
    "ScrollWindowRaster\0LendMenus\0DoGadgetMethodA\0SetWindowPointerA\0TimedDisplayAlert\0HelpControl\0"
    "ShowWindow\0HideWindow\0";
static const char n_utility[] =
    "FindTagItem\0GetTagData\0PackBoolTags\0NextTagItem\0FilterTagChanges\0MapTags\0AllocateTagItems\0"
    "CloneTagItems\0FreeTagItems\0RefreshTagItemClones\0TagInArray\0FilterTagItems\0CallHookPkt\0\0\0"
    "Amiga2Date\0Date2Amiga\0CheckDate\0SMult32\0UMult32\0SDivMod32\0UDivMod32\0Stricmp\0Strnicmp\0ToUpper\0"
    "ToLower\0ApplyTagChanges\0\0SMult64\0UMult64\0PackStructureTags\0UnpackStructureTags\0AddNamedObject\0"
    "AllocNamedObjectA\0AttemptRemNamedObject\0FindNamedObject\0FreeNamedObject\0NamedObjectName\0"
    "ReleaseNamedObject\0RemNamedObject\0GetUniqueID\0\0\0\0\0\0\0VSNPrintf\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
    "\0\0\0\0Strncpy\0Strncat\0SDivMod64\0UDivMod64\0";
static const char n_mathffp[] =
    "SPFix\0SPFlt\0SPCmp\0SPTst\0SPAbs\0SPNeg\0SPAdd\0SPSub\0SPMul\0SPDiv\0SPFloor\0SPCeil\0";
static const char n_mathtrans[] =
    "SPAtan\0SPSin\0SPCos\0SPTan\0SPSincos\0SPSinh\0SPCosh\0SPTanh\0SPExp\0SPLog\0SPPow\0SPSqrt\0SPTieee\0"
    "SPFieee\0SPAsin\0SPAcos\0SPLog10\0";
static const char n_mathieeedoubbas[] =
    "IEEEDPFix\0IEEEDPFlt\0IEEEDPCmp\0IEEEDPTst\0IEEEDPAbs\0IEEEDPNeg\0IEEEDPAdd\0IEEEDPSub\0IEEEDPMul\0"
    "IEEEDPDiv\0IEEEDPFloor\0IEEEDPCeil\0";
static const char n_mathieeedoubtrans[] =
    "IEEEDPAtan\0IEEEDPSin\0IEEEDPCos\0IEEEDPTan\0IEEEDPSincos\0IEEEDPSinh\0IEEEDPCosh\0IEEEDPTanh\0"
    "IEEEDPExp\0IEEEDPLog\0IEEEDPPow\0IEEEDPSqrt\0IEEEDPTieee\0IEEEDPFieee\0IEEEDPAsin\0IEEEDPAcos\0"
    "IEEEDPLog10\0";
static const char n_mathieeesingbas[] =
    "IEEESPFix\0IEEESPFlt\0IEEESPCmp\0IEEESPTst\0IEEESPAbs\0IEEESPNeg\0IEEESPAdd\0IEEESPSub\0IEEESPMul\0"
    "IEEESPDiv\0IEEESPFloor\0IEEESPCeil\0";
static const char n_mathieeesingtrans[] =
    "IEEESPAtan\0IEEESPSin\0IEEESPCos\0IEEESPTan\0IEEESPSincos\0IEEESPSinh\0IEEESPCosh\0IEEESPTanh\0"
    "IEEESPExp\0IEEESPLog\0IEEESPPow\0IEEESPSqrt\0IEEESPTieee\0IEEESPFieee\0IEEESPAsin\0IEEESPAcos\0"
    "IEEESPLog10\0";
static const char n_diskfont[] =
    "OpenDiskFont\0AvailFonts\0NewFontContents\0DisposeFontContents\0NewScaledDiskFont\0GetDiskFontCtrl\0"
    "SetDiskFontCtrlA\0EOpenEngine\0ECloseEngine\0ESetInfoA\0EObtainInfoA\0EReleaseInfoA\0OpenOutlineFont\0"
    "CloseOutlineFont\0WriteFontContents\0WriteDiskFontHeaderA\0ObtainCharsetInfo\0";

static const struct zl_lib t40[] = {
    { "exec.library", 786, 22, n_exec },
    { "dos.library", 990, 29, n_dos },
    { "graphics.library", 828, 39, n_graphics },
    { "intuition.library", 732, 19, n_intuition },
    { "utility.library", 30, 72, n_utility },
    { "mathffp.library", 30, 12, n_mathffp },
    { "mathtrans.library", 30, 17, n_mathtrans },
    { "mathieeedoubbas.library", 30, 12, n_mathieeedoubbas },
    { "mathieeedoubtrans.library", 30, 17, n_mathieeedoubtrans },
    { "mathieeesingbas.library", 30, 12, n_mathieeesingbas },
    { "mathieeesingtrans.library", 30, 17, n_mathieeesingtrans },
    { "diskfont.library", 30, 17, n_diskfont },
};

const char *zl_name40(const char *lib, unsigned offset)
{
    for (unsigned i = 0; i < sizeof t40 / sizeof t40[0]; i++) {
        const struct zl_lib *l = &t40[i];
        const char *a = l->name, *b = lib;
        while (*a && *a == *b)
            a++, b++;
        if (*a || *b)
            continue;
        if (offset < l->first || (offset - l->first) % 6 || (offset - l->first) / 6 >= l->n)
            return 0;
        const char *p = l->names;
        for (unsigned k = (offset - l->first) / 6; k; k--)
            while (*p++)
                ;
        return *p ? p : 0;
    }
    return 0;
}
