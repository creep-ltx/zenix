/* SPDX-License-Identifier: GPL-2.0-only */
/* Made by tools/zine-tables.py lvo from the RKRM Includes and Autodocs, 3rd edition (the 2.04 = V37
 * interface): its Function Offset Reference (the .fd files). Names and offsets only. Do not edit: run the
 * tool (docs/d29-zine.md, step 1 as built). V39/V40 functions are not here: needs the NDK (owner question 4). */
#include "lvo.h"

static const char n_exec[] =
    "Supervisor\0ExitIntr\0Schedule\0Reschedule\0Switch\0Dispatch\0Exception\0InitCode\0InitStruct\0MakeLibrary\0"
    "MakeFunctions\0FindResident\0InitResident\0Alert\0Debug\0Disable\0Enable\0Forbid\0Permit\0SetSR\0SuperState\0"
    "UserState\0SetIntVector\0AddIntServer\0RemIntServer\0Cause\0Allocate\0Deallocate\0AllocMem\0AllocAbs\0FreeMem\0"
    "AvailMem\0AllocEntry\0FreeEntry\0Insert\0AddHead\0AddTail\0Remove\0RemHead\0RemTail\0Enqueue\0FindName\0"
    "AddTask\0RemTask\0FindTask\0SetTaskPri\0SetSignal\0SetExcept\0Wait\0Signal\0AllocSignal\0FreeSignal\0"
    "AllocTrap\0FreeTrap\0AddPort\0RemPort\0PutMsg\0GetMsg\0ReplyMsg\0WaitPort\0FindPort\0AddLibrary\0RemLibrary\0"
    "OldOpenLibrary\0CloseLibrary\0SetFunction\0SumLibrary\0AddDevice\0RemDevice\0OpenDevice\0CloseDevice\0DoIO\0"
    "SendIO\0CheckIO\0WaitIO\0AbortIO\0AddResource\0RemResource\0OpenResource\0RawIOInit\0RawMayGetChar\0"
    "RawPutChar\0RawDoFmt\0GetCC\0TypeOfMem\0Procure\0Vacate\0OpenLibrary\0InitSemaphore\0ObtainSemaphore\0"
    "ReleaseSemaphore\0AttemptSemaphore\0ObtainSemaphoreList\0ReleaseSemaphoreList\0FindSemaphore\0AddSemaphore\0"
    "RemSemaphore\0SumKickData\0AddMemList\0CopyMem\0CopyMemQuick\0CacheClearU\0CacheClearE\0CacheControl\0"
    "CreateIORequest\0DeleteIORequest\0CreateMsgPort\0DeleteMsgPort\0ObtainSemaphoreShared\0AllocVec\0FreeVec\0"
    "CreatePrivatePool\0DeletePrivatePool\0AllocPooled\0FreePooled\0ExecReserved00\0ColdReboot\0StackSwap\0"
    "ChildFree\0ChildOrphan\0ChildStatus\0ChildWait\0ExecReserved01\0ExecReserved02\0ExecReserved03\0"
    "ExecReserved04\0";
