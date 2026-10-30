/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
/* ZINE's AmigaOS structure offsets (docs/d29-zine.md section 5): the V37 (2.04) interface, every number from the RKRM
 * Includes and Autodocs, 3rd edition, "Structure Reference" (names and numbers only). tools/zine-tables.py check
 * compares every ZO_ line with that text (the host test tools/host/zine-test.sh, when ZENIX_MANUALS names the
 * manuals' text folder). The form: ZO_<structure>_<field> = the byte offset, ZO_<structure>_SIZEOF = its size.
 *
 * Not here, because no allowed source has them (needs the NDK, owner question 4): ExecBase's V39 fields (ex_MemHandlers
 * and the rest; ZO_ExecBase_SIZEOF is the V37 size), GfxBase's V39 fields (MemType, the AA bits of ChipRevBits0;
 * ZO_GfxBase_SIZEOF is the V37 size) and the 68060 bit of AttnFlags. DosList's volume fields come with step 4. */

/* Node */
#define ZO_Node_SIZEOF 14
#define ZO_Node_ln_Succ 0
#define ZO_Node_ln_Pred 4
#define ZO_Node_ln_Type 8
#define ZO_Node_ln_Pri 9
#define ZO_Node_ln_Name 10

/* List */
#define ZO_List_SIZEOF 14
#define ZO_List_lh_Head 0
#define ZO_List_lh_Tail 4
#define ZO_List_lh_TailPred 8
#define ZO_List_lh_Type 12

/* MinList */
#define ZO_MinList_SIZEOF 12
#define ZO_MinList_mlh_Tail 4

/* Library */
#define ZO_Library_SIZEOF 34
#define ZO_Library_lib_Node 0
#define ZO_Library_lib_Flags 14
#define ZO_Library_lib_NegSize 16
#define ZO_Library_lib_PosSize 18
#define ZO_Library_lib_Version 20
#define ZO_Library_lib_Revision 22
#define ZO_Library_lib_IdString 24
#define ZO_Library_lib_Sum 28
#define ZO_Library_lib_OpenCnt 32

/* ExecBase */
#define ZO_ExecBase_SIZEOF 616
#define ZO_ExecBase_LibNode 0
#define ZO_ExecBase_SoftVer 34
#define ZO_ExecBase_LowMemChkSum 36
#define ZO_ExecBase_ChkBase 38
#define ZO_ExecBase_ColdCapture 42
#define ZO_ExecBase_CoolCapture 46
#define ZO_ExecBase_WarmCapture 50
#define ZO_ExecBase_SysStkUpper 54
#define ZO_ExecBase_SysStkLower 58
#define ZO_ExecBase_MaxLocMem 62
#define ZO_ExecBase_DebugEntry 66
#define ZO_ExecBase_DebugData 70
#define ZO_ExecBase_AlertData 74
#define ZO_ExecBase_MaxExtMem 78
#define ZO_ExecBase_ChkSum 82
#define ZO_ExecBase_IntVects 84
#define ZO_ExecBase_ThisTask 276
#define ZO_ExecBase_IdleCount 280
#define ZO_ExecBase_DispCount 284
#define ZO_ExecBase_Quantum 288
#define ZO_ExecBase_Elapsed 290
#define ZO_ExecBase_SysFlags 292
#define ZO_ExecBase_IDNestCnt 294
#define ZO_ExecBase_TDNestCnt 295
#define ZO_ExecBase_AttnFlags 296
#define ZO_ExecBase_AttnResched 298
#define ZO_ExecBase_ResModules 300
#define ZO_ExecBase_TaskTrapCode 304
#define ZO_ExecBase_TaskExceptCode 308
#define ZO_ExecBase_TaskExitCode 312
#define ZO_ExecBase_TaskSigAlloc 316
#define ZO_ExecBase_TaskTrapAlloc 320
#define ZO_ExecBase_MemList 322
#define ZO_ExecBase_ResourceList 336
#define ZO_ExecBase_DeviceList 350
#define ZO_ExecBase_IntrList 364
#define ZO_ExecBase_LibList 378
#define ZO_ExecBase_PortList 392
#define ZO_ExecBase_TaskReady 406
#define ZO_ExecBase_TaskWait 420
#define ZO_ExecBase_SoftInts 434
#define ZO_ExecBase_LastAlert 514
#define ZO_ExecBase_VBlankFrequency 530
#define ZO_ExecBase_PowerSupplyFrequency 531
#define ZO_ExecBase_SemaphoreList 532
#define ZO_ExecBase_KickMemPtr 546
#define ZO_ExecBase_KickTagPtr 550
#define ZO_ExecBase_KickCheckSum 554
#define ZO_ExecBase_ex_EClockFrequency 568
#define ZO_ExecBase_ex_CacheControl 572

