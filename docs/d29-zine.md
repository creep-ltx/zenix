# D29: ZINE, AmigaOS programs on Zenix (design note, revision 3)

Status: **revision 3, 2026-10-30: a new design, after two independent reviews (section 18), every finding worked in.**
Approved by the owner on 2026-10-29 and 2026-10-30 ([decisions.md](decisions.md), the entries "Amiga demos on Zenix" to
"ZINE runs the program under Zenix, no restarts"); ROADMAP block 3½. It replaces revisions 1-2 (a warm restart into the
ROM's AmigaOS and back; the appendix keeps their gist). Nothing of it is built; step 0 (section 16) measures the one
open fact the way out stands on before step 1 starts. Facts about Zenix are *read* from main (`a125c99`); facts about
AmigaOS come from the public manuals and autodocs (knowledge base `manuals/`: the RKRM Libraries, Devices and Includes
and Autodocs, the Hardware Reference Manual (HRM), the Guru Book), the knowledge base's verified notes and the AmigaOS
3.1 study (aos31-study/), never from AmigaOS source. Marks: *read*, *verified* (a test or a real machine showed it, with
where), *proposed* (our reasoning, to be checked by the step that says so).

## 1. What ZINE is (the owner's requirements)

The owner, 2026-10-30: "This demo runs under Zenix. Zenix just pointing the demo to the hardware that it needs."

- **ZINE is Zenix's own small AmigaOS for programs that take over the machine**, demos first. Zenix stays in charge
  the whole time: no restart, no ROM AmigaOS. Zenix loads the hunk executable itself, gives it an ExecBase at address 4
  with the library calls demos make, and hands it the hardware: chip and fast RAM, the custom chips, the CIAs, the
  interrupts and vectors. When the program ends Zenix puts its own state back and carries on at the prompt.
- `zine PROGRAM [ARGS]`, or `PROGRAM [ARGS]` alone (Zenix sees the hunk magic `00 00 03 F3` and calls `zine`). It
  prints `ZINE: handing the machine to NAME`. Demos, games and tools alike; no subcommands; no disk images.
- Machines **with fast RAM** first (the owner: demo watchers have at least a 68030 and 8 MB fast). The stock A1200
  (chip RAM only) is refused with a message.
- The program gets **nearly all chip RAM and all of Zenix's free fast RAM**; Zenix's own memory stays where it is,
  marked in use in the memory list the program sees, and is checked by a checksum on the way back. If it was damaged,
  Zenix says so and restarts cleanly; the disks were saved before the hand-over.
- What ZINE cannot support (Workbench and MUI programs, anything needing the full AmigaOS) fails cleanly back to Zenix
  with a message; never a damaged disk, never a hang without a way out where one can be avoided.

**The owner's five answers (decisions.md, 2026-10-30) in revision 3:**

| Answer | Still applies? | Meaning now |
|---|---|---|
| 1. Root and the `zine` group, at the Amiga's own console or through Wasabi, never ssh/telnet/serial | yes | the check is in the kernel (7.2); `/bin/zine` needs no set-user-id any more; "through Wasabi" read as typing into the console, `wasabi run` not (put to the owner: question 1) |
| 2. All of Zenix is paused while the program runs | yes | **changes a little:** Zenix is paused *in place* (its processes held, its memory untouched, its code ready), not asleep through a restart |
| 3. Files the program writes are gone afterwards, Zenix's disks untouched | yes, unchanged | the files live in ZINE's in-memory volume (5.6) instead of an FFS image |
| 4. ZINE may use the owner's `68060.library` from `/etc/zine/libs/` (never shipped) | yes | **changes:** ZINE's own exec loads and opens it, not the ROM's AmigaOS; it works if the calls it makes are ones ZINE has (10.1, step 8) |
| 5. Ctrl-Amiga-Amiga during a program means "back to Zenix" | yes, same for the user | the mechanism changes: the keyboard's reset lands in ZINE's own capture code (9.3) instead of a restarted AmigaOS |

## 2. Facts this design stands on

### 2.1 Zenix today (*read* from main)

- **Memory** (`kernel/mem.c`, architecture.md section 5): chip and fast **ranges**, each a run of blocks with a 16-byte
  header (size and used bit, the previous size, the owner, `ZMEM`); a free block holds nothing but its header. Blocks of
  16 KB or more come from the high end of a free block, small ones from the low end, so a fast range is mostly
  `[kernel][small blocks] ... free ... [big blocks]`. Outside the pools: `0`-`0x1FFF` (low memory: `0x500` the copier,
  `0x1000` bootinfo, `0x1100` the Pi hardware table), the kernel image and BSS (at the bottom of the largest fast
  region),
  the RAM disk, the klog area (top 16.5 KB of chip RAM, `KLOG_AREA_SIZE` `0x4200`, which also holds the boot note at
  `+0xF0`). Bootinfo keeps each region's `mh_Attributes` as AmigaOS had them.
- **What uses chip RAM on a fast-RAM machine:** only what asks for `MEM_CHIP`: the console's planes and Copper list
  (D14: about 160 KB at 16 colours), the floppy's track buffer, the beep's 32 bytes, the A3000's 64 KB bounce buffer.
  Processes get fast RAM first.
- **CPU state:** VBR points at the kernel's own table (`vectors_init`), the instruction cache on, the data cache off (no
  MMU to keep DMA coherent), `-msoft-float`; `cache_flush_code` (`start.S`) is the one flush (CACR bit 3 on 020/030,
  `cpusha bc` on 040/060 and Emu68).