static const char n_dos[] =
    "Open\0Close\0Read\0Write\0Input\0Output\0Seek\0DeleteFile\0Rename\0Lock\0UnLock\0DupLock\0Examine\0ExNext\0"
    "Info\0CreateDir\0CurrentDir\0IoErr\0CreateProc\0Exit\0LoadSeg\0UnLoadSeg\0ClearVec\0NoReqLoadSeg\0DeviceProc\0"
    "SetComment\0SetProtection\0DateStamp\0Delay\0WaitForChar\0ParentDir\0IsInteractive\0Execute\0AllocDosObject\0"
    "FreeDosObject\0DoPkt\0SendPkt\0WaitPkt\0ReplyPkt\0AbortPkt\0LockRecord\0LockRecords\0UnLockRecord\0"
    "UnLockRecords\0SelectInput\0SelectOutput\0FGetC\0FPutC\0UnGetC\0FRead\0FWrite\0FGets\0FPuts\0VFWritef\0"
    "VFPrintf\0Flush\0SetVBuf\0DupLockFromFH\0OpenFromLock\0ParentOfFH\0ExamineFH\0SetFileDate\0NameFromLock\0"
    "NameFromFH\0SplitName\0SameLock\0SetMode\0ExAll\0ReadLink\0MakeLink\0ChangeMode\0SetFileSize\0SetIoErr\0Fault\0"
    "PrintFault\0ErrorReport\0\0Cli\0CreateNewProc\0RunCommand\0GetConsoleTask\0SetConsoleTask\0GetFileSysTask\0"
    "SetFileSysTask\0GetArgStr\0SetArgStr\0FindCliProc\0MaxCli\0SetCurrentDirName\0GetCurrentDirName\0"
    "SetProgramName\0GetProgramName\0SetPrompt\0GetPrompt\0SetProgramDir\0GetProgramDir\0SystemTagList\0AssignLock\0"
    "AssignLate\0AssignPath\0AssignAdd\0RemAssignList\0GetDeviceProc\0FreeDeviceProc\0LockDosList\0UnLockDosList\0"
    "AttemptLockDosList\0RemDosEntry\0AddDosEntry\0FindDosEntry\0NextDosEntry\0MakeDosEntry\0FreeDosEntry\0"
    "IsFileSystem\0Format\0Relabel\0Inhibit\0AddBuffers\0CompareDates\0DateToStr\0StrToDate\0InternalLoadSeg\0"
    "InternalUnLoadSeg\0NewLoadSeg\0AddSegment\0FindSegment\0RemSegment\0CheckSignal\0ReadArgs\0FindArg\0ReadItem\0"
    "StrToLong\0MatchFirst\0MatchNext\0MatchEnd\0ParsePattern\0MatchPattern\0DosNameFromAnchor\0FreeArgs\0\0"
    "FilePart\0PathPart\0AddPart\0StartNotify\0EndNotify\0SetVar\0GetVar\0DeleteVar\0FindVar\0CliInit\0"
    "CliInitNewcli\0CliInitRun\0WriteChars\0PutStr\0VPrintf\0\0ParsePatternNoCase\0MatchPatternNoCase\0"
    "DosGetString\0SameDevice\0";
static const char n_graphics[] =
    "BltBitMap\0BltTemplate\0ClearEOL\0ClearScreen\0TextLength\0Text\0SetFont\0OpenFont\0CloseFont\0AskSoftStyle\0"
    "SetSoftStyle\0AddBob\0AddVSprite\0DoCollision\0DrawGList\0InitGels\0InitMasks\0RemIBob\0RemVSprite\0"
    "SetCollision\0SortGList\0AddAnimOb\0Animate\0GetGBuffers\0InitGMasks\0DrawEllipse\0AreaEllipse\0LoadRGB4\0"
    "InitRastPort\0InitVPort\0MrgCop\0MakeVPort\0LoadView\0WaitBlit\0SetRast\0Move\0Draw\0AreaMove\0AreaDraw\0"
    "AreaEnd\0WaitTOF\0QBlit\0InitArea\0SetRGB4\0QBSBlit\0BltClear\0RectFill\0BltPattern\0ReadPixel\0WritePixel\0"
    "Flood\0PolyDraw\0SetAPen\0SetBPen\0SetDrMd\0InitView\0CBump\0CMove\0CWait\0VBeamPos\0InitBitMap\0ScrollRaster\0"
    "WaitBOVP\0GetSprite\0FreeSprite\0ChangeSprite\0MoveSprite\0LockLayerRom\0UnlockLayerRom\0SyncSBitMap\0"
    "CopySBitMap\0OwnBlitter\0DisownBlitter\0InitTmpRas\0AskFont\0AddFont\0RemFont\0AllocRaster\0FreeRaster\0"
    "AndRectRegion\0OrRectRegion\0NewRegion\0ClearRectRegion\0ClearRegion\0DisposeRegion\0FreeVPortCopLists\0"
    "FreeCopList\0ClipBlit\0XorRectRegion\0FreeCprList\0GetColorMap\0FreeColorMap\0GetRGB4\0ScrollVPort\0"
    "UCopperListInit\0FreeGBuffers\0BltBitMapRastPort\0OrRegionRegion\0XorRegionRegion\0AndRegionRegion\0SetRGB4CM\0"
    "BltMaskBitMapRastPort\0\0\0AttemptLockLayerRom\0GfxNew\0GfxFree\0GfxAssociate\0BitMapScale\0ScalerDiv\0"
    "TextExtent\0TextFit\0GfxLookUp\0VideoControl\0OpenMonitor\0CloseMonitor\0FindDisplayInfo\0NextDisplayInfo\0"
    "AddDisplayInfo\0AddDisplayInfoData\0SetDisplayInfoData\0GetDisplayInfoData\0FontExtent\0ReadPixelLine8\0"
    "WritePixelLine8\0ReadPixelArray8\0WritePixelArray8\0GetVPModeID\0ModeNotAvailable\0WeighTAMatch\0EraseRect\0"
    "ExtendFont\0StripFont\0";