/* MemHeader */
#define ZO_MemHeader_SIZEOF 32
#define ZO_MemHeader_mh_Node 0
#define ZO_MemHeader_mh_Attributes 14
#define ZO_MemHeader_mh_First 16
#define ZO_MemHeader_mh_Lower 20
#define ZO_MemHeader_mh_Upper 24
#define ZO_MemHeader_mh_Free 28

/* MemChunk */
#define ZO_MemChunk_SIZEOF 8
#define ZO_MemChunk_mc_Next 0
#define ZO_MemChunk_mc_Bytes 4

/* MemList */
#define ZO_MemList_SIZEOF 24
#define ZO_MemList_ml_Node 0
#define ZO_MemList_ml_NumEntries 14
#define ZO_MemList_ml_ME 16

/* MemEntry */
#define ZO_MemEntry_SIZEOF 8
#define ZO_MemEntry_me_Length 4

/* MsgPort */
#define ZO_MsgPort_SIZEOF 34
#define ZO_MsgPort_mp_Node 0
#define ZO_MsgPort_mp_Flags 14
#define ZO_MsgPort_mp_SigBit 15
#define ZO_MsgPort_mp_SigTask 16
#define ZO_MsgPort_mp_MsgList 20

/* Message */
#define ZO_Message_SIZEOF 20
#define ZO_Message_mn_Node 0
#define ZO_Message_mn_ReplyPort 14
#define ZO_Message_mn_Length 18

/* IORequest */
#define ZO_IORequest_SIZEOF 32
#define ZO_IORequest_io_Message 0
#define ZO_IORequest_io_Device 20
#define ZO_IORequest_io_Unit 24
#define ZO_IORequest_io_Command 28
#define ZO_IORequest_io_Flags 30
#define ZO_IORequest_io_Error 31

/* Interrupt */
#define ZO_Interrupt_SIZEOF 22
#define ZO_Interrupt_is_Node 0
#define ZO_Interrupt_is_Data 14
#define ZO_Interrupt_is_Code 18

/* IntVector */
#define ZO_IntVector_iv_Data 0
#define ZO_IntVector_iv_Code 4
#define ZO_IntVector_iv_Node 8

/* SignalSemaphore */
#define ZO_SignalSemaphore_SIZEOF 46
#define ZO_SignalSemaphore_ss_Link 0
#define ZO_SignalSemaphore_ss_NestCount 14
#define ZO_SignalSemaphore_ss_WaitQueue 16
#define ZO_SignalSemaphore_ss_MultipleLink 28
#define ZO_SignalSemaphore_ss_Owner 40
#define ZO_SignalSemaphore_ss_QueueCount 44

/* Resident */
#define ZO_Resident_SIZEOF 26
#define ZO_Resident_rt_MatchWord 0
#define ZO_Resident_rt_MatchTag 2
#define ZO_Resident_rt_EndSkip 6
#define ZO_Resident_rt_Flags 10
#define ZO_Resident_rt_Version 11
#define ZO_Resident_rt_Type 12
#define ZO_Resident_rt_Pri 13
#define ZO_Resident_rt_Name 14
#define ZO_Resident_rt_IdString 18
#define ZO_Resident_rt_Init 22

