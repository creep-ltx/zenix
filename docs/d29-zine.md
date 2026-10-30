# D29: ZINE, AmigaOS programs on Zenix (design note, revision 2)

Status: **revision 2, 2026-10-30, after two independent reviews (four reports, section 16).** Approved by the owner on
2026-10-29 and 2026-10-30 ([decisions.md](decisions.md), the entries "Amiga demos on Zenix" to "The Amiga layer is
ZINE"); ROADMAP block 3½. Nothing of it is built; step 0 (section 14) measures the open facts on the real machines
before step 1 starts, and the note is revised by what it finds. Facts about Zenix are *read* from main (`fbfac19`);
facts about AmigaOS come from the public manuals and autodocs (knowledge base `manuals/`), the knowledge base's verified
notes and the AmigaOS 3.1 study (aos31-study/), never from AmigaOS source. Marks: *read*, *verified* (a test or a real
machine showed it, with where), *proposed* (our reasoning, to be checked by the step that says so). The owner's answers
to section 15 are not given yet: where the note follows a recommendation there, it says "pending".

## 1. What ZINE is (the owner's requirements)

ZINE ("ZINE Is Not an Emulator") runs an AmigaOS executable on the real hardware and brings Zenix back afterwards:

- `zine PROGRAM [ARGS]`, or `PROGRAM [ARGS]` alone: Zenix's loader sees an AmigaOS hunk file (magic `00 00 03 F3`)
  and calls `zine` for it. It prints `ZINE: handing the machine to NAME`.
- Demos, games and tools alike: no kinds, no subcommands. No disk images (ADF) for now.
- First target: machines **with fast RAM** (the owner: demo watchers have at least a 68030 and 8 MB of fast RAM). The
  stock A1200 (chip RAM only) is not a target; `zine` says so there.
- The program gets **all chip RAM and all fast RAM except a small Zenix corner** (about 1-2 MB at the top of fast RAM),
  marked in use in the memory list the program's AmigaOS sees. (A 4 KB anchor in chip RAM is kept too, section 3.3.)
- Zenix comes back when the program ends. A checksum of the corner is checked on the way back; if it is damaged, Zenix
  says so and reboots cleanly. The disks are saved before the hand-over (synced, every request finished, nothing
  half-written), so no path ever leaves a damaged disk.
- Order: bare-hardware demos first; system-friendly programs (the ROM's libraries) next; files from Zenix as an Amiga
  volume and the installed AmigaOS partition with its assigns later; an AmigaOS kernel of Zenix's own much later (out
  of scope here).

## 2. Facts this design stands on

### 2.1 How Kickstart starts after a reset (*read*)

- **The warm-start check.** At a reset the ROM reads the longword at address 4. It believes it is an ExecBase left by
  a running system only if it is even, `ExecBase + ChkBase` (offset 38) inverts to zero, and the word sum over
  `SoftVer` (34) up to and including `ChkSum` (82) is `0xFFFF` (the Guru Book's `SumExec` starts from -1 and subtracts
  the 24 words 34-80 into `ChkSum`; Guru Book ch. 10.1; Abacus System Programmer's Guide 2.9 for the 1.x ROM, which also
  compared the ROM's exec version with the base's and range-checked `MaxLocMem`/`MaxExtMem`). Only then are
  `ColdCapture` (42), `CoolCapture` (46), `WarmCapture` (50), `KickMemPtr` (0x222), `KickTagPtr` (0x226) and
  `KickCheckSum` (0x22A) taken over; otherwise everything starts from scratch.
- **The order on 2.0+** (Guru Book ch. 10): Kickstart's start; ColdCapture (later, during expansion, if ExecBase lies in
  autoconfig memory); chip and ranger memory; the ROM's residents gathered; `InitCode(RTF_SINGLETASK)`; expansion (pri
  110) configures the boards and their RAM; exec's initialisation; all residents gathered (KickTags included, if
  `KickCheckSum` matches `SumKickData()`); **CoolCapture**; `InitCode(RTF_COLDSTART)`: diag init (105) runs the boards'
  boot code, ..., the disk drivers add their boot nodes, strap (-60) boots, dos (-120).
- **CoolCapture** is called after exec is up and before the cold-start residents run, in user mode; it returns with
  RTS and is not cleared by the ROM. **ColdCapture** is cleared before it is called (aos31-study/exec-kernel.md item 8;
  Guru Book 10.1).
- **KickMem and KickTags.** During start-up exec `AllocAbs()`es every region of the `MemList`s on `KickMemPtr`; the
  `MemList` structures must lie inside the regions they protect; the first 8 bytes of each region are undefined
  afterwards (a `MemChunk` lands there), and `AllocAbs` may change the 8 bytes after a region's end (RKRM autodoc).
  `KickTagPtr` points to an array of `Resident` pointers ending in 0 (bit 31: a link). `KickCheckSum` must equal
  `SumKickData()`, which sums the list structures, not the memory (Guru Book 10.2; RKRM autodoc exec.library/
  SumKickData). Its algorithm is not in the public documents.
- **Only some memory can hold KickMem and KickTags** (Guru Book 2.4.6): on 2.0+ only memory configured before or
  during `RTF_SINGLETASK`; memory added during diag init ("pseudo-autoconfiguring memory on some accelerator boards") or
  by `AddMemList` later cannot. Exec marks the memory that can with `MEMF_KICK`; for full compatibility the book advises
  chip RAM. RAD: allocates `MEMF_KICK|MEMF_REVERSE` (aos31-study/dos-and-filesystems.md); the boot menu's verified
  KickTag path keeps everything in chip RAM.
- **Low memory is not tested at reset** (aos31-study/exec-kernel.md item 12). A fatal alert writes `HELP` at 0 and its
  code at `$100`, then resets; the ROM shows the alert once Intuition is up (same note, item 5). On real hardware
  `LastAlert` (514) carries it into the new ExecBase; **on Emu68 it does not** (2.2).

### 2.2 What survives a reset, on the owner's machines (*verified* where marked)

- **The KickTag reboot works on the A1200**: the boot menu leaves a MemList, a KickTag table and a resident
  (`RTF_COLDSTART`, pri -59) in chip RAM, sets the three fields with `SumKickData()` and calls `ColdReboot`; at the reset
  the resident moves a boot node to the head of expansion's mount list. Verified in FS-UAE (Kickstart 3.2) and on the
  owner's A1200 (PiStorm32-lite, Emu68 1.1, Kickstart 3.2.3), 2026-10-24 (man/bootmenu.txt).
- **Chip RAM survives Ctrl-Amiga-Amiga on the PiStorm** (the previous-boot log, audit-2026-10-22.md, 2026-10-23) and an
  FS-UAE keyboard reset (Kickstart 3.1, `tools/diag-test.sh`).
- **On Emu68 the top of the highest fast region survives a reboot; the bottom does not.** ExecBase lives in low fast RAM
  (`0800089C`, the first fast region), is rebuilt in place, and `LastAlert` poked before a reboot reads `FFFFFFFF` after
  it; a marker at the top of the highest region rode a dozen reboots (knowledge base amiga/gurus-and-exception-frames.md;
  which kind of reset was not recorded). Zenix's kernel lives in the other, highest region there (its home
  `0x41040000`, knowledge base amiga/zenix-pi-drivers-progress.md), so ExecBase and the corner are in different regions.
- On that machine `KickTagPtr`/`KickMemPtr` were already in use by some module (same note, "Verified neighbours"); the
  session runs without it (3.1).