- **Interrupts used:** level 2 CIA-A (the 100 Hz tick on timer A and the keyboard's SP; Gayle IDE shares it), level 3
  vertical blank, level 5 serial receive (sending is polled from a ring, `serial.c`); vectors 24-31 without a handler
  are logged once, and a Paula level 1, 4 or 6 among them switched off. CIA-B timers A and B are chained as the
  free-running E-clock counter `hr_time()`; nothing uses the CIAs'
  TOD counters.
- **The display** (`kernel/gfx.c`): `gfx_init` writes `BPLCON0`, `DIWSTRT`, `DIWSTOP`, `COP1LC`, `COPJMP1`, `DMACON`
  (master, bitplanes, Copper, blitter) and `INTENA` (vertical blank) once; the Copper list writes FMODE, `BPLCON3`
  `$0C00`, `BPLCON4` `$0011` (AGA), `BPLCON0`-`2`, the modulos, the fetch window and the colours every frame. Zenix
  never
  writes `BEAMCON0`: PAL or NTSC is what the beam shows (`frame_lines`), the E-clock's rate is VPOSR bit 12. No sprites.
- **Leaving for a reset:** `vfs_sync(VS_REBOOT | VS_BOUNDED)` (kernel/vfs.c) sends `FS_SYNC` to every mount, local ones
  first, network ones last within 2 s, and arms a deadline at which the tick resets the machine; `disk_frozen` (set by
  the reset warning and `SYS_reboot`, never cleared) makes `blk_write` and the floppy fail.
- **Drivers:** the disk drivers busy-wait inside the calling server's system call and the kernel is not preemptive, so
  no disk transfer is in flight while another system call runs. The network drivers are user processes, children of
  netd; the kernel holds their claim, DMA region and quiesce program (`kernel/drv.c`); a driver that ends is quiesced
  and
  netd restarts it (D19). The keyboard: one handshake at `kbd_init`, a timed handshake per byte; the reset warning
  (`$78` twice) syncs the disks through init (`kbd_reset_flush`) while `kbd_isr` holds KDAT, and the tick lets go after
  8 s; the
  PiStorm A1200's keyboard sends it (*verified* 2026-10-24, knowledge base amiga/zenix-pi-drivers-progress.md).
- **Executables:** ZEXE and ZEX2 only; `proc_spawn` reads the header and refuses anything else (-ENOEXEC). `ctty` is
  forced into 1..NCONS, so the console test needs more than `ctty` (7.2).
- **Sizes** (*measured* in the notes cited): the kernel image about 172 KB (`tools/ksize-budget`: 1,024 bytes left on
  the
  mount tests' floppy, the binding limit); Zenix with one login about 680 KB (D19, FS-UAE); netd and its driver about
  146 KB; an NFS client's cache up to 1 MB.

### 2.2 AmigaOS facts (*read*)

- **What a demo calls** (knowledge base demoscene/friendly-takeover.md, *verified* in FS-UAE under Kickstart 3.x;
  research_notes "audio_sizecoding_system.md" section 3, *read*): `OpenLibrary` graphics (and dos, sometimes
  intuition), `FindTask(0)`, `Forbid`, save `gb_ActiView`, `LoadView(NULL)`, `WaitTOF` twice, `OwnBlitter`, read
  `DMACONR`/`INTENAR`/`ADKCONR`, on a 68010+ read the VBR through `Supervisor` (`movec vbr`), `WaitBlit`, `DMACON`
  `$01FF`, `INTENA` `$3FFF`, its own level 3 (and with a CIA replayer level 6) vector at VBR + `$6C` (+ `$78`), or
  `AddIntServer`, or `ciab.resource`'s `AddICRVector`/`AbleICR`; at the end `COP1LC` = `gb_copinit`, `COP2LC` =
  `gb_LOFlist`, the saved values back, `LoadView(old)`, `WaitTOF` twice, `DisownBlitter`, `Permit`. Some read
  `AttnFlags`, `VBlankFrequency`, `gb_DisplayFlags`, `gb_ChipRevBits0`; C programs' start-up code adds dos's
  `Output`/`Write`/`Printf`, `pr_CLI`, `AllocMem`/pools; many demos load data files with dos before taking over.
- **A CLI program's start** (AmigaDOS Manual, RKRM dos): entered with `d0` the length and `a0` the argument text
  (ending in a newline), `(sp)` the return address and `4(sp)` the stack size; it returns its code in `d0`. Its process
  (`FindTask(0)`) has `pr_CLI` set; with `pr_CLI` 0 the start-up code waits for a Workbench message instead.
- **The reset and ColdCapture** (Guru Book ch. 10.1; the Abacus System Programmer's Guide 2.9, quoted in knowledge base
  amiga/gurus-and-exception-frames.md): at a reset the ROM reads the longword at address 4 and believes the ExecBase
  there if `ExecBase + ChkBase` (offset 38) inverts to zero and the word sum from `SoftVer` (34) to `ChkSum` (82) is
  `0xFFFF`; then, "before any memory testing has been performed", it clears `ColdCapture` (42) and jumps to it in
  supervisor mode, `a6` the ExecBase, the way back `jmp (a5)`. On 2.0+ this comes before the chip and ranger memory are
  set up, unless the ExecBase lies in autoconfig memory (then it waits for expansion). Only the 1.x checks are public;
  whether 2.0+ checks more is **not known** (step 0). A ColdCapture that is never cleared would loop; the ROM clears it.
- **Level 7** "is the nonmaskable interrupt and is not generated anywhere in the current system" (HRM, interrupts): only
  an expansion card can raise it (freezer cartridges). Emu68 delivers ARM error interrupts as level 7 (knowledge base
  amiga/emu68-jit-cache-rules.md, *read* from its source).
- **The keyboard reset** (HRM, keyboard): Ctrl-Amiga-Amiga resets the whole machine (CPU, custom chips, CIAs, Gayle,
  the boards); keyboards that have the feature send `$78` first and the computer may hold KDAT low for up to 10 s, but
  the reset comes whatever the computer does. Chip RAM keeps its contents (*verified* on the PiStorm A1200 and in
  FS-UAE, 2026-10-23, knowledge base amiga/gurus-and-exception-frames.md); on Emu68 the top of the highest fast region
  rode a dozen reboots, the bottom was reused by the ROM's start (*verified*, same note). Fast RAM through a keyboard
  reset on the Apollo 1260 is **not known** (step 0).
- **CIA TOD** (HRM, CIA chapter): CIA-A's TOD is a 24-bit 50/60 Hz event counter (VSYNC or the power line), CIA-B's
  counts horizontal syncs; both count on while the CPU does anything, unless written.

## 3. The shape: what lives where

| Part | Where | What it does | Cost |
|---|---|---|---|
| **The call** `SYS_zine` | kernel | who may; stops the processes; the disks synced and their writes held; the network drivers quiesced; the drivers to a known state; saves the CPU's and chips' state; calls the blob; on the way back re-initialises the drivers, puts the clocks right, releases everything | 2-4 KB (*proposed*, step 2 measures with `tools/ksize-check.sh`) |
| **The blob** `/lib/zine/zine.blob` | loaded and relocated by `zine` into a block of its own (a `SYS_memalloc` region), run there in place by the kernel in supervisor mode | the memory lists, saving chip RAM, the checksums, the low page, the small AmigaOS (exec, graphics, dos, intuition, two resources), the hunk loader, the vectors, both ways back | 24-32 KB, only while used |
| **The command** `/bin/zine` | user space | checks the file, builds the in-memory volume of the program's directory, reads and relocates the blob, prints the lines, calls `SYS_zine`, prints the program's output and how it ended, names unsupported calls | 15-20 KB |

**Decided** (the golden path: little kernel cost): the kernel keeps only what must run with Zenix's own state at hand
(processes, mounts, drivers); everything AmigaOS-shaped is in the blob, which costs nothing until `zine` runs. The blob
is a ZEXE image (`tools/mkzexe.py`), relocated by `zine` in user space (a few lines: the base added to each listed
word); its header carries magic `ZINB`, a layout version and a CRC32. The kernel accepts it only from the caller's own
memory, with the kernel's layout version, and flushes the caches before calling it (code was just written). Blob,
kernel and command are built from one source tree and shipped in the base group (D15/D22), so their layouts always
match; during an update the kernel can be newer than the blob for a while (D15 installs the kernel first), and then
`zine` says "ZINE's files and the kernel are from different versions: finish the update first" instead of a bare
-ENOEXEC. The blob runs as trusted code, but no more trusted than the program it hands the machine to (9.5): a member of
the `zine` group who brings their own blob gains nothing a demo does not already have.

Alternatives weighed: the AmigaOS layer in the kernel (every user pays 30 KB for a feature few use: no); in `/bin/zine`
as user code (it must run with interrupts off, on its own vectors, after the processes are stopped: it cannot be an
ordinary process); revision 2's restart into the ROM's AmigaOS (not what the owner wants).

## 4. Memory: who gets what

### 4.1 Chip RAM

Chip RAM is given to the program **almost whole**. What Zenix keeps there is copied to fast RAM before the hand-over
and back afterwards ("evacuated"): every used chip block (the console's planes, its Copper list, any other), each free
chip block's 16-byte header, and low memory `0`-`0x1FFF`. **A RAM disk in chip RAM** (zenixboot and the bootblock
load it there first, `MEMF_CHIP|MEMF_REVERSE`; it lies outside the pools, bootinfo `ram_pa`/`ram_size`) is not
evacuated, it can be megabytes: it stays where it is and the program's chip list leaves it out as allocated (the free
chunk is split around it), like a used fast block; the planner's host test covers it. On a fast-RAM machine that is
about 170-200 KB, a copy of
well under a frame's worth of time on any target (*proposed*; step 3 measures). What stays reserved in chip RAM:

| Range | Size | During the session |
|---|---|---|
| `0x000`-`0x3FF` | 1 KB | the CPU vectors (VBR is 0, 8.2); the program may write its own there, as demos do |
| `0x400`-`0xFFF` | 3 KB | **ZINE's low page:** ExecBase with exec's jump table, the reset capture code, the status record, the blank Copper list, the null sprite (5.1, 9.3) |
| the klog area (top 16.5 KB) | 16.5 KB | Zenix's log and the boot note, kept in place so that a cold start during the session still lands in Zenix and tells why (9.4) |

So on a 2 MB A1200 with no RAM disk in chip RAM the program's chip memory list is one chunk, `0x1000` up to the klog
area: **2,027.5 KB**, more than
AmigaOS 3.x leaves a demo with Workbench closed. Why the low page is chip RAM: the ROM reads address 4 at a reset before
any board's memory is configured (2.2); an ExecBase in chip RAM is one the reset can find on every machine. Why the klog
area is not given away: it costs 0.8 % of chip RAM and keeps the fall-back path (a cold start into Zenix with the
reason on the screen) working even when the program wrecks everything else.

### 4.2 Fast RAM

Fast RAM is **not copied**: Zenix's blocks stay where they are, and the program's memory list says they are in use.
Each fast region becomes one `MemHeader` spanning the region, whose free chunks are exactly Zenix's free blocks at the
moment of the hand-over (each starting at the free block's header, which is saved, 16-aligned, sizes multiples of 16).
Everything else in the region - the kernel, every used block and its header, the ZINE block, the volume - is
"allocated" from the program's point of view. Because big Zenix blocks sit at the top and small ones at the bottom, the
free space is mostly one large chunk in the middle: on the 1260 (64 MB) with one login about 62 MB in one piece
(*proposed*). The `MemHeader` attributes and priority are the region's own from bootinfo (`MEMF_FAST|MEMF_PUBLIC`...,
priority 0; chip at -10), so `AllocMem(MEMF_ANY)` takes fast RAM first, as on AmigaOS.

What Zenix keeps in fast RAM during the session: its own memory as it stands (one login: about 0.7-1 MB; with the
network about 0.85-1.2 MB; with an NFS mount's cache up to about 2.5 MB), **the ZINE block** (allocated by the kernel:
the blob's data, a 16 KB
supervisor stack, the 16 KB output ring, the saved chip pieces, the checksum table: about 250-300 KB) and
**the volume** (the program's files, 5.6), and the blob's own block (24-32 KB, `zine`'s). `zine` prints the three
numbers and what the program gets (12.1).

### 4.3 Planned before anything is frozen

`SYS_zine` first walks the pools (a small `mem_walk` in `mem.c`) and computes the ZINE block's size: the blob's runtime
size from its header, the evacuated chip bytes, the free-header table, the checksum table, plus a margin of 16 KB. The
block is allocated then, before any process is stopped; nothing is allocated after it. The walk is repeated by the
kernel with interrupts off (7.2 step 9: servers may have allocated during the sync) and its result passed to the blob;
if the evacuation no longer fits the block, the hand-over
is abandoned and everything released ("ZINE: Zenix's memory changed during the sync: try again"). If Zenix holds so much
that the program would get less than half the fast RAM, or the evacuation copy does not fit at all, `zine` refuses and
says how much Zenix holds.

### 4.4 The checksums

Taken after the processes are stopped and the drivers are quiet, checked on every way back before any Zenix code runs
or any Zenix memory is written: a CRC32 of every used Zenix block in every pool, the kernel image and BSS, the RAM disk
(if any), the klog area, the saved chip pieces and the free-header table, and the blob's code; the table of sums has its
own CRC.
The volume is not summed: Zenix never reads it again (`zine` frees it after the return), so damage there harms nobody.
Nothing of Zenix runs during the session (VBR 0 points at ZINE's vectors, 8.2), so none of this
memory changes unless the program writes it. Excluded: the ZINE block's changing data (its stack, the output ring, the
status record) and the program's own memory. CRC32 (slicing-by-4, a 4 KB table in the ZINE block) because these sums are
the gate before Zenix writes its disks
again. **They catch accidents, not intent** (9.5). Cost (*proposed*): a table CRC32 is about 30 cycles a byte on a
68030, so 1-3 MB take about 1-2 s each way on a 25-50 MHz 68030 with slicing, a fraction of that on the 060 and under
Emu68; step 5 measures it.

## 5. The small AmigaOS

Written from the public interface documents only: the autodocs give each function's registers and behaviour, the
include files the structures; the LVO offsets come from the public `.fd` files in the RKRM Includes and Autodocs
(a build-time table, step 1, with a host test that compares every structure offset ZINE uses with the include text).
Every library reports **version 40** (AmigaOS 3.1's interface, which ZINE follows; demos that test `lib_Version >= 39`
for AGA calls find them), with an `IdString` that says ZINE.

**A gap in the allowed sources** (review 1): the RKRM Includes and Autodocs here is the 2.04 (V37) edition. The V39/V40
additions this note uses - `LoadRGB32`, `SetRGB32`, `GetRGB32`, `AllocBitMap`, `FreeBitMap`, the AGA bits of
`gb_ChipRevBits0`, `REALLY_PAL`, `gb_MemType`'s values, `AFB_68060`, ExecBase's V39 fields (`ex_MemHandlers`) and the
LVOs above exec's -696 - are in no document on the clean-room list (knowledge base demoscene/aga-specifics.md says the
same of the AA bits). They are marked *proposed* here and built only from a public V39/V40 interface source the owner
adds to the list (owner question 4); until then the V39-only functions are the "not supported" stop, `zine` names them
by offset only, and the host test of 16 step 1 covers the V37 part.

### 5.1 ExecBase and the low page

ExecBase lies in the low page (at `0x0800`, the jump table below it from about `0x04E0`; exact layout fixed at step 3),
address 4 points to it, and the structure is a real V40 ExecBase: `LibNode` (`exec.library`, version 40), `SoftVer`,
`ChkBase` = its complement, `ColdCapture` = ZINE's reset capture (9.3), `CoolCapture`/`WarmCapture` 0, `SysStkUpper`/
`Lower` (ZINE's supervisor stack), `MaxLocMem` (the top of chip RAM), `MaxExtMem` 0, `ChkSum` (the Guru Book's sum),
the 16 `IntVects` (for the server chains PORTS, COPER, VERTB, EXTER and NMI each holds the chain's dispatcher
and its server list, so a demo's `SetIntVector` on VERTB gets that "chain" `Interrupt` back and can put it back,
review 1), `ThisTask` (the program's process), `IDNestCnt` -1 and `TDNestCnt` -1, **`AttnFlags` as AmigaOS
reported them to Zenix's boot** (bootinfo; on Emu68 `807F`, *verified*: bit 15 and bits 0-6, so 68040 and no 68060
bit; on the 1260 the 68060 bit is there only if AmigaOS had loaded `68060.library` before Zenix started, so on a CPU
flagged 68040 that is not Emu68 ZINE probes for PCR, an 060-only register, with the illegal-instruction vector caught,
and sets the 060 bit itself, *proposed*), `MemList` (4.1, 4.2), `ResourceList`
(`ciaa.resource`, `ciab.resource`, `potgo.resource`), `LibList` (ZINE's libraries, so a program that `FindName`s
`LibList` instead of opening finds them), empty but valid `DeviceList`, `IntrList`, `PortList`, `TaskReady`, `TaskWait`,
`SemaphoreList` (and V39's `ex_MemHandlers`, *proposed*, 5); `SoftInts` cleared; `LastAlert` -1;
`VBlankFrequency` 50 or 60 (what Zenix measured),
`PowerSupplyFrequency` 50 on a PAL machine and 60 on an NTSC one (the mains rate is not measurable: assumed, as the
crystal's region suggests), `ex_EClockFrequency` = Zenix's `eclock_hz`; `KickMemPtr`,
`KickTagPtr`, `KickCheckSum` 0 (a reset runs nothing of ZINE's but the capture). `ResModules` points to an empty
(zero-ended) array.

The rest of the low page: the reset capture code (about 300 bytes, 9.3), the **status record** (state, session number,
how it ended, details, its own CRC: the only thing written there during the session), **the blank Copper list** (sprite
pointers to the null sprite, `BPLCON0` `$0200`, `COLOR00` black, the end), the null sprite (16 zero bytes: enough at any
sprite fetch width); longword 0 stays 0 (some demos and
replayers read address 0 as a zero). Library jump tables for graphics, dos and intuition and their bases are in the ZINE
block (fast RAM).

### 5.2 exec.library (the first set)

| Group | Functions | How |
|---|---|---|
| Memory | `AllocMem`, `FreeMem`, `AllocAbs`, `AvailMem` (with `MEMF_LARGEST`, `MEMF_TOTAL`), `TypeOfMem`, `AllocVec`, `FreeVec`, `AllocEntry`, `FreeEntry`, `CopyMem`, `CopyMemQuick`, `CreatePool`, `DeletePool`, `AllocPooled`, `FreePooled` | exec's own structures (`MemHeader`, `MemChunk`), first fit, `MEMF_CLEAR`, `MEMF_REVERSE`, `MEMF_CHIP`/`FAST`/`PUBLIC`/`ANY`; pools on top (gcc's libnix `malloc` uses them); a free of memory not in a `MemHeader` is a dead-end alert (9.1) |
| Taking over | `Forbid`, `Permit`, `Disable`, `Enable`, `SetSR`, `GetCC`, `Supervisor`, `SuperState`, `UserState`, `StackSwap` | one task: `Forbid`/`Permit` count `TDNestCnt`; `Disable`/`Enable` count `IDNestCnt` and write `INTENA` `$4000`/`$C000`; `Supervisor` by the privilege-violation trick the autodoc describes (aos31-study/exec-kernel.md idea 2, the idea only), called in supervisor mode it builds the frame itself |
| The task | `FindTask`, `SetTaskPri`, `SetSignal`, `AllocSignal`, `FreeSignal`, `Signal`, `Wait`, `AddTask`, `RemTask` (16.16) | with more than one task as 16.16 says; with one process: `Wait` stops the CPU (`stop #$2000`) until an interrupt server signals; a `Wait` that nothing can end (interrupts disabled, or no server, vector or ICR handler installed) is a clean stop: "NAME waits for a signal nothing can send" |
| Interrupts | `SetIntVector`, `AddIntServer`, `RemIntServer` | 8.3 |
| Libraries | `OpenLibrary`, `OldOpenLibrary`, `CloseLibrary`, `OpenResource`, `OpenDevice` (always fails: `IOERR_OPENFAIL`, 5.5), `CloseDevice`, `SetFunction` (flushes the caches), `MakeLibrary`, `MakeFunctions`, `InitResident`, `FindResident` (0), `SumLibrary` | an unknown library or resource answers 0, as AmigaOS does when it has none: the program's own error path runs |
| Lists and messages | `Insert`, `AddHead`, `AddTail`, `Remove`, `RemHead`, `RemTail`, `Enqueue`, `FindName`, `AddPort`, `RemPort`, `FindPort` (0), `PutMsg`, `GetMsg`, `ReplyMsg`, `WaitPort`, `CreateMsgPort`, `DeleteMsgPort`, `CreateIORequest`, `DeleteIORequest`, `InitSemaphore`, `ObtainSemaphore`, `ReleaseSemaphore`, `AttemptSemaphore` | as the autodocs say (between tasks: 16.16) |
| Caches | `CacheClearU`, `CacheClearE`, `CacheControl`, `CachePreDMA`, `CachePostDMA` | 10.1 |
| The rest | `RawDoFmt`, `Alert` (9.1), `ColdReboot` (9.1), `Debug` (nothing) | |

Plus `AddLibrary` and `AddResource` (`InitResident` of an auto-init library needs the first, and `68060.library` may
call either: step 8). About 75 functions. **Every other exec, graphics, dos and intuition vector points at the "not
supported" stop** (9.1).

**Registers:** as the autodocs promise: `Disable`, `Enable`, `Forbid`, `Permit`, `GetCC`, `ObtainSemaphore` and
`ReleaseSemaphore` preserve all registers; `Supervisor`'s trap leaves every user register untouched (the autodoc: it
"does not modify or save registers"); every other function preserves `d2`-`d7`/`a2`-`a6`. Demo loops depend on it;
`ztake` checks it (15).

### 5.3 graphics.library

`GfxBase` with: `ActiView` = `&IntuitionBase->ViewLord` (an embedded `struct View`, 5.5, whose `LOFCprList->start`
points at the blank list, for demos that restore `COP1LC` through it; a program that saves it and later `LoadView`s it,
or `LoadView`s the `ViewLord`, gets the blank screen back), `copinit` and `LOFlist`/`SHFlist` (the blank list),
`DisplayFlags`
(`PAL` or `NTSC` from what Zenix measured, `REALLY_PAL` from the E-clock, *proposed*: 5), `ChipRevBits0` (`HR_AGNUS`
and `AA_ALICE` from the Agnus/Alice id in VPOSR bits 8-14, `HR_DENISE` and `AA_LISA` from `DENISEID`: an ECS Agnus with
an OCS Denise exists, the A500 rev 6; the AA bits *proposed*: 5), `MemType` as V39 sets it for those chips (*proposed*:
5), `NormalDisplayRows`/`Columns`, `MaxDisplayRow`/`Column`, and
`DefaultFont` = a `TextFont` of ZINE's own in the ZINE block, pointing at the Topaz 8 glyphs Zenix copied at boot
(the TextFont AmigaOS had in RAM is gone, and bootinfo's font field is unused). Functions: `OwnBlitter` and
`DisownBlitter` (one task: a flag, after `WaitBlit`), `WaitBlit` (polls `DMACONR` BBUSY with the repeated reads the
`WaitBlit` autodoc describes for the Agnus's early-BUSY quirk, ECS too; timed by the beam: 500 ms; preserves
`d0`/`d1`/`a0`/`a1` as the autodoc says), `LoadView` (`NULL` or ZINE's blank view: the blank list, and the chips'
display
state reset - FMODE 0, `BPLCON3` `$0C00`, `BPLCON4` `$0011`, `BPLCON2` 0, the machine's standard `BEAMCON0`, sprites
disarmed. The 3.1 autodoc says only that no View is shown and sprites go on; demos rely on the resets (knowledge base
demoscene/aga-specifics.md), and doing them is harmless: *proposed*. Any other `View` is a clean stop: "NAME uses
graphics' own displays"), `WaitTOF` (waits for the next vertical blank by the
beam counter, so it works with interrupts off; a timeout of 100 ms), `VBeamPos`, `LoadRGB4`, `LoadRGB32`, `SetRGB4`,
`SetRGB32`, `GetRGB32` (the hardware colour registers directly, 24-bit through `BPLCON3`'s LOCT on AGA; the `ViewPort`
argument is not used: ZINE has none), `AllocRaster`, `FreeRaster`, `InitBitMap`, `AllocBitMap`, `FreeBitMap` (chip RAM
from the program's list), `BltClear`.

### 5.4 dos.library

`DosBase` with a valid `dl_Root` -> `RootNode` -> `DosInfo` -> a device list holding ZINE's volume (5.6), for programs
that walk it. Functions: `Open` (`MODE_OLDFILE`, `MODE_NEWFILE`, `MODE_READWRITE`), `Close`, `Read`, `Write`, `Seek`,
`Flush`, `Input` (reads give end of file), `Output` (the output ring, shown after the return), `IsInteractive` (false),
`WaitForChar` (false), `Lock`, `UnLock`, `DupLock`, `Examine`, `ExNext`, `CurrentDir`, `ParentDir`, `IoErr`,
`SetIoErr`, `Delay` (50 ticks a second, counted in beam lines with the line length measured at the hand-over, so an
NTSC or other mode does not change it), `DateStamp` (the wall clock at the hand-over plus the time counted the same
way), `LoadSeg`, `UnLoadSeg` (6), `Exit` (ends the program: 9.1), `PutStr`, `VPrintf`,
`VFPrintf`, `FPuts`, `FPutC`, `Cli`, `GetProgramDir`, `GetProgramName`. `Open("*")` and `Open("CONSOLE:")` give
the output ring (C start-up code opens `*` for errors), `Open("NIL:")` a sink. Locks are real `FileLock`s (`fl_Volume`
a BPTR to the volume's DosList entry). Names are found by ZINE itself in the volume;
there is no handler process (a program that sends packets to `pr_FileSystemTask` or `pr_ConsoleTask` finds 0 there and
fails as under a missing handler); a packet sent to a file handle's own `fh_Type` is answered by ZINE (16.7). `Execute`
runs nothing and answers done (16.7), but an `Assign` ZINE does itself; what a program writes, on any path, is kept
in its memory and `DeleteFile` deletes there (16.8, 16.9).

### 5.5 intuition.library and the resources

`IntuitionBase` with `ViewLord` (the blank view: programs `LoadView(&IntuitionBase->ViewLord)` at their end) and
`FirstScreen`, `ActiveScreen`, `ActiveWindow` 0. Functions: `CloseWorkBench` (true: the autodoc's answer for a
Workbench that was not open is false, but demos treat false as "cannot close it" and quit, so ZINE says yes on
purpose), `OpenWorkBench` (0, the autodoc's failure), `ViewAddress`,
`RethinkDisplay`, `RemakeDisplay`, `MakeScreen`, `DisplayBeep` (nothing), `OpenScreen`, `OpenWindow`,
`OpenScreenTagList`, `OpenWindowTagList`, `LockPubScreen` (0: the program's own error path: "cannot open a screen"),
`CloseScreen`, `CloseWindow` (nothing). That is enough for the start-up code of hardware-banging demos; a program that
needs a real screen or window is told so by its own error path or by the stop.

`ciaa.resource` and `ciab.resource`: `AddICRVector` (0, or the current owner; it also enables the bit),
`RemICRVector` (disables it), `AbleICR` and `SetICR` (bit 7 sets or clears, both answer the old mask; `SetICR` with bit
7 set *causes* the interrupt), as the autodocs say. Each resource keeps a software copy of the pending bits: every ICR
read (which clears the chip's) is ORed into it, enabled bits or not, so `SetICR(0)` can report them, and a
software-caused bit is delivered by setting `INTREQ` PORTS or EXTER. The resources are servers in the PORTS and EXTER
chains (8.3); nothing in ZINE owns a CIA bit, so every bit is
free (the keyboard's SP bit included: a program that takes it gets the keyboard). `potgo.resource`: `AllocPotBits`,
`FreePotBits`, `WritePotgo` (right mouse button readers). **No devices** in the first version (`timer.device`,
`input.device`, `audio.device`, `trackdisk.device` fail to open, as on a system without them: `OpenDevice` answers
`IOERR_OPENFAIL` and also writes it into the request's `io_Error`, leaving `io_Device` 0, since the autodoc says its
answer is a copy of `io_Error`); they come when a test program or a demo the owner names needs one. Review 1's guess at
what will make the stop fire most, to be checked against the owner's demos (the log of 9.1): `OpenFont`/
`CloseFont`, graphics-built views (`InitView`, `MakeVPort`, `MrgCop`), `input.device` handlers, `trackdisk.device`'s
`TD_MOTOR`, `disk.resource` (track loaders), math and `lowlevel.library` from C programs. `OpenLibrary` loads a
library ZINE has not from `LIBS:` with ZINE's `LoadSeg` and `InitResident` (built 2026-11-04, 16.8), and `audio.device`
gives out its channels (16.8).

### 5.6 The program's files: the in-memory volume

`zine` reads the program's directory (recursively) into one block before the hand-over: a table (a name of up to 107
bytes, the parent, the size, the protection bits, the date, the data's offset) and the files' data. That replaces
revision 2's FFS image and device: AmigaOS's own filesystem is not running, so ZINE's dos finds names itself.

- **Names:** AmigaDOS's rules: compared without case (Latin-1 letters too), `/` and `:` separate; UTF-8 names that are
  Latin-1 become Latin-1; names outside Latin-1 or with `:` or `/` are left out with a warning; two equal but for case
  are both kept, the exact one found first (16.10). Protection bits from the Unix mode (`r` R, `w` W and D, `x` E), dates from the modification time.
- **Paths:** the volume is named after the directory (demos that open `NAME:data/...` by their disk's name find it);
  `SYS:`, `PROGDIR:` and the volume name are its root; `C:`, `S:`, `L:`, `LIBS:`, `DEVS:`, `FONTS:` its subdirectories
  where they exist; the current directory at the start is the program's. The owner's CPU library (10.1) is added as
  `LIBS:68060.library`.
- **Limits:** as built since 16.12: at most 2,000 entries (500 until 16.15; more: only the program goes in, said), 48 MB, and half the fast
  RAM the program leaves free; over that, archives and disk images and then the largest files are left out, each
  named (16.12). Since 16.23 a data file of 256 KB or more on the IDE drive's ext2 volume stays on the disk and is
  read from there during the session (it counts in none of these limits).
- **Writes** (owner answer 3, and 2026-11-04 "not all demos write to the same location"): as built (16.9), a file
  written on any path (`RAM:`, `T:`, `ENV:`, the program's own folder, `SYS:`, `DH0:`) lives in the program's own
  memory for the session (it can be read back by that path, loaded and deleted; it stands in front of the volume's
  file of that path) and is gone afterwards; nothing is ever written to a disk. `NIL:` and the other stream devices
  keep nothing. The volume block itself is never written, so it is in the checksums. `Rename` and the rest are the "not
  supported" stop.
- The reader is a shared C file of `zine` with a host test.

## 6. Loading the program

**The hunk check** (`zine`, before anything is allocated) and **the loader** (the blob's `LoadSeg`, in the session) are
one C source, compiled into both, so a file the loader would refuse is refused while Zenix runs, with its message:

- `HUNK_HEADER` (no resident library names; the first and last hunk numbers and the size table inside the file; each
  size's memory bits 30-31: chip, fast, any, or both set meaning an attribute longword follows), then each hunk, its
  type
  with bits 29-31 masked first: `HUNK_CODE`/`DATA`/`BSS` (their own size longword's memory bits 30-31 masked too; not
  larger than the table's size), `HUNK_RELOC32` and `HUNK_RELOC32SHORT` (`0x3FC`, "the same format as hunk_reloc32"
  with 16-bit words, AmigaDOS Manual; the public text does not say where the word stream is padded to a longword, so
  the loader accepts the padding once at the end of the hunk's stream and step 1 tests both forms), `HUNK_DEBUG` and
  `HUNK_NAME` skipped by their lengths, `HUNK_SYMBOL` walked unit by unit (a name length, the name, a value; a zero
  longword ends it: it has no length word of its own), `HUNK_END`. Every count and offset range-checked with arithmetic
  that cannot wrap; a
  relocation names a hunk that exists and an offset with offset + 4 within its size.
- `0x3F7` (`HUNK_DREL32` in the AmigaDOS Manual, the short form in the Guru Book): decided at step 1, it is
  `HUNK_RELOC32SHORT` (16.1).
- **Overlays** (`HUNK_OVERLAY`, `HUNK_BREAK`): loaded since 2026-11-04 as 16.6 says: the root at the start, the other
  parts by the program's own overlay supervisor through dos's `LoadSeg`, from the file in the volume.
- Refused, each with its own message: nested overlays and a `HUNK_BREAK` without an overlay table (16.6),
  `HUNK_RELRELOC32` (`0x3FD`), external references, anything else ("not an AmigaOS program
  (unknown hunk 0x3F9 at byte 1234)").
- **Loading** (from the volume): each hunk from the program's memory list with its memory type and `MEMF_CLEAR` (BSS and
  the part of a hunk beyond its stored data are zero, as LoadSeg leaves them), the size longword and the next-segment
  BPTR before it (the AmigaDOS segment list); relocations applied; then **the caches flushed** (`CacheClearU`: CACR bit
  3 on 020/030, `cpusha bc` on 040/060 and Emu68, CLAUDE.md's rule) before anything jumps into it. The same `LoadSeg`
  serves programs that load their own parts.
- **The process:** a `struct Process` (`NT_PROCESS`, the program's name, `tc_SigAlloc` the system's 16 bits,
  `tc_SPLower`/`SPUpper` its stack), `pr_CLI` (a BPTR) to a `CommandLineInterface` (`cli_CommandName` a BSTR of the
  name, `cli_Module` the segment list as a BPTR, `cli_DefaultStack` **in longwords** (64 KB = 16384), `cli_Interactive`
  false, `cli_FailLevel` 10), `pr_CIS`/`pr_COS` (BPTRs to file handles), `pr_CurrentDir`/`pr_HomeDir` (BPTRs to locks
  on the program's directory), `pr_WindowPtr` -1, `pr_Arguments`, `pr_ReturnAddr` ("points to just above the return
  address on the currently active stack", AmigaDOS Manual: most likely the initial stack pointer + 4, the stack-size
  slot; some start-up code `exit`s through it: read so at step 1, tested at step 3 with a start-up that does, 16.1).
- **The start:** a stack of 64 KB by default (`zine` passes another size if the owner gives one later; the boot shell's
  4 KB breaks demos), from the program's own memory; `d0` the length and `a0` the arguments ending in "\n"; `(sp)` the
  return address into ZINE and `4(sp)` the stack size; `a6` ExecBase (not promised by AmigaOS, harmless); user mode,
  interrupts on as ZINE set them (8.1). Its `rts` ends the session (9.1).

## 7. The way in

### 7.1 `zine` (user space, no set-user-id)

1. `ZINE_INFO` (can this machine run it, and why not; the memory numbers); the hunk check; the program's memory by type
   against what it will have.
2. The volume block (`SYS_memalloc`), filled from the directory; `/lib/zine/zine.blob` read into a second block,
   checked and relocated.
3. The lines, for example:

        ZINE: Zenix keeps 1.1 MB, ZINE 0.3 MB, the program's files 2.3 MB
        ZINE: the program gets 1.98 MB chip and 59.6 MB fast RAM
        ZINE: handing the machine to Arte (Ctrl-Amiga-Amiga brings Zenix back)

   On a machine where step 0 found that the keyboard's reset cannot bring Zenix back (9.3), the last line says
   "(Ctrl-Amiga-Amiga restarts the machine into Zenix)".
4. `SYS_zine` `ZINE_GO`; it returns when Zenix is back, with the result (9.6).

### 7.2 The kernel (`ZINE_GO`)

1. **Who** (owner answer 1): the kernel checks the caller itself: root, or a member of the group `zine`; and file
   descriptors 0-2 must be console ttys with the caller in that console's foreground (an ssh, telnet or serial session
   has none; keys typed through wasabid's `CON_INPUT` count as the console's). -EPERM otherwise. Answer 1 said "or
   through Wasabi": typing into the console through Wasabi is allowed by this rule, `wasabi run` (a remote command with
   no console descriptors, like ssh) is not; this narrowing is put to the owner (question 1). A group member logged in
   at
   the console could also pass it from ssh by opening their own `/dev/ttyN`; that gains nothing (ZINE is
   root-equivalent for them anyway, 9.5). -EBUSY in a second session or during an unconfirmed update trial
   (`UPD_F_TRIAL`, or the bundle still held: kernel-visible state); `zine` itself refuses while zup's lock file exists.
2. **The blob and the volume:** inside the caller's own memory; the blob's magic, layout version and CRC (-ENOEXEC; 3).
3. **Plan** (4.3): the ZINE block allocated; nothing stopped yet, so any failure just returns.
4. **Stop the user processes**, except what the servers need to finish the sync: netd and its drivers (an NFS mount's
   `FS_SYNC` goes through them) and, of course, the servers and the caller. The others are not scheduled any more (they
   stay in memory as they are). This closes the window in which a process could write between a server's sync and the
   freeze. From here until step 9 **`ZINE_GO` owns the reset warning** (init, which runs `kbd_reset_flush`, is
   stopped): if Ctrl-Amiga-Amiga's `$78` comes now, `ZINE_GO` gives up the hand-over, lets the sync in progress finish
   within its deadlines (or abandons it), turns any held writes into `disk_frozen`'s refusals, logs "kbd: reset warning
   during ZINE's hand-over: filesystems synced", and releases KDAT: the machine resets as it would have without ZINE.
5. **The disks:** a new `vfs_sync` mode, `VS_HOLD`: `FS_SYNC` to every mount as `VS_BOUNDED` does (local first, network
   last), with a deadline of its own per mount (5 s; network 2 s) that does **not** reset the machine and reports each
   mount that failed. This needs kernel machinery that does not exist yet (today `fs_call_on` waits for the reply and
   the only deadline is `reboot_deadline`, which resets): **a timed kernel call** whose message and reply buffer live in
   the kernel heap, not on the caller's 4 KB kernel stack, so a reply after the deadline lands in a buffer that is
   still there and is then freed (a cancel on timeout, as D23's `ipc_cancel` does for a killed client); and **a wait
   queue for held writes**: from the sync on, `blk_write` and the floppy's write path put the calling server to sleep
   until the resume (a new state for a non-preemptive kernel: it sleeps at the start of the request, before any byte
   moves). Writes are held, never failed, so no server sees an error half way through a request in a Zenix that goes on
   (`disk_frozen` stays one-way, for reboots). A mount that failed: everything released, "ZINE: /home did not answer
   the sync: not handing over". Every volume is then consistent: clean, or marked for a check where it needed one (D17).
   Both pieces are counted in 14's kernel size and are the first thing step 2 builds and tests.
6. **The network drivers:** the kernel runs each claim's quiesce program itself (D19's generic register writes: GENET's
   receive and send DMA stopped and its interrupts masked at the chip) and marks the driver process killed; it is not
   scheduled before the resume, dies then, and netd restarts it (D19's restart path; DHCP again). Nothing dies or
   changes owner inside the frozen window, so the second pool walk sees the same blocks. Then netd and the servers stop
   being scheduled too: all of Zenix is paused (owner answer 2).
7. **The notes:** the log line `zine: handing the machine to NAME (filesystems synced in N ms)`; the klog reason
   `KLOG_END_ZINE` and the boot note `BOOTNOTE_ZENIX` (both undone on the way back: a cold start during the session,
   9.4, then starts Zenix where the boot menu runs and says why).
8. **The drivers to a known state:** audio off, the floppy's motor off and its DMA off, a timed `WaitBlit`, the serial
   send ring drained (timed), the SD card idle, Gayle's interrupt enables off. Nothing Emu68-specific: the Pi's
   interrupt controller belongs to Emu68 and Zenix never touches it (`boot/genet-stop.h`); GENET is masked at the chip
   by its quiesce program, the SD controller is polled.
9. **Interrupts masked** (`0x2700`); the second pool walk (4.3), its result checked against the ZINE block and handed to
   the blob; **the resume point** saved (a setjmp-style routine in `start.S`: the registers, both stack pointers, SR,
   VBR, the full CACR, on a 68030 TC/TT0/TT1, on a 68040/060 TC, ITT0/1, DTT0/1, URP, SRP, on a real 68060 PCR; never
   PCR under Emu68); the readable chip registers (`DMACONR`, `INTENAR`, `ADKCONR`, the CIAs' ports, directions and
   control registers) and Zenix's records of the write-only ones (the CIA interrupt masks and timer latches, `SERPER`,
   the display set of 9.2); the CIA-A TOD count; then the blob's way in, which is given one kernel address only: the
   resume routine. `ZINE_GO`'s own frames stay shallow (the caller's kernel stack is a 4 KB block with no guard, and it
   is summed: nothing may be written on it after the sums).

### 7.3 The blob's way in

1. The supervisor stack switched to the ZINE block's own (before anything is summed); every chip piece copied into the
   ZINE block from the kernel's walk (low memory included); then address 4 = 0, so a reset before step 3 is a plain cold
   start rather than a half-trusted stale ExecBase; the free fast headers saved; the checksums (4.4).
2. The program's memory lists built (4.1, 4.2).
3. The low page written: the vectors (8.2), ExecBase and its jump table, the capture code, the blank list, the null
   sprite; the library bases and jump tables in the ZINE block; the process and CLI; the status record `RUNNING` with a
   new session number. Only now does address 4 point at a valid ExecBase (before this a reset finds 0: a cold start,
   9.4).
4. The chips set as AmigaOS leaves them for a CLI program (8.1); VBR 0; the caches flushed (code was just written: the
   jump tables, the vectors, the loaded program).
5. `LoadSeg` of the program from the volume (6); on a 68060 with the owner's library, that library first (10.1). A
   refusal now is a clean return (it should not happen: `zine` checked the same file).
6. Into the program, in user mode on its own stack.

## 8. While the program runs

### 8.1 The machine as the program finds it

As a CLI program finds it under AmigaOS with Workbench closed: `DMACON` master, bitplanes, Copper, blitter, sprites
(the Copper shows the blank list: no planes, null sprites, black); `INTENA` master, PORTS, VERTB, EXTER (ZINE's
handlers, 8.3); CIA-A's ICR with only SP enabled (ZINE answers the keyboard, 8.3), CIA-B's with none; every CIA timer
stopped; audio, disk and serial DMA off; `ADKCON` cleared; `BEAMCON0` the machine's standard (PAL `$20` where Zenix
measured PAL, else `$00`; written only where the Agnus/Alice id says ECS or AGA, 5.3); FMODE 0, `BPLCON3` `$0C00`,
`BPLCON4` `$0011` (the OS's defaults; a chip reset clears them, AGA.txt); the audio filter as Zenix
had it. The instruction cache on, the data cache off (10.1).

### 8.2 Vectors

**VBR is 0** for the whole session (unless the owner's `68060.library` moves it: then it is the library's, and
the way back sets Zenix's from the resume point): many demos write their level 3 or level 6 vector at the absolute
address `$6C` or
`$78`; those that read the VBR through `Supervisor` get 0 and agree. ZINE fills `0x000`-`0x3FF`: the spurious interrupt
(24) to a plain `rte`, the autovectors (25-31)
to ZINE's interrupt dispatchers, the privilege violation (8) to `Supervisor`'s trap with the crash path behind it,
every other exception and trap to the crash path (9.1), on a real 68060 vectors 61 (unimplemented integer
instruction), 60 (unimplemented effective address), 55 (unimplemented data type) and 11 (F-line, the FPU instructions
it lacks) to the owner's library if it is loaded, else to the crash path with their own message (10.1). A program may
replace any
of them: its exceptions are then its own, and Ctrl-Amiga-Amiga is the way out (9.3).

### 8.3 Interrupts as exec delivers them

ZINE's dispatcher for each level does what the RKRM's Exec "Interrupts" chapter describes, checked by review 1
(section 18). It saves `d0`/`d1`/`a0`/`a1`/`a5`/`a6` (handlers may destroy them); for every request bit set and enabled
(`INTREQR & INTENAR`; none set is a spurious interrupt and just returns), the handler set with `SetIntVector` is called
with `d1` the bits, `a0` the custom chips, `a1` `is_Data`, `a5` `is_Code`, `a6` ExecBase, and **clears its own
`INTREQ` bit** (exec does not do it for handlers). For the server chains (PORTS, COPER, VERTB, EXTER, NMI) the servers
are called in priority order (only `a1` `is_Data` and `a5` `is_Code` are promised; ZINE reloads `a0` = `$DFF000` and
`a6` = ExecBase before each, which also gives VERTB servers of priority 10 or more the `a0` they may rely on); a server
that returns with Z clear ends the chain; the system clears the chain's `INTREQ` bit after it. On a 68040/060 and
under Emu68 the `INTREQ` clear is written twice (or read back) before the `rte`, so the request is gone before the
interrupt mask drops (the knowledge base's take-over checklist, demoscene/engine-audit-2026-09-25.md).
`ciaa.resource` sits in the PORTS chain and `ciab.resource` in EXTER: each reads its ICR once (a read clears it) and
calls the handlers of the bits that are set and enabled. **The keyboard:** while ZINE's PORTS dispatcher runs and no
program handler owns CIA-A's SP bit, ZINE takes each byte and gives the handshake (KDAT low for three beam lines,
counted on the beam counter, never by a loop: at least 96 us even in a 31 kHz mode, above the HRM's 85 us), so the
keyboard does not resynchronise; the bytes are dropped.
ZINE's `Wait` and `WaitTOF` use the CPU's `stop` or the beam: nothing in ZINE runs behind the program's back.

### 8.4 What ZINE does not do

No screens or windows of graphics' own (5.3), no disk devices. Input events (the keys and the mouse) come since 16.17,
only to programs that ask for them through the OS. A program may run more than one task
(16.16, owner 2026-11-11: "we want elude demos with sound"): its own tasks and processes are switched as exec switches
them, nothing of Zenix runs meanwhile, and the session ends when the program's first process ends. A program that asks
for these through a call ZINE does not have is stopped cleanly (9.1); one that finds them missing by an answer (0 from
`OpenLibrary`, `IOERR_OPENFAIL`) takes its own error path and usually ends with a message, which `zine` shows.

## 9. The ways back

### 9.1 Path A: the program ends with the CPU in ZINE's hands

Every ending in which ZINE gets the CPU back without a reset:

| How | Reported as |
|---|---|
| `rts` from its entry | "NAME ended (return code 5)" |
| dos `Exit(code)` | "NAME ended (return code 7)" |
| exec `ColdReboot` | "NAME asked to restart the machine; Zenix is back instead" |
| a call ZINE does not have (any vector of 5.2-5.5's libraries not listed there) | "NAME called graphics.library `Text` (offset -60), which ZINE does not support yet: stopped" (`zine` names the function from the offset, a table built from the `.fd` files, and adds a line to `/var/log/zine`,
so what is added next follows the programs actually run) |
| an exception reaching ZINE's vectors (bus or address error, illegal instruction, zero divide, ...) | "NAME crashed: address error at 0001A2C4 (access 0002F001)" |
| a dead-end `Alert`, or exec finding its own lists damaged | "NAME crashed (alert 8100000C)"; a recoverable alert is noted in the output and the program goes on |
| `Wait` or `WaitTOF` that nothing can end; `LoadView` of a foreign view | the stop, with its own line |

Path A, in this order:
1. Interrupts masked (`0x2700`), the supervisor stack ZINE's own; **a timed `WaitBlit` while DMA is still on** (with
   BLTEN off a running blit freezes, its BUSY stays set, and it would resume into restored memory when the kernel turns
   DMA on again); then `INTENA`, `DMACON` and `INTREQ` all off, so no Copper, blitter, audio or disk DMA touches memory
   during the copying (the Copper would otherwise fetch Zenix's list address while it still holds the program's bytes,
   and a Copper move above `$80` can write `DMACON`, `INTENA`, the Copper pointers and the plane pointers without
   `CDANG`, HRM Copper chapter); the status record `RETURNING` (and, while the data
   cache may be on, that is under the owner's library, pushed with `cpushl` after every write to it: a reset drops
   unwritten copyback lines).
2. The CPU: on a 68040/060 data cache off, **then** `cpusha dc` (nothing can be cached between the two); the MMU off if
   the program or the library turned it on - 030: TC, TT0, TT1 by `pmove` (the line-F vector caught for an EC030); 040:
   TC and the transparent translation registers after `ptest` shows the ZINE block maps to itself; **060: the same with
   `plpar`** (the 060 has no `ptest`: MC68060UM 11.1.2.2.4); under Emu68, which emulates no 68k MMU, nothing (hwinfo
   says so); every probe with the illegal and line-F vectors caught, as zenixboot does.
3. **The checks** (4.4): any bad sum takes the damaged path (9.4).
4. **The low page is made harmless first:** address 4 = 0 and ExecBase's `ChkBase` cleared, so a Ctrl-Amiga-Amiga from
   here on is a plain cold start (9.4) instead of a jump into half-copied capture code.
5. Every saved chip piece and free fast header copied back, **low memory last**; then the chips' register set (9.2),
   with DMA still off; then
   address 4 = 0 again (the restored bytes would bring back AmigaOS's stale pointer from before Zenix's boot, which a
   later reset could half-trust; with 0 every later reset is a cold start, which is what Zenix's own `reboot` path
   assumes).
6. The caches flushed (Zenix's code is back where a program's code may have been cached, and Emu68 would run stale
   translations); the status record is gone with the low page; the jump to the kernel's resume routine (9.6), which runs
   with low memory, bootinfo and the Pi table back in place.

### 9.2 The hardware Zenix gets back (both paths)

One owner for each job. **The blob** (9.1 step 5) writes only the chips' register set, from Zenix's records (never read
back from write-only registers), so that nothing the program left running can disturb the copying:
- **DMA and interrupts:** already off since 9.1 step 1 (the blitter waited for while DMA was on); `ADKCON` cleared;
  `COPCON` 0; both CIAs' ICR masks cleared (`0x7F`, then a read) and their timers stopped; audio volumes 0, `DSKLEN`
  `$4000`; `BLTAFWM`/`BLTALWM` `$FFFF` (Zenix's fills assume them).
- **The display:** `BEAMCON0` the standard Zenix measured (on ECS and AGA, by the Agnus/Alice id), `DIWSTRT`/`DIWSTOP`
  as `gfx_init` wrote them (a write to either also disables `DIWHIGH` until it is written, AGA.txt, so the program's
  `DIWHIGH` is gone), `BPLCON0` `$0200`, the eight sprites disarmed (`SPRxCTL` 0), `COP1LC` and `COP2LC` = Zenix's list
  and the `COPJMP1` strobe, all with Copper DMA off; the Copper list writes the rest every frame (2.1); the screen's
  contents are back with the chip pieces. `DMACON` and `INTENA` are turned on last, by the kernel (9.6).
- **The CPU:** VBR, CACR (after `cpusha bc`), the MMU registers and a real 060's PCR come back with the resume point.

**The kernel** (9.6), with low memory back, then restarts the devices in one fixed order, through register-only
functions with no allocation and no log lines (new small ones split out of today's: `eclock_restart`, `timer_restart`,
`kbd_restart`, `gfx_restore`, `serial_restart`; `eclock_init` and `timer_init` are `static` in `main.c`, the main
session's file, 13):
1. **CIA-B's E-clock counter first**, as `eclock_init` starts it, with a software offset so that `hr_time()` goes on
   from its value at the hand-over (loading the old value through the latches would change the counter's period);
   everything after times itself with `hr_time()` (after a reset CIA-B is stopped, and the keyboard's handshake would
   spin its retry limit and log a failure).
2. CIA-A timer A (the tick); the keyboard: held keys, modifiers and the repeating key forgotten, one handshake; the
   ports
   and directions as saved (CIA-A's OVL bit stays 0, the LED/filter bit as Zenix had it); then the ICR masks Zenix uses
   (CIA-A TA and SP); `INTENA` and `DMACON` wait for the end (9.6).
3. `SERPER` and the serial port as `serial_init` set them; `ADKCON` to the saved bits; `POTGO` as `input_init` sets it,
   and the mouse counters' baselines read again.
4. `gfx_restore`: a frame later the beam's last line measured again and compared with the boot's (a different standard
   is logged).
5. The floppy, **on both paths**: motor off, the head position marked unknown, the track cache dropped (the program may
   have moved the head or changed the disk); on the next use it recalibrates and reads the disk's ID (a changed disk
   counts as changed).
6. On path B only (the machine was reset): Gayle and the drive (the full `ide_init`, the drive identified again: its
   size and serial must match, else that disk's writes stay held and the log says why); the SD controller under Emu68
   initialised again (zenixboot's start does the same today; `emmc_init` reads the Pi table at `0x1100`, which is back
   by then); the A3000's SCSI chip. On path A Gayle's interrupt enables and IDE latch are only set as `ide_init` leaves
   them.

### 9.3 Path B: Ctrl-Amiga-Amiga

While a demo has disabled every interrupt and replaced every vector, nothing ZINE installed can run: the NMI has no
source on an A1200, a 1260 or under Emu68 (2.2: level 7 comes only from expansion cards, Emu68 uses it for its own
errors); the keyboard's reset warning is a CIA-A interrupt the demo has masked (and only some keyboards send it); a CIA
timer reaches the CPU through `INTENA`, which the demo owns. **What always works is the reset itself**: Ctrl-Amiga-Amiga
resets the whole machine whatever the program does (2.2). So the way out is the documented reset hook:

1. The keyboard resets the machine; the ROM starts, reads address 4, finds ZINE's ExecBase in chip RAM valid and, before
   it sets up any memory, clears `ColdCapture` and jumps to ZINE's capture code in the low page (2.2).
2. The capture code (supervisor, interrupts masked; about 300 bytes in chip RAM, so it is there before any board's
   memory is configured) **first** points SSP and VBR into the low page, at a small table of its own in which every
   vector means "cold start" (VBR is 0 after a reset, and `0x000`-`0x3FF` hold whatever the program wrote there; the
   ROM has set up no stack either, Guru Book 10.1.1). Next, on a 68040/060 and under Emu68 (the low page's record says
   which CPU), `cpusha bc`: **a reset does not invalidate the 040's or 060's caches** (MC68040UM and MC68060UM, the
   cache chapters; nor the 040's ATCs), so stale instruction lines could run later and dirty copyback lines (under the
   owner's library) still hold what the program last wrote; pushing them first lets the sums see the truth. Never
   `cinva` (CLAUDE.md); on a 060 the branch cache is cleared (CACR's CABC) before it is enabled again. Only then does it
   touch fast RAM, which may not even be mapped yet (a bus error then lands in the capture code's own
   handler, a cold start). It checks the status record (magic, session number, its own CRC, state `RUNNING` or
   `RETURNING`), that the ZINE block's code is readable and intact (a bitwise CRC32, no table needed), and jumps into
   the blob's path B. Anything wrong: a cold start (9.4). It **never** returns to the ROM through `jmp (a5)`: the ROM
   would go on with the warm start of the ExecBase it has just trusted.
3. Path B: the blob's own vector table (every exception to a cold start), then as path A from step 2 (the CPU) on, with
   the CPU's caches, MMU, VBR and PCR at their reset values (Zenix's are set from the resume point) and the chips at
   reset (9.2's full set); the kernel then restarts the devices with the path-B items of 9.2. The result says "NAME was
   stopped by Ctrl-Amiga-Amiga; Zenix is back".
4. **Autoconfig boards** are unconfigured after a reset. If Zenix uses one (fast RAM on a Zorro board, a Zorro network
   card's claim), path B cannot rebuild it: it takes the cold start, and `zine` says so before the hand-over (7.1 step
   3).
   The owner's machines have none (*proposed*: step 0 checks the 1260's and Emu68's board lists). **FS-UAE's test
   setups do**: they use `fast_memory`, Zorro II autoconfig RAM, and Zenix's kernel lives there; so path B is tested
   on a setup with fast RAM that is not autoconfig (an accelerator board's RAM or an A3000/A4000's motherboard RAM;
   step 0 picks one FS-UAE offers), and one autoconfig setup checks that the cold start is taken there (15).

Whether the ROM honours ZINE's ExecBase (2.0+ may check more than the public 1.x checks), whether the 1260's fast RAM
is mapped at all when ColdCapture runs and keeps its contents through a keyboard reset (two questions), and what Emu68
does at a reset (its translated code, its own use of low memory) are **not known**: step 0 measures them on FS-UAE
(Kickstart 3.1, 3.2), the PiStorm and the 1260 before step 1.
Where path B does not work, Ctrl-Amiga-Amiga is a cold start into Zenix (9.4): still a way out, with the disks saved.

Also handled by path B: a program that resets the machine itself (the `RESET` instruction and a jump into the ROM, as
`ColdReboot` does) - "NAME reset the machine; Zenix is back".

### 9.4 The damaged path and the cold start

**Damaged** (a sum is wrong on either path): no Zenix code runs and no Zenix memory is written except the klog area,
and that only if its own sum is good: the reason `KLOG_END_ZINE_DAMAGED`, the boot note `BOOTNOTE_ZENIX` (already set,
7.2 step 7). Then the cold start below. The boot menu starts Zenix, which finds consistent disks and says "the previous
boot ended under ZINE: NAME wrote over Zenix's memory (the disks were saved first)". Where no boot menu runs, the
machine
boots what it boots normally.

**How ZINE makes a cold start** (the damaged path, and the capture code when anything is wrong): address 4, ExecBase's
`ChkBase`, `CoolCapture`, `KickMemPtr`, `KickTagPtr` and `KickCheckSum` cleared (when the ExecBase is still there), the
caches pushed, then the reset with ZINE's own copy of the reset code (the `RESET` and the jump in one longword,
aos31-study/exec-kernel.md idea 6; it does not trust the kernel's). The ROM then finds no valid ExecBase and starts from
scratch.

**A program that rewrites ZINE's ExecBase** (an old scene file with a reset-resident routine or a boot virus that sets
its own `ColdCapture`, `CoolCapture` or KickTags and fixes the sums): at Ctrl-Amiga-Amiga the ROM runs *its* code, not
ZINE's, and may warm-start AmigaOS with its residents, which could reach the owner's AmigaOS partitions before the boot
menu runs. ZINE cannot prevent this (9.5); the manual page says to run only programs one trusts, and step 0 records what
the ROM does when a ColdCapture returns to it, so the note can say exactly what such a program could do.

**A cold start** (the ROM ignored ZINE's ExecBase, the program destroyed the low page, the capture code found the
session wrong, an autoconfig board, power off): the machine boots from scratch; the boot note starts Zenix, whose
previous-boot line says "the previous boot ended under ZINE while NAME ran; Zenix could not be resumed (the disks were
saved first)" (reason `KLOG_END_ZINE`). Open programs and unsaved work in them are lost; the disks are consistent. With
a
RAM-disk root (the floppy-started Zenix) its changes since boot are lost, as at any restart.

### 9.5 Safety without an MMU: what Zenix can and cannot protect

A program run under ZINE owns the machine as under AmigaOS. **Running a program with ZINE is as trusting as running it
as root** (owner answer 1: root or the `zine` group, at the console; the manual page says it in its first lines).

- **Protected:** the disks' consistency (synced and held before the program starts; no Zenix driver runs during the
  session; Zenix writes nothing again unless every sum is good); Zenix's memory against well-behaved programs (the
  memory
  list says it is taken); Zenix against running on damaged memory (the sums, then a clean restart instead); the way out
  (Ctrl-Amiga-Amiga, else the cold start, else the power switch, which is safe at every moment).
- **Not protected:** a program can write anywhere (an MMU would be needed to stop it): it can wreck the low page (then a
  reset is a cold start), the klog area, or Zenix's memory (the sums catch it); it can read everything of Zenix's,
  other users' file caches and keys in memory included; it can change Zenix's memory and fix the sums; it can drive the
  IDE or SD hardware itself and write raw blocks, as under AmigaOS. **The sums catch accidents, not intent.**

### 9.6 The kernel's resume and the result

The resume routine restores the registers, both stack pointers, SR and the CPU registers; then the kernel: the device
restarts in 9.2's order, `DMACON` and `INTENA` to Zenix's values last, the network drivers (killed at 7.2 step 6)
allowed to die and netd to restart them, the boot note and klog reason cleared (`KLOG_END_RUNNING`), the held writes
released, the clock (9.7), the processes scheduled again, the log line `zine: back from NAME after 1 min 12 s
(it returned 0 | ...)`, interrupts on. `SYS_zine` returns to `zine` with `struct uzine_result` (how it ended, the return
code, the details: vector, PC and access address, or library and offset, or alert code; the seconds; the output's
length) and copies the output ring (at most 16 KB) into the caller's buffer; `zine` prints it and

        ZINE: Arte ended (return code 0) after 1 min 12 s; Zenix is back

### 9.7 Time while Zenix is paused

As a laptop asleep: the tick count and `hr_time()` do not count the session, so lwIP's and the service manager's timers
do not fire in a burst. The wall clock is moved on by the session's length, from CIA-A's TOD counter read at both ends
(demos almost never write it). Its source on the A1200 (the vertical sync or the power line's tick: the HRM names both)
is **not established** (step 0 records it); if it is the vertical sync, a demo that switches to NTSC or a 31 kHz mode
changes its rate by 20-40 %. So: the TOD count is checked against the battery clock where there is one, which wins on
any disagreement of more than a few seconds; with no battery clock the TOD time is used and the log marks the clock
"approximate after ZINE"; a TOD that went backwards is not used, the clock stays behind and the log says so
(`sntp` puts it right). The TOD is read most significant byte first (it latches on that read until the least
significant one, HRM CIA chapter). Network peers may give up on a connection meanwhile; bytes sent to the serial port
during the session are lost.

## 10. The CPUs and the machines

### 10.1 CPUs and caches

- **68020/68030:** the flush is CACR bit 3 with the enable bits kept. `CacheControl` answers the old bits and honours
  the instruction cache and burst bits; **the data cache stays off** in the first version (Zenix keeps it off; whether a
  given 030 board keeps chip RAM out of it is board-specific), and the answer says it is off. A program that turns the
  030's MMU on with `pmove` gets it turned off on the way back (9.1).
- **68040/68060:** the flush is `cpusha bc`, never `cinva` (CLAUDE.md). The program runs with the data cache off, as on
  the 030: Zenix's own copyback set-up (DESIGN D37: DTT0/DTT1, the 060's TCR) stays in the registers, but
  `zine_enter` switches the data cache off and `cpusha bc` before the blob runs (the blob is code `zine` just wrote),
  and the resume puts `CACR` back after `zb_mmu_back` has put the TTRs back. A real
**68040** lacks some FPU instructions (`68040.library` emulates them: FS-UAE's 040 setup will meet this; ZINE has the
same answer as for the 060 below, the crash line naming the 040). A real 68060 lacks 64-bit
  `mulu.l`/`divu.l`, `movep`, `cas2`, `chk2`/`cmp2` and some FPU instructions, which AmigaOS's `68060.library` emulates.
  **With the owner's library** (owner answer 4) ZINE loads `LIBS:68060.library` with its own `LoadSeg`, `InitResident`s
  it and opens it before the program, so the library installs its handlers and its MMU and cache set-up as under
  AmigaOS (the data cache is then the library's business, and the way back undoes it, 9.1). Which exec calls it makes is
  not known: step 8 runs it with every call logged on the serial port; a call ZINE lacks is added there or the library
  is left out. **Without it**, vectors 61, 60, 55 and 11 (8.2) go to the crash path with
  "NAME used an instruction the 68060 does not have: it needs 68060.library (see zine(1))". The way back restores
  PCR (the superscalar and FPU bits). Apollo 1260s are often fitted with an LC060 or EC060 (no FPU, or neither FPU nor
  MMU): step 0 records the owner's chip; on an EC060 the MMU registers are probed behind caught vectors before they
  are saved or restored (MC68060UM appendix B, to be read in step 8).
- **Emu68** (the PiStorm, the first target): `AttnFlags` `807F` with 68040 behaviour (*verified*), so `cpusha bc`, which
  also flushes the JIT's translations; never PCR; no library needed; the data cache question does not arise (Emu68's
  memory is coherent with the chips' bus). Every place that writes code flushes: the blob after relocation (the kernel),
  the jump tables and vectors, `LoadSeg`, `SetFunction`, `MakeFunctions`, and the way back before Zenix's code runs.
  Nothing Emu68-specific runs on other machines (hwinfo decides).

### 10.2 Chipsets and video standards

PAL or NTSC: `VBlankFrequency`, `gb_DisplayFlags`, the default display window and the `BEAMCON0` value come from
Zenix's measurement at boot (the beam's last line; the E-clock's rate from VPOSR bit 12), and are put back on the way
out (9.2). OCS, ECS, AGA: `gb_ChipRevBits0` from the id registers; `BEAMCON0` and `DIWHIGH` only with an ECS Agnus or an
Alice (they are Agnus registers: decided by the
Agnus/Alice id, not by Denise's); FMODE,
`BPLCON3`, `BPLCON4` and the 24-bit colours only on AGA. A demo that switches the standard (NTSC on a PAL machine, a
31 kHz mode) is put back to Zenix's on the way out.

### 10.3 The machines

- **The stock A1200** (chip RAM only): "ZINE needs fast RAM (this machine has only chip RAM)". Nothing of ZINE is
  loaded there; the kernel part (2-4 KB) is the only cost. `tools/ksize-budget` leaves 1,024 bytes on the mount
  tests' floppy, so **that floppy's kernel must be built without it** (a Makefile switch, the main session's): its
  `SYS_zine` then answers as an unknown call does and `zine` says "this Zenix kernel was built without ZINE"; the
  redirect (a few lines) stays in every kernel, so a program run by name gets the same message. A second kernel build
  weakens architecture.md's "the same `kernel.bin`" on floppy and disk; the main session weighs that against finding
  the bytes elsewhere (13).
- **The PiStorm A1200 (Emu68)**: the first target; fast RAM is the Pi's; path B depends on step 0. AmigaOS's SD driver
  does not run at all (no restart), so the owner's SD card is touched by nobody during the session but a program that
  drives the Pi's SD controller itself.
- **The Apollo 1260** (68060 80 MHz, 64 MB): 10.1's library question; path B depends on its fast RAM surviving a reset
  (step 0, under AmigaOS alone: Zenix has not run there yet).
- **A 68030 with fast RAM**: the plainest case; the data cache off.
- **FS-UAE** (the tests): A1200 with fast RAM, 68030/040/060, Kickstart 3.1 and 3.2 (the ROM matters only for path B),
  PAL and NTSC, an ECS setup for the chipset restore.

## 11. What can go wrong, and how each case ends

| What happens | How it ends |
|---|---|
| The file is damaged, not a hunk file, uses overlays, or needs more memory than there will be | refused before anything: a message, exit status 1 |
| No fast RAM; Zenix holds too much; not allowed (ssh, a non-member) | refused before anything |
| A filesystem server does not answer the sync; Zenix's memory changed during it | refused; everything released; nothing handed over |
| The program returns, calls `Exit`, `ColdReboot`, an unsupported function, crashes into ZINE's vectors, or alerts | path A: Zenix back, the line says which |
| The program never ends, or replaced the vectors and crashed | Ctrl-Amiga-Amiga: path B; where path B does not work, a cold start into Zenix |
| The program resets the machine itself | path B ("NAME reset the machine") |
| The program writes over Zenix's memory | the sums: the damaged path, a clean restart on consistent disks |
| The program wrecks the low page (ExecBase), then is reset | a cold start into Zenix |
| The program rewrote ExecBase's capture vectors or KickTags with valid sums (a reset-resident routine, a boot virus), then is reset | the ROM runs the program's code, not ZINE's, and may warm-start AmigaOS with its residents (9.4); ZINE cannot prevent it (9.5) |
| Ctrl-Amiga-Amiga during the way in (before the program starts) | `ZINE_GO` gives up the hand-over, the disks are synced, the machine resets as without ZINE (7.2 step 4) |
| Ctrl-Amiga-Amiga during the way back | before the low page is made harmless: path B again (the copies are idempotent); after: a cold start into Zenix (9.1 step 4) |
| The program writes over its own or free memory, or the hardware | nothing for Zenix: all of it was the program's; 9.2 puts the hardware back |
| The program writes to a disk itself (its own hardware access) | ZINE cannot stop it, as AmigaOS cannot (9.5) |
| Power off at any moment | consistent disks |
| A second `zine`; `zine` from a service or init (through the redirect) | refused (-EBUSY, -EPERM) |

No case writes a Zenix disk between the sync and a return whose sums are all good, and every fall-back is a fresh start
on
disks that were consistent at the hand-over.

## 12. What the user sees

### 12.1 The command and running by name (decided; by name built 2026-10-31, 16.4)

- **`zine PROGRAM [ARGS...]`** (`docs/man/zine.txt`, `commands.md`, `features.md` when built). No options in this
  version (later ones go here: a stack size, a system-friendly mode, a directory to give). NOTES: 9.5's warning;
  Zenix is paused while the program runs and other logins wait; Ctrl-Amiga-Amiga brings it back; files the program
  writes are not kept; overlays, disk images, devices, screens and windows are not supported; fast RAM is needed; the
  060's library.
- **By name:** an executable file (`x` bit) that starts with `00 00 03 F3` is run by the kernel as `/bin/zine PATH
  ARGS...` (`proc_spawn` reads the first longword already; a few lines). Services and init get there too and are
  refused by 7.2's check. No `/bin/zine`: -ENOEXEC, "an AmigaOS program: ZINE is not installed". A hunk file without
  `x` runs only as `zine FILE` (Unix's rule).
- **Messages:** the refusals ("ZINE needs fast RAM (this machine has only chip RAM)", "zine: not allowed (root or the
  zine group, at the Amiga's own console)", "NAME: not an AmigaOS program (...)", "NAME needs 2.5 MB of chip RAM; it
  would get 2.0 MB", "Zenix holds 9 MB at the moment: stop some programs first"), the lines of 7.1, 9.1's endings, the
  end
  line of 9.6, and after a fresh start the previous-boot lines of 9.4.
- **The screen:** Zenix's console with the lines; black for a moment (the blank list); the program; Zenix's console
  exactly as it was, with the end line. No grey Kickstart screens, no resets (except Ctrl-Amiga-Amiga's).

## 13. Interfaces and claims (for the main session)

These touch the main session's shared files (`kernel/kernel.h`, `kernel/main.c`, the Makefile) or frozen formats, so
the main session claims and builds them; this note only names them.

- **System call 93 `SYS_zine(op, a, b)`** (the next free number): `ZINE_INFO` 0 (`a`: a frozen `struct uzine_info`:
  can this machine, why not, chip and fast free, what Zenix holds, the CPU and chipset, whether path B is expected to
  work here), `ZINE_GO` 1 (`a`: a frozen `struct uzine_go`: the blob's and the volume's addresses and sizes, the name,
  the arguments, the stack size, flags 0; `b`: the frozen `struct uzine_result` and the output buffer). Errors: -ENODEV,
  -ENOEXEC, -ENOMEM, -EBUSY, -EPERM, -EIO. No new error numbers.
- **klog reasons** `KLOG_END_ZINE` 3 and `KLOG_END_ZINE_DAMAGED` 4 (free today); zenixboot's trial logic counts only
  PANIC and RUNNING, so they are no evidence of a failed start.
- **`vfs_sync` mode `VS_HOLD`** and held disk writes (kernel-internal; `disk_frozen` stays one-way for reboots);
  `mem_walk` (kernel-internal).
- **The kernel-blob interface** (the entry, the one kernel address it gets, the resume routine, and the layout
  version): internal to one build, shipped in the base group, never frozen (the kernel refuses a blob of another
  layout; `zine` explains a mismatch during an update, 3).
- **The group `zine`**: a group number and an `/etc/group` line; claims.md has no table of groups yet, so the main
  session adds one with this claim.
- **`main.c` and `start.S` changes** (main-session files): `eclock_init`/`timer_init` split into register-only
  restarts and made reachable from the resume (9.2), the `hr_time()` offset, the resume point; `mem_walk` in `mem.c`;
  the held-write queue in `blk.c` and the floppy; the timed kernel call in `ipc.c`/`vfs.c` (7.2 step 5); the
  `ZINE_GO` reset-warning path in `kbd.c` (7.2 step 4). The floppy kernel's build switch (10.3).
- **The kernel's redirect** (`00 00 03 F3` runs `/bin/zine`; the path too): frozen behaviour.
- **Files:** `/bin/zine` (no set-user-id), `/lib/zine/zine.blob`, `/etc/zine/libs/` (the owner's CPU library; never
  shipped; readable by the `zine` group). The `/etc` path is frozen.
- **Not needed any more** (revision 2 claimed them): bootinfo `+0x84`, the `0x1400` ExecBase copy, the chip anchor's
  place, `ZINE_VOLUME`, a new zenixboot. **No zenixboot change and nothing written to the owner's card.**
- Documentation the main session updates when this is approved: future-ideas.md "Amiga demos on Zenix" (the corner
  paragraph: Zenix keeps its blocks in place, not a corner; point to this note), ROADMAP block 3½'s wording ("saves its
  state and steps aside" stays true), and the README row of this note (in this commit).

## 14. Size and speed

- **Kernel:** 2-4 KB (*proposed*, the timed kernel call and the held-write queue of 7.2 step 5 included; step 2
  measures; the mount tests' floppy kernel is built without it, 10.3).
- **`/bin/zine`** 15-20 KB (the hunk check, the volume reader, the LVO name table, messages); **`zine.blob`** 24-32 KB
  (exec 10-12 KB, dos with `LoadSeg`, `RawDoFmt` and the volume 7-9 KB, graphics 2-3 KB, intuition and the resources
  1-2 KB, the way in and the two ways back in assembler 3-4 KB). Loaded only when used; **memory while not used: none.**
- **Time** (*proposed*; step 9 measures on the PiStorm): the sync as `reboot` (tens of ms); the chip copy (about 200 KB)
  and the sums (1-3 MB): a fraction of a second each way on the 060 and under Emu68, about 1-2 s on a 25-50 MHz 68030
  (4.4); loading the program's directory from Zenix's disk at Zenix's speed. No resets on the normal way.

## 15. Tests

No copyrighted demo goes into the repository. The test programs are written for the purpose with the m68k-elf toolchain
and made into hunk files by a new `tools/mkhunktest.py` (several hunks: code in any memory, data in chip RAM, BSS;
relocations across hunks, the short form, symbol and debug hunks), in `tools/tests/zine/`. Each FS-UAE test runs
headless
in its own slot; screenshots through the cage rig (`ZENIX_CAGE_RIG`), compared at fixed points.

| Program | What it does | The test checks |
|---|---|---|
| `ztake` | takes over as a demo does (Forbid, `LoadView(NULL)`, `WaitTOF` twice, `OwnBlitter`, the VBR through `Supervisor`, saves `DMACONR`/`INTENAR`/`ADKCONR`, Disable, its own level-3 vector at VBR + `$6C`), runs 100 frames by its own count, gives everything back, prints a line, returns 5 | "ZINE: ztake ended (return code 5)", its line shown; it also checks that every exec call it made kept `d2`-`d7`/`a2`-`a6` (and all registers for `Forbid`, `Disable` and the rest, 5.2) and exits through `pr_ReturnAddr` in a second run; afterwards a command at the prompt gives its output, a background job still runs, console 2's login is there, the console's text is as before (`CON_LINE`), `e2fsck -fn` of the root image clean |
| `zalloc` | `AvailMem` (largest, total) and `AllocMem` of all chip and fast RAM, `MEMF_CLEAR` checked, a pattern written and checked, `AllocVec`, `AllocAbs` inside a free chunk, `TypeOfMem`, a pool; frees all; prints the sizes | the sizes are what `zine` promised; the second `AvailMem` equals the first; Zenix comes back intact |
| `zcopper` | its own Copper list in chip RAM (colour bands), a sample played on Paula's channel 0 with its interrupt through `SetIntVector`, `ciab.resource` timer A counting, 200 frames | a screenshot during the session shows the bands; its output says "audio: 3 blocks, cia: N" with N in range; the console's screenshot after equals the one before |
| `zloop` | takes over (Disable, its own vectors) and loops for ever | on an FS-UAE setup whose fast RAM is not autoconfig (9.3 step 4), the test's keyboard reset (as `tools/diag-test.sh` sends it) brings Zenix back: "stopped by Ctrl-Amiga-Amiga"; on the usual `fast_memory` setup, and in a test build with path B switched off, a cold start into Zenix with 9.4's previous-boot line |
| `zsmash` | finds a gap in the fast `MemHeader`'s free list (Zenix's memory) and writes over 4 KB of it, returns | the damaged path: a fresh start with the message; the disks consistent (`e2fsck -fn` clean) |
| `zbadcall` | calls a function ZINE does not have (dos `Info`) | "zbadcall called dos.library Info (offset -114), which ZINE does not support yet: stopped"; Zenix fine |
| `zcrash` | an address error, and in a second run a dead-end `Alert` | "crashed: address error at ..." / "crashed (alert ...)" |
| `zfiles` | reads a data file by a relative path, by `PROGDIR:` and by the volume's name, `Examine`s it, writes a new file and reads it back, prints with `Printf` | its output; the written file is not on Zenix's disk afterwards |
| `zexit`, `zreset` | dos `Exit(7)`; exec `ColdReboot` | "return code 7"; "asked to restart the machine; Zenix is back instead" |
| `z060` | a 64-bit `mulu.l` (on the 68060 configurations only) | without the library the crash line names the 68060; with a test library standing in for the owner's, the right product |

Also: by name; the refusals (no `x` bit, a non-hunk file, damaged hunk files from a `tools/mkbadhunk.py`, overlays,
from a telnet login, a non-member, a chip-only machine); a server that does not answer the
sync (a test server that sleeps): refused and Zenix goes on, nothing lost, and its late reply harms nothing (7.2 step
5); a reset warning (FS-UAE's keyboard sends one) during the way in: the hand-over is given up, the disks synced, the
machine resets as without ZINE (7.2 step 4); a fast-RAM setup booted with a RAM disk in chip RAM: the RAM disk is not
given away (4.1); a write held during the hand-over completes
after the return; `ztake` twenty times in a row with `free` the same each time; the CPU matrix (68030, 68040, 68060 with
fast RAM; Kickstart 3.1 and 3.2 for path B); PAL and NTSC; an ECS setup; the network after the return (the driver
restarted, DHCP, ping); the clock moved on by the session's length within a second. Host tests: the hunk check and
loader
(every damage, and fuzzed as `zpkgfmt` is), the volume reader (names, case clashes, bits, dates, the limits), the memory
planner (synthetic pool layouts: every used chip byte saved, the free fast lists exactly Zenix's free blocks, nothing
given twice, a restore giving the same memory), the structure offsets against the includes (the V37 part, 5), the LVO
name table against
the `.fd` files. The real machines' runs are the owner's hardware time, each prepared to be ready for it.

## 16. Build order (small steps, each tested)

0. **Measure path B first (no Zenix change).** `zspike`, an AmigaOS program run under AmigaOS: builds an ExecBase as
   ZINE
   will (chip low page, `ColdCapture`, the checksums), fills chosen chip and fast areas (where Zenix's blocks and the
   ZINE
   block would be) with a pattern, takes the machine over as a demo does, and asks for Ctrl-Amiga-Amiga (and, in a
   second
   run, resets itself). Its capture code first sets its own stack and vectors (9.3 step 2), then reports: whether it was
   called; the CPU's state (VBR, CACR, PCR); whether fast RAM is mapped at that moment and, separately, whether the
   patterns kept their contents; which bytes of the low page changed. The patterns include the klog area and, on Emu68,
   the bottom of the first fast region (Zenix's kernel home, which the ROM reuses at boot: knowledge base
   amiga/gurus-and-exception-frames.md). The report goes to the serial port **and** into a checksummed record at the top
   of chip RAM that the next boot (zspike again, or Zenix) reads, since the owner's serial cable may not be there; then
   it clears address 4 and cold-starts. A third run lets the capture code return to the ROM through `jmp (a5)` and
   records what the ROM does then (9.4's warm start of a rewritten ExecBase). In FS-UAE (Kickstart 3.1 and 3.2,
   68030/040/060) first, then on the PiStorm (Emu68) and the Apollo 1260 under AmigaOS alone, with the owner; it also
   records the machines' autoconfig board lists, the source and rate of CIA-A's TOD (9.7), the 1260's CPU (full,
   LC or EC 68060) and where its MapROM copy lies (inside a bootinfo region or not: `mem_init` adds whole regions), and
   an FS-UAE setup with fast RAM that is not autoconfig for the tests (9.3 step 4). Results into this
   note and the knowledge base; where path B fails, that machine's Ctrl-Amiga-Amiga is the cold start (owner question
   2).
1. **Host parts:** the hunk check and loader, the volume reader, the memory planner, the structure-offset and LVO tables
   from the includes and `.fd` files; host tests and fuzzing; `0x3F7`'s form.
2. **The kernel's pause and resume** with a null blob (it returns at once): first the timed kernel call and the
   held-write queue (7.2 step 5) with their own tests; then `SYS_zine`, the checks, the process stop, the reset-warning
   path, `VS_HOLD` and held writes, the network quiesce, the resume routine, the drivers' re-init, the clock. Zenix
   pauses and
   comes back twenty times; free memory, disks, network, keyboard, display all as before; the restart order of 9.2 with
   the register-only functions. Kernel size measured.
3. **The blob, path A:** memory lists, chip evacuation, the low page, VBR 0, exec's set, the loader, the process;
   `ztake`,
   `zalloc`, `zexit`, `zcrash`, `zbadcall`, `zreset`.
4. **graphics, dos and the volume, intuition, the resources:** `zcopper`, `zfiles`.
5. **The sums and the damaged path:** `zsmash`; the time measured.
6. **Path B** as step 0 found it: `zloop`.
7. **By name** (the redirect); the manual page, commands.md, features.md.
8. **040/060:** caches, MMU, PCR; the owner's `68060.library` under ZINE's exec with its calls logged; `z060`.
9. **The owner's PiStorm A1200:** the tests and a demo of the owner's own; times and sizes into this note.
10. **The Apollo 1260**, the same list (Zenix itself must run there first).
11. A focused audit of ZINE (CLAUDE.md: a deep core change).

### 16.1 Step 1 as built (2026-10-30)

The host-side parts, plain C with no C library (`user/zine/zcore.h`), so the same files build into `/bin/zine`, the
blob and the PC's tests. Nothing in step 1 touches the reset, a capture vector or anything resident (path B is not
built). Test: `tools/host/zine-test.sh` (in `make check`'s host list as `zine-test`; *verified* 2026-10-30, 222 checks).

- **The hunk check and loader** (`user/zine/hunk.c`, `hunk.h`): one walk for both, as section 6 says; `zh_check` gives
  the hunks' memory by type and the refusal's message (`NAME uses overlays: not supported yet`, `NAME is not an AmigaOS
  program (unknown hunk 0x3F9 at byte 1234)`, and one for each damage), `zh_load` allocates through the caller, copies,
  clears, relocates and builds the segment list (it writes through a pointer and relocates with the address the
  program sees, so the PC's test checks the exact bytes). Decided here:
  - **`0x3F7` is `HUNK_RELOC32SHORT`** in a program (the Guru Book, ch. 22.2.6: the ROM's loader implements the short
    form under 1015/`$3F7`, and `HUNK_DRELOC32`, the AmigaDOS Manual's meaning of the number, "can occur only in object
    modules"); `0x3FC` is taken too. Both forms are tested.
  - **The short form's padding** (the Guru Book: a word to the next longword, "must"): taken when the stream ends off a
    longword and the padded reading is the one that gives a block type (or the file ends); files without it load the
    same. Tested both ways, also at the end of the file.
  - A missing `HUNK_END` before the next hunk or the end of the file ends the hunk; after the last hunk an overlay table
    or another hunk is refused, other bytes are ignored. At most 1,024 hunks and 256 MB.
  - **`pr_ReturnAddr`** (6): the AmigaDOS Manual's "just above the return address on the currently active stack" is
    read as the initial stack pointer + 4 (the stack-size slot); it needs a running program, so `ztake`'s second run
    tests it at step 3 (*read*, not yet verified).
- **The volume** (`user/zine/zvol.c`, `zvol.h`): builder (`zv_init`, `zv_add`: UTF-8 to Latin-1, the names left out
  with their reason, the protection bits and the DateStamp from the Unix mode and time) and reader (`zv_check`,
  `zv_find` with `NAME:`, `SYS:`, `PROGDIR:`, `:`, `C:` ... `FONTS:`, `/` and `//`, Latin-1 case folding; `zv_next`);
  `zv_fits` holds 5.6's limits. The block is internal to one build (native byte order), never frozen.
- **The memory planner** (`user/zine/zplan.c`, `zplan.h`): from a walk of the pools (blocks with their headers, the
  ranges, bootinfo's regions) the saved pieces, the program's chunks, the numbers and the ZINE block's size; a walk that
  is not one consistent picture is refused. The test checks on a model of memory that, after the program wrote all it
  was given, putting the pieces back restores every byte Zenix owns, and that nothing is given twice or given from the
  kernel, the RAM disk, the klog area or `0`-`0xFFF`. The block's fixed parts (stack, ring, margin, CRC table) are
  *proposed* constants until step 3 measures.
- **The tables** (`user/zine/lvo.c`, `lvo.h`, `amigaofs.h`; `tools/zine-tables.py`): 539 function names and offsets of
  exec, dos, graphics, intuition, the CIA and potgo resources (V37), made by the tool from the RKRM Includes and
  Autodocs' Function Offset Reference, and 359 structure offsets checked against its Structure Reference. The text is
  OCR'd: each offset is read in its three printed forms, which must agree and follow on from the one before; names are
  spelt as all the manuals spell them most often, with a list of letter/digit slips the vote leaves; six graphics lines
  the text layer lost and a few structure fields it garbled were read from the page images (each listed in the tool).
  `ZENIX_MANUALS` (the manuals' text folder, in the owner's private notes) runs this check; without it the part says
  SKIPPED. **Needs the NDK (owner question 4):** every V39/V40 function (exec above -780, `LoadRGB32`, `AllocBitMap`,
  ... : `zine` names them by offset only), the V39 names of exec's pool calls (the V37 list has `CreatePrivatePool` and
  `DeletePrivatePool` at -696/-702), ExecBase's and GfxBase's V39 fields, the AA bits and `MemType`, the 68060 bit.
- **Sizes** for the 68020-60 (code + data, `-Os` / `-O2`): hunk 3,623 / 5,844, volume 2,134 / 2,422, planner 1,122 /
  1,298, offset names 6,421 / 6,459 bytes; together 13.3 / 16.0 KB. No 64-bit multiply or divide, no outside call.
- **Fuzzing** (`tools/host/zine-test.sh fuzz [SECONDS]`, not in `make check`): the hunk check against the loader
  (whatever the check takes must load), a volume built from the input then damaged, plans from random walks; 300 s
  found nothing (2026-10-30).

### 16.2 Steps 2 and 3 as built, first part (2026-10-30)

*Verified* in FS-UAE (A1200, 68020, 4 MB fast RAM; `tools/zine-emu-test.sh`, in `make check`):
- **Step 2:** `SYS_zine` 93 (`kernel/zine.c`, +1,688 bytes of kernel) with the null blob (`/tests/zine/null.blob`,
  `user/zine/nullblob.S`: back at once through the kernel's resume routine): once and then twenty times in a row in a
  script; free memory after the twenty as after the first, the console's input, a file written and read back.
- **Step 3, first programs** with the real blob (`/lib/zine/zine.blob`, 19 KB): `zexit` (returns 7: zine reports it
  and exits with 7), `zhello` (dos.library's `Output()` and `Write()`: the line is shown after the program),
  `ztake` (graphics.library's `LoadView(0)`, its own Copper list in chip RAM for 150 frames, the system's list and
  view back: the screen is red, blue and white for 3 s, then Zenix's console as before; `tools/zine-shot.py` takes
  the pictures in the cage rig). Each program had 2015 KB chip and 3055 KB fast RAM; free memory afterwards as
  before. The test programs are AmigaOS hunk executables written for the purpose (`tools/tests/zine/*.S`,
  `tools/mkhunktest.py`: position-independent code, one code hunk).
- The serial line's input is the shown console's (kernel/tty.c), so zine typed on the serial line counts as the
  Amiga's own console; ssh, telnet and logins on other ttys are refused.
- `zcrash` (an ILLEGAL instruction: "crashed: illegal instruction at ..."), `zbadcall` (exec's PutMsg, which ZINE lacks: named, stopped) and `zalloc` (1 MB fast and 256 KB chip written, read back, freed; AvailMem as before) too, 2026-10-31.
- Not yet: the kernel review, a real demo, the owner's PiStorm.

### 16.3 Real demos (2026-10-31, the night after the owner said "When I wake up I want Amiga demos working")

*Verified* in FS-UAE (A1200 AGA, 68030 + 68882, 8 MB fast and 64 MB Zorro III fast; pictures taken in the cage rig
during each run): four demos run under ZINE from start to end and give Zenix back with its memory as before:
- **LTX4k** (the owner's own 4k intro, Ikadalawampu engine): about 10 minutes before the first picture in the emulator (Shrinkler unpacking the 4 KB, then the precalculation), then
  its effects; returns 0.
- **Ikadalawampu** (Loonies, Breakpoint 2010, 4 KB): the same, returns 0.
- **Moral Fiber** (Nah-Kolor, 2026): an intuition screen of 320 x 210 at 8 planes, timer.device, ciaa/ciab.resource,
  Cause, an AHX replayer; the whole demo with all its parts; returns 0.
- **Bacon of Hope** (Desire, the HD version, a 68000 OCS trackmo): takes the machine over by hand and plays. It needs
  its memory below 16 MB: with the program in Zorro III RAM (0x40000000) it froze on its first picture, with 8 MB of
  Zorro II fast RAM it plays (the owner's PiStorm's fast RAM is high too: to check there). It did not end within
  15 minutes (a trackmo that loops or waits).

What the demos needed and ZINE now has (the as-built list): exec TaggedOpenLibrary, StackSwap, Cause, the message
ports, DoIO/SendIO/CheckIO/WaitIO/AbortIO; OldOpenLibrary of any version; timer.device; ciaa/ciab.resource; graphics
InitBitMap, InitRastPort, AllocRaster (cleared), FreeRaster, AllocBitMap, FreeBitMap, LoadRGB32, SetRGB32; intuition
screens shown with their own Copper list (FMODE 3 on AGA lowres when rows and planes allow), windows, SetPointer,
ModifyIDCMP, ViewPortAddress; dos Open/Close/Read/Seek on the program's folder (zine puts the folder's plain files into
the volume; written files keep nothing); a3 = the segment list at the start (decrunchers walk it); GfxBase in
IntVects[INTB_BLIT].iv_Data; missing devices, resources, libraries and files named in the program's output; the trace
build `/tests/zine/trace.blob` (every call, crash registers and code on the serial port: `tools` side in the main
session's /tmp scripts for now).

Open: the review of 2026-10-31 (one bug fixed: zb_flush read the CPU flags from the page already Zenix's again; the
others recorded as findings, all but the CIA-B restart window fixed in 16.4: a second local disk's failed sync not refused, a floppy changed during the session, the
MMU/PCR registers on 040/060, the order in zine_resume, the session's seconds from CIA-A's TOD (one test run read 1 s
for 3 s: use the battery clock), Gayle's IDE interrupt, Ctrl-C during the sync, the CIA-B restart window, the mouse
counters); sound not yet checked (the pictures only); the owner's PiStorm; demos needing memory below 16 MB.

### 16.4 Running by name and the review's findings (2026-10-31, branch zine04)

*Verified* in FS-UAE (A1200, 68020, 4 MB fast RAM; `tools/zine-emu-test.sh`):
- **By name (12.1, step 7):** `proc_spawn` reads a file's first longword; `0x000003F3` (`HUNK_HEADER`) runs
  `/bin/zine PATH ARGS...` in its place (argv[0] "zine"; one level only: a hunk file at `/bin/zine` is -ENOEXEC, and
  without `/bin/zine` the kernel log says "PATH is an AmigaOS program: ZINE is not installed" and the spawn is
  -ENOEXEC). The 256 KB limit moved from `fs_exec_open` to after that check: a demo may be larger (Bacon of Hope's HD
  version is 927 KB). The x bit is the filesystem server's check as for any program. Tested: by path with arguments,
  as `./zexit`, found on `PATH`, and refused without the x bit (sh's 126). Kernel +292 bytes (proc).
- **The sync (review a, g):** `vfs_sync` mode `VS_LOCALS` refuses (-EIO) when any local mount's server fails its
  `FS_SYNC`, not only mount 0's. From the start of the sync to the resume `vfs_hold` is the zine process: every other
  process's filesystem request waits in `fs_call_k` (a 20 ms poll; a kill ends the wait with -EINTR) unless it is the
  server of a mount or a `priv` process (a server on an image file goes on), so each server is idle with its writes
  out at the hand-over; the requests go on after the resume. Ctrl-C during the sync (`current->killed`): -EINTR,
  `busy` and the hold cleared, nothing handed over. Tested: a background job appending 400 lines to a file on `/home`
  while `ztake` runs: all 400 there afterwards. The refusal of a second mount is *read* (no test server that fails
  its sync yet).
- **Floppies (b):** `fd_zine` marks every present drive's disk as changed (`drv[].changed`): requests fail until the
  disk server opens the drive again (`fd_info`), so the old disk's state is never written to a swapped one (a program
  that steps the heads resets the change line, so a swap cannot be seen afterwards). *Read*.
- **040/060 (c):** the blob saves TC, ITT0/1, DTT0/1, URP and SRP (and the 68060's PCR) in `zb_prepare` and writes
  them back first thing in `zb_back` (`cpusha bc`, TC, `pflusha`, the rest), each probed with the illegal-instruction
  and line-F vectors caught as zenixboot does (Emu68 reports a 68040: no PCR). Verified in 16.5's 68040 run.
- **The resume's order (d):** `zine_resume` puts VBR back first, then CACR with both caches cleared: CI and CD on the
  020/030 (the 030's data cache may hold lines the program's writes with the cache off did not update), `cpusha bc`
  on 040/060.
- **The seconds (e):** with a battery clock (`rtc_ok`, set by `rtc_read` at boot) the kernel reads its minutes and
  seconds before the hand-over and after (until two readings agree) and corrects the blob's CIA-A count to the
  clock's difference (the CIA-A count chooses the hour; the clock's minutes and seconds are exact). Tested: `ztake`'s
  150 frames now say 2-4 s.
- **Gayle (f):** `ide_zine` runs `gayle_quiet` again (nIEN, Gayle's enable 0, the latch cleared), as `ide_init` left
  them. **The mouse (h):** `input_init` again, so the counters the program moved are no jump.
- Kernel 169,340 -> 170,120 bytes; the mount tests' RAM disk 712 -> 708 KB for the room (12 blocks free).
- A read-only review of this (2026-10-31) found, fixed in the same branch: an exiting process's `FS_CLOSE` was dropped
  by the hold's -EINTR (it now waits for the resume); the clock correction could wrap below 0 (now skipped when it
  would: a program that set the clock) and an RP5C01 left in Kickstart's bank 1 shows no time at registers 0-3
  (`rtc_ok` only for a clock read in bank 0); `zb_mmu_back` wrote Zenix's TC before the other registers (now TC 0,
  the rest, then TC); `(sp)` in supervisor mode was refused by the line-F stand-in (16.5).
- Still open: the CIA-B restart window (the review's), a test server that fails its sync; a server starting on demand
  during the hold waits for the resume (its 30 s start deadline may count it as failed), and init's reset-warning sync
  waits too (the disks are already synced; the keyboard's own timeout resets).

### 16.5 The 68040/060 stand-ins: MMU instructions, missing FPU instructions, CacheControl (2026-10-31, zine04)

The owner's Bacon of Hope stopped on the PiStorm with "line F instruction at 0811d658". Emu68 runs `fsin`, `ftwotox`
and the other FPU instructions a real 040/060 lacks itself (knowledge base `amiga/emu68-and-pistorm.md`), and Bacon of
Hope is 68000 code, so the line F is *proposed* to be an MMU instruction: Emu68 has no MMU, so old demos' "switch the
030's MMU off" (`pmove` to TC, `pflusha`), or the 040's own `pflusha`, end there. Decided and built:
- **The line-F vector** (`zv_linef`, `zb_linef` in blob.c) takes as no-ops, with the MMU off as Zenix runs it: the
  68030's `PMOVE` to and from TC, TT0, TT1, CRP, SRP and MMUSR (a write skipped, a read gives 0), `PFLUSH`/`PFLUSHA`,
  `PLOAD` and `PTEST` (coprocessor 0's general form, every addressing mode but the full extension format; `(An)+`
  and `-(An)` move An as the CPU would); and the 040/060's own `PFLUSH`, `PFLUSHA`, `PTEST`, `PLPA` ($F5xx, one
  word). The program's output says once "ZINE: an MMU instruction (PMOVE, PFLUSH, PTEST) taken as a no-op: the MMU
  is off". Anything else at the line-F vector is a crash as before.
- **FPU instructions the CPU lacks** are not emulated (the 060 library's package is far larger than the blob): the
  crash line names them, "line F instruction at ... (instruction f200 000e: FSIN, which this CPU does not have in
  hardware; AmigaOS's 68040/68060.library emulates it, ZINE does not)", for the 040/060's missing set (FSIN, FCOS,
  FETOX, FLOGN, FSINCOS, FTWOTOX, FMOD, FREM, FINT ...). FS-UAE's 68040 stacks such a line F as format 2 with the
  next instruction's PC (the nearest FPU instruction word before it is named); a format-4 frame gives the faulted
  instruction's own address. An illegal-instruction crash shows its words too.
- **CacheControl** (exec -648) is real now: the old bits, and the instruction cache switched as asked (CACR's IE on
  040/060, EnableI/FreezeI/IBE on 020/030), both caches cleared after; the data cache stays off (10.1). Before, it
  answered "instruction cache on" and changed nothing, so a program that switches the cache off to change its own
  code ran stale code on a 68040: Bacon of Hope in FS-UAE's 68040 (8 MB fast RAM, FPU) crashed with an illegal
  instruction after 387 s that way; with this change it plays through to its end scroller (pictures taken at 20 s to
  750 s in the cage rig). Whether Emu68 drops its translations when
  the instruction cache is switched off is not known (*proposed*: it flushes on the CACR write, as on a CACR clear).
- *Verified* in FS-UAE (`tools/zine-emu-test.sh`): `zmmu` (PMOVE to and from TC, TT0, CRP, SRP by memory, `(An)+`,
  `-(sp)`, `d16(An)` and immediate, PFLUSHA; in supervisor mode the 040's PFLUSHA and PMOVE TC) returns 0 on the
  68020 and on the matrix's 68040, the no-op line in its output; `zfpu` (FSIN) named on the 68020 without an FPU and
  on the 68040; on the 68040 `ztake` and `zcrash` with Zenix back each time and free memory as before
  (16.4's MMU registers saved and put back). Not yet on the PiStorm: Bacon of Hope there is the owner's test.
- Blob 25,988 -> 27,032 bytes; the kernel unchanged.

### 16.6 Overlaid programs (2026-11-04, branch ovl)

Found on the owner's partner's A1200 with the Apollo 1260 (2026-11-04): `zine SpbPeek.exe` (Spaceballs' Peek) said
"uses overlays: not supported yet". Many demos of the era are overlaid; fixed next (CLAUDE.md "fix things when
found").

**What AmigaOS does** (*read*: the Guru Book 17.1.71, 17.1.76, 19.3.2.2 and 22.2.10; the AmigaDOS Manual's
"hunk_overlay", "Hunk Overlay Table-Overview" and InternalLoadSeg; numbers from the NDK 3.2's includes; the knowledge
base's amiga/amigaos-hunk-overlays.md has the layout in full):
- The file: the root node (a `HUNK_HEADER` whose table size counts every hunk slot of the tree), one `HUNK_OVERLAY`
  block (its length longword is the upper bound: length + 1 longwords follow: the offset to the entries, the ordinate
  table and its 0, eight longwords per overlay symbol: seek offset, 0, 0, level, ordinate, initial hunk, symbol hunk,
  symbol offset + 4), then the nodes, each a `HUNK_HEADER` (its first slot after its ancestors'), its hunks and
  `HUNK_BREAK`. A node's relocations may name its ancestors' hunks.
- LoadSeg of the file loads the root, copies the block from its third longword, allocates the hunk table (table size
  + 1 BPTRs, the last 0), keeps the file open and notes them in the first segment: after the linker's mark
  `$0000ABCD` at segment offset 8 come the handle (BPTR, 12), the table copy (APTR, 16), the hunk table (BPTR, 20)
  and the global vector (APTR, 24).
- The overlay supervisor is the program's own code (the linker puts it in): it frees the hunks from the entry's
  initial hunk on, `Seek`s the kept handle to the node, calls `LoadSeg(D1 0, D2 hunk table, D3 handle)`, which loads
  the node into the table and answers in D0 **and D1** (the original supervisor reads D1), and links the node to
  the hunk before it. A failed load raises `AN_BadOverlay` (`$0700000C`). The block's format "is not really enforced
  by LoadSeg(), but rather by the overlay supervisor; some third-party programs use the overlay block differently":
  the loader copies it and does not interpret it.

**What ZINE had:** dos `Open`, `Read`, `Seek`, `Close` on the volume (5.6, 16.3), no `LoadSeg`; the program's whole
file is always in the volume (when its folder is too big, the program alone).

**Decided: option A, with nothing extra to preload.** The file is already in memory in the volume, so the overlay
handle is an ordinary ZINE file handle on it and a node "loads from disk" by copying from memory; no new handle type,
no new interface:
- **The loader** (`user/zine/hunk.c`): `zh_check` takes an overlaid file: the root, the block (its length checked
  against the file), then every node that follows (each a full walk: hunks inside the root's table and after the
  root's slots, relocations to its own hunks or to slots below its first, `HUNK_BREAK` or the file's end); bytes
  after the block that are not a `HUNK_HEADER` are left alone (data the program reads through its handle). `zh_node`
  and `zh_load_node` check and load one node at a byte offset into the hunk table (an ancestor's slot that is 0 is
  refused, not followed); `zh_table` writes the node's segments and **clears every slot after it** (the supervisor
  frees up to the first 0 but does not clear: a longer sibling's stale slot would be freed twice).
- **The blob:** dos `LoadSeg` (-150) both ways and `UnLoadSeg` (-156). After the program's root is loaded,
  `zb_ovl_root` allocates the hunk table and the block's copy from the program's memory, opens the handle on the
  program's volume entry **at the first byte after the block** (where AmigaOS 1.x's in-order loader stopped; 2.0+ says
  only "any point after the last hunk loaded") and, when the mark `$0000ABCD` is there, writes the four fields; the
  global vector field is the process's `pr_GlobVec` (0: ZINE has no BCPL). Without the mark nothing is written into
  the program and its output says so. `LoadSeg(0, table, handle)` accepts only a table and a handle ZINE gave, loads
  the node at the handle's position, leaves the handle after its `HUNK_BREAK` and answers in d0 and d1;
  `LoadSeg(name)` loads a file of the volume the same way (an overlaid one gets its own handle and tables, up to 4 at
  once); a refusal or a lack of memory is named in the program's output (with the node's byte offset) and answers 0
  with `IoErr` (`ERROR_FILE_NOT_OBJECT` 121, `ERROR_BAD_HUNK` 235, `ERROR_NO_FREE_STORE` 103). `AN_BadOverlay` is a
  dead end (the Guru Book's supervisor retries forever): "NAME crashed (alert 0700000c: its overlay supervisor could
  not load a part)".
- **`zine`** prints "ZINE: NAME is overlaid: N later parts (X KB together, the largest Y KB), loaded from memory when
  it asks for them", and refuses with a plain line when the file (all in memory during the session) and its hunks
  (the root's, the largest node's, the tables) need more than all of Zenix's free chip and fast RAM: "zine: NAME does
  not fit in memory: its file (A KB, ...) and its hunks (B KB) need more than the C KB free". That is a floor, for
  every program: a program can still run out later (the volume, the ZINE block and Zenix's own memory come off the
  same RAM; `LoadSeg` then says "not enough memory to load N bytes of NAME's overlay part at byte M").

**Limits that are refused with their own message before the hand-over:** nested overlays (an overlay table inside a
node), a `HUNK_BREAK` without a table, an overlaid root that does not start at hunk 0, a hunk table of more than
16,384 slots (`ZH_MAXTAB`, so its size in bytes cannot wrap). At run time `LoadSeg` refuses a node over the root's
own slots (only nodes after each other in the file are checked before), and says when ZINE's 4 overlaid programs or
16 file handles are all in use.

**What overlaid programs still cannot do under ZINE:** a supervisor that calls BCPL
through the global vector, or a dos function ZINE lacks (`ErrorReport` on a read error, `InternalLoadSeg`,
`NewLoadSeg`), is stopped with the function named; one that reads its handle without a `Seek` first and expects
another position than the first node; resident library names in a node's header (refused, as in a root). The whole file
stays in memory for the session, so its size comes off what the program gets (the "had N KB chip and M KB fast"
line already counts it out).

*Verified* in FS-UAE (A1200, 68020, 4 MB fast; `tools/zine-emu-test.sh`): `zovl` (`tools/tests/zine/zovl.S`,
written as an overlaid hunk file by `tools/mkovltest.py`: vlink is not in the toolchain and the amigaos binutils
make no overlays): a root of a code and a data hunk with the overlay header and its own supervisor (written from the
Guru Book's description), two sibling nodes at level 1 (A: one code hunk relocated to itself and to the root's data;
B: code with short relocations and a chip BSS hunk), 16 bytes after them. LoadSeg wrote the header; A, B and A again
ran through the supervisor (`Seek`, `LoadSeg(0, table, handle)` read in d1), B's slot was cleared by A's load, the
data after the nodes was read through the handle, `AvailMem` after unloading as before, return code 0. `zovl bad`
(an entry whose seek offset is that data): "ZINE: zovl is damaged (no overlay part where its overlay table points)
(the overlay part at byte 1552)", then the supervisor's alert, Zenix back. On the PC (`tools/host/zine-test.sh`):
the overlaid file checked, the root, both nodes, B then A loaded into one table (relocations, the slots cleared), a
relocation to a slot not loaded and a node outside the table refused, eight damaged forms each with its message, a
last node ending with the file taken,
every prefix and one-byte change of the file (whatever the check takes loads, root and nodes); the fuzzer loads every
node the check takes. On hardware (2026-11-04, the owner's photos): Peek plays on the PiStorm A1200 under Zenix 2026110405 (the 3D glasses part, red/blue tunnels), with the packet fix of 16.7. A screen of character patterns in the middle is the demo's own part (its credits: "Pattern char sets courtesy of Ray Manta"); a mouse click ended it: "ended (return code 0) after 185 s; Zenix is back".

**Review** (one, independent, 2026-11-04, the loader and the blob): two bugs fixed: a hostile table size wrapped
`(tabsize + 1) * 4` (a 4-byte table, then writes 4 GB past it: now `ZH_MAXTAB`, and the host tests' skips that hid it
removed, both sizes tested), and a last node ending with the file without `HUNK_END` got end 0 (the check then walked
the root again as a node: now its end is the file's end, and a node that does not move on is refused). Also worked in:
nodes over the root's slots refused at run time, `UnLoadSeg` closes the overlay handle only while it is still ZINE's,
each limit says its own name, the messages name the program loaded, an overlaid root must start at hunk 0, the
emulator test no longer names a byte offset.

Sizes (the 68020-60 build): the blob 27,048 -> 31,020 bytes (memory 30,372 -> 34,796), `/bin/zine` 27,888 -> 29,656
bytes; the hunk core 3,623 -> 5,149 bytes at -Os (in both). The kernel unchanged.

### 16.7 Packets to a file's handler, Execute, and the desktop's terminal (2026-11-04, branch zine05)

Found on the owner's PiStorm A1200 (release 2026110404): Spaceballs' Peek "waits for a signal nothing can send", Latex's
Ltx-Snigelsaft stopped at dos `Execute`, and `zine` from a terminal on the desktop is refused.

**Peek** (*read* from its code; Spaceballs, Datastorm 2018) is a cruncher's container, not an overlaid program in the
linker's sense: a root of one code hunk and six BSS hunks (two of them chip RAM), `HUNK_OVERLAY` with a table of two
longwords (the packed data's length and the length - 1; no overlay symbols), `HUNK_BREAK`, then 249 KB of packed data.
Its decruncher reads the overlay header LoadSeg wrote (16.6) and then talks to the file's handler itself: it builds a
reply port and a `StandardPacket` on its stack, puts the packet on the handle's `fh_Type` with exec's `PutMsg`
(`dp_Arg1` the handle's `fh_Arg1`), and waits with `WaitPort`: `ACTION_SEEK` (-length, `OFFSET_END`: the data's start
whatever the handle's position), `ACTION_READ` in pieces of 1,960 bytes, `ACTION_FLUSH` at the end, then it clears the
caches and jumps into the unpacked code. ZINE's handles had `fh_Type` 0 and `PutMsg(0)` did nothing, so the first
`WaitPort` waited forever.

**Built:**
- **The handler port:** one `MsgPort` of ZINE's (`PA_IGNORE`, no task) is the `fh_Type` of every file handle ZINE
  gives (the volume's files, the overlay handle, `NIL:`, `Input()`, `Output()`; their `fh_Arg1` names them).
  `PutMsg` to it answers the packet at once, as the handler would (`dos/dosextens.h`'s `DosPacket`, NDK 3.2):
  `ACTION_READ` (82), `ACTION_WRITE` (87), `ACTION_SEEK` (1008), `ACTION_END` (1007) and `ACTION_FLUSH` (27) are
  ZINE's own `Read`, `Write`, `Seek`, `Close` and true; `dp_Res1` and `dp_Res2` set (`dp_Res2` the `IoErr` on a
  failure), `dp_Port` made the handler's port and the message put on the reply port the packet named, as
  `NT_REPLYMSG`, with its signal. Any other action answers `DOSFALSE` and `ERROR_ACTION_NOT_KNOWN` (209) and the
  program's output names its type (the first 4). A packet whose pointer is outside the program's memory is dropped.
- **PutMsg** to any other port now signals the port's task (`PA_SIGNAL`), as exec does; before it only queued.
- **`zine`'s line** for an overlay table with no parts after it: "ZINE: NAME keeps N KB of data after its overlay table
  and reads it itself through its overlay file, from memory" (it said "0 later parts").
- **dos `Execute`** (-222): ZINE has no shell, so no command runs. Demos `Execute` side jobs ("run >nil: a player",
  `assign`, `setpatch`, `stack`) and most go on whatever happens, while some quit when it fails; so it answers
  `DOSTRUE` and the program's output says "ZINE: NAME asked dos Execute to run "COMMAND": not run (ZINE has no shell),
  answered as done" (at most 100 bytes of the command). A demo that needed the command's work (a part started by
  `run`) shows it on the screen, and the line says which command it was.
- **A dos call ZINE lacks** now also shows its first argument when it reads as text (most dos calls take a name or a
  command in d1): "ZINE: the call's first argument (d1) reads "NAME"" before the stop line.

*Verified* in FS-UAE (`tools/zine-emu-test.sh`): `zpkt` (`tools/tests/zine/zpkt.S`; `tools/mkhunktest.py` writes a
section `.ovldata` after the root as Peek's table, `HUNK_BREAK` and data) sends `ACTION_SEEK` from the end,
`ACTION_READ` (the 16 bytes compared), `ACTION_FLUSH` and an unknown action to its overlay handle's `fh_Type`, and
`ACTION_WRITE` to `Output()`'s, waiting on its own port each time; then `Execute("run >nil: setpatch")`: return code
0 and each line checked. `zpkt u` calls `DeleteFile("RAM:zpkt-gone")`: stopped, the call and its text named. On the PC
(`tools/host/zine-test.sh`): Peek's form (a chip BSS hunk larger in the header than in its block, the two-longword
table, `HUNK_BREAK`, data) checked with no parts. Peek itself (not in the repository: a third-party production) in
FS-UAE: see the hand-back of 2026-11-04 and the knowledge base's `amiga/amigaos-hunk-overlays.md`.

**The desktop's terminal (not changed):** the rule stays (17, question 1: only the Amiga's own console). A shell in
`term` on the desktop has a pseudo-terminal, which the kernel's check (`allowed()` in `kernel/zine.c`: descriptors
0-2 a console in its foreground) refuses. Counting such a pty as local is a small check (the pty's master held by a
process whose own terminal is the console the display server has grabbed, that console shown), but it is not
enough: `ZINE_GO` also refuses while any console is grabbed (`gcon_grabbed()`, D38: the display server has the
Copper, the sprite and the blitter), and the way back puts the kernel's text Copper list up, not the server's. Doing
it properly means the resume giving the grabbed console's list and sprite back and telling the server (its blitter
state, its pointer), a D38 change with its own review; and the kernel has 12 bytes left over the mount tests' 800
(`tools/ksize-budget`). Until then: Amiga+F1..F10 to a text console and `zine` there.

Sizes (the 68020-60 build): the blob 31,020 -> 32,312 bytes (memory 34,796 -> 36,304), `/bin/zine` 31,632 -> 31,796
bytes; the kernel unchanged.

### 16.8 Libraries a program carries, RAM:, Assign, DeleteFile, audio.device (2026-11-04, branch zine06)

Found on the owner's PiStorm A1200 (release 2026110405): Latex's Ltx-Snigelsaft runs without its music and stops at
its end. Its output: Execute "Ram:Assign LIBS: Ram:" (not run), "no library playsid.library" twice, Execute "Ram:Assign
LIBS: SYS:LIBS/", then dos `DeleteFile("ram:playsid.library")`, which ZINE lacked. So the demo writes the SID player
library it carries (and an Assign command) to RAM:, assigns LIBS: there, opens the library, plays, puts LIBS: back and
deletes the file. ZINE kept nothing a program wrote, did no Assign and loaded no library from a file.

**Built** (the blob; *read* from the RKRM Includes and Autodocs' exec pages for InitResident, InitStruct, MakeLibrary,
MakeFunctions and the semaphores, the RKRM Devices manual for audio.device, the NDK 3.2's includes for the numbers):
- **RAM:** (`blibs.c`): a file opened for writing on `RAM:` (or `Ram Disk:`) lives in the program's own memory
  (AllocMem, grown as written, up to 32 files, names without case as AmigaDOS's) and can be read back, sought, loaded
  with `LoadSeg`, opened as a library and deleted; it is gone with the session (owner answer 3). A write anywhere else
  still keeps nothing; the program's own files stay read-only (since 16.9 every path keeps what is written).
- **Assigns** (`user/zine/zpath.c`, plain C with a host test): up to 8 names of up to 3 directories each, followed by
  `Open`, `LoadSeg`, `DeleteFile` and `OpenLibrary` (each directory in turn when looking a file up; the first when
  creating one; an assign through another followed up to 4 times). At the start `LIBS:` is `SYS:LIBS` (the program's
  volume's `LIBS` directory), `PROGDIR:` and `RAM:`. **Execute of an Assign** ("Assign NAME: DIR...", with or without
  a path to the command: "Ram:Assign", "C:Assign"; `ADD`; `REMOVE` or no directory removes the name; `DEFER` and `PATH`
  as a plain assign) is done by ZINE, answers DOSTRUE and says "ZINE: NAME asked dos Execute to run "Ram:Assign LIBS:
  Ram:": done by ZINE itself (LIBS: is now Ram:)". A listing or an option ZINE does not know: "an Assign ZINE does not
  do ..., answered as done". Every other command stays "not run".
- **DeleteFile** (dos -72): a `RAM:` file goes (while open: `ERROR_OBJECT_IN_USE`); one of the program's own files is
  refused (`ERROR_DISK_WRITE_PROTECTED`, said in the output); a path on another disk (`DH0:`) is refused
  (`ERROR_DEVICE_NOT_MOUNTED`, said); a missing file `ERROR_OBJECT_NOT_FOUND`. Nothing ever reaches a real disk.
- **Libraries from files** (`user/zine/blibload.c`): `OpenLibrary` of a name ZINE has not built in first looks in the
  library list (one loaded before, or added by the program), else loads `LIBS:NAME` (a name with `:` or `/`: that
  path) with ZINE's `LoadSeg`, finds the first RomTag in its hunks (`$4AFC` and a pointer to itself), and initialises
  it as `InitResident` does: with `RTF_AUTOINIT` `MakeLibrary` from the four longwords (the vectors, absolute or word
  displacements; the base cleared and longword aligned; the `InitStruct` table; the init code with d0 the base, a0 the
  segment list, a6 ExecBase) and `AddLibrary`; without it the init code is called (d0 0, a0 the segment list). Then the
  version is checked ("ZINE: NAME is version 3, the program asks for version 4") and its Open vector called. The output
  says "ZINE: NAME (version V) loaded from PATH". `CloseLibrary` calls its Close vector and unloads the segment list
  that answer gives (a library that expunged itself). Unused libraries stay loaded, as on AmigaOS; when `AllocMem`
  finds no room, every unused library not built in is asked to expunge (its Expunge vector) and the allocation tried
  again, as AmigaOS's low-memory flush does.
- **exec** gains `InitStruct`, `MakeLibrary`, `MakeFunctions` (the caches cleared after), `FindResident` (0: no
  resident modules), `InitResident`, `AddLibrary`, `RemLibrary` (the library's Expunge), `SumLibrary` (nothing),
  `AddResource` (`OpenResource` finds it), and the signal semaphores `InitSemaphore`, `ObtainSemaphore`,
  `ReleaseSemaphore`, `AttemptSemaphore` and the shared forms (one task: they never wait; `Obtain` and `Release` keep
  every register, as the autodocs say). `zbadcall` now calls `AddSemaphore`, which ZINE still lacks.
- **audio.device**: `OpenDevice` with an allocation (`ioa_Length`) gives the first wanted mask whose channels are free
  (io_Unit, an `ioa_AllocKey`), `ADCMD_ALLOCATE`, `ADCMD_FREE`, `ADCMD_LOCK` (in progress until the channels are
  freed), `ADCMD_PERVOL`, `CMD_STOP`/`CMD_RESET`/`CMD_FLUSH` (the channels' DMA off) and the others done at once;
  `CloseDevice` frees the channels. **It played nothing** (until 16.16, which plays `CMD_WRITE`): `CMD_WRITE` answered
  `IOERR_NOCMD` and the output said so once. A player that asks for channels and then writes Paula's registers itself, with its own level 4 handlers
  (`SetIntVector`) or a CIA timer (`ciaa/ciab.resource`), plays.
- Fixed on the way: a request completed with an error lost it (the reply cleared `io_Error`): the timer's "no room"
  and an unknown device's `SendIO` answered 0.

**What a SID player library typically needs** (*read*: PlaySID 3's own Developer.doc and Library.doc, which say
`StartSong` allocates "the audio device, timers and interrupts" and `AllocEmulResource` memory; nothing was
disassembled, the library is not in the repository): `OpenDevice("audio.device")` with an allocation, Paula's
interrupts through `SetIntVector` (or `AddIntServer`), a CIA timer through `ciab.resource`/`ciaa.resource`
`AddICRVector`, `AllocMem` of chip RAM, signals. ZINE has all of these. **Not there**, and a stop that names the call
if a player uses one: `AddTask`/`CreateTask` (ZINE runs one task: a player that plays from a task of its own stops
there), `Signal` to another task (ignored), `audio.device`'s `CMD_WRITE` (answered with an error: a player that plays
through the device is silent), `timer.device`'s `UNIT_MICROHZ` precision (ZINE's timer counts frames).

*Verified* in FS-UAE (`tools/zine-emu-test.sh`, A1200 68020 4 MB fast): `zlibs` (`tools/tests/zine/zlibs.S`) carries
`zt.library` (`ztlib.S`, written by `tools/mkhunktest.py -r` with `HUNK_RELOC32` for its RomTag and tables): writes it
to `RAM:` and reads it back; Executes "Assign LIBS: SYS:LIBS" (OpenLibrary fails: no such file), "Ram:Assign LIBS:
Ram:" (OpenLibrary loads it from RAM:: version 3.7 from InitStruct, its init given the segment list and ExecBase, its
Open counted, its functions answer, InitStruct's three extra forms right), a newer version refused; CloseLibrary;
"C:Assign LIBS: SYS:LIBS/"; DeleteFile("ram:zt.library") (then not found, a second delete `ERROR_OBJECT_NOT_FOUND`);
the library still opens (loaded), `RemLibrary` gives its segment list back, then it is gone; semaphores nested with
the registers kept; audio.device's four channels, a second allocation and `CMD_WRITE` refused; Paula's channel 0
interrupt through `SetIntVector` and CIA-B timer A through `ciab.resource` counted; `AvailMem` at the end as at the
start, return code 0. On the PC (`tools/host/zine-test.sh`): the assigns and the Assign parser (forms, quotes, ADD,
REMOVE, the limits, chains, every prefix of a command). Not yet: Ltx-Snigelsaft itself, on the owner's A1200.

Sizes (the 68020-60 build): the blob 32,312 -> 40,728 bytes (memory 36,304 -> 45,864; the assigns' table is 2.6 KB of it); the assigns core 1,863
bytes at -Os; `/bin/zine` and the kernel unchanged.

### 16.9 A write to any path kept in memory (2026-11-05, branch fix1105)

Owner, 2026-11-04: "not all demos write to the same location". 16.8 kept only what a program wrote to `RAM:`; a write
to `T:`, `ENV:`, the program's folder, `SYS:` or another disk was answered as done and dropped, so a demo that writes a
file and reads it back elsewhere than `RAM:` found nothing.

**Built** (`blibs.c`, `zpath.c` `za_key`; *read*: the AmigaDOS Manual's path rules, the NDK 3.2's `dos/dos.h` for the
error numbers):
- **One store for every path.** A file opened with `MODE_NEWFILE` (or `MODE_READWRITE` when the path names nothing)
  on any path goes to ZINE's memory files (16.8's `RAM:` files, now for every path): up to 32 files, in the program's
  memory (AllocMem, grown as written), gone with the session. Never a disk: ZINE has no disk to write to, and the
  volume block stays as `zine` made it (its checksum still holds at the way back).
- **The key.** A file is found by its path with the assigns followed, in one form (`za_key`, zpath.h, host-tested):
  `"data"`, `"PROGDIR:data"`, `"SYS:data"`, `":data"` and the volume's own name are one file (`SYS:data`); the
  volume's `C:`, `S:`, `L:`, `LIBS:`, `DEVS:`, `FONTS:` are its directories (`SYS:C/x`, as `zv_find` has them);
  `RAM:` and `Ram Disk:` one device; any other device by its name in capitals (`T:x`, `ENV:Sys/x`, `DH0:a/b`); `/`
  and `//` go up one, a `/` at the end is dropped, names compare without case. Too long (200 bytes) or above a
  device's top: `ERROR_INVALID_COMPONENT_NAME`.
- **Shadowing.** At each place a path stands for (16.8's assigns, in turn), a memory file comes before the volume's
  file: `Open`, `LoadSeg` and `OpenLibrary` read what the program wrote. `MODE_READWRITE` on one of the volume's files
  copies it into a memory file first, so a program that changes a file of its own changes its copy. A volume's
  directory cannot be opened for writing (`ERROR_OBJECT_WRONG_TYPE`).
- **Deletes.** `DeleteFile` of a memory file frees it (open: `ERROR_OBJECT_IN_USE`); of one of the volume's files, or
  of a memory file in front of one, leaves a memory file marked gone: the path finds nothing for the rest of the
  session (`ERROR_OBJECT_NOT_FOUND`), the disk keeps the file. A volume's directory is refused and said
  (`ERROR_DISK_WRITE_PROTECTED`). A missing path: `ERROR_OBJECT_NOT_FOUND` (16.8 answered `ERROR_DEVICE_NOT_MOUNTED`
  for another disk's; there is no "other disk" now).
- **Streams.** `NIL:`, `CONSOLE:`, `PRT:`, `SER:`, `PAR:` and `AUX:` take what is written and keep nothing; a read is
  at its end at once. Any other missing file is not found (16.8 gave an empty file for a path on a device ZINE did
  not know). A write to one of the volume's files opened to be read: `ERROR_WRITE_PROTECTED`.
- **Locks.** ZINE has no `Lock` yet (5.4 lists it, but the vector is still the "not supported" stop): a program that
  locks a file stops there and names the call, as before; when `Lock` comes, it finds the same files.

*Verified* in FS-UAE (`tools/zine-emu-test.sh`, A1200 68020 4 MB fast): `zwrite` (`tools/tests/zine/zwrite.S`) writes
`T:zw.tmp`, `ENV:Zw/pref`, `PROGDIR:zw.new` and `DH0:zw/deep` and reads each back by another form of its path
(`t:ZW.TMP`, `env:zw//Zw/pref`, `zw.new` and `SYS:zw.new`, `dh0:ZW/deep/`); `NIL:` keeps nothing; its folder's
`zexit` is read (the real file), shadowed by a written `PROGDIR:zexit` and read back as `zexit`; `zhello` opened
`MODE_READWRITE`, its first two bytes changed, read back with the rest its own; `DeleteFile` of the shadowed `zexit`,
of `zbadcall` (never written) and of `T:zw.tmp`: each then not found, a second delete `ERROR_OBJECT_NOT_FOUND`; return
code 0. Afterwards the shell finds the disk as before (`cmp` of `zexit`, `zhello`, `zbadcall` against copies taken
before; no `zw.new`, `zw` or `T` in the folder). `zlibs` (16.8) passes unchanged. On the PC (`tools/host/zine-test.sh`
core): `za_key`'s forms, the limits, every short buffer.

Sizes (the 68020-60 build): the blob 40,728 -> 42,116 bytes (memory 45,864 -> 47,480); `/bin/zine` and the kernel
unchanged.

### 16.10 Subfolders, and names found without case (2026-11-06, branch zcase)

Found on the owner's A1200 (release 2026110603): Ward's `XYLOP96.exe` (in a folder `xylop/` on a PFS3 partition
mounted read-only) stopped with "ZINE: no file PROGDIR:data/xyl.tp3"; the file is `data/XYL.TP3`. AmigaDOS names are
case-insensitive and case-preserving, Zenix's filesystems case-sensitive. *Read* from the source: ZINE's dos never asks
a Zenix filesystem for a name; it looks in the volume (5.6) with `zv_same`, which already compared without case. The
stop came first from 5.6's "recursively" not being built: `zine` copied the folder's plain files only, so no
`data/...` path could be found in any case. Both are now done.

**Built** (`user/zine.c`, `user/zine/zvol.c`; *read*: the AmigaDOS Manual's name rules; the case rule as 5.6 has it):
- **Subfolders.** `zine` copies the program's folder with its subfolders into the volume, 8 levels deep (deeper: left
  out, said), in two passes (count, then fill; the count stops once past 5.6's limits: 500 entries, files and
  folders together, 16 MB, half the free fast RAM). Past them only the program goes in, and now says so ("only NAME
  goes into ZINE's volume: its directory holds ..."). Names starting with `.` are left out as before; a folder the user
  may not read is left out. Every dos call that takes a path finds them: `Open`, `LoadSeg`, `OpenLibrary` from
  `LIBS:` (the folder's `Libs`, in any case), `DeleteFile`, the overlay and packet reads; `Lock` and `Examine` ZINE
  still has not (16.9).
- **The case rule** (`zv_same`, `za_key`'s compare, unchanged and now host-tested for it): ASCII letters and the
  Latin-1 letters `à`-`þ` against `À`-`Þ`, not `×`/`÷` (0xD7/0xF7), not `ß` against `ÿ`; nothing else. Each part of a
  path is found in its folder: the name spelt exactly so first, else the first entry the same but for case
  (`zv_find`'s `child`). The memory files of 16.9 compare their keys by the same rule, so a write to `Data/Foo` and a
  read of `DATA/FOO` meet, and a written file shadows the volume's file of any case. Nothing is cached: the volume is
  built for each session from the disk.
- **Two names the same but for case** (a Unix filesystem may hold `clash` and `CLASH`; 5.6 left the second out): both
  are kept; the exact name finds its own, another spelling the first found (the directory's order). `zine` says so
  once, before the program starts: "ZINE: Data/clash and Data/CLASH differ only in case, which AmigaDOS does not tell
  apart: a name the program uses finds the one spelt exactly so, else Data/clash". `zv_twin` (zvol.h) names the pair;
  `ZV_E_CLASH` is gone (the volume block is internal to one build, never frozen).

*Verified* in FS-UAE (`tools/zine-emu-test.sh`, A1200 68020 4 MB fast): `zcase` (`tools/tests/zine/zcase.S`) in a
folder the test makes (`Data/XYL.TP3`, `Data/Deep/Er/Mod.X`, `Data/clash` and `Data/CLASH`, `Data/Deep/Er/zexit`,
`LIBS/ZT.Library`): opens `DATA/Xyl.Tp3`, `PROGDIR:data/xyl.tp3`, `SYS:data/deep/er/mod.x`; `Data/clash` and
`Data/CLASH` each its own, `Data/Clash` the first; `LoadSeg("data/DEEP/er/ZEXIT")`; `OpenLibrary("zt.library")` loaded
from the folder's `LIBS/ZT.Library`; writes `data/NEW.txt` and reads `DATA/new.TXT`; writes `DATA/xyl.TP3` over the
folder's file and reads `Data/XYL.TP3` as written; deletes `data/Xyl.tp3`, then `DATA/Xyl.Tp3` is not found; return
code 0; the folder afterwards as before. The zine line about the pair is checked. On the PC (`tools/host/zine-test.sh`
core): the rule's letters and non-letters, the exact-first lookup, `zv_twin`, a path's every part without case, keys
of two spellings meeting. Not yet: `XYLOP96.exe` itself, on the owner's A1200.

Sizes (the 68020-60 build): `/bin/zine` 31,796 -> 33,108 bytes (the walk); the blob 47,480 -> 47,476 bytes (the exact-first lookup in, the clash check out); the kernel unchanged.

### 16.11 Execute runs a command that names a program file (2026-11-06, branch zexec)

Found on the owner's A1200: Spaceballs' HOG (its folder "044 HOG (Spaceballs)", main program `Stor_Gris.exe`) is a demo
made of separate programs named `1`, `2`, `3`, `4`, `45`, `5` ... `11` in its folder; the main program runs each with
dos `Execute("1", ...)`. 16.7 answered every one "not run (ZINE has no shell), answered as done", so nothing played
and the program ended with return code 1 after 0 s.

**Built** (`blibs.c` `c_Execute`, `blobs.S` `zb_runseg`, `zpath.c` `zx_parse` and `zx_known` with a host test; *read*:
the RKRM Includes and Autodocs' `Execute` and `Exit`, the AmigaDOS Manual's "Initial Environment in Assembler" and
its redirection rule, the NDK 3.2's `dos/dosextens.h` for the CLI's fields):
- **The line.** `[run] [<IN] [>OUT | >>OUT] NAME [<IN] [>OUT] ARGUMENTS` (`zx_parse`): a leading `run` taken off, the
  redirections before the name or right after it (the AmigaDOS Manual: they "MUST come before" the arguments; a `>`
  later is an argument), a quoted name, a `;` outside quotes a comment. Several lines run one after another; the rest
  is not run after one that fails or returns at least `cli_FailLevel` (10), as the Shell does.
- **Which commands run.** An Assign stays ZINE's own (16.8). A command of AmigaOS 3.1's Shell or C: (`zx_known`: CD,
  Echo, Stack, Wait, SetPatch, LoadWB, Mount ... about 70 names, by the name's last part) is not run and answered as
  done, as before: these act on a Shell or a system ZINE does not have, and a demo's own copy of SetPatch would stop
  at the first call ZINE lacks. Every other name is looked up as `Open` finds a file (the memory files of 16.9 first,
  the volume with its subfolders, names without case as 16.10, the assigns followed); a name with no `:` or `/` in
  the current folder (the program's) and then in `C:`. Found: it **runs for real**. Not found: "ZINE: NAME asked dos
  Execute to run "x": no program x in its folder or C: (a Shell says "x: Unknown command"), answered as failed",
  `DOSFALSE` and `ERROR_OBJECT_NOT_FOUND` (the autodoc: the answer says whether Execute "was successful in finding and
  starting the specified program").
- **How it runs** (`run_file`): `LoadSeg`, then on the same task as a Shell would run it, synchronously (after `run`
  too, said: "after run, but to its end before the program goes on: ZINE runs one task"): a fresh stack of the size a
  `$STACK:n` cookie in the file asks for (the form later Shells read), else the CLI's `cli_DefaultStack` (64 KB, the
  main program's), at least 8 KB and at most 1 MB (ZINE's own calls run on it too); the return address on it and its
  size at 4(sp); `a0` the argument line ending in `\n` (blanks at both ends off), `d0` its length; `a3` the segment
  list's first link and `a6` ExecBase as `zb_go` gives the main program; the task's `tc_SPLower`/`tc_SPUpper`,
  `pr_StackSize`, `pr_StackBase`, `pr_ReturnAddr`, `pr_Arguments`, `pr_CIS`/`pr_COS`, and `pr_CLI`'s `cli_Module`,
  `cli_CommandName` (the name as given, a BSTR) and `cli_CurrentInput`/`cli_CurrentOutput` its own, put back after.
  The current folder stays the main program's (a Shell's current folder does not change either); `PROGDIR:` is the
  command's own folder while it runs (an assign ZINE sets and removes; with all 8 assigns taken it stays the main
  program's, said). `Input()` and `Output()` are the redirections (`NIL:` keeps nothing, a file goes to the memory
  files of 16.9, `>>` at its end), else Execute's output handle, else the caller's; `PutStr`, `VPrintf` and
  `VFPrintf` follow them.
- **The end.** The command's `rts` (`zb_segret`) or dos `Exit` (inside a command it ends only the command, as the
  autodoc says for a program run under a CLI) comes back with its return code; the segments, the stack and the
  redirections' handles are freed, Execute answers `DOSTRUE`, and the return code is `IoErr()` and the CLI's
  `cli_ReturnCode`, `cli_Result2` the command's own `IoErr()` (where AmigaOS puts the return code for Execute's caller
  the documents do not say; this is ZINE's choice). The output says "ZINE: the command 1 ended with return code 0;
  Stor_Gris.exe goes on". Memory a command forgets to free is freed with the session, as before. The demo and its
  parts share the hardware in turn, as on AmigaOS: a part that leaves an interrupt running into its own code after it
  returns crashes there, as it would on AmigaOS once the Shell unloads it.
- **Inside one another.** A command's own Execute works the same, 8 deep at most (the ninth is answered as failed
  and the output says so). A crash or a call ZINE lacks inside a command ends the session as before.

*Verified* in FS-UAE (`tools/zine-emu-test.sh`, A1200 68020 4 MB fast): `zexec` (`tools/tests/zine/zexec.S`) runs
`zpart` (`zpart.S`, with a `$STACK:12000` cookie; every run checks `d0`, the `\n` at the line's end, 4(sp), the task's
stack bounds and `cli_Module`): `zpart >NIL: one  "two words"  ` (its argument line read back from the file it wrote,
its `Output()` line gone to `NIL:`, return code 5 as `IoErr()` and `cli_ReturnCode`); `parts/SECOND two` (a copy in
the folder `Parts` as `Second`: reads `PROGDIR:here`, its own folder's, and shows `cli_CommandName`); `zpart nest`,
which runs `run >NIL: zpart deep` (ends with `Exit(7)`: `IoErr()` 7 in the nested caller, return code 3); `zpart loop`
(8 inside one another, the ninth refused once); `nosuchpart x` (`DOSFALSE`, 205); `stack 20000` (not run); `AvailMem`
at the end as at the start, return code 0. On the PC (`tools/host/zine-test.sh` core): `zx_parse`'s forms, every
prefix of a line, the limits, `zx_known`. Not yet: HOG itself, on the owner's A1200.

Sizes (the 68020-60 build): the blob 42,112 -> 46,752 bytes (memory 47,476 -> 52,724: the parser and the command
list 1.1 KB, the run 2.6 KB, the messages); `/bin/zine` and the kernel unchanged. Each command's Execute takes about
1.4 KB of its caller's stack.

### 16.12 The Top 100's blockers (2026-11-07, branch zinefix)

The scene's Top 100 lists (500 productions) were run through ZINE in FS-UAE ([zine-top100-findings.md](zine-top100-findings.md)
ranks what stopped them). Fixed here, biggest win first (*read*: the RKRM Includes and Autodocs, the AmigaDOS Manual,
the NDK 3.2's include and `.fd` files for names and numbers; nothing from AmigaOS code):

- **exec `SuperState` (-150) and `UserState` (-156)** (20 productions): from user mode `SuperState` traps to ZINE's
  privilege handler, which goes on in supervisor mode on the caller's own stack (its return address popped, the
  caller's SR but S) and answers the system stack without the trap's frame in d0; in supervisor mode it does nothing
  and answers 0. `UserState(d0)` makes the stack it runs on the user stack, the system stack d0 again, and returns
  in user mode; from user mode it does nothing (blobs.S).
- **The cruncher's overlay form** (14: Love, Closer, Boogietown, Fullmoon, Real, faktory...): a header that lists two
  hunks (the second of size 0) with only the first before `HUNK_OVERLAY`, whose table is one longword (`$3F6`), then
  the packed data. The root's hunks that the file does not give before `HUNK_OVERLAY` are now allocated as the
  header sizes them and stay empty, as a loader that allocates the header's hunks first leaves them; the rest is
  16.6/16.7 (the overlay handle, the program reads its data through it). A node's hunks must still all come before
  its `HUNK_BREAK`, and a file that ends before its last hunk (Animotion, really cut short) is still refused.
- **Memory: the program loaded from its file** (the 7-26 MB AGA demos "cannot be read"; "does not fit" when the file
  and its hunks did not fit together). `zine` no longer reads the whole file into its own memory: the check walks it
  with only its first 16 KB (the header) in memory and a 4 KB window for the rest (`zh_check_rd`, hunk.h). When the
  file and its hunks fit in Zenix's free memory together (path A) the file is read once, straight into the volume,
  and the blob loads it in the session as before (overlaid programs, programs that read their own file). Otherwise
  (path B) `zine` loads the hunks itself before the hand-over, reading each hunk's data straight from the file into
  its place (`zh_load_rd`), into at most 8 regions of its own (`SYS_memalloc`, planned before the load, the largest
  hunks first: fast and any hunks in fast RAM, chip hunks in chip RAM, each region as large as what is left to place,
  halved down to the largest hunk left when that cannot be had; after the volume, the blob and the ZINE block have
  their fast RAM, which a fast region taken first could push into chip RAM, the program's), relocated
  to their real addresses; the volume's header names the segment list and the regions. For the blob those regions
  are the program's memory (`ZP_KEEP` in the planner, zplan.h): a chip one is neither saved nor given away (its
  header saved), a fast one counts as the program's for the half-the-fast-RAM rule; `LoadSeg` is skipped. The file
  is not kept: zine says so ("NAME is loaded from its file now ...; its file is not kept during the session"). Path
  B cannot hold an overlaid program (its parts come from its file) or chip hunks larger than the chip RAM Zenix has
  free at that moment (a chip hunk loaded in the session can have nearly all chip RAM, one loaded before cannot);
  each refusal says which.
- **The volume's limit: decided (owner's delegation, 2026-11-07).** Reading data files from the real folder on demand
  is not possible within D29: Zenix is paused during the session (owner answer 2), its filesystem servers do not run
  and ZINE has no disk driver of its own, nor may it write a disk Zenix's servers own; the volume stays in memory.
  What changed is the rule: at most 500 entries and 48 MB (was 16 MB: the block stays one `SYS_memalloc`), in **half
  the fast RAM the program leaves free** (its fast and any hunks, and its file in path A; was half the free fast
  RAM whatever the program needed): a demo usually reads its data files into memory of its own, so as much again
  stays for that. A folder over that is no longer all-or-nothing: its archives and disk images (`.lha`, `.lzx`,
  `.zip`, `.dms`, `.adf`, ...: what a download carries, never a file the program reads) are left out first, then
  the largest files, until the rest fits (every file of one size in or out together); each file left out is named
  (the first 5, then a count) and the line says the program will not find them. Only more than 500 entries still
  leaves only the program.
- **Calls** (bmore.c, bmath.c, blibs.c): exec `RawIOInit`, `RawMayGetChar`, `RawPutChar` (the debug serial port at the
  speed Zenix set; a timed wait), `CreateIORequest`, `DeleteIORequest`, `AddPort`, `RemPort`, `FindPort`; dos
  `IsInteractive` (the console's own handles are), `FRead`, `FWrite`, `GetProgramName`, `GetArgStr`, `ReadArgs` and
  `FreeArgs` (the template's /A /K /S /T /N /M /F, keywords and aliases, `KEY=value`, quotes and their `*`
  escapes, a /M giving back to /A items after it, the program's arguments or the RDArgs' CSource; errors 114-118 in
  `IoErr`), `AllocDosObject`/`FreeDosObject` (a FileInfoBlock, an RDArgs); graphics `OpenFont` (topaz 8: ZINE's
  stand-in for the ROM font, drawn from Zenix's own console glyphs, tools/mkzfont.py; any other name 0, said),
  `CloseFont`, `SetFont`, `AskFont`, `TextLength`, `Text` (the CPU into the RastPort's BitMap: JAM1, JAM2,
  COMPLEMENT, INVERSVID, the Mask), `Move`, `SetAPen`, `SetBPen`, `SetDrMd`, `RectFill`, `SetRast`, `InitRastPort`
  and a screen's RastPort with the default font, `GfxBase->DefaultFont`, and the V39 `SetChipRev` (the bits the
  chips have), `GetBitMapAttr`, `BestModeIDA` (the native mode for the size, depth and DIPF flags asked); intuition
  `LockPubScreen` (NULL or "Workbench": a 640-wide hires stand-in of 2 planes, made once, never shown),
  `UnlockPubScreen`, `AllocScreenBuffer`, `FreeScreenBuffer`, `ChangeScreenBuffer` (the buffer's BitMap shown; the
  DBufInfo's two messages replied at once). **utility.library** (version 40): the tag calls (`FindTagItem`,
  `GetTagData`, `NextTagItem`, `PackBoolTags`, `TagInArray`, `AllocateTagItems`, `CloneTagItems`, `FreeTagItems`),
  `CallHookPkt`, `SMult32`, `UMult32`, `SDivMod32`, `UDivMod32`, `SMult64`, `UMult64`, `Stricmp`, `Strnicmp`,
  `ToUpper`, `ToLower`, `Amiga2Date`, `Date2Amiga`, `CheckDate`, `GetUniqueID`. **The math libraries** (version 40):
  mathffp, mathtrans, mathieeesingbas/-singtrans and mathieeedoubbas/-doubtrans, every function of their `.fd`
  files, in software on every CPU (gcc's soft float; the transcendental functions ZINE's own), the condition codes
  set as the autodocs say for the compares and tests. FFP's two-argument calls take d1 and d0 as the autodocs word
  them (SPCmp "GT: fnum2 > fnum1", SPPow "fnum2 raised to the fnum1 power"), so `SPSub` and `SPDiv` answer d0 - d1
  and d0 / d1: *read*, not measured on AmigaOS.
- **Names for every offset:** `zine` names a stopping call from the NDK 3.2's `.fd` files when the RKRM's V37 table
  has none (`user/zine/lvo40.c`, made by `tools/zine-ndk-names.py`): graphics -888 SetChipRev, -960 GetBitMapAttr,
  -1050 BestModeIDA, intuition -768 AllocScreenBuffer, exec -912 (a private slot: `execPrivate33`), and the calls of
  utility and the math libraries (their numbers in the block's `lib`, kernel/zine.h).
- **Latin-1 file names:** a name on the Zenix disk that is not UTF-8 at all (copied byte for byte from an AmigaDOS
  disk) is taken as Latin-1 as it is; UTF-8 names are converted as before (zvol.c).

Not done: disk fonts (diskfont.library; display/server/amifont.c reads them, but OpenDiskFont is not ZINE's yet),
the 040/060-only and disk-image-only productions (out of scope), and every call still missing (the stop line names
it).

*Verified* in FS-UAE (`tools/zine-emu-test.sh`, A1200 68020 4 MB fast): `zsuper` (SuperState from user mode: the
same stack, S set, d0 not 0; again in supervisor mode: 0; UserState back; UserState from user mode: nothing);
`zcrunch` (zpkt.S as the cruncher's form, `tools/mkhunktest.py -c`: loaded, its data read through the overlay handle
with packets); `zcalls` (every call above called and its answer checked: the 'A' glyph's rows read back from a
BitMap, ReadArgs on its own arguments and on a CSource, 3 * 3, sqrt and sin in double, 10 / 2 and a compare in FFP,
the transcendental libraries, a line through RawPutChar on the serial port); `zbig` (`tools/mkbigtest.py`: a 1.5 MB
chip and a 1.25 MB fast data hunk and a BSS hunk, whose file and hunks do not fit together: loaded by zine, every
hunk's contents and memory type checked; its folder too big: the archive and the large file left out and named, the
small file found); a program whose file name is Latin-1 runs. On the PC (`tools/host/zine-test.sh`): the cruncher's
form and a cut root, the check and load from a file through a 64-byte window the same as in memory (three forms,
every prefix, a read that fails anywhere), the planner's kept blocks, the Latin-1 names, the volume's limits,
`lvo40.c` against the NDK's `.fd` files (with `ZENIX_NDK_FD`) and `zfont.h` against `font_8x8.c`.

Sizes (the 68020-60 build): the blob image 46,772 -> 76,232 bytes (memory 52,744 -> 85,768: the math libraries with gcc's soft float about 16 KB, utility.library, ReadArgs, the font and text about 8 KB, the stubs 4 KB); `/bin/zine` 30,872 -> 38,432 bytes (memory 33,132 -> 44,808: the NDK's names 3 KB, the file reader's 4 KB window, the volume's rules); the hunk core 5,225 -> 5,956 bytes at -Os; the kernel unchanged (183,508 bytes).

### 16.13 The Top 100's third round: the next calls, disk fonts, the black screens (2026-11-10, branch zine2)

The calls the 68030 + 68882 run of the Top 100 stopped at after 16.12 ([zine-top100-findings.md](zine-top100-findings.md)
"Round 3"), and their neighbours, as the owner decided (*read*: the RKRM Includes and Autodocs, the NDK 3.2's
autodocs, include and `.fd` files for names, numbers and documented behaviour; nothing from AmigaOS code):

- **exec** `Allocate`/`Deallocate` (a program's own MemHeader and chunk list: the first free block large enough, 8-byte
  blocks, a block given back only inside the header's bounds; blob.c shares the chunk code with AllocMem/FreeMem),
  `FindSemaphore`, `SumKickData` (0: ZINE has no KickMem or KickTag lists).
- **dos locks and directories** (blibs.c): `Lock`, `UnLock`, `DupLock`, `Examine`, `ExNext`, `Info`, `CurrentDir`,
  `ParentDir`, `DupLockFromFH`, `OpenFromLock`, `ParentOfFH`, `ExamineFH`, `NameFromLock`, `SameLock` (and `FilePart`,
  `PathPart`, `AddPart`; `Fault` and `PrintFault` with the texts of the commonest codes, else "error N"), on the volume's
  files and folders, the memory files (16.9) and RAM:'s top. A lock is a FileLock (20 bytes) in the program's
  memory; ZINE keeps which it gave (64 at once), so a lock it never gave is ERROR_INVALID_LOCK. `ExNext` lists a
  folder of the volume as it was at the start (a file written during the session is kept but not listed there;
  RAM:'s top lists the memory files written there), then ERROR_NO_MORE_ENTRIES. `Info` says the volume is
  write-protected (its writes go to memory). `CurrentDir` to a folder of the volume or RAM: makes every name without
  a device relative to it (turned into "SYS:folder/name" before anything else looks at it, so the memory files'
  keys stay one per file). `DateStamp`: Zenix's clock at the hand-over (`zine` puts it in the volume's header,
  `ZV_VERSION` 3) and the frames since.
- **A disk's name the program asks for** ("Love:FLT-Lve2.Skl", "sumea:beams.sum": Virtual Dreams' demos open their files
  on the volume they came on, and wait for it forever when it is not there): a volume or device ZINE does not have is
  taken as the program's own folder when the path is there, as "Assign Love: <its folder>" would make it under
  AmigaOS; said once a name. Not T:, ENV:, ENVARC:, CLIPS: or the stream devices. This answers the open item that
  the volume is named "ZINE", not after the program's disk, for the demos that name their disk.
- **graphics** `GetVPModeID` (ZINE's screens keep their display ID in a V39 ColorMap's `cm_VPModeID`, with `cm_vp`;
  another ViewPort's from its `vp_Modes` and the PAL or NTSC monitor), `ModeNotAvailable` (the native PAL/NTSC modes
  0, other monitors DI_AVAIL_NOMONITOR), `VideoControl` (border blank and border sprites, the sprite resolution, the
  playfields' sprite priority, the sprites' colour bases: BPLCON2-4 of the shown screen; the ColorMap's ViewPort and
  mode ID; VTAG_IMMEDIATE's flag cleared; a _GET tag answered in its own ti_Data; the genlock tags taken and
  changing nothing), `ScrollVPort`, `MakeVPort`, `MrgCop` (the shown screen shown again; every frame the screen's
  planes now come from its ViewPort's RasInfo: its BitMap, if the program set another, and RxOffset/RyOffset, the
  part of RxOffset below a word as BPLCON1's delay), `BltClear` (by the processor after the blitter, only in the
  program's memory), `GetSprite`/`FreeSprite` (numbers handed out), `Draw`, `PolyDraw`, `WritePixel`, `ReadPixel`,
  `WaitBOVP` (the beam below the ViewPort, at most a frame), the V39 double buffers `AllocDBufInfo`, `FreeDBufInfo`
  and `ChangeVPBitMap` (the ViewPort's RasInfo shows the BitMap; the two messages replied at once, as
  ChangeScreenBuffer's), ColorMaps (`GetColorMap`: a V39 one with its colour and low-nibble tables; `FreeColorMap`,
  `GetRGB4`, `SetRGB4CM`, `GetRGB32`, `SetRGB32CM`), `InitView`, `InitVPort`, and **a View of the program's own**:
  `LoadView` of a View that is not intuition's shows its first ViewPort as ZINE shows a screen (a screen of ZINE's
  own made for it: the ViewPort's BitMap, Modes and height; its RasInfo's offsets and BitMap every frame; the
  ColorMap's colours), `MrgCop` and `MakeVPort` show it again; until 16.13 such a View stopped the program. User
  Copper lists (`UCopperListInit`, `CMove`, `CWait`) are not shown yet.
- **intuition** `QueryOverscan` (ZINE's own rectangles: text 320 x 256 lowres pixels (NTSC 200), standard 352 x 283 from
  -16, -14, max and video 368 x 283 from -24, -14, scaled to the mode; under AmigaOS the user's Overscan preferences
  decide them), `RethinkDisplay`, `RemakeDisplay`, `MakeScreen` (the shown screen shown again), `MoveScreen` (ZINE's
  screens stay at the top), `WBenchToFront`/`WBenchToBack` (FALSE: no Workbench), `ActivateWindow`,
  `AllocRemember`/`FreeRemember`, `DrawBorder`, `EasyRequestArgs` (the requester's title, text and gadgets, its
  format strings filled as RawDoFmt does, in the program's output; the leftmost gadget chosen: 1, or 0 when there is
  one), `DisplayAlert` (its text in the output; FALSE, as the right mouse button).
- **Disk fonts: diskfont.library** (bfont.c; the library's number 11 in the block's `lib`). `OpenDiskFont` looks for a
  font in memory of that name and size (`OpenFont` now finds every font on graphics' list, nearest size), else
  `NAME.font` in FONTS: (a name with a path from there, else from FONTS: by its last part): the contents file is read
  by the desktop's reader (display/server/amifont.c `af_contents`), the font file checked by `af_load` in a check-only
  form (every length, offset, relocation and glyph; a hostile file gives its reason and nothing else), then loaded
  with LoadSeg as AmigaOS loads it and its TextFont named and put on graphics' list. A size the font lacks: the
  nearest it lists is given and said (AmigaOS would scale a bitmap), unless FPF_DESIGNED is asked. `Text` and
  `TextLength` now draw proportional fonts (CharSpace and CharKern, glyphs up to 64 pixels wide). `AvailFonts` lists
  the memory fonts and every size of FONTS:'s `.font` files. Where the fonts come from: `zine` puts Zenix's
  `/usr/share/fonts/amiga` (NAME.font files and their folders, as in FONTS:) into the volume's FONTS folder,
  merged with the program's own (its own files first), when they fit in what the program's files leave of the
  volume's budget and its 500 entries; else it says they were left out. A font that is not found is named in the
  program's output with its size and where to put it ("ZINE: NAME needs xen.font size 8: copy it into
  /usr/share/fonts/amiga ..."); a damaged one with its reason.
- **timer.device's units:** a request's unit was not looked at (every TR_ADDREQUEST a time from now); now
  UNIT_WAITUNTIL is a system time (GetSysTime's), UNIT_ECLOCK E-clock ticks from now and UNIT_WAITECLOCK an E-clock
  value (ReadEClock's), as devices/timer.h has them (`OpenDevice` keeps the unit's number in `io_Unit`).
- **The math libraries on the FPU** (bfpu.S): on a machine with an FPU (AttnFlags AFF_68881, AFF_68882 or AFF_FPU40)
  the arithmetic and conversions of bmath.c's soft float (libgcc's `__adddf3` ... `__fixsfsi`, 14 calls, through ld's
  `--wrap`) are one FPU instruction each (FADD, FSUB, FMUL, FDIV, FMOVE; FMOVE.L under round-to-zero for the
  conversion to int, as the 68040 has no FINTRZ; FP0 kept for the caller); without one, libgcc as before. The black
  screens of Unique's cathode and Supergroup's superkewl and supercool were their precalc in ZINE's soft-float
  math on a machine with a 68882.
- **The trace build** (tests only): its table of traced slots held 256, fewer than the slots ZINE had since 16.12 (it
  wrote past its end); now 512, and a slot past it is left untraced. It also writes `P` and the interrupted PC every
  250 frames from the vertical blank (when the program has not taken level 3 itself) and `C` with an address in the
  blob's code, so a run that shows nothing can be told apart: in a ZINE call, in the program's own loop, or no
  vertical blank of ZINE's at all; `Q` and the 16 bytes at that PC when it is the program's; `O` and each name Open is
  asked for; the program's output on the serial line as it is written. (Its slot table is searched on every call:
  a program that calls the math libraries a million times runs far slower in the trace build.)

Not done: the calls still missing (each stop line names it), disk images and 68040/060-only productions (out of
scope), a font file over 256 KB or taller than 64 lines (the reader's limits: refused, said).

*Verified* in FS-UAE (`tools/zine-emu-test.sh`, A1200 68020 4 MB fast): `znext` (tools/tests/zine/znext.S, run by
`zfix17` in a folder with Data/Sub/f.txt and a Fonts folder of its own) calls each of the above and checks the answer:
Allocate/Deallocate on a MemHeader of its own, Lock/Examine/ExNext/NameFromLock/ParentDir, a name relative to
CurrentDir opened and read, a disk's name found as its folder (T: not), WritePixel/ReadPixel, a ColorMap's colours, a
View of its own loaded and merged, ChangeVPBitMap, a UNIT_WAITECLOCK request 0.1 s ahead (0.05-1 s), FilePart/PathPart/AddPart, Fault and PrintFault's texts, GetVPModeID $29000, VideoControl's tags read back, QueryOverscan's rectangles, BltClear,
the pixels Draw, DrawBorder and PolyDraw set, the Remember list freed whole (AvailMem as before), the requester's and
the alert's lines; OpenDiskFont of a font in /usr/share/fonts/amiga (the proportional widths TextLength gives), of
one in the program's own Fonts folder, a missing one (the line), a damaged one (refused, its reason), a size the font
lacks (the nearest, said; refused with FPF_DESIGNED); AvailFonts. On the 68040 (FS-UAE's, with its FPU) `zcalls`'
math checks pass on the FPU path. The Top 100 productions these calls stopped were run again
(zine-top100-findings.md "Round 3").

Sizes (the 68020-60 build, *measured*): the blob image 76,232 -> 97,328 bytes (memory 85,768 -> 109,316: the locks and
directories about 3 KB, the display and intuition calls 5 KB, diskfont 2.5 KB and the desktop's font reader 4.4 KB,
the FPU wrappers 0.4 KB); `/bin/zine` 38,432 -> 40,152 bytes (memory 44,808 -> 46,932: the fonts' walk and the
volume's lookups); the kernel unchanged (182,820 bytes). ZINE is not on the DD demo floppy; the floppy's displayd
carries the font reader's check-only form (a few bytes): the DD floppy was already 512 bytes over before this
branch (main at 064777e: `make demo-dd` refuses it, packed stream 882,711 bytes; 882,790 here).

### 16.14 The 68040/060's missing instructions, and the early quits (2026-11-11, branch zine4)

**What a 68040 or 68060 lacks** (the CPUs' user's manuals): the FPU's transcendental and other instructions (FSIN,
FCOS, FTAN, FATAN, FETOX, FLOGN, FSINH, FTWOTOX, FMOD, FREM, FSCALE, FGETEXP, FGETMAN; on the 68040 also FINT and
FINTRZ; FMOVECR), packed decimal, denormalised and unnormalised operands, on the 68060 some effective addresses (an
immediate double or extended, FMOVEM with a register list in a data register) and the integer unit's 64-bit
MULU.L/MULS.L/DIVU.L/DIVS.L, MOVEP, CAS2, CHK2/CMP2 and a misaligned CAS. They trap (line F, the unimplemented data
type, effective address and integer vectors); AmigaOS's 68040.library and 68060.library emulate them. Until 16.14
ZINE named them in the crash line (16.5).

**Built:** Motorola's own packages, as the Linux kernel carries them (`user/zine/msp`, notice `msp/NOTICE`: Motorola's
licence allows use, change and distribution with the notice kept and changed versions named; GPL-2.0 compatible; the
Linux kernel, GPL-2.0, ships the same files): the M68060 Software Package's FPU and integer kernel modules (`fpsp.sa`,
`isp.sa`, Motorola's hex images, with its source and documentation) and the M68040 Floating Point Software Package
(its source in the GNU assembler's form). ZINE writes only the call-outs: `user/zine/zsp060.S` (the 060's 128-byte
call-out tables before each package, as its documentation lays them out) and `zsp040.S` (in place of the package's
sample skeleton: its handling of the FPU state's E1/E3 flags and the 68040 inexact bug, then ZINE's handlers).
- **Two files, loaded on demand:** `/lib/zine/sp060` (61,340 bytes) and `/lib/zine/sp040` (41,016 bytes), ZEXE images
  with a "ZSPK" header (the CPU, the hooks' address, the vector entries, an entry marked "FPU" only with one). `zine`
  loads and relocates them only on a 68040/060 (AttnFlags): the 060's on a 68060, both when AttnFlags say 68040 (a
  68060 without 68060.library at boot says so); not on Emu68, which runs every instruction itself. Their memory is
  zine's (Zenix's), named to the blob in the volume's header (`zv_hdr.sp`, ZV_VERSION 4); a 68020/030 loads nothing.
  A file missing or damaged is said and the session goes on without it. They are in the root image and ZINE's zup
  package, not on the demo floppies (their root is a RAM disk on every machine: 102 KB more RAM, 46 KB of the HD
  floppy, for the 040/060 alone).
- **The blob** (`sp_install`, before the program starts): on a 68060 (PCR read by zb_mmu_save) the 060 package, on a
  68040 with an FPU the 040 one; the header checked (every address inside the file, in fast RAM), the hooks set to
  ZINE's line-F handler (so the 030 MMU no-ops of 16.5 still work: the package passes an F-line that is no FPU
  instruction on) and its crash handler, the vectors 11, 48-55, 60, 61 pointed at the entry points. The FPU is
  "there" when `fmove.l fpcr` answers (zb_mmu_save's probe) or the 68060's PCR says a full 68060 (ID $0430; $0431 is
  the EC/LC part): FS-UAE's 68060 starts with the FPU switched off in PCR (DFP), as a 68060 does before 68060.library,
  and the package's "FPU disabled" call-out switches it on at the first FPU instruction (zb_mmu_back puts Zenix's PCR
  back after). An EC/LC 68060 gets the integer package only, said with the PCR. The program's output says it once:
  "ZINE: the 68060's missing instructions are emulated (Motorola's M68060 Software Package)".
- An FP exception the program enabled in FPCR, after the package's IEEE handling, ends at ZINE's crash handler (no
  task trap handler in ZINE; AmigaOS's default one shows an alert); zine names the vectors 48-55, 60 and 61 now.
- **The Zenix kernel** needs none of this: Zenix's programs are built `-msoft-float -m68020-60` (no FPU instruction, no
  64-bit MULU.L), and the kernel keeps no FPU state.
- **FS-UAE's CPUs** (*verified*, the PCR as the blob read it): `uae_cpu_model = 68060` alone is the EC/LC part (no
  FPU, PCR $04310103); with `uae_fpu_model = 68060` (or `cpu = 68060` and `fpu = 68060` in FS-UAE's own words) it is a
  full 68060 (PCR $04300103, DFP set). `uae_cpu_model = 68040` alone has no FPU; `uae_fpu_model = 68040` adds it.
  The test machines with FPUs are `fsuae/zine/a1200-040fpu.fs-uae` and `a1200-060fpu.fs-uae` (outside the matrix).

**The early quits of the Top 100 on the 68030** (zine-top100-findings.md "Round 4"): of the 50 that ended by themselves
with an error code, most ended after their full run (a return code left in d0: clean ends) or for want of memory on
the 16 MB test machine; the ZINE gaps found and filled here:
- **console.device** (blibload.c): a unit on a window as old programs open it (The Juggler): `OpenDevice` gives a
  ConUnit of ZINE's (its window, an 8 x 8 grid: cu_XMax, cu_YMax, the tab stops, the pens); CMD_WRITE's text goes into
  the program's output with the control sequences left out; CMD_READ stays in progress until AbortIO (ZINE gives
  programs no keys; WaitIO on it waits as Wait does); the other commands done at once; CONU_LIBRARY (-1) for
  RawKeyConvert, which gives no characters.
- **intuition ViewAddress** (-294): IntuitionBase's ViewLord.
- **input.device** (blibload.c): opened by programs that add an input handler (TBL's demo system, seen on the 68060
  machine once Ocean Machine had the memory): the handler is taken and never called (ZINE gives programs no input
  events); every command done at once; PeekQualifier 0.
- **The first large AllocMem that fails** (64 KB or more) is said once: "ZINE: AllocMem of N KB ... refused: the
  largest free block is K KB (C KB chip, F KB fast RAM free)". Most early quits were this (14-49 MB asked for).

*Verified* in FS-UAE (`tools/zine-emu-test.sh`): `zfpux` (tools/tests/zine/zfpux.S) computes 37 instructions and
compares each with its known value (FSIN ... FREM, FMOVECR, FSINCOS, a packed decimal and a denormal source, an
immediate double; MULU.L/DIVU.L with 64 bits, MOVEP.L, CAS2.L, CHK2/CMP2): every answer right on FS-UAE's 68030 +
68882 (in hardware), its 68040 with FPU (the 040 package) and its 68060 with FPU (the 060 package, the FPU switched on
by the call-out); `zfpu` (FSIN) returns 0 on both; zcrash and zcalls as before, Zenix's memory as before the sessions. `zcon`
(tools/tests/zine/zcon.S, on the 68040 machine): console.device's CMD_WRITE in the output without its control
sequence, a CMD_READ in progress until AbortIO (WaitIO then -2), the ConUnit's 80 x 25 grid; input.device's
IND_ADDHANDLER and IND_REMHANDLER; ViewAddress; a refused 1 GB AllocMem said.
One answer is left out of zfpux: FINT of 2.5 (a tie, to 2 under round-to-nearest) came out wrong on all three FS-UAE
FPUs alike, the 68882 included, so it is FS-UAE's rounding, not the packages' (2.7 to 3 is checked). Not yet on a real
68040 or 68060 (the Apollo 1260 is the owner's test).

Sizes (*measured*, the 68020-60 build): the blob image 97,328 -> 99,960 bytes (memory 109,316 -> 112,264: the
installer, console.device, the AllocMem line); `/bin/zine` 40,152 -> 41,392 bytes (memory 46,932 -> 48,172); the two
packages 61,340 + 41,016 bytes on the disk, in memory only on a 68040/060 during a session; the kernel unchanged.

### 16.15 The 68060's black screens, a second process, CAAR, the FPU in AttnFlags (2026-11-11, branch zine5)

**The 68060's black screens are not its data cache.** The round 4 rerun had 42 black screens on the 68060 machine, 6
of them working on the 68030; the guess was the 040/060's copyback data cache holding the CPU's chip RAM writes
(Copper lists, bitplanes) from the chips. Read from the code, ZINE already runs the program with the data cache off
on a 68040/060 (`zine_enter` clears CACR's DE/EDC and pushes the caches, 10.1), and Zenix's transparent translation
stays in place for the session (DESIGN D37: DTT0 makes the first 16 MB, chip RAM, the chips and the CIAs, inhibited
and serialized; DTT1 copyback for fast RAM), so a program that switches the data cache on itself with MOVEC (as
demos do) still writes chip RAM straight through. `zcache` checks that (below). FS-UAE does not model the 060's
data cache for the chips at all (zcache's control: a write kept in the cache on purpose is still seen by the
blitter), so no black screen in FS-UAE can come from it; on the Apollo 1260 the same set-up holds. What the six were
(targeted reruns): Silkcut and Little Nell "worked" on the 68030 only because a 28 MB / 14 MB AllocMem was refused and
they quit cleanly; on the 68060 they get the memory and are still loading at 50 s. superkewl and supercool precalc
for 734 and 1,933 s emulated on the 68030 and are still in it at 50 s. A500 Homage and Talking Heads show their
picture on the 68060 in every rerun (Talking Heads' comes after a precalc close to the 50 s limit): timing, not ZINE.
The other 36: see zine-top100-findings.md "Round 5".

**A second process** (dos `CreateNewProc` -498, `CreateProc` -138; superseded by 16.16, which runs it): Elude's demo engine (Dust, Soliloquy, Serenity,
Shake Off The Dust, kakao) starts a sound process ("[Sound]: Create process:14bit"). ZINE runs one task with no task
switching (8.4). Built: both calls answer as AmigaOS does when it cannot make a process (0, IoErr
`ERROR_NO_FREE_STORE` 103), said once with the process's name ("ZINE: NAME asked dos for a second process "N":
refused (ZINE runs one task), the program goes on without it"); the program takes its own error path instead of being
stopped. **What a real second task would take** (not built; it changes 8.4, so it is the owner's decision): a task
table of a few entries, each with its registers, USP, SR and (with an FPU) FSAVE/FMOVEM state, its own Process
(signals, `pr_MsgPort`, a stack from `NP_StackSize`) and its own `TDNestCnt`/`IDNestCnt`; `ThisTask` switched with
it; a switch when the running task blocks (`Wait`, `WaitPort`, `WaitIO`, `Delay`, a timer request) to the highest
ready one; a `Signal` (from a task or an interrupt server) that readies a higher-priority task switches at the
interrupt's exit (exec's preemption by priority; time slicing among equals optional); the child started from
`NP_Entry` or `NP_Seglist`, removed when it returns; the session still ends when the main program does. About 2-3 KB
of blob; nothing of Zenix runs meanwhile, only the program's own tasks. Most such processes are sound mixers woken by
an audio or timer interrupt, so preemption by priority is the part that matters.

**MOVEC CAAR on a 68040/060:** CAAR (the 020/030's cache address register) does not exist there: MOVEC to or from it
is an illegal instruction (MC68060UM, MOVEC). Paradox's Cobalt writes it after its CACR write and crashed on the
68060. The illegal-instruction vector (`zv_illegal`, `zb_illegal` in blob.c) now takes MOVEC to CAAR as a no-op and
MOVEC from it as 0 on a 68040/060, from supervisor mode only, said once ("ZINE: a MOVEC to or from CAAR (68020/030
only) taken as a no-op on this CPU"); anything else is the crash it was.

**The FPU in AttnFlags:** with the CPU package and an FPU (16.14), ExecBase's AttnFlags now also say `AFF_68881`,
`AFF_68882` and `AFF_FPU40` (as 68040/68060.library make them say). Elude's Lightshaft said "This program requires a
math co-processor" on the 68060 machine, whose Kickstart saw the FPU switched off; and ZINE's math libraries now use
the FPU there too.

**The volume's entries:** at most 2,000 files and folders (was 500; 5.6); Lorem Ipsum's Dolor Sit Amet has 671 in
21 MB, which the 68060 machine's memory takes. The other limits (48 MB, half the free fast RAM) are as before. An
entry is 136 bytes of the volume block, counted only for the entries there are.

*Verified* in FS-UAE (`tools/zine-emu-test.sh`): `zcache` (tools/tests/zine/zcache.S, on the 68040 and 68060
machines): the program switches the data cache on with MOVEC, writes a chip RAM line with the CPU and the blitter
copies it: the copy is right; the control (DTT0 made copyback for one write, inhibited again before the blit) says
whether the machine models the data cache (FS-UAE: it does not); MOVEC CAAR a no-op reading 0, said; AttnFlags say
the FPU; CacheControl leaves the data cache off. `zproc` (on the 68060 machine): CreateNewProc and CreateProc answer 0
with IoErr 103, said once with the name, the child's entry never run. Not yet on a real 68040 or 68060.
The targeted Top 100 reruns (zine-top100-findings.md "Round 5"): Soliloquy plays to its end and four more of Elude's
demos run silently past where they stopped; Cobalt runs on the 68060; Lightshaft loads.

Sizes (*measured*, the 68020-60 build): the blob's code 100,216 -> 100,928 bytes (memory 106,543 -> 107,261: the
illegal-instruction no-op, the two dos calls); `/bin/zine` 48,169 -> 48,173 bytes in memory; the kernel unchanged.

### 16.16 More than one task (2026-11-11, branch zinetask; the owner: "we want elude demos with sound")

The design, written before the code; it replaces 8.4's single-task rule and 16.15's refusal of a second process.
Facts from the autodocs (RKRM Includes and Autodocs, exec and dos; the NDK 3.2's `dos/dostags.h`,
`devices/audio.h` and `exec/tasks.h` for names and numbers).

**The rule:** the program's tasks are switched as exec switches them. The highest-priority ready task runs; among
equals, each runs for a quantum of 4 frames (ExecBase `Quantum`, as exec's default) and then the next of the same
priority; a task that waits (`Wait`, `WaitPort`, `WaitIO`, `DoIO`, `Delay`, `WaitTOF`, a semaphore held by another)
gives the CPU to the next ready one; a `Signal` (from a task or an interrupt) that readies a task of higher priority
than the running one switches to it at once (from a task) or when the interrupt returns (from an interrupt). With no
task ready the CPU stops (`stop #$2000`) until an interrupt. `Forbid` holds the running task (until `Permit` or until
it waits: the autodoc's "breaks the forbidden state"), and so does `Disable` ("implies Forbid"); each task keeps its
own `TDNestCnt`/`IDNestCnt` (`tc_TDNestCnt`, `tc_IDNestCnt`), and the interrupts are on while a task that waited with
them disabled is away. Only a task interrupted **in user mode** is switched: code in supervisor mode (an interrupt,
`Supervisor`, `SuperState`) is never preempted. Nothing of Zenix runs: the tasks are all the program's.

**ZINE's own code is not preempted.** An interrupt that would switch tasks while the CPU is inside the blob (a library
call in progress) leaves the switch pending: it happens at the next interrupt that finds the task in its own code, or
when the call waits or yields. So the blob's functions need no locks against each other between tasks (exec's
`Forbid` in its own functions has the same effect); only what interrupts also touch (the task lists, a port's message
list, the signals) is changed with the interrupts held off (`INTENA`'s master bit, put back only if it was on). The
blob's waiting loops (`WaitTOF`, `WaitBlit`, `Delay`) are switch points when tasks are waiting to run.

**One task costs nothing new.** Until the program makes a second task, ZINE runs as before (the single-task paths of
8.4's time); the interrupt's return tests one byte. The first `AddTask` or `CreateNewProc` turns task switching on for
the rest of the session.

**A task's state:** its registers (d0-d7/a0-a6, USP, SR, PC) and, with an FPU, its FPU state (`FSAVE`, and when that
frame is not a null frame the eight FP registers and FPCR/FPSR/FPIAR; on a 68060 whose FPU is still switched off in
PCR, nothing) are kept in a ZINE record of about 450 bytes from the program's memory (the first task's in the blob);
`tc_SPReg` gets the stack pointer as exec's does. The lists are exec's own: the running task in `ThisTask`, the ready
ones in ExecBase `TaskReady` by priority (Enqueue: first in, first out among equals), the waiting ones in `TaskWait`;
`tc_State` `TS_RUN` 2, `TS_READY` 3, `TS_WAIT` 4, `TS_REMOVED` 6. A switch is the privilege-violation trick ZINE
already uses (5.2): the task's `ori.w #0,sr` at one known address traps, the trap saves the task and runs the next
with `rte`; an interrupt's return goes the same way when a switch is pending.

**The calls:**
- exec `AddTask(task a1, initialPC a2, finalPC a3)`: the caller's Task (its `tc_SPReg`, `tc_SPLower`/`Upper`,
  name, priority); initial registers 0 but the PC; `finalPC` (0: ZINE's own end, as exec's "general finalizer") is
  the return address on its stack; `tc_Flags` cleared, `tc_SigAlloc` = ExecBase `TaskSigAlloc`; answers the task.
  `RemTask(task a1)` (0: the caller): the task leaves; its `tc_MemEntry` lists are freed (`CreateTask` puts the Task
  and its stack there), and what ZINE allocated for it. `FindTask(name)` searches the running, ready and waiting
  tasks. `SetTaskPri` sets the priority, answers the old one, and switches if another now comes first.
- `Signal`, `Wait`, `SetSignal`, `AllocSignal`, `FreeSignal` on any task (the last three on the caller's).
- Ports: `PutMsg` signals the port's `mp_SigTask` (`PA_SIGNAL`), calls `mp_SoftInt` (`PA_SOFTINT`, as `Cause`) or
  nothing (`PA_IGNORE`); `ReplyMsg` puts the message on its `mn_ReplyPort` as `NT_REPLYMSG` (none: `NT_FREEMSG`);
  `WaitPort` waits for the port's signal while its list is empty; `GetMsg`, `PutMsg`, `ReplyMsg` hold interrupts off
  around the list (messages are also replied from interrupts: the timer, audio.device).
- Semaphores: `ObtainSemaphore` (and the shared form, taken as the exclusive one) waits while another task holds it
  (a `SemaphoreRequest` on `ss_WaitQueue`, `SIGF_SINGLE` bit 4, as exec's); `ReleaseSemaphore` hands it to the first
  waiter; `AttemptSemaphore` answers 0 when another task holds it.
- dos `CreateNewProc(tags d1)` with `NP_Seglist` or `NP_Entry` (one is needed: else 0), `NP_FreeSeglist`,
  `NP_StackSize` (default 4,000 bytes, at least 1,024), `NP_Name` (copied; default "New Process"), `NP_Priority`
  (default 0), `NP_Arguments` (`pr_Arguments`; d0/a0 at entry, as a Shell gives them), `NP_Input`/`NP_Output`
  (default the session's console handles), `NP_CurrentDir` (default the creator's), `NP_WindowPtr`, `NP_ExitCode`
  and `NP_ExitData` (called with d0 the return code and d1 the data when it ends); the others are accepted and
  ignored (`NP_Cli`: no CLI structure, `Cli()` answers 0 in the child). The Process is the creator's twin in what ZINE
  has: `pr_MsgPort` (signal 8), `pr_CIS`/`pr_COS`, `pr_StackSize`, `pr_StackBase`, `pr_SegList` 0; answers the
  Process. `CreateProc(name d1, pri d2, seglist d3, stack d4)` the same from a segment list, answering the process's
  `pr_MsgPort` (the autodoc's "process identifier"), 0 when it cannot.
- **The end of a task:** a process or task whose entry returns, calls dos `Exit` or `RemTask(0)` ends (its exit code
  hook first, its seglist unloaded with `NP_FreeSeglist`); the session goes on with the others. **The session ends
  when the program's first process ends**, whatever its other tasks are doing (their memory goes with the program's).
- `Delay` and graphics' `WaitTOF`, with other tasks: the task sleeps (in `TaskWait`, no signal used) until its frame
  comes (CIA-A's frame counter, 5.4's clock); the vertical blank wakes it. `timer.device` requests are completed by the
  vertical blank (and by the idle loop), so a task waiting for one does not hold the CPU; `WaitIO` and `DoIO` wait for
  the request's reply-port signal.

**audio.device plays (it only gave channels before, 16.8):** `CMD_WRITE` queues on its channel (the lowest one in
`io_Unit`; a channel the request does not hold, by `ioa_AllocKey`, answers `ADIOERR_NOALLOCATION` as the autodoc
says) and plays through Paula's DMA: `ioa_Data` (chip RAM), `ioa_Length` (2-131,072 bytes), `ioa_Cycles` (0:
for ever), with `ADIOF_PERVOL` the period and volume, with `ADIOF_WRITEMESSAGE` `ioa_WriteMsg` replied when it starts.
The channel's audio interrupt (level 4, `AUD0`-`AUD3`) moves it on: each interrupt means the buffer in the registers
has started, so the next write's address and length are loaded behind it and Paula plays them without a gap; a write
is replied (`zb_io_done`) when its last cycle has played; after the last queued write the channel's DMA goes off.
`CMD_STOP` holds the channels (writes queue), `CMD_START` starts them together (one `DMACON` write, so 14-bit pairs
stay in step), `CMD_FLUSH` and `CMD_RESET` end every write with `IOERR_ABORTED`, `AbortIO` one, `ADCMD_FINISH`
ends the playing write after its current cycle, `ADCMD_WAITCYCLE` is replied when the playing write's cycle ends,
`ADCMD_FREE` and closing the device end the channel's writes. A channel's interrupt is ZINE's only while
audio.device has writes on it: a program's own level 4 handlers (`SetIntVector`, as module players have) are called
as before for the other channels.

**Not built** (each named if a program asks): task exceptions (`SetExcept`, `tc_ExceptCode`), traps (`AllocTrap`),
`tc_Switch`/`tc_Launch` hooks, a CLI for a new process (`NP_Cli`), `RunCommand`, `SystemTagList`. **Limits:** the
number of tasks is what memory allows; a task's stack has no guard (as on AmigaOS).

**Size and speed** (the budget): about 3 KB of blob for the switching, the calls and audio.device's playing; a task
switch is about 70 instructions plus the FPU's state. The single-task path: one `tst.b` at each interrupt's return.

**Tests** (FS-UAE, `tools/zine-emu-test.sh`): `ztask` (two tasks ping-ponging signals and messages, priorities and
preemption from an interrupt, Forbid holding the task, a semaphore waited for, a task's end, RemTask of another;
on the 68020 and with the FPU state on the 68060), `zsound` (a process from `CreateNewProc` plays a sound through
audio.device, double-buffered with two writes, while the first task draws; the channel's DMA seen on, the writes
replied in order, the time they took close to the sound's length). The existing tests and the Top 100's single-task
programs must run as before.

**As built (2026-11-12):** `user/zine/btask.c` (the lists, the choice, the calls), `blobs.S` (the trap, the switch,
the interrupt's return, Permit/Enable, the CFS stubs), `bfpu.S` (the FPU's state), `blibload.c` (semaphores between
tasks, audio.device's playing), `blibs.c` (ports, ReplyMsg, WaitPort, WaitIO, Delay, WaitTOF, timer requests
completed by the vertical blank). `zproc` (16.15's refusal) is gone with the refusal. The trace build's line every
250 frames now also says DMACONR, audio.device's writes and interrupts, the task switches, ThisTask and the level 4
interrupts (`A`), for checking a program's sound.

*Verified* in FS-UAE (`tools/zine-emu-test.sh`): `ztask` and `zsound` on the 68020 machine and the 68060 machine
(there with "FP0 kept for each task"); the rest of the test programs as before. The Top 100: Elude's five make and
run their sound process and the four channels' DMA is seen on; in FS-UAE the emulated 68060 decodes their music at
about a quarter of real time, so the sound comes in bursts (zine-top100-findings.md "Round 6"). Not yet on a real
machine.

Sizes (*measured*, the 68020-60 build): the blob's code 100,928 -> 106,988 bytes (memory 107,261 -> 113,899): the
task switching and calls 3,434, audio.device's playing about 1,600, the rest in the stubs and the trace line; more
than the 2-3 KB estimated above, mostly audio.device. `/bin/zine` and the kernel unchanged.

### 16.17 Input for programs, gadtools, the unsettled ones (2026-11-12, branch zine7)

**The rule:** the session owns the machine (8.1), so ZINE gives a program the keyboard and the mouse as AmigaOS does,
through the OS's own paths, and only when the program asks for them that way. A program that takes the hardware itself
(its own level 2 or level 3 vector, a `ciaa.resource` handler for the serial port's interrupt, the interrupts switched
off, the CIA or `JOYxDAT` read directly) never meets this code and works as before.

**The path** (`user/zine/binput.c`; the RKRM Devices manual's input, console, keyboard and gameport chapters, the RKRM
Libraries' IDCMP and keymap chapters, the HRM's keyboard and controller port sections; the NDK 3.2's
`devices/inputevent.h`, `input.h`, `keyboard.h`, `gameport.h`, `intuition/intuition.h` for numbers):
- **The keyboard:** CIA-A's serial port interrupt (level 2) already took the byte and gave the handshake (8.3); the code
  (the byte inverted and rotated right) goes into a ring of 32, nothing else happens in the interrupt. The keyboard's
  own codes (`$78` the reset warning, `$F9`-`$FE`) are not keys.
- **The vertical blank** (level 3) makes the InputEvents: `IECLASS_RAWKEY` for each key (the qualifiers held,
  `IEQUALIFIER_NUMERICPAD` for the pad, the previous two key-downs in `ie_position` for dead keys), `IECLASS_RAWMOUSE`
  for port 0's movement (`JOY0DAT`'s counters, `IEQUALIFIER_RELATIVEMOUSE`) and buttons (CIA-A PRA bit 6, `POTINP` bit
  10; `IECODE_LBUTTON`/`RBUTTON`, up with `IECODE_UP_PREFIX`), and with a handler installed an `IECLASS_TIMER` about
  ten times a second. The chain goes through the program's input handlers of priority above 50 (highest first; a0 the
  events, a1 `is_Data`, d0 the chain passed on), then intuition's part, then the handlers of 50 and below. This runs in
  the interrupt, where AmigaOS runs it in input.device's task at priority 20: the handler sees the same events.
- **Intuition's part:** the active window is the last one opened or the one `ActivateWindow` names
  (`WFLG_WINDOWACTIVE`, IntuitionBase `ActiveWindow`). It gets `IDCMP_VANILLAKEY` for a key-down that is one character
  on the keymap, else `IDCMP_RAWKEY` (key-ups and the qualifier keys too); `IDCMP_MOUSEBUTTONS` (`SELECTDOWN`/`UP`;
  `MENUDOWN`/`UP` with `WFLG_RMBTRAP`, else a right-button release is an `IDCMP_MENUPICK` with `MENUNULL`: ZINE draws
  no menus); `IDCMP_MOUSEMOVE` with `WFLG_REPORTMOUSE` (`IDCMP_DELTAMOVE` honoured); `IDCMP_INTUITICKS` ten times a
  second; at most one MOUSEMOVE and one INTUITICKS not yet replied, as intuition. The window's `MouseX`/`MouseY`
  (and `GZZMouseX/Y`), its screen's and IntuitionBase's follow the pointer, kept inside the screen. The IntuiMessages
  (`im_IAddress` pointing at the previous keys, as MapRawKey wants them; the time in `Seconds`/`Micros`) come from a
  pool of 32 made with the first window; a message the program replies is free again, one the pool lacks is dropped.
  A window's port now has a signal of its own (`AllocSignal(-1)`), freed with the window.
- **console.device:** a `CMD_READ` is done at once when typed characters wait in the unit's buffer (64), else when the
  next come: at least one, at most `io_Length`. The characters are the USA keymap's (the ROM's default; Shift, Caps
  Lock, Ctrl), the cursor keys `CSI A`-`D` (shifted `CSI T`, `S`, `CSI " A"`, `" @"`), F1-F10 `CSI 0~`-`9~` (shifted
  `10~`-`19~`), Help `CSI ?~`. `RawKeyConvert` (-48) and keymap.library's `MapRawKey` give the same for one event;
  `MapANSI` the other way; `CDInputHandler` passes the events on.
- **input.device:** `IND_ADDHANDLER`/`REMHANDLER` change the handler list (an Interrupt whose `is_Code` is not in
  the program's memory is refused with `IOERR_BADLENGTH` rather than called), `IND_WRITEEVENT` sends the program's event
  through it (and intuition's part) at once; `PeekQualifier` (-42) answers the qualifiers held.
- **keyboard.device** (new): `KBD_READEVENT` gives the queued key events (16 kept while it is open; done when one
  comes), `KBD_READMATRIX` the 16 bytes of keys held, the reset handler commands are taken. **gameport.device** (new),
  units 0 and 1: `GPD_SETCTYPE`/`ASKCTYPE` (mouse, relative or absolute joystick), `GPD_SETTRIGGER`/`ASKTRIGGER`,
  `GPD_READEVENT` done when the trigger is met (a button down or up as `gpt_Keys` asks, a movement of `gpt_XDelta`/
  `gpt_YDelta`, a direction change of an absolute joystick, or `gpt_Timeout` frames).
- **The cost when nobody listens:** none of it runs until a program installs a handler, opens a window, a console unit,
  keyboard.device or gameport.device (`zi_live`); the keyboard's byte is dropped as before and the vertical blank tests
  one byte. A single-task `WaitPort` on an empty port no longer stops the program while the input can still fill it.

**gadtools.library** (`user/zine/bgad.c`, library number 13): enough for a program that builds its gadgets and menus
with it and reads its window through it (Efreet's deeppan opened it, did not check the answer and called
`GT_GetIMsg` through base 0). `CreateContext`, `CreateGadgetA` (a Gadget from the NewGadget, linked behind the
previous; 0 when the previous is 0, the autodoc's chaining), `FreeGadgets`, `CreateMenusA` (Menu and MenuItem lists
from the NewMenu array, with `GTMENU_USERDATA` behind each, sub-items, `NM_IGNORE`, disabled and check flags, the
command key), `FreeMenus`, `GetVisualInfoA`/`FreeVisualInfo`, `GT_GetIMsg`/`GT_ReplyIMsg` (exec's GetMsg and ReplyMsg),
`GT_FilterIMsg`/`GT_PostFilterIMsg` (the message itself), `LayoutMenusA`/`LayoutMenuItemsA` (TRUE); the drawing and
refresh calls, `GT_SetGadgetAttrsA`, `DrawBevelBoxA`, `SetDesignFontA` and `ScaleGadgetRectA` do nothing,
`GT_GetGadgetAttrsA` answers 0. ZINE draws no gadgets and no menus. **keymap.library** (number 12): `MapRawKey`,
`MapANSI`, `AskKeyMapDefault` (a KeyMap with empty tables), `SetKeyMapDefault` taken.

**The screens in depth order** (found with the demos below): IntuitionBase `FirstScreen` and each Screen's
`NextScreen` are kept as intuition keeps them (a new screen in front, `SA_Behind` at the back; `ScreenToFront`,
`ScreenToBack` (-246, new) and `CloseScreen` move them); `LockPubScreen`'s stand-in (16.12) is not among them, and
intuition's View's `ViewPort` stays 0. With a front screen there, Elude's 14bit sound driver asks the display
database for its mode's monitor (before, it found no screen and skipped it), so graphics' **display database** came
too: `FindDisplayInfo` (-726, a handle for a native PAL or NTSC mode, 0 for another), `NextDisplayInfo` (-732, the
native modes in turn) and `GetDisplayInfoData` (-756): `DTAG_DISP` (properties, ticks per pixel, the colours'
bits), `DTAG_DIMS` (depth, sizes, the nominal rectangle), `DTAG_MNTR` (a frame's rows and colour clocks: 312 x
227 PAL, 262 x 227 NTSC), `DTAG_NAME` ("PAL:LowRes" ...), as many bytes as asked, 0 for another tag.
Closing the shown screen shows the next one (before: the blank display). `RethinkDisplay`/`RemakeDisplay` give
intuition's View Copper lists as AmigaOS does: when the program has cleared `View.LOFCprList`, a new record of
ZINE's (a `cprlist` header, then "ZCPR", the screen and the BitMap its RasInfo shows now) is put there, else the one
there is renewed; `LoadView` of intuition's View shows the front screen with the record's BitMap (taken at the next
vertical blank, so no tear), and `FreeCprList` (-564, new) frees a record. The Juggler double-buffers that way: it
keeps two such lists made with its two BitMaps, and each frame sets `RasInfo->BitMap` (where its RastPort draws)
and the View's `LOFCprList` (what is shown: the other one) and calls `LoadView(ViewAddress())`.

**The demos** (zine-top100-findings.md "Round 7"): The Juggler shows its text, starts its HAM movie at a key and
ends at Esc (return code 0); Ephidrena's J waited for `IntuitionBase->FirstScreen` to be its screen before its
vertical-blank code signals the main task, and now ends by itself (253 s); deeppan opens its window through gadtools
and runs (before: a crash through base 0). Not ZINE's: Fetish loops opening `CrM.library` (Crunch-Mania's, not in
its folder); Gift needs AHI (`ahi.device` with a mode requester and a mixing player: an AHI is a project of its own,
so ZINE keeps answering `IOERR_OPENFAIL`, and Gift's own cleanup then calls through a base of 0); Lola, total
triple trouble and The Fall take the machine (Lola waits in `STOP` for its own interrupt's flag; the other two make
no call and run their own interrupts); My Kingdom part 1 runs its exit code twice and calls LoadView through the
GfxBase it cleared the first time.

*Verified* in FS-UAE: `tools/zine-input-test.sh` (the test program `zinput`, keys typed on the emulated keyboard in
the cage rig: a, b, c, F1, a joystick's fire in port 0, q): console reads, VANILLAKEY and RAWKEY through
`GT_GetIMsg`, MOUSEBUTTONS, the input handler, keyboard.device, MapRawKey, the gadtools calls, `FirstScreen`,
`GetDisplayInfoData`'s monitor rows; the
rest of the test programs as before (`tools/zine-emu-test.sh`). Not yet on a real machine.

Sizes (*measured*, the 68020-60 build): the blob's code 107,012 -> 117,252 bytes (memory 113,923 -> 125,373): the
input path about 5.9 KB (binput.c, and 1 KB of buffers: the key ring, a frame's events, keyboard.device's queue),
gadtools and keymap 1 KB, the display database about 1.2 KB, the screens' order, the Copper list records,
FreeCprList and ScreenToBack the rest. The
IntuiMessage pool (32 x 60 bytes) comes from the program's memory with its first window. `/bin/zine` 48,173 ->
48,213 bytes in memory (two library names); the kernel unchanged; the HD demo floppy 296,960 -> 289,280 bytes free
(its RAM disk 12 KB larger unpacked).

### 16.18 Round 7's "regressions", lowlevel.library, the DOS list, the ZINE block's spare end (2026-11-12, branch zine8)

**The 68060's twelve "regressions" were not caused by round 7.** The 68060's "before" results were the round 4 rerun
(`runs-060-before-zine5`); 11 of the 12 had ended there with return code 0 after 7-258 s, which the counting rule
takes as working, but each had printed why it quit: "FPU required", "error: fpu required (if you have 040/060, run
setpatch first)", "68020+ and FPU required!", "FPU: None ... These were not detected on your system". Round 5 made
AttnFlags say the FPU the 68060 package gives (16.15), so they now go on and meet what ZINE still lacks
(zine-top100-findings.md "Round 8"). Read from the old and new runs' logs.

**lowlevel.library** (library number 14; `user/zine/binput.c`, beside the input it reads; names and numbers from the
NDK 3.2's `libraries/lowlevel.h` and `lowlevel_lib.fd`, what each call does from its public description). Flex's and
Ephidrena's demo system opens it and calls through the answer unchecked (Sojourn of the Soul and Dead Ahead crashed
through base 0; Software Make The Dance Foam Oil, fake elektronik lightshow and Respirator Stories quit after 70-90 s).
- `ReadJoyPort(port d0)` (-30): port 0 a mouse (`JP_TYPE_MOUSE`, JOY0DAT's counters, left button red, right blue),
  port 1 a joystick (`JP_TYPE_JOYSTK`, the four directions from JOY1DAT, fire red, the second button blue), unless
  `SetJoyPortAttrsA` (-132, `SJA_Type`, `SJA_Reinitialize`) forced a type; ports 2 and 3 `JP_TYPE_NOTAVAIL`. A CD32
  pad reads as a joystick (its extra buttons are not clocked).
- `GetKey` (-48): the key held (its raw code, 0xFF none) and the qualifier keys as `LLKF_` bits (the IEQUALIFIER low
  byte moved to bit 16); `QueryKeys` (-54) from the key matrix; `AddKBInt`/`RemKBInt` (-60/-66): routines called for
  each key event (at the vertical blank, where ZINE makes its key events, 16.17). Opening the library makes ZINE take
  the keys (it counts as a listener, `zi_live`).
- `AddVBlankInt`/`RemVBlankInt` (-108/-114): one routine, a server on exec's VERTB chain. `AddTimerInt`,
  `StartTimerInt`, `StopTimerInt`, `RemTimerInt` (-78 .. -96): CIA-B's timer B (else A) through ZINE's ciab.resource,
  loaded with the microseconds in E-clock ticks (at most 65,535: 92 ms PAL), continuous or one-shot.
- `ElapsedTime` (-102): seconds since the context's EClockVal as 16.16 fixed point (ZINE's E-clock has a frame's
  resolution, 5.4); `GetLanguageSelection` (-36) LANG_ENGLISH; `SystemControlA` (-72) takes every tag and answers 0
  (the program has the machine already).

**dos:** `CompareDates` (-738, the AmigaDOS manual's order: below 0 when the first date is the later);
`LockDosList`/`AttemptLockDosList`/`UnLockDosList`, `NextDosEntry`, `FindDosEntry` (-654 .. -690): a DOS list of the
program's volume (`DLT_VOLUME`, the name Info and Lock know), the RAM device and its volume "Ram Disk", `dol_Task` the
handler port (16.7); `GetDeviceProc`/`FreeDeviceProc` (-642/-648) and `DeviceProc` (-174): the handler port for every
name (a volume ZINE does not know is the program's folder, 16.13); a second GetDeviceProc with the first's DevProc
answers 0 and `ERROR_NO_MORE_ENTRIES`. Asked for by Reborn (CompareDates) and THE CURE (LockDosList, then
GetDeviceProc of "ZINE:").

**The ZINE block's spare end:** `zine` allocated the block as Zenix's used chip RAM plus 256 KB before the plan
knows its tables; what the blob needs ends at its supervisor stack (about 75 KB of the 256 on the test machines). The
rest (about 180 KB on the 68020 test machine) was held for nothing during the session. The planner's new `zp_give`
(zplan.c, host-tested) gives it to the program as one more chunk of its fast MemHeader, in address order, refused
unless it lies inside one used Zenix block of a fast range and overlaps no chunk. That chunk is not next to the
program's largest one (the block's own start lies between), so `zine` now allocates 128 KB beside the saved chip RAM
instead of 256: the block ends 128 KB lower and the largest free chunk next to it is that much larger; a block that
proves too small is asked for again with the size the blob names (b->need, as before). Seen with Dual Panoply on the
68020: its 2,349 KB AllocMem missed the largest free block by 13 KB after round 7's blob grew, and still by 12 KB with
the spare end given as a chunk of its own.

**User Copper lists** (graphics/copper.h of the NDK 3.2 for the structures; asked for by Loonies' The Morning Trip
on the 68030 and 68060): `UCopperListInit` (-594) makes a CopList block of n instructions for the UCopList,
`CMove` (-372) and `CWait` (-378) write the one at `CopPtr`, `CBump` (-366) moves on (a full block gets another of
the same size behind it), `FreeCopList` (-546) and `FreeVPortCopLists` (-540) free them. When a screen or a View of
the program's own is shown, the ViewPort's `UCopIns` instructions follow the screen's own in its Copper list: a MOVE
as it is (none to the copper's own registers or below $40, which would stop the copper), a WAIT at the ViewPort's
line plus its DyOffset and the display window's first line ($2C), past line 255 after the copper's wait for the end
of line 255. The screen's Copper list grows to hold them (its size kept beside it).

**A big program's file is not kept when nothing needs it.** Path A (16.12) keeps the program's file in the volume
beside its hunks whenever both fit; on the 68020 test machine that took 3-4 MB of 8 from AGA demos whose next
AllocMem then failed (Smoke Bomb asked 3,162 KB with 3,048 KB in one piece; fake elektronik lightshow and Massive the
same way). `zine` now takes path B (the hunks loaded from the file before the hand-over, the file not kept) also when
the program is plain (no overlays, at most 4 bytes after its last hunk: nothing it could read itself), its file is at
least 128 KB and a sixteenth of the free memory, its chip hunks leave at least 64 KB of the chip RAM free now with
the largest one finding a free piece (path B puts them in the chip RAM Zenix has free; in the session the program
then has the same chip RAM as on path A), and its largest other hunk would land below 16 MB (Zenix's allocator gives
big blocks from the top, so on a machine with 32-bit RAM as well path B would put the program there, where path A's
AllocMem starts low: Virtual Dreams' sumea, from 1992, then showed nothing on the 68030 machine); it says so ("its file (N KB) is not
kept during the session, which leaves the program that much more memory"). The rule is `zh_lean` (hunk.c,
host-tested). A program that opens its own file to read its hunks' bytes would not find it; none of the Top 100 was
seen doing that (the ones that read data from their own file keep it after an overlay table, and stay path A).

**Copying:** the blob's `zmemcpy` (dos `Read`, `CopyMem`, the saved chip RAM) copied bytes whenever source and
destination were not both long-aligned; it now aligns the destination and copies longs four at a time (a 68020 and
later read a long at any address). Demos that read their data in odd-sized pieces spent minutes of the 68060's
emulated time there (The Martini Effect: a `Read`, then a thousand `CopyMem` calls of a few bytes each).
`RectFill` set or complemented a pixel at a time, plane by plane; it now works a
byte at a time, the edges masked. Efreet's deeppan clears its window with RectFill every frame: its frames stayed
off the screen for minutes of emulated time.

**The trace build** writes each AllocMem's size, attributes and answer (`M`), and on a crash in supervisor mode the
supervisor stack above the frame (`S`) with the code before the first stacked address that looks like a return.

*Verified* in FS-UAE: `tools/zine-input-test.sh` on the 68020 and the 68060 machine (zinput opens lowlevel.library:
SystemControlA, ReadJoyPort's two types, GetKey in its keyboard interrupt, the keyboard, vertical-blank and 20 ms
timer interrupts counted, ElapsedTime; the test's 68060 run had never found its disk images before, fixed),
`tools/zine-emu-test.sh` (zscreen: a user Copper list's MOVE to DMACON at line 10 seen in DMACONR, gone after
FreeVPortCopLists; znext: the DOS list, GetDeviceProc, CompareDates, CopyMem at odd addresses, RectFill set
and complemented across bytes; znext had 2,709
KB of fast RAM before and 2,890 KB with the block's spare end), `tools/host/zine-test.sh` (zp_give: in address order,
refused in a free block, over a chunk, in chip RAM, across two blocks, over a header, with a full array; zh_lean's
eight cases). Not yet on a real machine.

Sizes (*measured*, the 68020-60 build): the blob's code 117,252 -> 122,470 bytes (memory 125,373 -> 130,645):
lowlevel.library about 2.3 KB, user Copper lists about 1.2 KB, the DOS list, GetDeviceProc and CompareDates about
1 KB, the rest the copy, RectFill, zp_give and the trace lines. `/bin/zine` 45,904 -> 46,376 bytes in memory (a
library name, zh_lean, the chip probe and its message); the kernel unchanged.

### 16.19 Low memory on request, sprites, the null bases (2026-11-12, branch zine9)

**Low memory on request** (the main session's decision on round 8's question). `SYS_memalloc`'s second argument has a
new bit, `MEMA_LOW` 2 (docs/claims.md, docs/frozen-interfaces.md): the region ends below 16 MB, cut from the low end
of a free block (the kernel's `MEM_LOW16M`, beside the SD driver's `MEM_LOW1G`); without fast RAM there it falls back
to chip RAM as a plain call does, and is 0 when nothing fits. With memory protection on it is whole pages as before,
also taken from the low end (a limited block was cut from the high end of a free block that crossed the limit before,
which could end above it). *Verified* on the host: `tools/mem-test.sh` (a range crossing 16 MB: small and big blocks
below it, an ordinary big one still above, one too big refused, whole pages with protection on, a range wholly above
gives none). The kernel's mem.o grew 40 bytes, kernel.bin 193,948 -> 194,012.

**ZINE's loader asks for it** (`user/zine.c`). Path B's fast regions (16.12) are asked for below 16 MB first
(`MEMA_LOW`), else anywhere: an old program's hunks land where a 24-bit program expects them whenever they fit there,
as path A's AllocMem starts low too. A big plain program (16.18's `zh_lean`) takes path B when its chip hunks find
chip RAM and its other hunks together a piece below 16 MB, or at least the largest one a piece anywhere (16.18 asked
for the largest one below 16 MB from an allocator that gives big blocks from the top, which on the 68030 machine left
lux aeterna, You are Lucy, Outside Planet Earth and Concrete on path A, where their big AllocMem failed). Found on
the way: round 8's last commit had lost the line that sends a lean program to path B, so the rule of 16.18 was not
used at all on main; it is back, and `zlean` (below) tests it. And `zh_lean` now also needs memory to be short: the
program's file and hunks leave less than 16 MB of the free memory (on the 68060 test machine, with 72 MB, Kilofix
crashed late on path B where it runs on path A; the file is kept there as before, which costs nothing a program
misses). Host-tested (`tools/host/zine-test.sh`, two more zh_lean cases).

**Sprites** (graphics; the RKRM Libraries' sprite chapter and the NDK 3.2's autodocs for what each call does,
graphics/sprite.h for the structures, the AHRM and the AGA documents for SPRxPOS/SPRxCTL and FMODE; `user/zine/bnext.c`).
Asked for by Loonies' Impossible and The Morning Trip (AllocSpriteDataA).
- `GetSprite` (-408, 16.13) and `FreeSprite` (-414) as before (1-7 first, 0 the pointer's last); `ChangeSprite` (-420)
  shows the data at the sprite; `MoveSprite` (-426) writes the two control words from x and y in the ViewPort's
  resolution (hires and superhires halved and quartered, interlace halved, the ViewPort's offsets and the display
  window's corner $2C/$81 added; a SimpleSprite one pixel to the left, as the autodoc's BUGS section says), the
  ATTACHED bit kept; an attached partner moves along.
- `AllocSpriteDataA` (-1020): a BitMap converted (planes 0 and 1, or 2 and 3 for `SPRITEA_Attached`), `SPRITEA_Width`
  16, and 32 or 64 on AGA (else 0), `_XReplication` / `_YReplication` (-2 .. 2), `_OutputHeight`, `_OldDataFormat`;
  the data in chip RAM aligned to its fetch: the control words' two fetches, each row's plane words, two empty
  fetches. `FreeSpriteData` (-1032) frees only ZINE's own (a mark after the ExtSprite).
- `GetExtSpriteA` (-930): `GSTAG_SPRITE_NUM`, `GSTAG_ATTACHED` (a pair, even and odd, the odd one's ATTACHED bit set);
  refused (-1) when sprites of another width are in use, as the autodoc says. `ChangeExtSpriteA` (-1026).
- Shown through the screen's Copper list (and the blank one): its first 32 words are the eight sprite pointers,
  renewed in place when a sprite changes (the null sprite where none is shown); FMODE's sprite bits (32 or 64 wide)
  follow the widest sprite taken.

**dos DoPkt** (-240, asked for by THE CURE after GetDeviceProc): the action done at once for ZINE's handler port (every
file handle's, lock's and DOS list entry's port, 16.7), dp_Res1 the answer and dp_Res2 IoErr(); another port is no
handler ZINE knows (DOSFALSE, ERROR_ACTION_NOT_KNOWN). The handler port knows more actions now, for DoPkt and PutMsg
alike: LOCATE_OBJECT, FREE_LOCK, COPY_DIR, EXAMINE_OBJECT, EXAMINE_NEXT, DISK_INFO, INFO, PARENT, CURRENT_VOLUME and
IS_FILESYSTEM, each through the dos call that does the same.

**64 files open at once** (was 16). Ephidrena's Hexel opens its precalculated tables and closes not every one; after
16 open handles every Open failed ("Unable to open ram:packscript for write or append"), its loader went on with
null pointers and crashed later (at $5FE after 1,537 s on the 68060: one of round 8's "calls through a base of 0").
A limit that is hit now says so once in the program's output.

**The trace build** writes the program's free memory chunk by chunk at the start (`K` address, size).

**Not settled** (zine-top100-findings.md "Round 9"): Loonies' "CIA-timer could not be allocated!" (Impossible, The
Morning Trip: they look for a free CIA timer without calling the resource), the crashes of Jesus Christ Motocross,
Lupus Reditus and Beam Riders on the 68060 (no failing call before them in the trace), and destroy fascism's stack in
ZINE's ExecBase page on the 68020.

*Verified* in FS-UAE: `tools/zine-emu-test.sh` (zscreen: a 16x16 sprite from a BitMap by AllocSpriteDataA, taken as
sprite 2 by GetExtSpriteA, MoveSprite's control words checked, and the hardware's collision of sprite 2 with plane 1
seen in CLXDAT only while it is shown, none after FreeSprite; znext: DoPkt to the handler port and to a non-port, 40
files open at once; zlean, a new 420 KB plain program built by `tools/mkbigtest.py`: its file not kept, its fast hunk
loaded from the file below 16 MB). Not yet on a real machine.

Sizes (*measured*, the 68020-60 build): the blob's code 122,470 -> 125,908 bytes and its bss 8,119 -> 8,963 (the
sprites about 2.4 KB, DoPkt and the handler's new actions about 0.8 KB, 48 more file slots 768 bytes of bss);
`/bin/zine` 46,452 -> 46,520 bytes in memory; the kernel 64 bytes (above).

### 16.20 The CIA resources' own fields, MLISA, the trace build's memory dump (2026-11-13, branch zine10)

**The CIA resources as a program finds them without calling them.** Loonies' replayer (Impossible, The Morning
Trip) never calls `OpenResource` or `AddICRVector` to find its timer. Read from its own code (dumped from memory, below):
between `Forbid` and `Permit` it takes the first node of ExecBase's EXTER chain (`IntVects[13].iv_Data`'s `lh_Head`),
steps back 42 bytes to the resource's base, and takes timers A and B as free when bits 0 and 1 of the byte 40 bytes
in are clear; then it writes CIA-B's registers and calls `AddICRVector` through that base. In its timer B interrupt it
writes a new handler straight into the resource's vector for bit 1 (80 bytes in). So ZINE's `ciaa.resource` and
`ciab.resource` (`user/zine/blibs.c`) now hold, after the Library: at 40 the mask of the bits that have a handler,
at 42 the resource's own Interrupt (`zs_goon`, which leaves the interrupt to the next server), first in the PORTS
(CIA-A) and EXTER (CIA-B) chains, at 64 the five vectors as exec's IntVector (data, code, node). `AddICRVector`
fills a vector, `RemICRVector` clears it, the mask follows both, and the level 2 and 6 dispatchers call the code and
data in the vector, so a handler written there is the one called. 124 bytes after the Library (was 34). Before, the
first EXTER node was the list's tail: the byte read as the mask was not 0 and both intros printed "CIA-timer could
not be allocated!". Both now play (The Morning Trip on the 68030 and 68060 machines, Impossible on the 68060; on
the 68030 machine Impossible asks for 8,790 KB in one piece, and that machine's fast RAM is two blocks of 8 MB). Its clouds are hardware sprites (16.19): in a
scratch build that showed only the null sprite they were gone from the same scene.

**MLISA** (round 9's open question on Nerve Axis' Relic). Relic's own check reads GfxBase's `ChipRevBits0` and
wants more than 15: `GFXF_AA_MLISA` (16) besides ALICE and LISA. A real A1200 shows `$1F` after SetPatch's
`SetChipRev(SETCHIPREV_BEST)` (knowledge base `amiga/iff-ilbm-viewer.md`, *verified* on the A1200 2026-10-09);
ZINE gave `$0F`. On AGA it is `$1F` now; `SetChipRev` answers it.

**The trace build's memory dump.** Packed productions unpack into memory the file does not show (16.19). The trace
build now writes, on the way back, the ring of the program's last 16 library calls (`L`: the slot and the caller's
return address), 1 KB around each of the last 8 callers, 2 KB around a crash's PC, and then the program's hunks
(`H` the address and size, the first 512 KB) as `D` lines (an address and 8 longs). A host script turns them into
binaries for `objdump`. Lines can be lost on the serial line (a few gaps of 1.5 KB in 300 KB were seen).

**What the dumps showed** (zine-top100-findings.md "Round 10"):
- **destroy fascism** (68020): not a memory problem. Its unpacked code (at `$50000`, which is in the program's free
  chip RAM already) sets its stack to `$1000` (the end of ZINE's low page, where nothing of ZINE's lies above
  `$B90`), then copies five routines one after the other to `$2000` and calls each without flushing the instruction
  cache. The 68020's cache still holds the previous routine: it crashes. With the instruction cache off (a scratch
  build) it runs to the limit. A 1993 68000 intro: a real A1200 with its cache on does the same. Not changed (a
  question for the owner: start such programs with the cache off?).
- **Lupus Reditus** (68060): it plays its whole show and its end part, which waits for the Esc key. Its timeline
  table (frame ranges and routines, walked by its own vertical-blank handler) has no end mark: some 7 minutes after
  the last entry the walk reaches the code after the table and calls a value from there. Its own fault, the same on
  a real machine.
- **Beam Riders** (68060): reads the VBR, puts its own level 3 handler there (FSAVE/FMOVEM around a callback in an
  unpacked part); the crash is a jump to `$100` from there. The callback's code fell in a gap of the dump. Not
  settled.
- **Jesus Christ Motocross** (68060): the crash is an `rts` to 0 at the end of its sine and square-root precalc: the
  user stack is 12 bytes short, as if `fmove.x (sp)+,fp7` (or the `fmove.x fp7,-(sp)` at the routine's start) had
  not moved it. A test program doing the same (an unnormalized and a zero value, in user mode, under the 68060
  package) moves it right. Not settled.

*Verified* in FS-UAE: `tools/zine-emu-test.sh` (zlibs: the first nodes of EXTER and PORTS 42 bytes into the
resources, the mask 40 bytes in, a handler written into timer A's vector called by CIA-B's interrupt and the old one
not, the mask clear after `RemICRVector`; zcalls: `SetChipRev` `$1F` on AGA). Sizes (*measured*, the 68020-60 build):
the blob's code 125,908 -> 126,588 bytes (the dump about 0.4 KB, called only in the trace build), its bss 8,963 ->
8,923; the two resources take 180 bytes more of the ZINE block.

### 16.21 A part's hunks handed over in cli_Module, the trace dump made whole (2026-11-13, branch zine11)

**Execute unloads what cli_Module holds** (`user/zine/blibs.c`, 16.11's run of a command). The Black Lotus' Tint
and Captured Dreams run their parts through dos Execute; each part is packed with CrunchMania, whose first hunk (432
bytes) decrunches the rest, puts the list of the other hunks in the CLI's `cli_Module` and unlinks itself from it. A
Shell unloads what `cli_Module` holds when a command ends (the AmigaDOS manual's CLI structure: the loaded command); ZINE
unloaded the list it had loaded, of which only the decruncher was left, so every part's hunks stayed: by the fourth
part of Tint chip RAM was down to 45 KB (each part takes 300-985 KB of it) and the part was "not started". Now the
list in `cli_Module` is unloaded (none when it is 0: a command that keeps itself), as the Shell does; the 432-byte
first hunk stays, as it would there. Captured Dreams now plays on the 68030 and 68060 machines, Tint all seven of
its parts (23 minutes 45 seconds; it ends with return code 1).

*Verified* in FS-UAE: `tools/zine-emu-test.sh` (zexec runs `zpart hand`, which makes a 64 KB block a segment linked
to its own list and puts it in `cli_Module`; zexec's AvailMem is as at its start only when that list is unloaded:
without the fix zexec ends with 8).

**The trace dump made whole.** Round 10's dump lost lines on the way to the PC. The cause was on the PC: the serial
tap (`tools/serial-tap.py`) searched the whole log on every read (the greeting's search copied it, a wait searched
everything since it began), so on a trace build's megabytes its reader fell behind, the pty filled, and FS-UAE dropped
what it wrote (in a Beam Riders dump about 40 % of the second pass, in runs of 2-3 KB). Both searches now cover the
new part only (`tools/tap-test.sh` checks 60,000 lines at about 0.9 MB/s: none lost; the old tap lost half). The dump
itself is also sturdier: each `D` line ends with a check long (the nine longs, each rotated left by one into it),
the dump is sent twice (`G 1`, `G 2` before each pass), and paced (64 beam lines a line, about 4 ms: FS-UAE's port
does not wait for the baud rate). `tools/zine-dump.py LOG DIR` drops a damaged line, takes each line from the pass
that has it whole, writes one binary per piece and lists the gaps left in the hunks (`--test`: its own check, run
by `tools/host/zine-test.sh`). Jesus Christ Motocross's dump: both passes whole, no damaged line.

**The trace build counts the CPU support package's entries.** Its vectors point at small stubs (blobs.S) that count
each entry and keep the last 32 (the vector, the stacked SR and PC), then go on into the package with the stack as the
CPU left it. On the way back: `F` (a vector and its count) and `Q` (the last 32). Jesus Christ Motocross and a test
program with its sine and square-root precalc take the same entries: one line F, its first FPU instruction with
the FPU switched off (PCR's DFP), and none else: FS-UAE's 68060 does `fsin`, `fsqrt`, `fintrz` and `fmovecr` itself,
so the package does not run for them, and the 12 bytes its stack is short come from elsewhere.

**What the whole dumps showed** (zine-top100-findings.md "Round 11"):
- **Beam Riders** (68060): its level 3 handler (FSAVE, FMOVEM, then the INTREQ bits) calls its replayer's frame
  routine, which counts frames, asks for the end at a frame limit through entry 28 of its own jump table (a flag),
  calls a per-frame routine (the music player and two hooks, both empty) and a list of queued routines. None of
  these jumps to `$100`, and the dumped code has no jump through a register plus an index that could; the crash
  comes with d2-d7 just saved above the frame by a `movem.l d1-...` of its own. Not settled: a return or `jsr (an)`
  through a bad value, or code outside the dump.
- **Jesus Christ Motocross** (68060): the package is not involved (above); the crash is the same `rts` to 0 with
  the saved FP7 (+0.0) still on the stack. Not settled.

**Memory on the 68030 machine** (task of this round): the harness's 68030 machine has 16 MB of fast RAM as two blocks
of 8 MB (FS-UAE's `fast_memory`, 24-bit at `$200000`, and `zorro_iii_memory`); 15 productions there fail only on an
AllocMem of 9-16 MB in one piece (Impossible, Nature's 4k intros, Mo'saik...). An A1200 has no Zorro slots: a 68030
board with 16 MB on one SIMM gives one block (the Blizzard 1230-IV's SCSI kit adds a second SIMM socket, so two
blocks exist, rarely). The split is the harness's, not ZINE's; left as it is (the harness is not in this repository).
The 68060 machine is the same: 8 MB at `$200000` and 64 MB of Zorro III memory (72 MB in all; the real 1260 has 64 MB
in one block). There, Starstruck's 48 MB request fails for another reason: its 18.7 MB data file lies in ZINE's
volume in fast RAM (5.6), so the largest free piece is 43.6 MB; a real machine reads the file from disk.

Sizes (*measured*, the 68020-60 build): the blob's code 126,588 -> 127,000 bytes (the dump's check and second pass,
the stubs' hook, the Execute fix); the trace blob's bss 13,155 -> 13,607 (the counts and their ring).

### 16.22 The user vectors, exec's puddles, a bad FreeMem named (2026-11-13, branch zine13)

**Round 12's "newly failing" first** (zine-top100-findings.md "Round 13"): each was run twice more with round 12's
build. Seven were noise (they ran this time: on the 68020 machine Psychedelic and Clubisque, on the 68030 Real,
Supercool, Talking Heads and Zeon; on the 68060 Noxie ran in one of three runs). The real ones are below.

**The user vectors point at a low stub** (`user/zine/blob.c`, `ZUSERVEC` in `blob.h`). Limbo's Three Men and a Glenz
Vector clears address 4 itself (`move.l #0,4.w`, then `jmp $1000` into its copied code); its replayer then reads
ExecBase through that 0: it tests the 68010 bit of `AttnFlags` at 297 (`$129`) and, when it is set, calls
`Supervisor` at -30 through the base of 0, a jump to `$FFFFFFE2` (68020 machine: "line F at $FFFF"; 68030: a call
reported as -912). A null base reads the vector table: `$128` is vector 74, a user vector (64-255; no Amiga device
supplies a vector number). AmigaOS's exec fills those with a "no handler" stub in the ROM (the ROM boot-sequence
hand-off, step 29), so their high word is `$00F8` or so and bit 0 of `$129` is clear; ZINE filled them with its crash
path in the ZINE block (`$0093xxxx` on the 68020 machine, so the bit was set, and whether it was depended on where the
block lay: the production ran in earlier rounds). Now the 192 user vectors point at a 6-byte `jmp` to the crash path
at `$0BA0` in the low page (above the null sprite), high word 0. Three Men plays on the 68020 and 68030 machines.

**Pools as exec keeps them** (`c_CreatePool` and the rest, `blob.c`). ZINE made every `AllocPooled` block its own
AllocMem with its link and size in the 8 bytes before it. Haujobb's Signals writes 4 bytes before one of its 100-byte
pooled blocks; the size there became about 3 GB, and at its end (after 1,300 s) `DeletePool`'s FreeMem of it ended in
`AN_BadFreeAddr` (`$8100000F`; with two memory blocks in round 11 the layout differed and it ended cleanly). Now a pool
is what the autodoc (the NDK 3.2's exec CreatePool) describes: puddles of `puddleSize` bytes, each with a MemHeader,
the blocks of `threshSize` bytes or less made in them by `Allocate`, so no bookkeeping lies next to a block (4 bytes
before one is the padding of the block before it, as in exec's puddles); a larger block gets a puddle of its own; a
puddle whose blocks are all freed goes back; `CreatePool` refuses a `threshSize` above `puddleSize`, as the autodoc
says; `MEMF_CLEAR` clears each block. Signals now ends by itself after its whole show on the 68060 machine.

**A bad FreeMem says which block.** Before `AN_BadFreeAddr`, ZINE writes "FreeMem of N bytes at A, in no memory list"
into the program's output: that line pointed at the pool above.

**Not ZINE's, or not settled:**
- **Elysium** (Sanity, 68020 machine): black in six runs on the 68EC020 and a CHK instruction crash in a seventh; on a full 68020 and on a 68030 with the
  same 24-bit fast RAM it plays. It copies itself to `$800` from supervisor mode and decrunches there; what the EC020
  model does differently was not found.
- **sumea** (68030 and 68060): with all fast RAM in one block at `$40000000` (round 12's machines) it loads its files
  and stops; with round 11's 8 MB below 16 MB it plays. A 1992 program; the knowledge base's note on high fast RAM
  (Bacon of Hope) is the same. Real CPU-slot boards keep their RAM high too. A question (17.6).
- **Serenity** (Elude, 68060): its pool grows to about 55 MB; ZINE leaves it 53.5 MB of the 64 MB (Zenix, ZINE and its
  6 MB data file in ZINE's volume), and an `AllocPooled` of 129 KB is refused; with round 11's 72 MB it plays. ZINE's
  volume design (5.6).
- **Software Make The Dance Foam Oil**: on the 68030 machine its two 1 MB tables are left out of ZINE's volume (half the
  free fast RAM, 3 MB, and its 7.6 MB music) and "demo_init() failed" follows "Unable to load file
  'data/torus512.dat'"; its round 11 file on disk was older than round 11 (`asmstart() failed`, before lowlevel), so
  it was not newly failing. On the 68060 its precalculation is still running at 400 s (black).
- **the castle** (Loonies, 68060): stuck in its own Allocate/Deallocate loop on a dark screen in every run; round 11's
  dark brown screen was just over report.py's "black" threshold.
- **Goon Royale** (Spaceballs, 68060): a zero divide after 3,608 s in every run (round 10's too): after an hour.

*Verified* in FS-UAE: `tools/zine-emu-test.sh` (zsuper: every user vector's high word 0 and no 68010 bit at 297, step
8 fails without the change; zalloc: a pool of 4096/1024 with `MEMF_CLEAR`, a 316- and a 100-byte block 320 bytes apart
in one puddle, both cleared, a long written before the second, a 2000-byte block apart, all freed, a fourth, DeletePool,
AvailMem as at the start; zalloc b: FreeMem of 64 bytes at `$F80000` named, then the alert). The test's own memory
check was moved past one thing: zalloc b runs after the last reading, because one more command line grew the
shell's history heap by 6 KB (shown with the same line only echoed).

Sizes (*measured*, the 68020-60 build): the blob's code 127,000 -> 127,544 bytes (pools about 0.4 KB, the FreeMem line
and the stub the rest); bss unchanged.

### 16.23 Big data files read from the disk, chip RAM only, ExecBase written over (2026-11-13, branch zine14)

**Big data files stay on the disk** (a design change to 5.6; `user/zine.c`, `user/zine/blibs.c`, `kernel/zine.c`,
`user/fsd.c`, `kernel/ext2.c`). Until now every file of the program's folder was copied into ZINE's volume, in memory,
before the hand-over, in at most half the fast RAM the program leaves free. A program then reads a data file into
memory of its own, so a big file was in memory twice, and a folder bigger than the budget lost its largest files
("left out"). Round 13 counted 48 failing productions on the 68020 machine, 34 on the 68030 and 6 on the 68060 with
files left out; the 68060's Serenity failed with its 6 MB file in the volume (its pool needs 55 MB of the 64).

*The design.* A file of 256 KB or more (`ZV_DISKMIN`) that is no program file (its first longword not `$3F3`) and not
one of Zenix's fonts (`/usr/share/fonts/amiga`) stays on the disk. `zine` asks where its sectors lie and puts that list in the volume instead of the bytes
(`struct zv_ent nx`, `struct zv_run`); ZINE's dos reads them from the drive when the program asks (`Read`, packets,
`Seek` as before), while Zenix is paused.
- *Where the sectors lie.* `SYS_zine` op `ZINE_MAP` (root's; `struct uzine_map`, frozen): the kernel asks the file's
  filesystem server with `FS_BMAP` (new, frozen: runs of sectors on the server's device from a file's sector; holes
  as `0xFFFFFFFF`). The built-in ext2 server answers it (`ext2_runs`, from the block map); every other server answers
  -ENOSYS and its files stay in memory as before. `zine` adds the partition's start (the mount's device from
  `SYS_mountinfo`, matched by `stat`'s device; its start from the disk server's `DISK_LIST`) and keeps only devices of
  the drive `ide0`.
- *Who reads.* Only the IDE drive (the A1200's Gayle port, the A4000's): the kernel answers `ZINE_MAP` with -ENODEV
  when its disk is another (the PiStorm's SD card, the A3000's SCSI), and `zine` then keeps every file in memory. The
  kernel gives the blob the drive's registers (`struct zine_args ide`, layout 2); the blob reads with its own small
  reader: polled (the drive raises no interrupt, nIEN as Zenix left it; Gayle's latch is cleared by the kernel's
  `ide_zine` on the way back), LBA28, the kernel's sectors a command and long or word moves, the status register and
  the program's interrupts left alone, every wait timed by the beam's frames (1,600 frames: 32 s PAL, 27 s NTSC; CIA-B's
  counter may be the program's), with a count of looks as a backstop. Aligned whole sectors go straight into the
  program's buffer, the rest through a cache of 8 sectors. A failed read resets the drive (SRST, nIEN kept), so the
  next command, the session's or Zenix's, meets a drive at rest; it answers -1 with `IoErr` 226 and is said once. The kernel's own IDE driver is not
  called: it writes the kernel's memory (which the blob checks at the way back) and masks interrupts.
- *Nothing is written.* The disks were synced before the hand-over and other programs' requests are held (7.2 step
  5), so the sectors are the file's. A file changed between `zine`'s question and the hand-over (another user's
  program in those milliseconds) would be read as it was asked: a member of the group zine can do anything to the
  machine during a session anyway (owner answer 1).
- *What stays in memory.* Program files (LoadSeg, Execute and the overlays read from memory), Zenix's fonts, files
  under 256 KB, a file whose server cannot say, more than 65,536 runs. `LoadSeg`, `Execute`, `OpenLibrary` or
  diskfont of a big non-program file left on the disk (one that could never load) is refused and said; `MODE_READWRITE`
  copies it into a memory file 2 KB at a time. `zine -m` keeps every file in memory, as before. A block number out of
  the volume (a damaged block map) ends the runs there: the file then stays in memory, whose read meets the damage as
  before.
- *Trade-offs.* Memory: the program gets the bytes the copies took (the 68030 machine's Foam Oil: 10 MB of its folder
  stay on the disk). Speed: the reads take the drive's time during the session, as under AmigaOS from a hard disk
  (a demo that streams from its file, as AmigaOS would read it); the polled reads take the CPU, the program's
  interrupts go on. A cut in the middle (Ctrl-Amiga-Amiga) is safe: nothing is written. Size: kernel.bin + 672 bytes
  (`ZINE_MAP`, the built-in fsd's `FS_BMAP` and `ext2_runs`); the mount tests' RAM disk 683 -> 682 KB for it.
- *Not done.* Other filesystems (ZenixFS, FAT, FFS through their servers) and other drives; a write to such a file
  is kept in memory as any write (16.9).

**Chip RAM only, or no fast RAM above 16 MB** (question 6, decided by the main session: yes). `zine -c` (or
`--chip-only`) gives the program chip RAM only, as AmigaOS's NoFastMem: its memory lists hold no fast RAM, its hunks
go into chip RAM (the volume, ZINE's block and the blob stay in fast RAM, which nobody else uses meanwhile).
`zine -l` (`--low`) leaves out the fast RAM above 16 MB (a block across it cut there): the 24-bit addresses a 1992
program may assume. The mode travels in the volume's header (`mem`, `ZV_VERSION` 5); the blob builds the memory lists
without the rest, and zine's "had N KB fast RAM" line says what the program could see. Neither saves sumea on the
68030 machine: its hunks need 3.3 MB, more than the chip RAM, and the machine has no fast RAM below 16 MB (as a real
A1200 with a CPU-slot board).

**ExecBase written over** (`zb_irq`, `blob.c`). ZINE's ExecBase lies in the low page at `$800`. Sanity's Elysium
turns its interrupts off, copies its decruncher to `$100` (over the user vectors), points the illegal-instruction
vector there, and decrunches 274 KB of itself to `$800`. An interrupt the program has not taken over would then go
through ZINE's handler, which reads exec's vectors from the program's bytes and jumps there. AmigaOS's ExecBase is
elsewhere, so its handlers still work in that case. Now the handler first checks exec's library node (its type and
its negative size). When they are not exec's, it acknowledges the request (the CIA's register read, a key's
handshake) and calls nothing. This is a real hole, and zsuper's test shows it: without the check, the test crashes.

It is not the whole of Elysium's trouble.
- Plain runs on the 68EC020 machine after the change: it played in 3 of 4 and stayed black in 1.
- The trace build failed in 3 of 3: "called a library (offset -0, from `$12C`)". That is a slide through the vector
  table into exec's jump table below `$800`, as in round 12.
- What sends it into low memory, and why it depends on timing, was not found.

*Verified* in FS-UAE: `tools/zine-emu-test.sh` (zbig: zbig.huge, 400 KB, read from the disk, its first longword, an
aligned 1 KB, 8 bytes through the cache, 8 across a sector's end, the last 8 and the end; `zine -m` as before:
left out; `zine -c` zhello with 0 KB fast RAM; on the 68060 machine `zine -l` zalloc with the 8 MB below 16 MB of
72 MB; zsuper: ExecBase written over and a SOFTINT acknowledged, which crashed without the change).

**The reruns** (zine-top100-findings.md "Round 14"): 68020 246 -> 248, 68030 315 -> 313, 68060 354 -> 361. On the
68030 three productions that ran their course with their big files left out now find them and run out of memory
(16 MB); on the 68060 Elude's Shake Off The Dust stays black with its files on the disk and 62 MB free, and plays with `zine
-m` (49 MB free) or with its files on the disk and 30 MB free: too much free memory stops it, not the reads.
(Corrected in 16.24: it plays to its end; neither of the other two runs got past its loading picture.)

### 16.24 Shake Off The Dust's end, a file's attributes, line reads (2026-11-14, branch zine15)

**Shake Off The Dust was never stuck** (round 14's "too much free memory" was wrong). Traced on the 68060 machine
(64 MB, its files on the disk): it loads, starts its 7.3 MB soundtrack (4-bit ADPCM, 27,429 Hz stereo: 272 s), plays
the whole of it (52 of the trace's 5-second marks, its chunky-to-planar loop running throughout), says "[Sound]:
Flushing buffers... Done." and then shows a black screen, waiting for a key in a loop of GetSysTime, Delay and
GetMsg. In warp mode it gets there within the harness's 120 s, so its last screenshot was black. The two runs that
"played" (`zine -m`, 49 MB free; 32 MB of fast RAM) were still on its loading picture at 120 s. With 32 MB it never
gets further: its AllocMem calls of 128 KB fill the fast RAM, one is refused, and it loops (out of memory, as on a
real 32 MB machine). So no cap on AvailMem is needed for it, and none was built: no Top 100 production has been seen
to fail for having too much memory. (`zine -l` already gives a 1992 program only the fast RAM below 16 MB.)

Escape at its black end screen ends it: "[Sound]: Finished!", then it writes `ds2_demo.vars` (its settings) and
calls dos **SetProtection** on it, which ZINE lacked: stopped there. Now `SetProtection` (-186), `SetComment` (-180)
and `SetFileDate` (-396) answer DOSTRUE for a file or directory that is there (the volume's, or one written in
memory) and 0 with ERROR_OBJECT_NOT_FOUND otherwise; the attribute is not kept (the session's files are copies,
nothing goes back to the disk, 16.9) (`c_SetAttr`, blibs.c).

**FGetC, UnGetC, FGets** (-306, -318, -336). Nature's Jesus Christ Motocross reads a text file a byte at a time
(found when it got more memory, below). ZINE's file handles have no buffer, so FGetC is a Read of one byte (-1 at the
end), UnGetC steps the file back by one (Seek; 0 when it cannot), FGets reads up to `len - 1` bytes to and with a
newline, ends them with a 0 and answers the buffer (0 when nothing was read).

**The trace build dumps low memory** when a session ends on an unsupported call or a crash below `$1000` (the first
4 KB as the program left them, read before Zenix's low memory comes back): for a program that runs its code there,
as Elysium's decruncher at `$100` and its program at `$800`.

*Verified* in FS-UAE: `tools/zine-emu-test.sh` (zwrite: SetProtection of a written file, SetComment of one of its
own, SetFileDate by another name, SetProtection of a deleted file refused with 205; FGetC, UnGetC and FGets read a
written file a line at a time, then 0 at the end).

## 17. Questions for the owner (what ZINE should be)

1. Owner answer 1 allowed "the Amiga's own console or through Wasabi". Typing into the console through Wasabi counts
   as the console; should `wasabi run` (a remote command, like ssh) also be allowed to start a program under ZINE?
   [No: only the console, typed there or through Wasabi, so a person can see the screen and press Ctrl-Amiga-Amiga.]
2. Where step 0 shows that Ctrl-Amiga-Amiga cannot bring Zenix back on a machine, it restarts that machine into Zenix
   instead (disks safe, open programs lost), and `zine` says so first. Acceptable? [Yes.]
3. Others logged in (ssh, telnet) are paused with the rest of Zenix (answer 2). Warn them first, or refuse while they
   are logged in? [Warn and go ahead: "Zenix pauses for an Amiga program" on their terminals.]
4. AGA demos need the AmigaOS 3.0/3.1 parts of the interface (the AGA chip bits, `LoadRGB32`, `AllocBitMap`), which
   no document on the clean-room list describes. May a public developer kit's interface files (for example the free
   NDK 3.2's includes and `.fd` files) be added to the list, for names and numbers only? [Yes, interface files only,
   never source.]
5. (Round 10, 16.20.) An old 68000 production that copies code and runs it without flushing the instruction cache
   (Angels' destroy fascism) crashes on a 68020+ with the cache on, as on a real A1200 without a "degrader". ZINE
   cannot tell such a program in advance. Should ZINE offer a way to start a program with the caches off (an option
   of `zine`, or the cache off until the program's first library call, which slows every packed program's unpacking
   on a 68020 a little)? [Open.]
6. (Round 13, 16.22.) Some 1992-era productions (sumea, Bacon of Hope) stop when their memory lies above 16 MB, as
   on a real machine whose fast RAM is on a CPU-slot board (AmigaOS users ran them with fast RAM switched off). Should
   ZINE offer a way to start a program with chip RAM only (an option of `zine`)? [Decided by the main session,
   2026-11-13: yes. Built in 16.23: `zine -c` (chip RAM only) and `zine -l` (no fast RAM above 16 MB).]

## 18. Reviews

Revision 3 was reviewed by two independent read-only reviewers (2026-10-30): **1**, the AmigaOS subset and the hardware
hand-over and restore against the RKRM, the HRM, AGA.txt, the Guru Book, the Abacus guide, the AmigaDOS Manual and the
Motorola manuals; **2**, feasibility and safety on the PiStorm (Emu68), the Apollo 1260 and a 68030, and coherence with
Zenix's kernel, its claims and frozen interfaces. Every finding is worked in:

- **The allowed sources stop at V37** (1, major): the RKRM Includes and Autodocs here is the 2.04 edition; every V39/V40
  item is now *proposed*, built only from a public source the owner adds (5, question 4).
- **DMA during the copy back** (1, major): `WaitBlit` while DMA is on, then all DMA and interrupts off, the copy, the
  Copper pointers with Copper DMA off, `DMACON`/`INTENA` last in the kernel (9.1, 9.2, 9.6).
- **A reset does not invalidate the 040's and 060's caches** (1, major): `cpusha bc` first in the capture code, the
  060's branch cache cleared (9.3).
- **A RAM disk in chip RAM was given away** (2, major): left out of the program's list as allocated (4.1, a test).
- **Stopping all user processes broke the NFS sync and the drivers' ending; the reset warning raced the way in** (2,
  major): netd and its drivers run until the sync is done, the kernel runs the quiesce programs itself and the drivers
  die only after the resume, `ZINE_GO` owns the reset warning during the way in (7.2 steps 4 and 6, 11, a test); the
  A1200 keyboard's warning is *verified* (2.1).
- **`VS_HOLD` needs machinery that does not exist** (2, major): the timed kernel call with a heap buffer and a cancel,
  and the held-write queue, designed, counted and built first (7.2 step 5, 14, 16 step 2).
- **The capture code touched fast RAM before having its own vectors and stack; it must never return to the ROM** (1
  minor, 2 major): own SSP and VBR first, fast RAM only after; cold start rules (9.3, 9.4).
- **FS-UAE's fast RAM is autoconfig** (2, major): path B tested on non-autoconfig fast RAM, one run expecting the cold
  start (9.3, 15, 16).
- **No `ptest` on a 68060** (2, major): `plpar` there, nothing under Emu68, probes behind caught vectors (9.1).
- **The order and the owner of the device restarts** (2, major): the blob writes registers only; the kernel restarts
  the devices in one order, E-clock first, through register-only functions; nothing before low memory is back (9.2).
- **A reset during the copy back** (2, major): address 4 and `ChkBase` cleared before the low page is touched, address
  4 left 0 afterwards (9.1, 11).
- **The Pi's interrupt controller** (2, major): Zenix never touches it; the quiesce programs mask GENET (7.2 step 8).
- Minor and nits, all worked in: the exec interrupt conventions (handlers clear their own bit, chains in `IntVects`,
  registers saved, `INTREQ` twice on fast CPUs, the spurious vector) (8.2, 8.3); `cia.resource`'s software pending
  bits and `SetICR` causing interrupts (5.5); the registers each call preserves (5.2, `ztake`); BPTRs, BSTRs,
  `cli_DefaultStack` in longwords, `pr_ReturnAddr`, `Open("*")`, real FileLocks (5.4, 6); `HUNK_SYMBOL`'s units, the
  short form's padding, the in-hunk size bits (6); `HR_AGNUS` and the ECS registers by the Agnus id (5.3, 8.1, 10.2);
  ZINE's own TextFont (5.3); `CloseWorkBench`'s deliberate yes, `OpenDevice`'s `io_Error` (5.5); `Delay` and `DateStamp`
  in measured time (5.4); the TOD's source and read order, `PowerSupplyFrequency` (5.1, 9.7, step 0); the handshake in
  three lines (8.3); likely stops and later `LIBS:` libraries, `AddLibrary`/`AddResource` (5.2, 5.5); the 68040's own
  library, the 060's vectors, LC/EC060 (8.2, 10.1); `ActiView` = `&ViewLord` with a valid Copper list pointer (5.3);
  the null sprite, longword 0, `LoadView(NULL)`'s resets *proposed*, `WaitBlit`'s source (5.1, 5.3); the checksum cost
  and the volume left out of the sums (4.4, 14); the stacks (7.2, 7.3); where the blob lives (3, 4.2); the update's
  version mismatch (3); the floppy kernel without ZINE (10.3, 13); the status record pushed under copyback (9.1);
  the floppy's state on path A (9.2); step 0's extra measurements and its report kept in chip RAM (16); the
  `AttnFlags` 060 bit, the VBR under the 060's library, zup's lock, the `main.c` changes and the groups table (5.1, 8.2,
  7.2, 13); the Wasabi narrowing of answer 1 put to the owner (question 1; the log of unsupported calls became a
  technical decision, 9.1).
- Two errors the reviewers found in the knowledge base (`AttnFlags` `807F` has no 68060 bit; `$0C00`/`$0011` are the
  OS's defaults for `BPLCON3`/`BPLCON4`, not their reset values) are corrected there.

Checked and found right by the reviewers: the ExecBase, GfxBase and IntuitionBase offsets used; the ROM's two reset
checks and how ColdCapture is called; the server chains and the handler registers; the CLI start; the hunk memory bits;
`DIWHIGH` disabled by `DIWSTRT`/`DIWSTOP` writes; the CIA ICR and TOD rules; the keyboard's timings; level 7; the cache
rules; system call 93 and klog reasons 3 and 4 free; the boot note; the memory figures; no zenixboot change; the owner's
answers honoured (answer 1 with the narrowing now asked).

## Appendix: Superseded: revision 2 (a restart into the ROM's AmigaOS)

Revisions 1 and 2 (2026-10-30, reviewed twice per angle) ran the program under the ROM's own AmigaOS: zenixboot kept a
copy of the real ExecBase at `0x1400`; Zenix synced and froze, copied every byte it needed into a checksummed "corner"
at the top of fast RAM, wrote the ExecBase back with a CoolCapture and reset; a first pass set KickTags and reset again,
and ZINE's residents protected the corner, removed the owner's partitions from the mount list and booted an in-memory
FFS volume through a `zine.device`, where `zinerun` ran the program with the ROM's LoadSeg; every ending went through a
third reset back into the corner, which copied Zenix back and resumed it. The owner replaced it on 2026-10-30 ("This
demo runs under Zenix. Zenix just pointing the demo to the hardware that it needs"). Kept from it in revision 3: the
hunk check (6), the fast-RAM requirement, the sync with per-mount deadlines and held writes (7.2), the network drivers'
quiesce and restart, the security rule and the console check (9.5, 7.2), files gone afterwards, the time handling (9.7),
the checksums as the gate before Zenix writes again, the klog reasons and the boot note, and the reviews' hardware
lessons (the cache order, the MMU off with `ptest`, PCR only on a real 060, the E-clock by a software offset, the full
display set). Dropped: the ExecBase copy and zenixboot's change, KickTags, CoolCapture passes, the chip anchor, the
corner and its copying, the FFS writer, `zine.device`, the residents and `zinerun`, every reset on the normal way. The
full text is in git (`3c536e6`, docs/d29-zine.md).

## Owner decisions on revision 3 (2026-10-30)

All four questions answered as recommended. **Path B is dropped:** Ctrl-Amiga-Amiga during a program is the clean
restart into Zenix through the boot note on every machine (section 9's fallback becomes the only way); no ColdCapture,
CoolCapture, KickTag or other reset-surviving code is written (a safety check stops it, as such code is what boot
viruses use). Path A (the program returns, exits, crashes into ZINE's vectors, alerts, or calls what ZINE lacks) is
the way back. The NDK 3.2's interface files may be used for names and numbers (clean-room list amended for them).
Step 0's measurements shrink to what path A and the restart need.