/* Task */
#define ZO_Task_SIZEOF 92
#define ZO_Task_tc_Node 0
#define ZO_Task_tc_Flags 14
#define ZO_Task_tc_State 15
#define ZO_Task_tc_IDNestCnt 16
#define ZO_Task_tc_TDNestCnt 17
#define ZO_Task_tc_SigAlloc 18
#define ZO_Task_tc_SigWait 22
#define ZO_Task_tc_SigRecvd 26
#define ZO_Task_tc_SigExcept 30
#define ZO_Task_tc_TrapAlloc 34
#define ZO_Task_tc_TrapAble 36
#define ZO_Task_tc_ExceptData 38
#define ZO_Task_tc_ExceptCode 42
#define ZO_Task_tc_TrapData 46
#define ZO_Task_tc_TrapCode 50
#define ZO_Task_tc_SPReg 54
#define ZO_Task_tc_SPLower 58
#define ZO_Task_tc_SPUpper 62
#define ZO_Task_tc_Switch 66
#define ZO_Task_tc_Launch 70
#define ZO_Task_tc_MemEntry 74
#define ZO_Task_tc_UserData 88

/* Process */
#define ZO_Process_SIZEOF 228
#define ZO_Process_pr_Task 0
#define ZO_Process_pr_MsgPort 92
#define ZO_Process_pr_Pad 126
#define ZO_Process_pr_SegList 128
#define ZO_Process_pr_StackSize 132
#define ZO_Process_pr_GlobVec 136
#define ZO_Process_pr_TaskNum 140
#define ZO_Process_pr_StackBase 144
#define ZO_Process_pr_Result2 148
#define ZO_Process_pr_CurrentDir 152
#define ZO_Process_pr_CIS 156
#define ZO_Process_pr_COS 160
#define ZO_Process_pr_ConsoleTask 164
#define ZO_Process_pr_FileSystemTask 168
#define ZO_Process_pr_CLI 172
#define ZO_Process_pr_ReturnAddr 176
#define ZO_Process_pr_PktWait 180
#define ZO_Process_pr_WindowPtr 184
#define ZO_Process_pr_HomeDir 188
#define ZO_Process_pr_Flags 192
#define ZO_Process_pr_ExitCode 196
#define ZO_Process_pr_ExitData 200
#define ZO_Process_pr_Arguments 204
#define ZO_Process_pr_LocalVars 208
#define ZO_Process_pr_ShellPrivate 220
#define ZO_Process_pr_CES 224

/* CommandLineInterface */
#define ZO_CommandLineInterface_cli_Result2 0
#define ZO_CommandLineInterface_cli_SetName 4
#define ZO_CommandLineInterface_cli_CommandDir 8
#define ZO_CommandLineInterface_cli_ReturnCode 12
#define ZO_CommandLineInterface_cli_CommandName 16
#define ZO_CommandLineInterface_cli_FailLevel 20
#define ZO_CommandLineInterface_cli_Prompt 24
#define ZO_CommandLineInterface_cli_StandardInput 28
#define ZO_CommandLineInterface_cli_CurrentInput 32
#define ZO_CommandLineInterface_cli_CommandFile 36
#define ZO_CommandLineInterface_cli_Interactive 40
#define ZO_CommandLineInterface_cli_Background 44
#define ZO_CommandLineInterface_cli_CurrentOutput 48
#define ZO_CommandLineInterface_cli_DefaultStack 52
#define ZO_CommandLineInterface_cli_StandardOutput 56
#define ZO_CommandLineInterface_cli_Module 60

/* DosLibrary */
#define ZO_DosLibrary_SIZEOF 66
#define ZO_DosLibrary_dl_lib 0
#define ZO_DosLibrary_dl_Root 34
#define ZO_DosLibrary_dl_GV 38
#define ZO_DosLibrary_dl_A2 42
#define ZO_DosLibrary_dl_A5 46
#define ZO_DosLibrary_dl_A6 50
#define ZO_DosLibrary_dl_Errors 54
#define ZO_DosLibrary_dl_TimeReq 58
#define ZO_DosLibrary_dl_UtilityBase 62