- **Not known yet** (step 0 measures each, on the PiStorm and the Apollo 1260, before step 1): whether fast RAM keeps its
  contents through a **keyboard** reset; whether the corner's region is `MEMF_KICK` (on the 1260 it may be added during
  diag init); whether CoolCapture is honoured on 3.2.3 under Emu68; where ExecBase lies on the 1260, with and without
  `68060.library`, and whether a MapROM sits at the top of its fast RAM; what Kickstart writes into fast RAM before the
  corner is protected (the supervisor stack, `MEMF_REVERSE` allocations); whether `$0`/`$100` survive an alert reset;
  which ConfigDev strap accepts for a memory disk; whether any driver mounts a partition after pri -59.

### 2.3 How strap boots a disk (*read*)

- `AddBootNode(bootPri, flags, deviceNode, configDev)` (expansion, V36): "Autoboot from an expansion card before DOS is
  running requires the card's ConfigDev structure. Pass a NULL ConfigDev pointer to create a non-bootable node." With
  no `dn_SegList`/`dn_Handler`/`dn_Task` the standard dos file handler is used; `ADNF_STARTPROC` starts the handler at
  once (RKRM autodoc). A `DOS\1` partition with no FileSysHeader is booted with the ROM's FFS on 2.05, 3.1, 3.2
  (*verified* in FS-UAE, knowledge base amiga/hd-boot-without-amigaos.md).
- An FFS boot volume needs nothing but `S/Startup-Sequence` and the program it starts (*verified*, same note). The boot
  CLI's window opens only when something is printed (2.0+, Guru Book 10.3).

### 2.4 Zenix today (*read* from main)

- **The hand-over from AmigaOS** (architecture.md section 3, `boot/handoff.S`, `boot/zenixboot.c`): zenixboot loads and
  checks everything while AmigaOS runs, then turns AmigaOS off and jumps through the copier at `0x500`, which writes the
  1 KB table to `0x1000`-`0x13FF` (bootinfo and the Pi table use `0x1000`-`0x1297`) and the kernel to its **home: the
  bottom of the largest fast region, 256 KB aligned**. `0x1400`-`0x1FFF` is written by nobody; the kernel keeps
  `0`-`0x1FFF` out of its pools, and zenixboot keeps its buffers out of `0`-`0x10000`. zenixboot is installed by hand on
  BOOT: (d15-self-updates.md), never by a bundle.
- **Memory** (`kernel/mem.c`): chip and fast ranges of blocks with 16-byte boundary-tag headers; a free block holds
  nothing but its header. Blocks of 16 KB or more are cut from the high end of a free block, small ones from the low
  end. Outside the pools: `0`-`0x1FFF`, the kernel image and BSS, the RAM disk, the klog area (top 16.5 KB of chip RAM).
  The bootinfo region list carries each region's `mh_Attributes` (so `MEMF_KICK` is known).
- **Leaving**: `vfs_sync(VS_REBOOT)` (kernel/vfs.c) sends `FS_SYNC` to every server and arms a deadline at which the
  tick *resets the machine* (kernel/kbd.c); it returns an error only for the root mount; `disk_frozen` is set by the
  reset warning and `SYS_reboot` and never cleared, and while it is set `blk_write` and the floppy fail with -1. The ext2
  clean mark is set again at `FS_SYNC` unless the volume needs a check, zup holds it or an orphan is open (D17).
- **Drivers**: the kernel's disk drivers busy-wait (the floppy's `wait_us`, the SD card's `hr_time` loops) inside the
  calling server's system call, and the kernel is not preemptive, so no disk transfer is in flight while another system
  call runs. The network drivers (GENET, the Zorro cards) are **user processes**, children of netd; the kernel holds
  only their claim, DMA region and quiesce program (`kernel/drv.c`); a driver that dies is stopped by the kernel and
  restarted by netd, which then does DHCP again (D19, `tools/eth-test.sh`).
- **The klog area** survives warm resets; its header holds the reason a boot ended (`KLOG_END_RUNNING` 0, `PANIC` 1,
  `REBOOT` 2); zenixboot's trial logic counts only PANIC and RUNNING after a hand-over as evidence of a failed start.
- **Executables**: ZEXE and ZEX2 only; `proc_spawn` reads the header and refuses anything else (-ENOEXEC). `ctty` is
  forced into 1..NCONS for every process, so a telnet or ssh session's children also look like console 1 (proc.c,
  tty.c); D24's `CON_INPUT` lets wasabid type into a console.