static const char n_intuition[] =
    "OpenIntuition\0Intuition\0AddGadget\0ClearDMRequest\0ClearMenuStrip\0ClearPointer\0CloseScreen\0CloseWindow\0"
    "CloseWorkBench\0CurrentTime\0DisplayAlert\0DisplayBeep\0DoubleClick\0DrawBorder\0DrawImage\0EndRequest\0"
    "GetDefPrefs\0GetPrefs\0InitRequester\0ItemAddress\0ModifyIDCMP\0ModifyProp\0MoveScreen\0MoveWindow\0OffGadget\0"
    "OffMenu\0OnGadget\0OnMenu\0OpenScreen\0OpenWindow\0OpenWorkBench\0PrintIText\0RefreshGadgets\0RemoveGadget\0"
    "ReportMouse\0Request\0ScreenToBack\0ScreenToFront\0SetDMRequest\0SetMenuStrip\0SetPointer\0SetWindowTitles\0"
    "ShowTitle\0SizeWindow\0ViewAddress\0ViewPortAddress\0WindowToBack\0WindowToFront\0WindowLimits\0SetPrefs\0"
    "IntuiTextLength\0WBenchToBack\0WBenchToFront\0AutoRequest\0BeginRefresh\0BuildSysRequest\0EndRefresh\0"
    "FreeSysRequest\0MakeScreen\0RemakeDisplay\0RethinkDisplay\0AllocRemember\0AlohaWorkbench\0FreeRemember\0"
    "LockIBase\0UnlockIBase\0GetScreenData\0RefreshGList\0AddGList\0RemoveGList\0ActivateWindow\0"
    "RefreshWindowFrame\0ActivateGadget\0NewModifyProp\0QueryOverscan\0MoveWindowInFrontOf\0ChangeWindowBox\0"
    "SetEditHook\0SetMouseQueue\0ZipWindow\0LockPubScreen\0UnlockPubScreen\0LockPubScreenList\0UnlockPubScreenList\0"
    "NextPubScreen\0SetDefaultPubScreen\0SetPubScreenModes\0PubScreenStatus\0ObtainGIRPort\0ReleaseGIRPort\0"
    "GadgetMouse\0SetIPrefs\0GetDefaultPubScreen\0EasyRequestArgs\0BuildEasyRequestArgs\0SysReqHandler\0"
    "OpenWindowTagList\0OpenScreenTagList\0DrawImageState\0PointInImage\0EraseImage\0NewObjectA\0DisposeObject\0"
    "SetAttrsA\0GetAttr\0SetGadgetAttrsA\0NextObject\0FindClass\0MakeClass\0AddClass\0GetScreenDrawInfo\0"
    "FreeScreenDrawInfo\0ResetMenuStrip\0RemoveClass\0FreeClass\0lockPubClass\0unlockPubClass\0";
static const char n_cia[] =
    "AddICRVector\0RemICRVector\0AbleICR\0SetICR\0";
static const char n_potgo[] =
    "AllocPotBits\0FreePotBits\0WritePotgo\0";

const struct zl_lib zl_libs[] = {
    { "exec.library", 30, 126, n_exec },
    { "dos.library", 30, 160, n_dos },
    { "graphics.library", 30, 133, n_graphics },
    { "intuition.library", 30, 117, n_intuition },
    { "ciaa.resource", 6, 4, n_cia },
    { "ciab.resource", 6, 4, n_cia },
    { "potgo.resource", 6, 3, n_potgo },
};

const char *zl_name(const char *lib, unsigned offset)
{
    for (int i = 0; i < ZL_NLIBS; i++) {
        const struct zl_lib *l = &zl_libs[i];
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