/* RootNode */
#define ZO_RootNode_SIZEOF 56
#define ZO_RootNode_rn_TaskArray 0
#define ZO_RootNode_rn_ConsoleSegment 4
#define ZO_RootNode_rn_Time 8
#define ZO_RootNode_rn_RestartSeg 20
#define ZO_RootNode_rn_Info 24
#define ZO_RootNode_rn_FileHandlerSegment 28
#define ZO_RootNode_rn_CliList 32
#define ZO_RootNode_rn_BootProc 44
#define ZO_RootNode_rn_ShellSegment 48
#define ZO_RootNode_rn_Flags 52

/* DosInfo */
#define ZO_DosInfo_di_McName 0
#define ZO_DosInfo_di_DevInfo 4
#define ZO_DosInfo_di_Devices 8
#define ZO_DosInfo_di_Handlers 12
#define ZO_DosInfo_di_NetHand 16

/* DosList */
#define ZO_DosList_SIZEOF 44
#define ZO_DosList_dol_Next 0
#define ZO_DosList_dol_Type 4
#define ZO_DosList_dol_Task 8
#define ZO_DosList_dol_Lock 12
#define ZO_DosList_dol_misc 16
#define ZO_DosList_dol_Name 40

/* FileLock */
#define ZO_FileLock_SIZEOF 20
#define ZO_FileLock_fl_Link 0
#define ZO_FileLock_fl_Key 4
#define ZO_FileLock_fl_Access 8
#define ZO_FileLock_fl_Task 12
#define ZO_FileLock_fl_Volume 16

/* FileHandle */
#define ZO_FileHandle_SIZEOF 44
#define ZO_FileHandle_fh_Link 0
#define ZO_FileHandle_fh_Port 4
#define ZO_FileHandle_fh_Type 8
#define ZO_FileHandle_fh_Buf 12
#define ZO_FileHandle_fh_Pos 16
#define ZO_FileHandle_fh_End 20
#define ZO_FileHandle_fh_Funcs 24
#define ZO_FileHandle_fh_Func2 28
#define ZO_FileHandle_fh_Func3 32
#define ZO_FileHandle_fh_Args 36
#define ZO_FileHandle_fh_Arg2 40

/* FileInfoBlock */
#define ZO_FileInfoBlock_SIZEOF 260
#define ZO_FileInfoBlock_fib_DiskKey 0
#define ZO_FileInfoBlock_fib_DirEntryType 4
#define ZO_FileInfoBlock_fib_FileName 8
#define ZO_FileInfoBlock_fib_Protection 116
#define ZO_FileInfoBlock_fib_EntryType 120
#define ZO_FileInfoBlock_fib_Size 124
#define ZO_FileInfoBlock_fib_NumBlocks 128
#define ZO_FileInfoBlock_fib_Date 132
#define ZO_FileInfoBlock_fib_Comment 144
#define ZO_FileInfoBlock_fib_Reserved 224

/* DateStamp */
#define ZO_DateStamp_SIZEOF 12
#define ZO_DateStamp_ds_Days 0
#define ZO_DateStamp_ds_Minute 4
#define ZO_DateStamp_ds_Tick 8