- **Sizes** (*measured* in the notes cited): the kernel image about 172 KB, BSS about 45 KB (`tools/ksize-budget`: total
  172,000, 1,024 left on the mount tests' floppy, the binding limit); Zenix with one login takes about 680 KB on a 2 MB
  machine (1,365 KB free, D19, FS-UAE); the 16-colour console on a fast-RAM machine 160 KB of chip RAM (D14); netd and
  its driver about 146 KB (D19); the NFS client's cache up to 1 MB and a 1 MB heap on big machines (`user/nfscd.c`).

## 3. How ZINE gives the program an AmigaOS

A demo is an AmigaOS program: it calls exec (`AllocMem`, `Forbid`, `OpenLibrary`), graphics (`LoadView`, `WaitTOF`,
`OwnBlitter`, `gb_copinit`), dos (its files, `Delay`, `Output`), and returns to its caller. So it needs a real AmigaOS
underneath, at least at its start and end.

| | How | Safety | Compatibility | Emu68 / 030 / 060 | Size |
|---|---|---|---|---|---|
| **A. Warm restart of the ROM's own Kickstart, Zenix's state protected** | a valid ExecBase at 4 with a capture vector and KickTags; Kickstart starts as after Ctrl-Amiga-Amiga, protects the corner, runs ZINE's residents, boots a ZINE volume | a reset is the most used path on every Amiga; every write before the protection is measurable; every failure ends in a reset | the owner's own ROM: exec, graphics, intuition, dos | as a reset on each machine; the boards' own ROMs run as usual | a few residents, a device, a starter |
| B. Build ExecBase ourselves, our own exec | a "demo layer" answering demos' calls | Zenix's code under every call | only what we wrote | no board ROMs, no 060 support | thousands of lines: the much later "own AmigaOS kernel" |
| C. Jump into the ROM without a reset | as A, no RESET | boards stay configured: expansion adds none of their RAM or drivers | breaks on autoconfig RAM | board set-up skipped | as A |
| D. The ROM's residents on an exec of ours | B with the ROM's libraries | exec's private fields, interrupt sharing | future-ideas.md's Wine route | | 15,000-25,000 lines |

**Decided: A** (both reviews agree): exactly the AmigaOS the ROM boots, on all three kinds of machine, with little code;
every failure ends in a reset with Zenix's disks already saved.

### 3.1 The valid ExecBase: AmigaOS's own, kept by zenixboot

Zenix overwrites AmigaOS's memory, and only the 1.x warm-start checks are public, so **zenixboot keeps a copy of the
real ExecBase**: while AmigaOS runs it copies the positive part (`lib_PosSize` bytes; over 1,000 means no copy) and
notes its address, exec's version and revision and whether `KickTagPtr` was in use; after `handoff_kill`, between the
shared macros as it already does with its own code, it writes `{magic "ZEXB", the address, the size, the versions, the
flag, a CRC32, the bytes}` to **`0x1400`-`0x17FF`**, and sets bootinfo `+0x84` to `0x1400` (a new field at the end:
"this boot's ExecBase copy"; the bootblock's table is cleared memory, so it is 0 there). The kernel trusts the copy only
when `+0x84` says so, and clears the magic otherwise (a copy from an earlier boot, perhaps of another Kickstart, survives
warm resets in chip RAM). The shared `handoff.S` macros and the floppy bootblock do not change. A Zenix started from the
floppy, or by an older zenixboot, has no copy and `zine` says "ZINE needs Zenix started by zenixboot 2026-11 or newer".
zenixboot is installed by hand on BOOT:, so its new version reaches the owner's card only by a step with the owner
(section 14, step 2).

At the hand-over ZINE writes the copy back **at its own address**, so the ROM finds the ExecBase it built, where it
built it; only the capture and Kick fields change. A base placed in the corner instead could make a ROM rebuild exec
over the corner. If the original ExecBase (its negative part included) lies in the corner or the guard, `zine` refuses.
The session runs **the bare ROM**: modules the owner adds at boot (LoadModule, reset-resident tools) are not in it, on
purpose; `zine` says so when the copy's flag shows a KickTag was in use. Pass 1 compares the new ExecBase's version with
the copy's; a different Kickstart (Emu68 can switch ROMs) takes the way back at once.

`ChkSum` is updated by the difference of the changed words (CoolCapture lies at 46-49, inside every documented range),
so it stays right whatever range a ROM sums; `ChkBase` is unchanged (same address).

### 3.2 Two passes: CoolCapture first, then KickTags summed by the ROM

1. **Pass 1.** The written-back ExecBase has `CoolCapture` = ZINE's pass-1 routine (in the chip anchor, 3.3), the other
   capture vectors and both Kick pointers 0, `LastAlert` -1. Zenix resets. Kickstart finds a valid ExecBase, sets up
   memory and the boards, gathers the ROM's residents and calls CoolCapture with exec running. The routine checks the
   anchor's and the corner's sums, compares the exec version, clears `CoolCapture`, sets `KickMemPtr` (the anchor, and
   the corner if its region is `MEMF_KICK`) and `KickTagPtr`, `KickCheckSum = SumKickData()` (exec's own), fixes
   `ChkSum`, `CacheClearU()`, `ColdReboot()`. No disk driver has run (they start after CoolCapture).
2. **Pass 2** is the documented KickTag path of the boot menu, verified on the A1200: exec `AllocAbs()`es the anchor
   (and the corner), and runs ZINE's residents with the ROM's cold-start modules (section 6.4).

aos31-study/exec-kernel.md item 13 calls `SumKickData` "an additive checksum that any program can compute"; if step 0
finds a sum that matches `SumKickData()` on 3.1, 3.2 and 3.2.3 by testing the public function's behaviour, Zenix may
compute it itself and drop pass 1 (one reset less, and no CoolCapture question). Until then pass 1 is the design: it
relies only on documented behaviour. A Kickstart whose version step 0 did not check is then refused.

### 3.3 The chip anchor

The KickMem list, the KickTag table, the two residents' entry code, the pass-1 routine and the **status record** live in
a 4 KB **anchor in chip RAM** directly below the klog area (chip RAM is always `MEMF_KICK`, and that place survives
resets on the PiStorm and in FS-UAE). The fast corner holds the rest (switcher, device, snapshot, volume). So the
residents run on every machine even where the corner's region is not `MEMF_KICK`; there `zine.early` (pri 104, after
diag init has added the board's RAM) `AllocAbs()`es the corner itself and checks that it got it (6.4). The program
loses 4 KB of its chip RAM for this.

The **status record** (state, how it ended, return code, alert code, a session number, its own CRC) is the only thing
written during the session. Every AmigaOS-side write to it is followed by `CacheClearU()` (a keyboard reset is a CPU
reset and drops dirty copyback lines). It is not part of the corner's sums; the kernel reads it as untrusted input
(range-checked values, the mailbox length clamped to its size).

## 4. The corner

**Everything Zenix needs to come back lies in one range at the top of the largest fast region** (the kernel's region;
on Emu68 also the highest, whose top survives resets), plus the anchor.

Zenix's memory is not in one place: the kernel sits at the bottom of that region, small blocks too; big blocks are cut
from the top; chip RAM holds the screen, the low tables and the klog area. Nothing can be moved (no MMU). So ZINE
**copies**: what lies in the corner stays where it is; every other byte Zenix needs is copied into the corner's free
space before the reset and copied back on the way home, as Linux's hibernation does with its pages.

### 4.1 What is saved

Everything Zenix owns outside the corner: in every pool range each used block whole and each free block's 16-byte
header; the kernel image and BSS; the RAM disk, if zenixboot loaded one; low memory `0x0`-`0x1FFF`; the klog area; the
anchor's place (Zenix's chip blocks there); the 8 bytes after the corner's end (an `AllocAbs` may change them); and the
registers and hardware state that cannot be read back (7.4), in the session header. Free memory is not saved.

### 4.2 Where it is and how big (planned before anything is frozen)

Before anything is stopped, `zine` gets the **volume block** (`ZINE_VOLUME`, cut from the top of the corner's region
only) and the kernel makes the **code block** (the switcher, the device, the session header; the blob's size, from the
top too). Nothing is allocated after that. The switcher then uses **the free space inside the corner as the snapshot
space**: the corner is

    corner = [X, the region's top - G)

where X is the highest address at which the free bytes inside the range (after each free block's header, which stays)
hold the index plus every byte to be saved (4.1); 16 bytes at X are left unused (exec's `MemChunk` lands there). `G` is a
guard at the very top in case a Kickstart puts its supervisor stack or `MEMF_REVERSE` allocations there before the corner
is protected (64 KB until step 0 measures it; Zenix blocks inside the guard are saved like any outside). The corner is
bounded: if X would leave the program less than half the fast RAM, `zine` refuses and says how much Zenix holds. The
planner runs twice: once before the freeze (to refuse early, with a margin) and once with interrupts off (exact).

Estimate (*proposed*, from 2.4): the kernel about 210 KB, processes and buffers with one login about 500 KB, the
console's chip RAM about 200 KB, low memory and the klog area 25 KB, code about 24 KB: **about 1 MB of corner with one
login, 1.2 MB with the network, up to 3 MB with an NFS mount's cache**. The volume comes on top: the program's files,
which AmigaOS then loads into its own memory, so a 3 MB demo on an 8 MB machine leaves the program about 3 MB of fast
RAM. `zine` prints the three numbers separately (6.1).

### 4.3 The checksums

A CRC32 per piece (the code, the session header's constant part, each saved range, each Zenix block left in place, and
the volume until strap boots it), in the piece table with its own CRC; CRC32 because these sums are the gate before Zenix
writes its disks again (the switcher carries its own table). Checked: in pass 1, in `zine.early` before the program
starts, by the caller before it jumps into the switcher (the code piece), and by the switcher before anything is
restored. Cost (*proposed*): about 1.2 MB at 10-40 MB/s on a 68030/68060, 30-120 ms; step 7 measures it. **The sums
catch accidents, not intent**: a program can read every byte of Zenix and recompute them (9.3).

### 4.4 The session header

At the start of the code block: magic `ZINE`, layout version, the session number (also in the status record), the
corner's bounds, the piece table, the ExecBase address and versions, the program's name and arguments, the stack size,
the mailbox's place (16 KB at the end of the volume block), the times at the hand-over (7.5), the kernel's resume point
and the saved hardware state. Never written during the session. Internal to ZINE: the kernel, the switcher and the
AmigaOS side are built from one source and shipped together (section 11); the kernel refuses a blob of another layout.

## 5. How the program file gets loaded

| | How | For | Against |
|---|---|---|---|
| a. Zenix relocates the hunks before the reset | a hunk loader in Zenix; the program's memory in KickMem | no dos needed | LoadSeg re-implemented; the memory decided by Zenix; data files still need a volume |
| b. ZINE's own DOS handler | a packet handler in memory | small | every packet's meaning ours: a source of differences |
| **c. The ROM's FFS on a memory disk, the ROM's LoadSeg** | Zenix builds an FFS image of the program's directory; `zine.device` serves its blocks from memory; Kickstart boots it | AmigaOS's own LoadSeg and filesystem; data files found by path and `PROGDIR:`; libraries later | the files are held twice (image and loaded) |

**Decided: c.** Zenix still reads the hunk file fully first (9.1), so a damaged or unsuitable file is refused with a
clear message while Zenix runs, never after the reset.

### 5.1 The volume

- **What goes in:** the program's directory (recursively), so a demo finds its data files; if that is over 8 MB or 500
  files, only the program, with a warning ("only NAME was copied: its directory holds 23 MB; a program that loads files
  may not find them: give it a directory of its own"). ZINE adds `S/Startup-Sequence` (`SYS:.zine/zinerun`) and
  `.zine/zinerun`; a `S/Startup-Sequence` of the directory's own is left out (it is for booting a disk). 256 KB free for
  files the program writes, then the mailbox.
- **Format:** `DOS\3` (FFS, international names; in every ROM from 2.04), so Latin-1 names hash and compare as
  AmigaOS's own do; the writer and `tools/ffsread.py` use the same hash. **The volume's name** is the directory's
  (truncated to 30 bytes): demos that open `NAME:data/...` by their disk's name find it. dos assigns `SYS:` to it and
  `C:`, `S:`, `LIBS:`, `DEVS:`, `FONTS:`, `L:` to its directories that exist.
- **Names:** UTF-8 names that are Latin-1 become Latin-1; names over 30 bytes, outside Latin-1, containing `:` or `/`,
  or equal to another one but for case (FFS ignores case, Unix does not) are left out with a warning. Protection bits
  from the Unix mode (`r` R, `w` W and D, `x` E; stored inverted), dates from the modification time, clamped to 1978 and
  later.
- **Writes** go into the image in memory and are gone when Zenix comes back (owner question 3, pending).
- The writer is a C port of `tools/mkffs.py` (root, bitmap, headers, extension blocks, directories), in a shared file
  for `zine` and a host test.

## 6. The way in, step by step

### 6.1 The `zine` command (`/bin/zine`, set-user-id root)

1. Checks, while nothing has happened: `ZINE_INFO` (fast RAM, this boot's ExecBase copy, the corner's region and size,
   the ExecBase not in it) with its messages; the hunk file (9.1) and its memory by type against what there will be.
2. Gets the volume block (`ZINE_VOLUME`) and writes the image into it; reads `/lib/zine/zine.blob` and checks its layout.
3. Prints, for example:

        ZINE: Zenix keeps 1.1 MB, the program's files 2.3 MB; the program gets 2.0 MB chip and 59.3 MB fast RAM
        ZINE: the session runs the plain Kickstart (modules added at boot are not in it)
        ZINE: handing the machine to Arte (Ctrl-Amiga-Amiga brings Zenix back)

4. Calls `SYS_zine` `ZINE_GO` (a struct: the blob, the program's name, arguments, stack size). It returns when Zenix is
   back, with the result (7.4).

### 6.2 The kernel (`SYS_zine` `ZINE_GO`)

1. **Who:** the kernel checks the caller itself (a set-user-id check alone can be passed by root over ssh): root or a
   member of the group `zine` (owner question 1, pending), and file descriptors 0-2 are console ttys (an ssh, telnet or
   serial session has none). Keys typed through wasabid's `CON_INPUT` count as the console's (the owner's own remote
   control; the man page says the person at the Amiga must then press Ctrl-Amiga-Amiga). -EPERM otherwise. Refused
   during an unconfirmed update trial, while zup holds its lock, or while another session runs (-EBUSY).
2. **Plan and allocate** (4.2): the code block; the first planner run; on any failure nothing has been stopped.
3. **Stop the processes:** no user process is scheduled any more (they stay in memory as they are); only the servers
   and `boot` run. This closes the window in which a process could write between one server's sync and the freeze.
4. **The disks:** a new `vfs_sync` mode, `VS_HOLD`: `FS_SYNC` to every filesystem server with a deadline of its own
   per mount (5 s; network mounts 2 s, last) that does **not** reset the machine and reports each mount that failed;
   a reply after the deadline is dropped (D23's cancel). After the sync, disk writes are **held** (a request waits until
   the resume), not failed: so no server sees an error half way through a request, which `disk_frozen`'s -1 would cause
   in a Zenix that goes on. Any mount that failed: writes released, processes released, "ZINE: /home did not answer the
   sync: not handing over". Every volume is then consistent: clean, or marked for a check if it needed one or an orphan
   is open (D17); never half-written.
5. **The network drivers:** each claim's quiesce program is run (as when a driver dies) and the driver processes are
   ended; netd restarts them after the resume (D19's restart path, DHCP again).
6. **The log and notes:** `zine: handing the machine to NAME (filesystems synced in N ms)`; the klog reason
   `KLOG_END_ZINE`; the boot note `BOOTNOTE_ZENIX` (so a cold start during the session lands in Zenix where the boot menu
   runs; both are undone on resume).
7. The other drivers stop their DMA and interrupts (audio off, the floppy's motor, a timed `WaitBlit`, the SD card idle:
   no transfer can be in flight, 2.4); interrupts masked, every DMA channel off.
8. Saves the resume point (a setjmp-style routine in `start.S`: the registers, the supervisor and user stack pointers,
   SR, the full CACR, VBR, the 060's PCR on a real 060) and calls the switcher's way in.

The kernel's part is the call, `VS_HOLD`, the held writes, the process stop, the resume routine and a small table of
entry points the blob calls; the planner, the copying, the hardware save and the reprogramming of the CIAs, Paula and
the display are in the blob (it runs in supervisor mode with the kernel's table). Each driver's resume reuses its init
function (`ide_init`, `emmc_init`, `gfx_init`'s register part, `timer_init`, `kbd_init`). Estimate 3-6 KB in the kernel
(*proposed*); step 3 measures it with `tools/ksize-check.sh`. If the RAM floppy's room cannot take it, the RAM floppy's
kernel is built without ZINE (a Zenix started from the floppy refuses ZINE anyway, 3.1): a Makefile change for the main
session.

### 6.3 The switcher's way in (in the corner; interrupts off; its own stack and vector table outside every summed piece)

1. On Emu68 (hwinfo says so; nothing Emu68-specific runs elsewhere): the Pi-side DMA stopped (`boot/genet-stop.h`, the
   SD controller's reset) **before** anything is copied.
2. The second planner run; every piece outside the corner copied into the corner's free space, the index first; the
   CRCs.
3. The anchor written (MemList, tags, resident code, pass 1, status `STARTING` with the session number).
4. The ExecBase copy written back at its address with pass 1's fields; 4 = that address; 0 written to address 0 (no
   false `HELP`); VBR 0 (a belt: the ROM copes with Zenix's VBR today, but nothing depends on it).
5. The caches flushed (`cpusha bc` on 040/060 and Emu68, CACR bit 3 on 020/030: code was just written).
6. The reset, as `SYS_reboot` does (RESET and the jump in one longword).

### 6.4 The AmigaOS side

- **Pass 1** (CoolCapture, 3.2): sums, version, KickMem and KickTags, `ColdReboot`. Anything wrong: the way back (7.2).
- **`zine.early`** (resident, `RTF_COLDSTART`, pri 104: after diag init has added every board's RAM, before the disk
  drivers): checks the status record's CRC and session number, `AllocAbs()`es the corner if KickMem did not take it,
  and under `Forbid` walks every MemHeader's free list: a free chunk overlapping the corner or the anchor means it is not
  protected, and the way back is taken at once. Checks the corner's and the volume's CRCs. State `STARTING`: sets
  `BOOTING`, returns. State `BOOTING`, `RUNNING` or `RETURNED`: the session's end or a reset during it (Ctrl-Amiga-Amiga,
  an alert's reset, the program's own `ColdReboot`, a failed boot): the way back (7.2), with the alert code from the
  status record, else from `HELP` at 0 and `$100`, else "unknown".
- **`zine.boot`** (resident, `RTF_COLDSTART`, pri -59, as the boot menu's): under `Forbid` makes `zine.device`
  (`MakeLibrary`/`AddDevice`; synchronous `CMD_READ`, `CMD_WRITE`, `CMD_UPDATE`, `CMD_CLEAR`, `TD_MOTOR`,
  `TD_CHANGESTATE` 0, `TD_PROTSTATUS` 0, `TD_CHANGENUM`, `TD_GETGEOMETRY`; `TD_ADDCHANGEINT` held unanswered until
  `TD_REMCHANGEINT`; everything else `IOERR_NOCMD`); **removes every node from expansion's mount list**; adds the volume
  with `MakeDosNode` (`DOS\3`, the image's geometry, Mask and BufMemType allowing any memory, boot priority 127) and
  `AddBootNode` with **a fresh ConfigDev from `AllocConfigDev()`**, no `ERTF_DIAGVALID`, no DiagArea (never a borrowed
  one: a real board's BootPoint would boot that board's disk); patches `Alert()` (`SetFunction`, LVO -108) to write the
  code into the status record, `CacheClearU()`, then chain on.
- **strap** boots the volume with the ROM's FFS; dos runs `S/Startup-Sequence`.
- **`zinerun`** (an AmigaOS program built like zenixboot: m68k-elf gcc, `tools/mkhunk.py`): finds the session through
  `FindResident("zine.early")` (the anchor's address sits behind the Resident structure). **Walks the DosList first**:
  any disk device or volume other than the ZINE volume, `DFx:` and handlers such as `RAM:`/`NIL:`/`CON:` means a driver
  mounted a partition after pri -59: the way back without running the program ("the AmigaOS start mounted DH0:: not
  running NAME"), so the owner's disks are never offered to the program's AmigaOS. Then: status `RUNNING`; the
  program's output to `SYS:.zine/output` (`SelectOutput`), input `NIL:`; `SetProgramDir` and `CurrentDir` the
  program's directory on the volume (`PROGDIR:` works); `SetProgramName`; `LoadSeg`; the seglist in `cli_Module`;
  `RunCommand(seg, stack, args)` with the arguments ending in "\n" (stack 64 KB by default: the boot shell's 4 KB breaks
  demos); `IoErr()` tells a failed stack allocation from a program that returned -1. On a 68040/68060 it first opens the
  owner's `68040.library`/`68060.library` from `LIBS:` if given (owner question 4, pending); Emu68 needs none (knowledge
  base amiga/emu68-and-pistorm.md). When the program returns: `UnLoadSeg`, the last 16 KB of the output copied into the
  mailbox, status `RETURNED` with the return code, `CacheClearU()`, `ColdReboot()`.

## 7. The way back

### 7.1 One path: through a reset

Every ending reaches Zenix the same way: a reset into `zine.early` (pri 104). A program that returns is followed by
zinerun's `ColdReboot`; a keyboard reset, an alert's reset or the program's own reset arrive there anyway. One path, one
set of tests; the reset puts the custom chips, CIAs, Gayle and the boards in their reset state, so the way back never
depends on what a program left in their write-only registers or interrupt enables (about one second more than a direct
jump). zinerun does not call `LoadView` or `WaitTOF` (they hang if the program left the vertical blank off).

### 7.2 The switcher's way back (from `zine.early`)

1. Its own stack and vector table (VBR), with a return-from-exception stub for level 7 (Emu68 delivers ARM errors as a
   non-maskable level-7 interrupt). Interrupts masked; INTENA, INTREQ, every DMA channel, both CIAs' ICR (`0x7F` and a
   read), every Gayle interrupt enable (IDE and the card's) off; floppy motors off; FMODE/BPLCON0/BPLCON3 reset; a timed
   wait for the blitter; audio off. On Emu68 the Pi-side DMA stopped.
2. The CPU: data cache off, **then** `cpusha dc` (nothing can be written into the cache between the two); on a 68030 its
   MMU off (`pmove` TC, TT0, TT1; the line-F vector caught for an EC030); on a 68040/060 TC and the transparent
   translation registers off, after `ptest` shows the corner maps to itself (if not: the damaged path). A software reset
   leaves the CPU's MMU and caches as the program's AmigaOS set them; a keyboard reset clears them.
3. **The sums.** All good: go on. Any bad: the **damaged path**: the klog area is put back if its own piece is good, the
   reason set to `KLOG_END_ZINE_DAMAGED` (the boot note `BOOTNOTE_ZENIX` is already there), address 4 cleared (the next
   start is a cold one); reset. Where the boot menu runs (the owner's card), Zenix starts on consistent disks and says
   "the previous boot ended under ZINE: NAME wrote over Zenix's memory (the disks were saved first)"; elsewhere the
   machine boots what it boots normally (for example the owner's AmigaOS).
4. Every saved piece copied back; then address 4 cleared (low memory's restore brought back zenixboot's old pointer).
   The anchor needs no clearing: with no valid ExecBase no reset looks at it, and the kernel clears its magic at boot.
5. The caches flushed (`cpusha bc`, or CACR bit 3): Zenix's code is back where the program's code ran, and Emu68 would
   otherwise run its old translations.
6. Jump to the resume routine (supervisor mode, SR `0x2700`), with the status record's values.

### 7.3 Zenix wakes up (the resume routine and the kernel)

The resume routine restores the registers, both stack pointers, the full CACR, VBR and, on a real 68060 only, PCR
(the superscalar bit; never on Emu68, which reports AttnFlags `807F` with 68040 behaviour). Then the kernel and the blob
reprogram, from Zenix's own records (write-only registers cannot be read back):
- CIA-A: the 100 Hz tick, the keyboard's interrupt and one handshake; held keys and modifiers forgotten; port A and its
  directions as saved. CIA-B: the E-clock counter started again as `eclock_init` does, with a software offset so that
  `hr_time` goes on from the value it had at the hand-over (loading the old value through the latches would change the
  counter's period); port B as saved.
- Paula: the serial port's speed and interrupts, ADKCON, audio off; INTENA as the kernel had it.
- The display: a full set of registers, not only `gfx_init`'s writes: BEAMCON0 as Zenix found it at boot, the copper
  list address (the palette is in the copper list), BPLCON0-4, FMODE, the display window and DIWHIGH, the modulos, every
  sprite off; the screen's contents are back from the snapshot.
- The disks: the Gayle IDE settings, and the drive identified again (its size and serial must match, else that disk's
  writes stay held and the log says why); the SD controller and card initialised again (AmigaOS's own SD driver ran under
  Emu68; zenixboot's start does the same today); the A3000 SCSI chip; the floppy recalibrated and its disk checked
  again (the ID it reads must match, else it counts as changed, and a filesystem mounted from it fails until mounted
  again: the man page says so). Then held writes are released.
- The input baselines, the Pi's interrupt controller, the boot note cleared, the klog reason `KLOG_END_RUNNING`,
  the processes scheduled again, netd told nothing (it restarts the drivers it lost), the log line `zine: back from NAME
  after 1 min 12 s (it returned 0 | it was stopped by a reset | it crashed: alert 8000000B | alert unknown)`, interrupts
  on. `SYS_zine` returns to `zine`, which prints the program's output from the mailbox (only after a normal return:
  after a reset it is lost) and

        ZINE: Arte ended (return code 0) after 1 min 12 s; Zenix is back

### 7.4 Time while Zenix sleeps

Zenix is paused as a laptop is asleep: the tick count and `hr_time` do not count the session (Linux's monotonic clock
does not count a suspend either), so lwIP's and the service manager's timers do not fire in a burst. The wall clock is
moved on by the session's length: from the Pi's free-running system timer under Emu68 (it counts from the Pi's boot,
knowledge base amiga/emu68-and-pistorm.md), else from the battery clock, read at both ends; with neither, the clock
stays behind and the log says so (`sntp` puts it right). Network peers may give up on a connection meanwhile; the DHCP
lease is renewed by the drivers' restart; bytes sent to the serial port during the session are lost.

### 7.5 A program that never ends

No key can be watched while a demo owns the interrupts: **Ctrl-Amiga-Amiga is the way out** (owner question 5,
pending): the keyboard resets the machine whatever the program does, and `zine.early` takes the way back. If the program
destroyed ExecBase or the anchor, the reset is a cold start: where the boot menu runs, `BOOTNOTE_ZENIX` starts Zenix on
consistent disks; elsewhere the machine boots normally. Switching off is safe at every moment.

## 8. The machines

- **The stock A1200** (chip RAM only): `zine` says "ZINE needs fast RAM (this machine has only chip RAM)". Nothing of
  ZINE is loaded there; its kernel part (6.2, 3-6 KB) is the only cost, and the RAM floppy's kernel may leave it out.
- **PiStorm with Emu68** (the first target): the warm reset is the one `reboot` and the boot menu use (*verified*); the
  top of the highest fast region survives a reboot (*verified*), and ExecBase is in the other region. CLAUDE.md's rules:
  every place that writes code flushes the instruction cache (the code block, the switcher before the jump home); never
  `cinva`; no delay loops (every wait timed by the CIA, with a limit); `cpusha` chosen by AttnFlags `807F`; no 060-only
  `movec` (PCR). The Pi-side DMA is stopped before the copy, the reset and the restore. AmigaOS's SD driver
  (`brcm-emmc.device`) runs during the session: its unit 0 (the whole card) is read-only by default (step 7 checks the
  owner's Emu68 configuration), its partitions' boot nodes are removed, and zinerun refuses to run the program if one is
  mounted anyway. Unknown: fast RAM through a keyboard reset, CoolCapture on 3.2.3 (step 0).
- **The Apollo 1260** (68060 80 MHz, 64 MB): its RAM may be added during diag init (not `MEMF_KICK`): then the anchor in
  chip RAM carries the residents and `zine.early` takes the corner (3.3). A 68060 lacks 64-bit `mulu.l`/`divu.l` and some
  FPU instructions; a program that needs them crashes under ZINE as when booted from its own floppy, unless the owner's
  `68060.library` is given (owner question 4, pending). The way back turns the 060's data cache, MMU, VBR and PCR back.
  Step 0 runs there under AmigaOS alone (Zenix itself has not run on it yet).
- **68030 machines with fast RAM**: CACR bit 3; the data cache and the MMU off on the way back.
- **FS-UAE** (the tests): Kickstart 3.1 and 3.2 with fast RAM, 68030/040/060; the keyboard reset of `tools/diag-test.sh`.

## 9. What can go wrong, and how each case ends

### 9.1 The program file (untrusted)

`zine` walks the whole file before anything is allocated: `HUNK_HEADER` (no resident library names; the first and last
hunk numbers and the size table inside the file, each size with its memory bits 30-31: chip, fast, any, or both set
meaning an extra attribute longword), then each hunk, its type longword with bits 29-31 (memory and advisory flags)
masked first: `HUNK_CODE`/`DATA`/`BSS` (sizes not larger than the table's), `HUNK_RELOC32` and `HUNK_RELOC32SHORT`
(0x3FC; word entries padded to a longword when their count is odd), `HUNK_SYMBOL`/`DEBUG`/`NAME` skipped by their
lengths, `HUNK_END`; every count and offset range-checked with arithmetic that cannot wrap; a relocation names a hunk that
exists and an offset with offset + 4 within its size. Refused for now, each with its own message: `HUNK_OVERLAY`/`BREAK`
("NAME uses overlays: not supported yet"), 0x3F7 (the public documents disagree on its form: the AmigaDOS Manual's
DRELOC32 with long entries, the Guru Book's 2.0 short form; step 1 checks LoadSeg on 3.1 and 3.2 and the note follows),
`HUNK_RELRELOC32` (0x3FD, V39). Anything else: "not an AmigaOS program (unknown hunk 0x3F9 at byte 1234)". A file that
passes is still loaded by AmigaOS's own LoadSeg.

### 9.2 The cases

| What happens | How it ends |
|---|---|
| The file is damaged, not a hunk file, or needs more memory than there will be | refused before anything: a message, exit status 1 |
| The corner would be too big, the ExecBase lies in it, no copy, a floppy-started Zenix | refused before anything |
| A filesystem server does not answer the sync | refused; writes and processes released; nothing handed over |
| The corner or anchor does not survive the reset (pass 1 or `zine.early` finds a bad sum, or the corner is not protected) | the way back finds it: the damaged path, or (if Zenix's pieces are intact) Zenix back with "the AmigaOS start failed" |
| The ROM ignores the ExecBase (a check we did not satisfy) | a cold start: the boot note starts Zenix where the boot menu runs; elsewhere the normal boot |
| Pass 2's KickTags are refused (a checksum or KickMem failure) | the ROM boots as usual: the boot menu (note: Zenix), or the machine's normal boot; the owner's AmigaOS then behaves as at any boot; Zenix is not resumed but its disks are consistent |
| A different Kickstart than the copy's | pass 1 sees the version: the way back |
| A driver mounts a partition anyway | zinerun sees it and does not run the program: the way back |
| strap does not boot the volume | a bootable floppy or Gotek image in DF0 boots instead (dos may validate an AmigaOS floppy, as at any boot; the man page says so), or Kickstart waits for one; Ctrl-Amiga-Amiga: `zine.early` sees `BOOTING` and goes back: "the AmigaOS start failed" |
| LoadSeg refuses the program, or it returns an error code | zinerun reports it: "NAME could not be loaded (error 121)" / "returned 20", with its output |
| The program crashes (an alert, "Software Failure" and Reboot) | the alert's reset: "NAME crashed (alert 8000000B)", or "alert unknown" |
| The program never ends | Ctrl-Amiga-Amiga (7.5) |
| The program resets the machine itself | "NAME reset the machine" |
| The program writes over Zenix's corner | the sums: the damaged path |
| The program writes over Zenix's memory outside the corner | nothing: that memory was the program's; the copies bring Zenix's back |
| The program destroys ExecBase or the anchor, then is reset | a cold start (7.5) |
| The program writes to a disk itself (its own device calls) | ZINE cannot stop it, as AmigaOS cannot; the owner's partitions have no handler in the session; a program that writes raw blocks is as dangerous as under AmigaOS |
| Power off at any moment | consistent disks |
| A second `zine`, `zine` from ssh, a service or init (through the redirect) | refused (-EBUSY, -EPERM) |

No case writes a Zenix disk between the sync and a successful return, and the fallback is always a fresh start on disks
that were consistent at the hand-over. The sums guard against accidents (a demo's stray writes), not against a program
that means harm (9.3).

### 9.3 What ZINE gives away

A program run under ZINE owns the machine, as under AmigaOS: it can read all of Zenix's memory (other users' files in
the caches, keys in memory), change it and fix the sums, or write any disk. **Running a program with ZINE is as
trusting as running it as root.** That is why only root or the `zine` group may (owner question 1, pending), and the
manual page says it in its first lines: run programs you trust.

## 10. What the user sees

- **`zine PROGRAM [ARGS...]`** (`docs/man/zine.txt`, `commands.md`, `features.md` when built). No options in this
  version (later ones go here: a system-friendly mode, a directory to give, assigns). NOTES: what 9.3 says; Zenix is
  paused while the program runs; Ctrl-Amiga-Amiga brings it back; files it writes are not kept; overlays and disk images
  are not supported; fast RAM and a new enough zenixboot are needed; a floppy in DF0 is seen by the session as at any
  boot; a floppy mounted in Zenix must be mounted again if its disk was changed.
- **By name:** an executable file (`x` bit) that starts with `00 00 03 F3` is run by the kernel as `/bin/zine PATH
  ARGS...` (`proc_spawn` reads the first longword already; a few lines). This also applies to services and init, which
  the kernel's check then refuses. If `/bin/zine` is missing: -ENOEXEC, "an AmigaOS program: ZINE is not installed". A
  hunk file without `x` runs only as `zine FILE` (Unix's rule); wasabid's `PUT` gives new files 0644 (D24), so its 0755
  rule could also cover the hunk magic (a D24 question for the main session).
- **Messages**: the refusals ("ZINE needs fast RAM (this machine has only chip RAM)", "ZINE needs Zenix started by
  zenixboot 2026-11 or newer", "zine: not allowed (root or the zine group, at the Amiga's own console)", "NAME: not an
  AmigaOS program (...)", "NAME needs 2.5 MB of chip RAM; this machine has 2 MB", "Zenix holds 9 MB at the moment: stop
  some programs first"), the lines of 6.1, the end line of 7.3, and after a fresh start the previous-boot lines.
  (Kickstart older than 2.04 cannot happen: zenixboot itself needs dos 37.)
- The screen: Zenix's console with the lines; then the resets (dark or grey for a second or two, plus the IDE probe on a
  real A1200); then the program; then a reset again and Zenix's console exactly as it was, with the end line.

## 11. Interfaces and claims (for the main session)

These touch the main session's shared files (`kernel/kernel.h`, `kernel/main.c`, the Makefile) or frozen formats, so
the main session claims and builds them; this note only names them.

- **System call 93 `SYS_zine(op, a, b)`** (the next free number): `ZINE_INFO` (a frozen `struct uzine_info`: can this
  machine, why not, the sizes), `ZINE_VOLUME` (a block of size `a` at the top of the corner's region, owned by the
  caller), `ZINE_GO` (`a`: a frozen `struct uzine_go` - the blob and its size, name, arguments, stack size -; `b`: the
  frozen `struct uzine_result` - how it ended, the return code, the alert code, seconds asleep, the output's length).
  Errors: -ENODEV, -ENOEXEC, -ENOMEM, -EBUSY, -EPERM, -EIO. No new error numbers.
- **bootinfo `+0x84`**: the address of this boot's ExecBase copy (0: none), a new field at the end; **low memory
  `0x1400`-`0x17FF`**: the copy's layout (magic `ZEXB`, ...). Both frozen; frozen-interfaces.md gets a new "low-memory
  map" item (it lists only the bootinfo layout today). zenixboot writes them; the kernel clears the magic when `+0x84`
  does not point there.
- **The chip anchor's place** (4 KB below the klog area) and its magic, which the kernel clears at boot: frozen with the
  klog area's layout.
- **klog reasons** `KLOG_END_ZINE` 3 and `KLOG_END_ZINE_DAMAGED` 4 (free today). zenixboot's trial logic counts only
  PANIC and RUNNING, so they are no evidence of a failed start; claimed so nobody reuses them.
- **`vfs_sync` mode `VS_HOLD`** and held disk writes (kernel-internal; `disk_frozen` stays one-way for reboots).
- **The group `zine`** (a group number in claims.md and an `/etc/group` line), if the owner agrees.
- **The kernel's redirect** (`00 00 03 F3` runs `/bin/zine`; the path too): frozen behaviour.
- **Files:** `/bin/zine` (set-user-id root), `/lib/zine/zine.blob` (switcher, pass 1, residents, device, the blob's
  part of the resume, and zinerun, which `zine` writes into the volume), `/etc/zine/libs/` (the owner's CPU library, if
  any; never shipped). In the base group (they move with the kernel, D15/D22): the blob's layout must match the kernel's.
- **zenixboot's new version** reaches BOOT: only by hand, with the owner (CLAUDE.md: no write to the owner's card
  without an agreed plan).
- Documentation the main session updates when it records the answers: decisions.md (section 1's corner, checksum and
  saved-disk conditions and section 15's answers), future-ideas.md "Amiga demos on Zenix" (still describes a demo layer,
  option B, and block 4 as needed first: point it to this note).

## 12. Size and speed

- **Kernel:** 3-6 KB (*proposed*; step 3 measures; the RAM floppy's kernel may leave it out, 6.2).
- **`/bin/zine`** about 20-30 KB (the hunk check, the FFS writer, messages); **`zine.blob`** about 16-24 KB (the
  switcher and the hardware parts in assembler, the residents and the device, zinerun about 6 KB); loaded only when used.
- **Memory while not used:** none.
- **Time** (*proposed*; step 7 measures on the PiStorm): the sync as `reboot` (tens of ms); copying and summing about
  1 MB (about 50-150 ms on a 68030, less on the 060; chip RAM slower on the A1200's bus); three resets (pass 1, pass 2,
  the way back) and the AmigaOS start (1-3 s each way under Emu68; on a real A1200 the ROM's IDE probe adds seconds).

## 13. Tests

No copyrighted demo goes into the repository. The test programs are written for the purpose with the m68k-elf toolchain
and made into hunk files by a new `tools/mkhunktest.py` (several hunks: code in any memory, data in chip RAM, BSS;
relocations across hunks, the short form, symbol and debug hunks), in `tools/tests/zine/`:

| Program | What it does | The test checks |
|---|---|---|
| `ztake` | takes over as a demo does (Forbid, `LoadView(NULL)`, `WaitTOF`, `OwnBlitter`, Disable, its own copper list and level-3 interrupt, a CIA-B timer, a short sound, BEAMCON0 and sprites changed), runs 100 frames by its own count, gives everything back, prints a line, returns 5 | "ZINE: ztake ended (return code 5)", its line shown, Zenix answers, a background job still runs, the second console's login is there, the screen's text as before, `e2fsck -fn` of the root image clean afterwards |
| `zalloc` | `AvailMem`/`AllocMem` of all chip and fast RAM, a pattern written and checked, freed; prints the sizes | the sizes are what `zine` promised (within Kickstart's own use); Zenix comes back intact |
| `zloop` | takes over and loops for ever | the keyboard reset brings Zenix back: "stopped by a reset" |
| `zsmash` | finds the allocated range at the top of fast RAM in exec's list and writes over it, returns | the damaged path: a fresh start with the message; the disks consistent (clean unless an orphan was open, which the test arranges not to be) |
| `zcrash` | a dead-end alert | "crashed (alert ...)" or "alert unknown" after the alert's reset |
| `zfiles` | reads a data file by a relative path and by `PROGDIR:`, writes one, prints `Info()` of its volume | its output; the written file is not on Zenix's disk |
| `zreset` | calls `ColdReboot()` | "reset the machine" |

Also: by name; the refusals (no `x` bit, a non-hunk file, damaged hunk files from a `tools/mkbadhunk.py`, overlays,
0x3F7, from a telnet login, a non-member, a chip-only machine, a floppy-started Zenix, a stale copy at `0x1400`); a
server that does not answer the sync (a test server that sleeps): refused and Zenix goes on, nothing lost; a write held
during the hand-over completes after the return; `ztake` twenty times in a row with `free` the same each time; the CPU
matrix (68030, 68040, 68060 with fast RAM; Kickstart 3.1 and 3.2); the network after the return (the driver restarted,
DHCP, ping) and the clock (moved on by the session's length within a second). Host tests: the hunk check (every damage,
and fuzzed as `zpkgfmt` is), the FFS writer (read back by `tools/ffsread.py`: names, case clashes, directories, bits,
full images), the planner (synthetic pool layouts: every used byte outside the corner saved, nothing twice, the free
space enough, a restore giving the same memory). The A1200 steps are the owner's hardware time and are prepared so that
each run is ready for it.

## 14. Build order (small steps, each tested)

0. **Measure first, on all three kinds of machine (no Zenix change).** `zspike`, an AmigaOS program (hunk file), run under
   AmigaOS in FS-UAE (Kickstart 3.1, 3.2) and then on the PiStorm and the Apollo 1260 (AmigaOS only), with the owner:
   it prints every region's attributes (`MEMF_KICK`), ExecBase's place (on the 1260 with and without `68060.library`),
   whether a MapROM or a KickTag module is in use (and which); copies ExecBase as zenixboot will, **takes the machine over
   as zenixboot does**, fills all RAM with a pattern except the copy, address 4, the anchor and a pretend corner of the
   planned size plus G (so it really is in Zenix's place), writes the copy back with CoolCapture set and resets. Pass 1
   reports on the serial port which words Kickstart changed before it, whether the corner and anchor survived, then
   installs the KickTags and resets; pass 2's residents report again, check the corner shows as allocated, try the
   fresh ConfigDev, boot a small FFS image from a memory device, list the DosList, run a program, raise an alert, and
   compare a candidate `SumKickData` sum with the ROM's. Each run also with **keyboard resets** (including a long hold
   on the PiStorm). At its end it clears the Kick pointers, so the next boot is normal. Results into this note ("As
   built") and the knowledge base; the design is confirmed or revised before step 1.
1. **Host parts:** the hunk check and the FFS writer in a shared C file with host tests and fuzzing; LoadSeg's answer
   to 0x3F7 on 3.1 and 3.2.
2. **zenixboot's copy** (`0x1400`, bootinfo `+0x84`), tested in FS-UAE; installed on the owner's card only with the owner.
3. **The AmigaOS side alone**: the blob's residents, device and zinerun, tested from AmigaOS in FS-UAE with a harness that
   builds an anchor and corner in AmigaOS's memory and resets into the ZINE flow.
4. **The way in, back by a fresh start:** `SYS_zine`, the process stop, `VS_HOLD` and held writes, the planner, the
   switcher's way in; every ending takes the damaged path (a fresh start on consistent disks). Kernel size measured.
5. **The real return:** the switcher's way back, the resume routine, the reprogramming, the drivers' resume, the clock,
   the network restart, the result and the output. Then `zloop`, `zsmash`, `zcrash`, `zreset`.
6. **By name** (the redirect); the manual page, commands.md, features.md. **040/060**: caches, MMU, VBR, PCR; the
   owner's CPU library if the owner says yes.
7. **The owner's PiStorm A1200**: `ztake`, `zalloc`, `zloop` and a demo of the owner's own; times and sizes into this note.
8. **The Apollo 1260**, the same list (Zenix itself must run there first).
9. A focused audit of ZINE (CLAUDE.md: a deep core change).

## 15. Questions for the owner (what ZINE should be)

1. Who may hand the machine over? A program under ZINE can do anything to the machine, as root can. [Root and the
   members of a `zine` group, which your user is in; only at the Amiga's own console (or through your Wasabi), never
   from ssh, telnet or the serial port.]
2. While the program runs, all of Zenix is paused (other logins, the network, timers), as a laptop asleep. Right for
   now? [Yes; Zenix running beside the program is the much later "own AmigaOS kernel".]
3. Files the program writes live in memory and are gone when Zenix comes back. Right for now? [Yes; Zenix's disks stay
   untouched; keeping them comes with "Zenix files as an Amiga volume".]
4. On the Apollo 1260: may ZINE use your own `68060.library`, copied once into `/etc/zine/libs/`? [Yes, if you copy it;
   ZINE never ships it.]
5. During a program, Ctrl-Amiga-Amiga brings Zenix back instead of restarting the machine. Is that the meaning you
   want? [Yes; a real restart is the power switch, or `reboot` once back.]

## 16. Reviews

Revision 1 was reviewed twice from each of two angles (four independent reports, 2026-10-30): correctness and safety
of the hand-over and return against the manuals, and feasibility on the three machines with coherence against Zenix's
boot path and frozen interfaces. Every finding is worked in; the main ones:

- **The sync and freeze** (blocker in one report, major in three): `vfs_sync(VS_REBOOT)` resets the machine at its
  deadline, reports only the root, and `disk_frozen` fails requests half way. Now: user processes stopped first, a
  `VS_HOLD` mode with per-mount deadlines and no reset, writes held instead of failed, "consistent" instead of "clean"
  (6.2, 2.4, 9.2).
- **KickMem/KickTags in fast RAM** (blocker/major): only `MEMF_KICK` memory can hold them (Guru Book 2.4.6). Now: the
  chip anchor (3.3), `zine.early` at pri 104 taking the corner itself where needed, step 0 reports the attributes.
- **The summed header was written during the session** (major in all four): the status record is separate, in the
  anchor, with its own CRC and a cache flush after every write (3.3, 4.4).
- **`LastAlert` does not survive on Emu68** (major): the `Alert()` patch, `$0`/`$100`, "alert unknown" (6.4).
- **A borrowed ConfigDev could boot the owner's disk** (major): only a fresh one (6.4).
- **Nothing checked that the corner was protected; drivers could mount partitions after pri -59** (major): the free-list
  walk in `zine.early`, the DosList walk in zinerun (6.4).
- **The network drivers are user processes** (major): quiesce and end them; netd restarts them (6.2, 7.3).
- **The kernel's cost** (major): 3-6 KB, most of ZINE moved into the blob, the RAM floppy's kernel may leave it out
  (6.2, 8, 12).
- **Corner planning was circular and allocated after the freeze** (major): planned first; the snapshot uses the corner's
  free space; bounded; the 16 bytes at X and the 8 bytes after the end handled (4.2).
- **Step 0 could not settle feasibility** (major): on the real machines, the 1260 included, with keyboard resets, after a
  real take-over, filling all RAM, checking the protection, before step 1 (14).
- **The stale ExecBase copy and zenixboot's install** (major/minor): bootinfo `+0x84`, the kernel clears a stale magic,
  an install step with the owner, a new frozen item (3.1, 11, 14).
- **Security** (major): ZINE is root-equivalent; root or the `zine` group, the check in the kernel, -EPERM, `CON_INPUT`
  stated; the sums are for accidents only (6.2, 9.3, 15).
- **`PROGDIR:`, RunCommand's arguments and -1, cli_Module** (major/minor): fixed in zinerun (6.4).
- **One way back through a reset** (from a minor finding: board interrupt enables and write-only registers left by the
  program): every ending goes through `zine.early`; the display set made complete, BEAMCON0 and sprites included (7.1,
  7.3).
- Minor and nits: the cache order (data cache off, then `cpusha dc`); the 030 MMU; `ptest` before MMU off; PCR, USP,
  full CACR; the level-7 stub; Pi-side DMA stopped before the copy; address 4 cleared after the restore; the E-clock by
  a software offset; the boot note written before the hand-over; cold starts without the boot menu; ExecBase in the
  corner refused; the bare ROM stated; `lib_PosSize` over 1,000 refused; the CHKSUM difference update; hunk details
  (masked type bits, the short form's padding, 0x3F7's two definitions, 0x3FD, offset + 4); FFS details (`DOS\3`, case
  clashes, `:` and `/`, dates); `TD_ADDCHANGEINT` held; Mask and BufMemType; no `WaitTOF` on the way out; the floppy's
  recalibration and the IDE drive identified again; the size and time estimates corrected; CRC32 instead of
  add-and-rotate; the redirect for services and a missing `/bin/zine`; the impossible Kickstart 1.3 case dropped; the
  questions marked pending; the decisions.md and future-ideas.md updates listed for the main session. A Zenix-side
  `SumKickData` (dropping pass 1) is kept as an option for step 0 to confirm, not the design.
