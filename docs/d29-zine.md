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
| The task | `FindTask` (0 or its own name; else 0), `SetTaskPri` (answers the old one, no effect), `SetSignal`, `AllocSignal`, `FreeSignal`, `Signal`, `Wait` | one process; `Wait` stops the CPU (`stop #$2000`) until an interrupt server signals; a `Wait` that nothing can end (interrupts disabled, or no server, vector or ICR handler installed) is a clean stop: "NAME waits for a signal nothing can send" |
| Interrupts | `SetIntVector`, `AddIntServer`, `RemIntServer` | 8.3 |
| Libraries | `OpenLibrary`, `OldOpenLibrary`, `CloseLibrary`, `OpenResource`, `OpenDevice` (always fails: `IOERR_OPENFAIL`, 5.5), `CloseDevice`, `SetFunction` (flushes the caches), `MakeLibrary`, `MakeFunctions`, `InitResident`, `FindResident` (0), `SumLibrary` | an unknown library or resource answers 0, as AmigaOS does when it has none: the program's own error path runs |
| Lists and messages | `Insert`, `AddHead`, `AddTail`, `Remove`, `RemHead`, `RemTail`, `Enqueue`, `FindName`, `AddPort`, `RemPort`, `FindPort` (0), `PutMsg`, `GetMsg`, `ReplyMsg`, `WaitPort`, `CreateMsgPort`, `DeleteMsgPort`, `CreateIORequest`, `DeleteIORequest`, `InitSemaphore`, `ObtainSemaphore`, `ReleaseSemaphore`, `AttemptSemaphore` | as the autodocs say, for one task |
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
there is no handler process and no packets (a program that sends packets to `pr_FileSystemTask` or `pr_ConsoleTask`
finds 0 there and fails as under a missing handler).

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
`TD_MOTOR`, `disk.resource` (track loaders), math and `lowlevel.library` from C programs. A cheap later step:
`OpenLibrary` loading a library from the program's own `LIBS:` with ZINE's `LoadSeg` and `InitResident`, as it already
does for the 060's library.

### 5.6 The program's files: the in-memory volume

`zine` reads the program's directory (recursively) into one block before the hand-over: a table (a name of up to 107
bytes, the parent, the size, the protection bits, the date, the data's offset) and the files' data. That replaces
revision 2's FFS image and device: AmigaOS's own filesystem is not running, so ZINE's dos finds names itself.

- **Names:** AmigaDOS's rules: compared without case (Latin-1 letters too), `/` and `:` separate; UTF-8 names that are
  Latin-1 become Latin-1; names outside Latin-1, with `:` or `/`, or equal to another but for case are left out with a
  warning. Protection bits from the Unix mode (`r` R, `w` W and D, `x` E), dates from the modification time.
- **Paths:** the volume is named after the directory (demos that open `NAME:data/...` by their disk's name find it);
  `SYS:`, `PROGDIR:` and the volume name are its root; `C:`, `S:`, `L:`, `LIBS:`, `DEVS:`, `FONTS:` its subdirectories
  where they exist; the current directory at the start is the program's. The owner's CPU library (10.1) is added as
  `LIBS:68060.library`.
- **Limits:** if the directory holds more than 16 MB, more than half the free fast RAM, or more than 500 files, only the
  program goes in, with a warning ("only NAME was copied: its directory holds 23 MB; a program that loads files may not
  find them: give it a directory of its own").
- **Writes** (owner answer 3): a new file, or a write to an existing one (copied first), lives in the program's own
  memory and is gone afterwards. The volume block itself is never written, so it is in the checksums. `DeleteFile`,
  `Rename` and the rest are the "not supported" stop in the first version.
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
- Refused, each with its own message: `HUNK_OVERLAY`/`BREAK` ("NAME uses overlays: not supported yet"),
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

No task switching (one process), no input events, no timer requests, no screens, no windows, no disk devices. A
program that asks for these through a call ZINE does not have is stopped cleanly (9.1); one that finds them missing by
an answer (0 from `OpenLibrary`, `IOERR_OPENFAIL`) takes its own error path and usually ends with a message, which
`zine`
shows.

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
- **68040/68060:** the flush is `cpusha bc`, never `cinva` (CLAUDE.md). The data cache stays off as on the 030 (with no
  MMU tables, chip RAM would be cached and the blitter and Copper would read stale data; a later option, *proposed*:
the data cache on with DTT0/DTT1 marking the low 16 MB cache-inhibited and fast RAM copyback, measured first). A real
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

### 12.1 The command and running by name (decided)

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
- Not yet: `zcrash`, `zbadcall`, `zalloc`, the kernel review, the owner's PiStorm.

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