/* GfxBase */
#define ZO_GfxBase_SIZEOF 422
#define ZO_GfxBase_LibNode 0
#define ZO_GfxBase_ActiView 34
#define ZO_GfxBase_copinit 38
#define ZO_GfxBase_cia 42
#define ZO_GfxBase_blitter 46
#define ZO_GfxBase_LOFlist 50
#define ZO_GfxBase_SHFlist 54
#define ZO_GfxBase_blthd 58
#define ZO_GfxBase_blttl 62
#define ZO_GfxBase_bsblthd 66
#define ZO_GfxBase_bsblttl 70
#define ZO_GfxBase_vbsrv 74
#define ZO_GfxBase_timsrv 96
#define ZO_GfxBase_bltsrv 118
#define ZO_GfxBase_TextFonts 140
#define ZO_GfxBase_DefaultFont 154
#define ZO_GfxBase_Modes 158
#define ZO_GfxBase_VBlank 160
#define ZO_GfxBase_Debug 161
#define ZO_GfxBase_BeamSync 162
#define ZO_GfxBase_system_bplcon0 164
#define ZO_GfxBase_SpriteReserved 166
#define ZO_GfxBase_bytereserved 167
#define ZO_GfxBase_Flags 168
#define ZO_GfxBase_BlitLock 170
#define ZO_GfxBase_BlitNest 172
#define ZO_GfxBase_BlitWaitQ 174
#define ZO_GfxBase_BlitOwner 188
#define ZO_GfxBase_TOF_WaitQ 192
#define ZO_GfxBase_DisplayFlags 206
#define ZO_GfxBase_SimpleSprites 208
#define ZO_GfxBase_MaxDisplayRow 212
#define ZO_GfxBase_MaxDisplayColumn 214
#define ZO_GfxBase_NormalDisplayRows 216
#define ZO_GfxBase_NormalDisplayColumns 218
#define ZO_GfxBase_NormalDPMX 220
#define ZO_GfxBase_NormalDPMY 222
#define ZO_GfxBase_LastChanceMemory 224
#define ZO_GfxBase_LCMptr 228
#define ZO_GfxBase_MicrosPerLine 232
#define ZO_GfxBase_MinDisplayColumn 234
#define ZO_GfxBase_ChipRevBits0 236
#define ZO_GfxBase_ActiViewCprSemaphore 410
#define ZO_GfxBase_UtilityBase 414
#define ZO_GfxBase_ExecBase 418

/* View */
#define ZO_View_SIZEOF 18
#define ZO_View_ViewPort 0
#define ZO_View_LOFCprList 4
#define ZO_View_SHFCprList 8
#define ZO_View_DyOffset 12
#define ZO_View_DxOffset 14
#define ZO_View_Modes 16

/* cprlist */
#define ZO_cprlist_SIZEOF 10
#define ZO_cprlist_Next 0
#define ZO_cprlist_start 4
#define ZO_cprlist_MaxCount 8

/* BitMap */
#define ZO_BitMap_SIZEOF 40
#define ZO_BitMap_BytesPerRow 0
#define ZO_BitMap_Rows 2
#define ZO_BitMap_Flags 4
#define ZO_BitMap_Depth 5
#define ZO_BitMap_pad 6
#define ZO_BitMap_Planes 8

/* TextFont */
#define ZO_TextFont_SIZEOF 52
#define ZO_TextFont_tf_Message 0
#define ZO_TextFont_tf_YSize 20
#define ZO_TextFont_tf_Style 22
#define ZO_TextFont_tf_XSize 24
#define ZO_TextFont_tf_Baseline 26
#define ZO_TextFont_tf_BoldSmear 28
#define ZO_TextFont_tf_Accessors 30
#define ZO_TextFont_tf_LoChar 32
#define ZO_TextFont_tf_HiChar 33
#define ZO_TextFont_tf_CharData 34
#define ZO_TextFont_tf_Modulo 38
#define ZO_TextFont_tf_CharLoc 40
#define ZO_TextFont_tf_CharSpace 44
#define ZO_TextFont_tf_CharKern 48

/* TextAttr */
#define ZO_TextAttr_SIZEOF 8
#define ZO_TextAttr_ta_Name 0
#define ZO_TextAttr_ta_YSize 4
#define ZO_TextAttr_ta_Style 6
#define ZO_TextAttr_ta_Flags 7

/* IntuitionBase */
#define ZO_IntuitionBase_SIZEOF 80
#define ZO_IntuitionBase_LibNode 0
#define ZO_IntuitionBase_ViewLord 34
#define ZO_IntuitionBase_ActiveWindow 52
#define ZO_IntuitionBase_ActiveScreen 56
#define ZO_IntuitionBase_FirstScreen 60
#define ZO_IntuitionBase_Flags 64
#define ZO_IntuitionBase_MouseY 68
#define ZO_IntuitionBase_MouseX 70
#define ZO_IntuitionBase_Seconds 72
#define ZO_IntuitionBase_Micros 76
