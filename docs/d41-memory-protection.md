# D41: memory protection on CPUs with an MMU

Revision 2, 2026-11-12: revision 1 after its two independent design reviews, every finding folded in or answered
(section 17); built the same day on branch `mmu` (section 15). **Revision 3, 2026-11-17: on by default** where the CPU
is known to have an MMU (the partner's order of that day; section 19, branch `mmudef`). Since 2026-11-22 the one
shared tree and its page flips (sections 4, 5, 8 and 9) are replaced by a tree per process: DESIGN D60 stage 1,
docs/d60-full-protection.md "As built"; the rest of this model stands until D60's later stages. Roadmap block 6, "optional
memory protection on CPUs with an MMU", next in the order of work after sound (approved 2026-10-25); the details are
delegated (docs/decisions.md, 2026-11-12). Sources: the MC68030, MC68040 and MC68060 user's manuals (the knowledge
base's `manuals/`; pages named where a fact is used), the knowledge base's `amiga/emu68-mmu.md` (Emu68 has no 68k MMU)
and D37 (docs/d37-data-cache.md: the transparent translation registers Zenix already sets). No other system's code was
read for this.

## 1. What the partner gets, in plain words

Without an MMU, a program that writes through a bad pointer can damage another program, a server or the kernel, and
the damage shows up later somewhere else, or never (DESIGN D8, docs/limits.md "The no-MMU limit"). On an Amiga whose
CPU has a working MMU (a full 68030, 68040 or 68060: the A3000, the Apollo 1260, most 030 accelerators), Zenix can now
use it to stop that: an ordinary program may touch only its own memory. A write or a read anywhere else (the kernel,
another program, free memory, the custom chips, address 0) stops the program at once with a line saying where it
went, and the program is killed as for any other fault (exit status 139, SIGSEGV's); everything else goes on as if
nothing happened. Writes into a program's own shared code (D13) are stopped too.

It is **on by default** since 2026-11-17 (section 19) on a 68030 whose MMU works, a full 68040 and a full 68060;
`zenixboot MMU=OFF` (or the left mouse button held at boot, as for D37) switches it off, and `MMU=ON` also tries an LC
or EC 68040/68060, which nothing safe tells apart (section 19). Machines without an MMU (the stock A1200's 68EC020, a
68EC030, a 68EC040/EC060, the PiStorm's Emu68) run exactly as before, and pay nothing; the boot log says once, in a
plain line, why protection is off.

## 2. What it protects, and what it does not (the honest limits)

- **Protected processes** are every program a user runs (root's too): the shells, the commands, the desktop's
  programs, `init`, `login`, `boot`. Each sees only its own memory (section 6). It cannot reach the kernel, another
  process, the hardware or free memory, by reading or by writing.
- **System processes** see everything, exactly as today: the processes the service manager starts (the disk and
  filesystem servers, `displayd`, `sndd`, the mounts' servers, `msgpingd`), a process that creates a reserved port
  (`fs`, `fs.*`, `disk`, `net`, `audio`: `netd`, the test servers that stand in for it), and a network driver once it
  holds a device claim (D19). These read and write their clients' buffers in place (D1: messages carry pointers, not
  copies) and drive the hardware; they are the trusted base already (D8, "servers are trusted code"). A bug in one of
  them can still damage anything.
- **The kernel** is not protected from itself: it runs through the transparent translation registers (section 4),
  as fast as before, with no checks of its own but the existing ones (`user_ok_*` for every pointer a system call is
  given, the heap's magic words, the stack guards).
- **A protected server** (a program that creates an ordinary port and answers messages: `pingd`, the tests'
  servers) sees, while it holds a client's request, that client's memory too (section 8): the D1 model needs it.
- **Not a security boundary yet**: the trust rules of D8 stay as they are written (a system process is still
  unprotected; the documents still say so). What changes is that an ordinary program can no longer damage the rest by
  accident, nor reach the kernel's memory or the hardware on purpose. Two doors stay open: a program that calls a
  protected server lends that server its whole view while the call lasts (section 8: any user may run a server on a
  port that is not reserved, and a caller chooses to call it), and root keeps root's powers (a Copper list through
  `CON_GRAB`, the raw disk, the services it starts).
- **Memory shared by passing an address** between two protected programs no longer works (only `priotest` did it:
  its children read the parent's block; it runs with protection off). Pointers handed down a chain of protected
  servers (a client's buffer passed from one protected server to another) fault in the second; the system's servers,
  which do such things, are system processes.
- **Stack overflow** stays as today: a program's stack lies in its own block, above its BSS, so an overflow into its
  own BSS is its own memory; the stack guard (architecture.md section 6) still finds it. No guard pages (they would
  cost a page per process and move every stack).

## 3. Which CPUs, and how Zenix knows

| CPU | MMU | Zenix |
|---|---|---|
| 68EC020 / 68020 | none (a 68851 beside a 68020 is rare and has its own formats) | nothing changes |
| 68030 | yes (PMMU, MC68030UM section 9) | protection (the default) |
| 68EC030 | none (the access control registers sit where TT0/TT1 are) | found by the probe, nothing changes |
| 68040 | yes (MC68040UM section 3) | protection (the default: its FPU answers) |
| 68LC040 | yes (no FPU) | protection with `MMU=ON` (section 19: not told from an EC040) |
| 68EC040 | none | left alone; with `MMU=ON` found by the probe |
| 68060 | yes (MC68060UM section 4) | protection (the default: `PCR` 0x0430) |
| 68LC060 | yes (no FPU) | protection with `MMU=ON` (`PCR` 0x0431, as the EC060) |
| 68EC060 | none: `URP`, `SRP` and `TCR`'s E bit "are not supported and must not be used" (MC68060UM 1.1.2.2) | left alone, no MMU register touched; with `MMU=ON` found by the probe |
| Emu68 (the PiStorm) | none: Emu68 stores `TC` with its E bit cleared on purpose and ignores PFLUSH (knowledge base `amiga/emu68-mmu.md`) | never tried: `hwinfo`'s `HW_DT` says Emu68 |

The EC parts must not be given the MMU's instructions (MC68060UM 1.1.2.2: `PFLUSH`, `URP`, `SRP` and `TCR`'s E bit
"must not be used" on the EC060; MC68040UM appendix B: `PFLUSH`/`PTEST` on the EC040 give "an indeterminate result").
The 060's `PCR` says 0x0430 for a full 68060 and 0x0431 for both the LC and the EC060, and the 040 has no identity
register at all, so an LC and an EC part can only be told apart by trying. That is done only when `MMU=ON` asked for it
(the risk is written here and in zenixboot's page; the default leaves a part without an FPU alone, section 19): AttnFlags cannot tell an EC part from a full one, so the kernel
**tests the MMU it is about to use** (`mmu_probe`,
section 12): with its real tables built, one test page is mapped onto another page and one is left invalid; with the
illegal-instruction, line-F, access-fault and MMU-configuration (030 vector 56) vectors caught, translation is switched
on and the two pages are read with `MOVES` as user data (function code 1, so the kernel's own transparent window does
not apply). Protection is used only if the first read returns the other page's word (translation works) and the second
faults (an invalid page stops a user access); otherwise the registers are put back as they were and the log says
"no working MMU". A part that lacks a register traps; a part (or an emulator) that takes the registers and does nothing
returns the test page's own word. Never tried at all: 68020, Emu68, a machine whose loader gave no hardware table (its
boards are unknown, as D37), and a 68040/68060 with a board that is not RAM above 16 MB outside the fast RAM's block
(section 4: the kernel's window would not cover it; neither test machine has one).

## 4. The shape: one table, identity mapped, two windows for the kernel

- **The MMU only protects; it never translates.** Every page descriptor maps an address to itself, so nothing in
  Zenix's single address space moves: no fork, no per-process addresses, pointers in messages stay valid, the loader,
  shared code (D13) and ZINE are unchanged.
- **One translation tree for everybody** (the user root pointer: `URP` on 040/060, `CRP` on 030). In it, a page is
  valid only while it belongs to the process whose memory is "flipped in" (the **tenant**, section 9) or to a client
  it is serving; every other page (the kernel, other processes, free memory, every I/O address) is invalid. 4 KB
  pages on all three CPUs (section 5).
- **The kernel never uses the tree.** Its accesses are supervisor accesses, and the transparent translation
  registers are set to match supervisor accesses only (the TTRs' `S` field `01`, MC68040UM 3.1.3 and MC68060UM 4.1.3;
  on the 030 `TT0`'s function code base 4 with mask 3, MC68030UM 9.3, 9.7.3), so the kernel's code and data accesses
  go through them exactly as before, with no table search and no ATC entries:
  - 040/060: `DTT0` = 0x0000A040 (the first 16 MB inhibited, serialized / precise), `DTT1` D37's block over the fast
    RAM in D37's mode (`cache_block` in cache.c gives it, also when D37 left the cache off: then inhibited; no fast
    RAM above 16 MB: no `DTT1`), `ITT0` = 0x00FFA000 (every address, cachable, for instruction fetches; D37 itself
    leaves `ITT0` 0 and lets the 060's `TCR` default or the 040's fixed default cache them, the same effect), `ITT1` 0.
    The supervisor root pointer `SRP` is the same root as `URP`: a supervisor access outside the windows (none is
    meant to happen) walks the tree, finds no valid page and stops the kernel with a panic, as a fault in the kernel
    does today.
  - 030: `TT0` = 0x00FF8143 (all addresses, function codes 4-7, `RWM` 1 so read-modify-write cycles match too, not
    cache-inhibited: the 030's data cache stays off, D37 section 7), `TT1` 0.
- **System mode and protected mode.** A system process (section 7) runs with the TTRs matching every access, user
  and supervisor (`S` = `1x`: `DTT0` 0x0000C040, D37's value, `ITT0` 0x00FFC000; 030 `TT0` 0x00FF8107, function code
  mask 7), so it sees all memory and the hardware
  through the windows, exactly as today, never touching the tree. A protected process runs with the TTRs matching
  supervisor accesses only, so its own accesses go through the tree. A switch between the two modes rewrites two or
  three registers; the tree and the ATC are left alone (a TTR match wins over the ATC: MC68040UM 3.1.3).
- **Translation stays on** from boot to reset while protection is on (`TCR`/`TC` E = 1; the 060's `TCR` 0x8200: D37's
  `DCO` kept, `NAD`, `NAI`, `FOTC`, `FITC` 0); it goes off only for ZINE's session (section 11) and before a software
  reset.
- **The state the loader leaves.** zenixboot's hand-over (`boot/handoff-c.S`, `zb_go`) switches AmigaOS's
  translation off on every 68030, 68040 and 68060 before the kernel is copied (AmigaOS tools leave it on through
  tables in memory Zenix will use: 68040.library and 68060.library, and on a 68030 Enforcer, MMULib or `CPU FASTROM`;
  the 68030's part since 2026-11-17, audit 2026-11-12 A6-14), with the illegal and line-F vectors caught for an EC part;
  D37 then sets the windows. The kernel checks again: on every 68030 boot, protection or not, `mmu_setup` reads `TC`,
  `TT0` and `TT1` (a missing register traps, as on the EC030) and, if any is on (a loader without that step), switches
  them off ("mmu: the 68030's translation was left on: switched off"). The floppy's bootblock starts from a reset,
  with no AmigaOS tool run before it. "Off" in this note always means this clean state
  (040/060: D37's windows, `TCR` with E clear, `ITT0`/`ITT1` 0; 030: all 0), never the loader's.

## 5. The tables

- **Geometry**, the same on all three CPUs: 4 KB pages; a root table of 128 entries (32 MB each, A31-A25), pointer
  tables of 128 entries (256 KB each, A24-A18), page tables of 64 entries (A17-A12). On the 040/060 that is the fixed
  4 KB layout (`TCR` P = 0, MC68040UM 3.2.1); on the 030 the translation control register says the same: `TC` =
  0x80C07760 (E, PS = 12, IS = 0, TIA = 7, TIB = 7, TIC = 6, TID = 0; PS + IS + TIA + TIB + TIC = 32), `CRP`
  0x80000002 (L/U 1 with a lower limit of 0: no limit; DT 2, short descriptors).
- **Rules for changing a page** (both reviews): every change that makes a page valid or invalid is followed, before
  the kernel returns to user mode, by a push of the changed descriptors' lines (040/060 with the data cache on) and an
  ATC flush (`PFLUSHA`): the ATC keeps invalid entries too (MC68040UM figure 3-9, the R bit clear; the 030's B bit),
  so a stale one could fault a page just made valid or, worse, keep a freed page reachable. One `PFLUSHA` per change
  (a retenant, a lend, a block made or freed) rather than a `PFLUSH` per page: the code is shorter and a change is
  rare outside a retenant. A page of a process that is not flipped in needs nothing (it is invalid both before and
  after).
- **Descriptors** (short format on the 030): a table descriptor is the next table's address + U + DT/UDT = 2 (the
  same bits on both families); a page descriptor is the page's address + U + M + the cache bits + W + resident (bit 0).
  U and M are set from the start, so the table search never writes a descriptor (MC68040UM 3.2.5, MC68060UM 4.2.5;
  and "resident, not used, modified" is a state the manuals forbid). Cache bits (040/060): **a page's mode is
  the mode of the kernel's window at the same address**, so the kernel and a program never hold one line two ways: the
  first 16 MB (chip RAM, a Zorro II or trapdoor fast RAM) inhibited (`CM` 10, as `DTT0`), the fast block D37's mode
  (copyback, or write-through with `DCACHE=WT`); with the data cache off (`CACR`'s DE clear) only instruction fetches
  use the mode, and every page is then write-through, cachable, as fetches were before. So a program's code in chip
  RAM, or in fast RAM inside the first 16 MB, runs without the instruction cache while protection and the data cache are
  both on (rare: the 1260's fast RAM is above 16 MB). 030: no cache bits (its data cache is off). W (write-protect,
  bit 2 on both) on shared code. **A page is valid or invalid by bit 0 alone**; flipping a page is setting or
  clearing it (and W).
- **Only the memory pools are mapped**: page tables exist for every 256 KB that holds part of a range of the memory
  manager (chip and fast, mem.c), and a pointer table for every 32 MB that holds one. Root entries and pointer
  entries elsewhere are invalid (a user access there faults at the first level). Everything outside the pools (low
  memory, the kernel image, the klog area, the RAM disk, the chips, the ROM, the boards) is never valid for a user.
- **Where they live**: one kernel block from the pools at boot (`mmu_setup`; fast RAM first, as every kernel block),
  page tables aligned to 256 bytes, the root and pointer tables to 512 (MC68040UM 3.2.2). They lie in the kernel's
  window, which is copyback on a 040/060 with the data cache on, and the 060's manual says "Translation tables must not
  be placed in copyback space ... the processor only uses tables in external memory" (MC68060UM 4.2.5; the 040 reads
  through the cache, but its manual "highly recommends" inhibited tables and warns that a cached descriptor with the
  cache then disabled locks the table search up, MC68040UM 3.2.1). There is no third data window to make them
  inhibited, so the rule is: **every run of descriptors the kernel touches is pushed with `cpushl` (which also
  invalidates: `DPI` is 0, D37) before the ATC flush**, reads included (a flip reads before it writes, so the lines it
  touched are pushed), and translation is always switched off before `CACR`'s DE is cleared (ZINE's hand-over and the
  reset do it in that order), so no table search ever meets a cached descriptor with the cache off. Chip RAM for the
  tables would avoid the pushes but cost the scarce chip RAM and make every table search slow.
- **Cost**: 512 + 512 per 32 MB + 256 per 256 KB of pools. The Apollo 1260 (2 MB chip, 64 MB fast at 0x01000000):
  8 + 256 page tables, 3 pointer tables, the root: about 68 KB, 0.1% of its memory. A 68030 with 4 MB of fast RAM
  and 2 MB of chip: about 8 KB. (Two pool ranges that share a 256 KB or a 32 MB piece count it twice when the block is
  sized: a table or two too many, never too few.) Chosen over 8 KB pages (half the tables) because every protected block is rounded to
  whole pages (section 6) and 4 KB halves that rounding; chosen over tables made as needed because those add an
  allocation inside every allocation for 0.1% of RAM.

## 6. Who owns which page

- **A protected process's view** is exactly what the kernel already lets it hand to a system call (`user_ok_wr`,
  `user_ok_rd`, proc.c): its own block (image, BSS, stack, arguments: `base`/`span`), its `SYS_memalloc` regions
  (`reg[]`, at most `NREG`), read and write; and its code (`code`/`csize`: a ZEX2 program's shared segment, or its
  private copy), **read only** (W). Nothing else: not its kernel stack (kernel only; the servers that read a kernel
  message on it are system processes, section 7), not its environment block (read and written by system calls only).
- **Those blocks are whole pages** while protection is on (for a protected process; a process that is a system one
  from its start, a service's server, gets its blocks as before, section 7): `mem_alloc` with the new flag `MEM_USER` (the image block,
  the private code copy, `SYS_memalloc`) gives a block whose data starts on a page and is a whole number of pages;
  its 16-byte header is the last 16 bytes of the page below, a page that holds no other protected block's data (the
  block before it ends at least a page lower), so no two owners share a page and no program can reach a header. The block's last page is wholly its own (the rounding is
  inside the block, and a tail too small to stay free is taken in too); in front of the header there is up to a page
  of free memory the kernel's small blocks use (the allocator cuts a free block three ways: free, used, free). Big
  blocks still come from the high end of a free block and small ones from the low end, as before (architecture.md
  section 5), so a small protected block may lie among kernel blocks: harmless, its pages are its own. Off, `MEM_USER`
  changes nothing: no machine without protection loses a byte.
- **What the rounding costs** (estimated, then measured in section 15): about half a page at the end of each block
  and up to a page in front that only the kernel's small blocks reuse. A process has an image block and, through
  libz's allocator (D27), up to 16 regions whose chunks are 4 KB x k + 64 bytes (the worst case: 4,160 becomes 8 KB):
  some 8-40 KB a process, a few hundred KB for 15-30 processes. On the 1260's 64 MB nothing; on a 68030 with 4 MB a
  real cost, which is one reason the switch is optional. `ps` and `free` show the rounded blocks as used memory (free
  counts what the pools give out). A later libz can size its chunks in whole pages (not built).
- **The block headers are unchanged** (ZINE's blob walks them: `user/zine/blob.c`, size, used bit, magic). No flag
  marks a protected block. Instead, **every page wholly inside a block that is freed is made invalid** (any block:
  for the kernel's own blocks those pages were invalid already), so a freed region can never stay reachable.
- **Shared code (D13)**: a segment's block is owned by `SEG_OWNER`; it is valid, read only, for each process running
  it while that process is the tenant. A write into it now faults: `/tests/crashpure poke` ends with the protection
  line. `tools/pure-test.sh` expects the checksum's "damaged" path after that write, so it runs with protection off.
- **The update bundle (D15)**: `zup` (root; stage 1 may be an older zup from the root, so no new system call can
  replace this) reads the bundle zenixboot left in memory in place. The bundle is a kernel block, not on page bounds;
  for every process with uid 0 its pages are valid read only until `UPD_RELEASE` (then flipped out, the partial pages
  at its ends too, before it is freed). Those partial pages may show a few bytes of the kernel's neighbouring blocks
  to root, read only; never another program's data (a protected block's data starts and ends on pages).
- **Chip RAM** a protected program asked for (`MEMA_CHIP`) is its own like any region, uncached (`CM` inhibited).
  DMA needs the custom chips, which no protected process can reach: a protected program cannot start the blitter,
  the Copper or a disk DMA at all.
- **Drivers' DMA memory** (`DRV_OWNER`, drv.c) and the display server's screen are system processes' and need nothing.

## 7. System processes

A per-slot mask (`mmu_sys`, bit n for `procs[n]`, cleared when a slot is given to a new process) marks them:

1. `svc_born` (svc.c): every server the service manager starts, including the built-in images and `netd` (a service
   boot registers) and the mounts' servers `mount` registers.
2. `port_create` of a reserved name (ipc.c `reserved`): only root may do it, and the kernel sends such a port its own
   requests whose message lies on the caller's kernel stack.
3. Any `SYS_drv` call that passes its checks (drv.c: a child of the running network server, or the sound server):
   a network driver reads its hardware table and claims its device, then drives the registers and its DMA memory.
4. A successful `CON_GRAB` (gcon.c, root): a display server drives the blitter, the Copper and chip RAM itself.
5. A successful `SYS_mount` (vfs.c, root): the owner of the port mounted gets the kernel's filesystem requests, whose
   message, its group list and some buffers lie on the caller's kernel stack.

A process marked while it runs (rules 2-5) switches to system mode at once, before its system call returns.
Nothing else is a system process (in particular not a child of one: `displayd`'s desktop programs, `init`, `login`,
the shells `telnetd` and `sshd` start, `wasabid`, `zinstall`, `fdisk`, `dd`; the reviews found none of them touching
memory outside their own in user mode). A system process stays one until it ends.

## 8. Protected servers: the client's memory lent while its request is held

A protected process that serves a port reads the client's message (`struct umsg`, in the client's memory) and its
buffers in place. So while a protected tenant holds a request (`MSG_SERVING`, server = it), the client's view (section
6) is valid for it too:

- when `ipc_recv` hands a message to a protected tenant, the client's view is flipped in at once (`mmu_lend`);
- when the request ends (`ipc_finish`: the reply, or `-ECONNRESET`, also when the server faults while holding it),
  nothing is done at once: the client's view leaves at the next retenant, which comes before the client runs again
  (it is flipped out with the rest, then the new tenant and the clients it still serves in, so a D13 segment several
  of them run is never left out by one flip-out). Until then the server could still reach the client's memory; a
  server normally answers and waits for its next request in one trap (`replyrecv`), so the window is usually that
  trap; but when `replyrecv` finds another request already queued, the server goes back to user mode still seeing the
  earlier client's memory while it serves the next one, until the next retenant (audit 2026-11-12 A6-13: read, left
  as it is; making the set again at once is the cost measured below).
  Measured: making the set again at every reply cost a whole retenant per message (section 15). A lent client that
  ends is flipped out at once (`mmu_forget`);
- when such a server becomes the tenant again, every client whose request it holds is lent again (a scan of the 32
  slots, as `ipc_serving` does).

A system client of a protected server (rare) is lent the same way: its view is its own memory, as for anyone (not
its DMA memory). A protected server that hands a client's pointer on to another protected server is not supported
(section 2).

## 9. The context switch

`schedule()` calls `mmu_switch(next)` just before `switch_to` (interrupts masked, as the rest of the switch):

- **pid 0 (idle)**: nothing (it never runs in user mode).
- **A system process**: the TTRs to system mode, if they are not already (two movec on 040/060 plus `ITT0`; one
  `PMOVE` on the 030). The tree stays as it is: its tenant is still flipped in, harmless (a system process never
  reaches it).
- **A protected process that is already the tenant** (the common case: a program that called a filesystem server
  and is answered): the TTRs to protected mode, nothing else. So a system call into a server and back costs at most
  four register writes.
- **Another protected process** (`retenant`): the old tenant's view and its lent clients' are flipped out (bit 0
  cleared), the new one's flipped in (and its held clients'), the changed descriptors pushed (040/060 with the data
  cache on), then `PFLUSHA`. The cost is linear in the pages of the two views. `schedule()` holds every interrupt
  masked; the flips lower the mask to 4 so the serial port's receive interrupt (level 5, which only queues the byte)
  still runs: a big view takes longer than one character at 115,200 baud (87 us).

A process that ends is flipped out first (`mmu_forget`, from `proc_exit` before its memory is freed) and forgotten, so
no stale view is ever applied. A memory block allocated for the tenant (or a client lent to it) is valid at once
(`SYS_memalloc` in a running program); a block freed is invalid at once (section 6).

**Cost, estimated** (proposed, to be measured; FS-UAE's figures in section 15 are emulator figures): a protected
switch to the tenant or to a system process is two to four `MOVEC`s, well under a microsecond on a 50 MHz 68060.
A retenant of two small programs (a shell of about 100 KB and a command of about 60 KB: some 40 pages each way)
writes about 80 descriptors in some 20 cache lines, pushes them and flushes the ATC; the program then refills the ATC
with one table search (three reads) per page it touches. On a 50 MHz 68060 that is of the order of 2,000-4,000 clocks,
40-80 microseconds, once per switch between two different protected programs. That is not once per slice: a pipe
between two protected programs (`ls | less`, the shell and the command it waits for) retenants at every wake-up, and
a call to a protected server costs a retenant there, a lend, a retenant at the reply and one back: a pipe's round
trip of two switches some 100-150 us on the 060, on a 25 MHz 68030 (22 ATC entries) perhaps four times that. A switch
to or from a system process (every file, disk, network or display request) costs only the window registers. A program
with a large region pays for its pages at every retenant (a 4 MB region: 1,024 descriptors each way). Two later
steps, not built: flip whole 256 KB page tables at the pointer level, and keep the client flipped in when the next
process is a protected server holding its request.

## 10. A fault

A user access the tree refuses raises an access fault (vector 2) in user mode; `exc_c` (main.c) already kills the
faulting process for any user-mode exception. With protection on it first says what happened, from the frame:

- 68030, format $A or $B: the data cycle's fault address (+$10) when the SSW's DF bit says a data fault, its RW bit
  for read or write (MC68030UM 8.2.x); otherwise an instruction fetch at the PC.
- 68040, format $7: the fault address (+$14), the SSW (+$0C) RW bit (MC68040UM 8.4.6).
- 68060, format $4: the fault address (+$08), the FSLW (+$0C): RW in bits 24-23, IO in bit 15 (MC68060UM 8.4.4).

The line: `[kernel] pid 12 (crash): memory protection: a write to 0x1000 (the kernel's memory): killed (SIGSEGV)`,
the owner found by the memory manager (`mem_owner_of`: the kernel's, `pid 7 (sh)`'s, shared code, free memory, or
"no memory (the hardware, or nothing)" for the chips and unmapped space). Then the usual line and `proc_exit(139)`. The frame is never returned
from, so the 040's pending write-backs and the 030's continuation state are simply dropped with the process. A fault
in supervisor mode stays a panic (it would mean the kernel touched an address outside its windows, which section 3's
refusal rules out).

## 11. ZINE, reset, Emu68

- **ZINE** (D29) hands the machine to an AmigaOS program that expects no MMU: in `zine.c`'s `go()`, with interrupts
  masked, before `zine_enter`, `mmu_hand(0)` switches translation off (`TC`/`TCR` E = 0; on 040/060 the TTRs back to
  D37's values; on the 030 `TT0` off), and flushes the ATC; the blob's own save and restore of the 040/060 MMU
  registers (`zb_mmu_save`, `zb_mmu_back`) therefore see and restore that state; after the resume `mmu_hand(1)` puts
  the root pointer, the TTRs and `TC` back and flushes the ATC. The tables are kernel blocks: ZINE's plan keeps
  every Zenix block out of the program's reach as before.
- **A software reset** (`machine_reset` through `cache_before_reset`, which calls `mmu_before_reset` first on every
  CPU, whatever D37 did): translation off (the 030's `TC` by `PMOVE`), the windows back to the clean state, then D37's
  `cache_reset_state` clears the cache registers and the TTRs, before `RESET`, as D37 does for the cache registers (a `RESET` instruction leaves the CPU's own registers as they are,
  MC68060UM 8; the ROM must start without Zenix's translation). A hardware reset clears `E` by itself.
- **Emu68**: never switched on (section 3).

## 12. Boot

After `cache_setup` (D37), with interrupts masked, before any process exists: `mmu_setup(bi)`:

0. Not at all: an option says off (section 13), the left mouse button is held, the CPU is not in section 3's table,
   Emu68, no hardware table, a board outside the windows. The log says which.
1. The tables (section 5) from the pools; all pages invalid.
2. `mmu_probe` (section 3), with the TTRs in protected mode.
3. If it passed: translation on, the TTRs in system mode, `mmu_on` = 1, and from then on `MEM_USER` blocks are page
   aligned. If not: everything put back, the tables freed, `MEM_USER` means nothing.
4. The log lines (the kernel prints them; `%u` KB rounded up):
   `mmu: memory protection on: 68060, 4 KB pages, 68 KB of tables`
   `mmu: memory protection off: MMU=OFF` (or `not asked for (the kernel file's default)`, `mouse button held`,
   `no MMU on a 68020`, `Emu68`, `no hardware table`, `an LC or EC CPU (MMU=ON tries it)`,
   `no MMU (the probe found none: N)`, `no MMU (a register is missing)`, `a board above 16 MB at ADDR`,
   `out of memory for the tables`)

## 13. The switch, and the default

- **bootinfo `+0x84` `kopts`** grows two bits (frozen, claimed): `KOPT_MMU_ON` 4 and `KOPT_MMU_OFF` 8; both set, OFF
  wins. zenixboot's
  new keyword `MMU=ON|OFF` sets them (an unknown word is an error before anything is loaded, as `DCACHE=`). Neither
  bit (every older loader, the floppy's bootblock): the kernel's default.
- **The kernel's default** was off until 2026-11-17 (protection had never run on a real 68030/040/060); since then
  it is `KOPT_MMU_AUTO` (16): on where the CPU is known to have an MMU (section 19), the partner's order of
  2026-11-17 (docs/decisions.md), tested in two emulators on every CPU model they have before the 1260 has seen it.
- **The kernel's default options**, `kopt_default`, are a long in the kernel image after the eight bytes
  `ZKOPTDEF`; `tools/kopts.py FILE MMU=ON|OFF|AUTO` sets it in a kernel file (once, found by the marker): 4 on wherever
  the probe passes (LC and EC parts tried), 16 the default, 0 off. The tests use it to boot the emulators from the
  floppy with protection off or forced on; it is also how a floppy, which has no options, can be made to start
  without it. The bits in `kopts` win over the default; the mouse button wins over both (and also turns D37's data
  cache off: a boot with the cache on and protection off needs `MMU=OFF`). `kopts.py` works on an
  unpacked kernel file only (not the demo floppy's packed one), and a kernel changed so is not one a signed update
  carries: test builds and hand-made floppies only.

## 14. Tests

- **`tools/mmu-test.sh`** (FS-UAE; the matrix's 68030 with `uae_mmu_model = 68030`, its 68060 and 68040, which
  already have FS-UAE's MMU model for D37): a build copy whose kernel says `MMU=ON` (`tools/kopts.py`), then over the
  serial port: the boot line; `crash victim` (a region filled with a pattern, its address said) in the background;
  `crash write` and `crash read` of the victim's region, `crash write 0x1000` (the bootinfo), `crash read 0`, a write
  into the kernel's image (its address from the boot log), `/tests/crashpure poke` (its own shared code); each must
  end with the protection line naming the right owner and status 139, no stray access may get through, the victim
  must find its pattern intact at the end, and the system goes on. A protected server (`msgpingd` from the shell)
  answers 200 calls of `msgping` and upper-cases the client's buffer in place (the lent view). `free` and `bench`
  give the figures; `MMU_TEST_OFF=1` runs the same with protection off, for the comparison.
- **`tools/fsuae_conf.py`** takes `ZENIX_FSUAE_SET` (`key=value;...`) to change a config's keys for one run, so an
  existing test runs on a 68030/68060 with the MMU model: `ZENIX_BUILD=<an MMU=ON copy>
  ZENIX_FSUAE_SET='uae_cpu_model=68060;uae_mmu_model=68060;uae_mbresmem_size=64' tools/cmds-test.sh`. The regular
  suites run that way (section 15 says which, and what they found).
- **Run with protection off only**, and why: `prio-test` (priotest's children share the parent's block by its
  address, section 2), `pure-test` (it expects the D13 checksum to find a write into shared code, which protection
  now stops).
- **The matrix** (protection off by default) must be unchanged on every machine, the 68020 ones above all.
- **Not tested in FS-UAE** (left for the 1260 or later): the probe refusing a part (FS-UAE without its MMU model
  answers the first read with the test page's own word: that refusal is the 030 run without `uae_mmu_model`), a
  reboot with protection on, the serial port during a big retenant, an instruction fetch fault, the data cache's
  pushes (FS-UAE has no 040/060 data cache, D37 section 8).

## 15. As built (2026-11-12, branch mmu)

- **Code**: `kernel/mmu.c` (5,510 bytes of code and strings, 124 of BSS; the flips and the switch at -O2, the rest
  -Os), hooks in `mem.c` (the three-way cut and `mem_owner_of`: +690 bytes), `proc.c`, `ipc.c`, `svc` marking in
  `proc_load`, `drv.c`, `gcon.c`, `vfs.c`, `zine.c`, `cache.c` (`cache_block`), `main.c`. **kernel.bin 183,192 ->
  190,012 bytes (+6,820)**; the mount tests' RAM disk 699 -> 692 KB for it (`tools/ksize-budget`, the Makefile).
  zenixboot `MMU=` +160 bytes. Machines without protection run almost none of it: every hook is an inline gate in
  `mmu.h` that calls nothing while `mmu_on` is 0, and since DESIGN D45 revision 3 (docs/d45-boot-memory.md section 13)
  the runtime code lies in a span given back after the boot (calls go through the table `mmu_ops`); what runs on every boot is
  `mmu_setup`'s head (and on a 68030 the switch-off of a translation AmigaOS left on), the system-process mask
  (`mmu_sysmark`, `mmu_slot_new`) and the allocator's `alloc_in`/`carve` split (the same cuts as before; audit
  2026-11-12 A6-16). A 68040/060 not asked for protection has no MMU register read (audit A6-11).
- **Tables** (the boot line): FS-UAE's A1200 with a 68030, 2 MB chip and 8 MB Zorro II fast: 12 KB; with 4 MB fast: 8
  KB; the 68060 with 2 MB chip, 8 MB Zorro II and 64 MB at 0x08000000: 77 KB (73 KB with 4 MB Zorro II).
- **Memory after boot and one login** (`free`, FS-UAE): 68030 with 8 MB fast 7,553 -> 7,529 KB (24 KB: 12 KB of
  tables, 12 KB of rounding); 68060 with 72 MB fast 73,089 -> 73,000 KB (89 KB: 77 of tables); chip RAM unchanged.
- **Emulator figures** (FS-UAE 3.2.35 in warp mode, its MMU model on, `bench` and `msgping`, protection off -> on;
  only the ratios mean anything, and FS-UAE's MMU emulation is itself slow, so these are no estimate of a real CPU):

  | | 68030 | 68060 |
  |---|---|---|
  | a null system call (no switch) | 35 -> 37 us | 17 -> 17 us |
  | `stat` through the filesystem server (two switches to a system process) | 4,036 -> 4,217 us (+4%) | 2,374 -> 2,458 us (+4%) |
  | a call to a protected server and back (two retenants and a lend) | 554 -> 2,154 us (rebuilding the set at every reply) | 239 -> 1,229 us then, 938 us with the reply leaving the set alone (section 8) |

  So a switch to and from a system process costs little (the registers), a switch between two protected programs a
  few hundred emulated microseconds here; on a real 060 it is estimated in section 9.
- **What the tests found at once**: `sed`'s addresses (`/re/p`, `/re/c`) called the regex matcher with no captures
  array, and every match wrote the captures to address 0 (the vector table) without anyone noticing; with protection
  on `sed` was killed there ("a write to 0x0 (the kernel's memory)"). Fixed in `user/regex.c` (commit of the same day).
- **Verified in FS-UAE** (all with protection on, FS-UAE's MMU model, warp mode): `tools/mmu-test.sh` on the 68030,
  the 68040 and the 68060 (every check of section 14); with `ZENIX_FSUAE_SET` on a 68030 (A1200, 4 MB fast) and a 68060 (A1200, 4
  MB Zorro II and 64 MB 32-bit fast): `cmds-test` (after the sed fix), `users-test`, `memtest` (45 of 45),
  `signal-test`, `mounttest` (its D26 links part failed once, before the sed fix: sed is in that script),
  `dpy-test`. `zine-emu-test`: all 70 hand-overs came back with protection switched off and on again around each,
  on both CPUs; its failures on the 68060 machine (zbig, zmmu, zfpu) are the same with protection off (the test is
  made for its 68020 and its own 040/060 configs, which `ZENIX_FSUAE_SET` overrides). `zdesk-test` and `sound-test`
  fail the same with and without protection on these machines (they expect a 68020's speed: "the outline drag on a
  68020"; a WAV that ends before the Ctrl-C). `svc-test` and the other tests that need `zenix-zroot.hdf` could not
  run: that image is too small for the programs (`zenixfs-tool: ... error -28 (no space)`), on main too (found here,
  told to the main session). The matrix (protection off by default): all 14 machines pass, the 68020 ones too.
- **Not done**: a reboot with protection on; the serial port at 115,200 during a big
  retenant; the 1260 (section 18). The two later steps of section 9.

## 16. Interfaces

New and frozen: `KOPT_MMU_ON` 4 and `KOPT_MMU_OFF` 8 in bootinfo's `kopts`; zenixboot's `MMU=` keyword; the kernel
file's `ZKOPTDEF` marker and the long after it; the log line's start `mmu: memory protection on:` (the tests read it).
Internal, not frozen: `MEM_USER` (a kernel flag), the per-slot system mask, the descriptor layout. No system call,
message, struct or file format changes; `user_ok_*` keep their meaning.

## 17. Reviews

Two independent reviews of revision 1 (2026-11-12), each a fresh read of the note against DESIGN.md, architecture.md,
frozen-interfaces.md and the code; their findings and what was done:

**Review A** (the MMU, the ownership model, the reset and the interfaces):
1. No ATC flush outside a retenant: the rule in section 5; every change flushes (lend, unlend, a block made or freed).
2. A software reset left translation on for a 030 and a 040/060 with D37 off: `mmu_before_reset` is called on every
   CPU (section 11).
3. The loader's MMU state (Enforcer, FASTROM on a 030): switched off at every 030 boot; "off" is the clean state
   (section 4).
4. Tables in copyback space: the push rule and the order of DE and E written down, with both manuals' passages
   (section 5).
5. A page's cache mode must be its window's: the rule (section 5); Zorro II and trapdoor fast RAM inhibited.
6. The EC parts and the probe's instructions: the risk written, only under `MMU=ON` (section 3).
7. The register values: written out (section 4), `SRP` = `URP`, 030 `TT1` and `ITT1` 0, the 060's `TCR`.
8. D37's windows when D37 is off: `cache_block` gives them in every case (section 4).
9. A mount on a port that is not reserved: rule 5 (section 7).
10. `CON_GRAB`: rule 4 (section 7).
11. Views that share code: a client leaves only at a whole retenant (everything out, then in), never by its own
    flip-out (section 8).
12. The cost per wake-up and per call, not per slice: section 9; the two later steps named.
13. The memory cost of the rounding: estimated in section 6, measured in section 15.
14. Forwarding between protected servers: not supported, written (sections 2, 8).
15. The free hook belongs in `free_blk` (it is there: `mem_free_owner` and `upd_release` go through it).
16. Marking mid-run takes effect at once; two kopts bits: OFF wins (sections 7, 13).
17. The fault line: the 030's instruction fault uses the PC (the stage B address is close to it, not exact);
    memory outside the pools is named "the kernel's memory" when it is RAM, else "no memory".
18. "On purpose" said too much for root: section 2.
19. Code in chip RAM loses the instruction cache: section 5.
20. Claims and documents: claims.md, frozen-interfaces.md, DESIGN, limits.md, the boot banner (section 16).
21. A host test of the layout: not built (the emulator runs check every allocation path on three CPUs); a first-run
    plan for the 1260: section 18.
22. Tables in fast RAM: they are (a kernel block, fast first); the 1260's figure uses its real address.

**Review B** (the fit with the rest of the system, what breaks):
1. zup reads the update bundle in user mode: read only for root while it is there (section 6).
2. A process made system mid-run: at once (section 7), as review A 16.
3. The retenant with every interrupt masked loses serial input: the mask lowered to 4 during the flips (section 9).
4. priotest shares memory by address: runs with protection off; the limit written (sections 2, 14).
5. pure-test's claim: corrected (section 6).
6. There is no `pingd`: the protected server of the test is `msgpingd` from the shell (section 14).
7. Kernel messages to a protected server through a mount: rule 5, as review A 9.
8. The rounding's cost: section 6, measured in section 15; a service's server is not rounded at all.
9. AmigaOS's 030 MMU, `TT1`/`ITT1`: as review A 3 and 7.
10. `SRP`: as review A 7.
11. Test gaps: section 14 lists the runs and what stays untested.
12. The lists of system processes: corrected (section 7).
13. "On purpose": as review A 18, with the lending door named.
14. Pointer chains between servers: as review A 14.
15. The allocation wording: corrected (section 6).
16. The fault line naming a kernel stack or an environment block as a process's memory: such a fault is named
    "the memory of pid N" (true: the block is the pid's), not split further; noted.
17. `kopts.py` and signed or packed kernels: section 13.
18. The documents and strings to change: section 16.

## 18. The first run on the Apollo 1260

0. Since revision 3 protection is on by default (section 19), so before the new kernel goes on the card, add a way
   back: a line in `S:BootMenu.conf`, `Zenix (unprotected) = run SYS:zenixboot MMU=OFF` (this zenixboot on the card,
   copied by hand, zenixboot(8)); holding the left mouse button while Zenix starts does the same.
1. Boot the usual line. `dmesg | grep -e mmu -e cache` must show `mmu: memory protection on: 68060, 4 KB pages, ...
   KB of tables` and D37's two cache lines as before. (If the 1260's CPU is a 68LC060 the line says `an LC or EC CPU
   (MMU=ON tries it)`: then a line with `MMU=ON` tries it.) A photo of the screen if it stops at boot: then the
   unprotected line boots as before.
2. `crash write 0x1000; echo $?` must print the protection line and 139; `crash victim 30 &`, then
   `crash write ADDR` with the address it said: the line names the victim, and 30 s later the victim says "intact".
3. Daily things with it on: a login on two consoles, `ls -laR / > /dev/null` timed three times (compare with step 1 of
   D37 section 9: the cost of the switches), the desktop (`zdesk`), a `wget` over the FA411, `play` and `modplay`,
   `zine` with a demo and back.
4. `free` with and without `MMU=OFF` after one login: the memory the page rounding takes.
5. Anything odd: the unprotected line; tell what happened. If it only happens with protection on, the photo and the
   command are what is needed.

## 19. As built 2 (2026-11-17, branch mmudef): on by default, every CPU model in two emulators

The partner, 2026-11-17 (docs/decisions.md): "The one thing that I would like us to do first is real MMU memory
protection on by default for computers that has an MMU ... test it using a good emulator and for all the different
CPU models." Done in the emulators; the 1260's first run (section 18) is still to come.

### 19.1 The default, and the parts nothing safe tells apart

- **On by default where the CPU is known to have an MMU** (`KOPT_MMU_AUTO` 16 in the kernel file's default long,
  section 13): a **68030** (its MMU goes through the probe of section 3, which is safe on a 68EC030: a missing
  register traps, or the probe finds no translation), a **68040 with an FPU** (an `FRESTORE` of a null frame runs; it
  also leaves the FPU in its reset state), a **68060 whose `PCR` says 0x0430**.
- **An LC or EC 68040/68060 is left alone**, no MMU register touched, the boot line saying `mmu: memory protection off:
  an LC or EC CPU (MMU=ON tries it)`. The reason: a 68LC040 and a 68EC040 both lack the FPU and the 040 has no
  identity register; a 68LC060 and a 68EC060 both answer 0x0431 in `PCR` (MC68060UM appendices A and B). Only the MMU
  itself tells them apart, and its instructions are undefined on the EC parts (MC68060UM 1.1.2.2: `URP`, `SRP` and
  `TCR`'s E "must not be used", "no exceptions are reported"; MC68040UM B.1: `PFLUSH`/`PTEST` an "indeterminate" number
  of bus cycles). Section 3's promise (no EC part given an MMU instruction unless asked) is kept. `zenixboot MMU=ON`
  (or `tools/kopts.py KERNEL MMU=ON`) tries an LC or EC part with the probe as before. A real 68LC060 (common on
  Amiga accelerators) therefore needs `MMU=ON`; the partner's 1260 has a full 68060 (rev 6) and gets it by default.
- **Off**: `zenixboot MMU=OFF`, the left mouse button held at boot, `tools/kopts.py KERNEL MMU=OFF` for a floppy.
  No MMU (a 68020, an EC part, Emu68, no hardware table) is one plain line in the boot log, never an error.
- **The hand-over (audit 2026-11-12 A6-14, fixed):** zenixboot now switches a 68030's leftover translation off too
  (section 4), each of `TT0`, `TT1`, `TC` on its own so a trap on one leaves the others done, then both of the
  030's caches cleared; the 040/060 path lost its `PFLUSHA` (with E clear the ATC is unused; an EC040 must not be
  given one).
- **Costs in the kernel**: 130 bytes (`mmu` 5,592 -> 5,722; the mount tests' RAM disk 677 -> 676 KB). One
  independent review of the kernel and hand-over changes (its six findings: the 030 steps' order and caches, the
  `PFLUSHA`, `FRESTORE` instead of `FNOP`, the old `kopts.py`'s meaning written in frozen-interfaces.md; fixed).

### 19.2 What each emulator really has

FS-UAE 3.2.35: the 68030, 68040 and 68060 MMU models and a CPU without an FPU (`uae_fpu_model = 0`: line F on FPU
instructions, the 060's `PCR` 0x0431), so the LC parts; **no EC part** (`uae_mmu_model = 68ec030`, `68ec040`,
`68ec060` crash FS-UAE at start). Amiberry 8.5.0 has all nine (`mmu_model=68ec0x0`: the registers are taken and
nothing is translated, so the probe reads the test page's own word). Amiberry runs headless through
`ZENIX_EMU=amiberry` (`tools/run-test.sh`: the FS-UAE config turned into Amiberry's by `tools/fsuae_conf.py`, its
serial port on TCP joined to the tap's PTY by `tools/serial-tcp.py`), always at the machine's own speed: with
`cpu_speed=max` its IDE stopped answering once, as FS-UAE's does in its fastest modes. Knowledge base:
`amiga/emulator-cpu-models-and-amiberry-serial.md`.

### 19.3 The matrix

`tools/mmu-test.sh MODEL` (section 14's checks where protection is on: six stray accesses killed with the right owner
named, the victim intact, a protected server editing its client's buffer, 200 calls; where it is off: the reason in
the boot line and the system going on). `+on`: the kernel made `MMU=ON`. All emulator runs on an A1200 with Kickstart
3.x, 2 MB chip RAM; the 030 with 8 MB Zorro II fast, the 040/060 with 8 MB Zorro II and 64 MB 32-bit fast.

| CPU | FS-UAE 3.2.35 | Amiberry 8.5.0 |
|---|---|---|
| 68020 | off, "no MMU on a 68020": pass | the same: pass |
| 68030 | **on** (14 KB of tables), every check: pass | **on** (15 KB), every check: pass |
| 68EC030 | not modelled (crashes) | off, "no MMU (the probe found none: 0)"; the same with `MMU=ON`: pass |
| 68040 | **on** (79 KB), every check: pass | **on** (80 KB), every check: pass |
| 68LC040 | off, "an LC or EC CPU"; `+on`: **on**, every check: pass | the same: pass |
| 68EC040 | not modelled | off, "an LC or EC CPU", no MMU register touched; `+on`: the probe finds none, off: pass |
| 68060 | **on** (79 KB), every check: pass | **on** (80 KB), every check: pass |
| 68LC060 | off, "an LC or EC CPU"; `+on`: **on**, every check: pass | the same: pass |
| 68EC060 | not modelled | off, "an LC or EC CPU", no MMU register touched; `+on`: the probe finds none, off: pass |

The regular suites with protection on (`tools/mmu-suite.sh CPU`: each test unchanged, its machine changed by
`ZENIX_FSUAE_SET` to the CPU with its MMU and FPU; each test on a fresh copy of the build):

| Test | FS-UAE 030 | FS-UAE 040 | FS-UAE 060 | Amiberry 030 | Amiberry 040 | Amiberry 060 |
|---|---|---|---|---|---|---|
| cmds-test | pass | pass | pass | pass (1) | pass | pass |
| memtest | pass | pass | pass | pass | pass | pass |
| mounttest | pass (2) | pass | pass | pass | pass | pass |
| svc-test (all parts but f) | pass | pass but part t (3) | pass | pass but one check (4) | the same (4) | the same (4) |
| svc-test f, sound-test (stock speed) | - | - | - | pass | pass | pass |
| lua-test | pass | pass | pass | pass | pass | pass |
| tcc-test | pass (5) | pass | pass | pass | pass | pass |
| zdb-test | pass | pass (6) | pass | pass | pass | pass |
| dpy-test, zdesk-test, files-test (FS-UAE's cage rig, stock speed) | not comparable (8) | | not comparable (8) | | | |
| zine-emu-test (its own 040 and 060 machines, protection on, still on after every session) | | pass | pass | | protection: pass (7) | protection: pass (7) |
| matrix (the 030, 040, 060 machines boot protected), bootmem-test | pass | pass | pass | | | |

(1) Failed once because two cmds-tests ran at once on the PC and shared the PC's `/tmp/cmds.t` for their expected
output: fixed in the test (a lock). (2) D26 9's two programs ended before the next line in warp mode, protection off
the same: the test's fault, fixed (they run until killed). (3) The tap's greeting byte lands in the first line typed
after the reboot (`sh: .echo: command not found`), protection off the same: the test's timing on that machine, not
fixed here. (4) "fsd is pid 3, at nice -10" finds fsd in state `sleep` where FS-UAE shows `msg`, protection off the
same: Amiberry's timing; every other check passes. (5) Its root image had no room for 101_cleanup's program
beside today's programs: the test's fault, fixed (a 24 MB image). (6) Once a serial interleave cut a section marker
(`==fend/`); passed when run again. (7) Protection on at boot and after every session; three other checks (a
56th 'back' line, zbig's volume, `zine -l` seeing 8,703 KB below 16 MB) fail the same with protection off: Amiberry's
memory layout, not this. (8) The desktop's programs ran with protection on and none was stopped, but the
screenshot checks fail on any CPU but the 68020 (the text console's geometry, then every placement), protection off
the same: these tests are made for the 68020 machine (they pass there). The desktop's protection is shown by the
runs, not by these checks.

In all these runs no program and no server was stopped by protection other than the tests' own deliberate stray
accesses: the default found no new bug (the first runs of 2026-11-12 had found sed's, section 15).

### 19.4 The speed and memory cost (emulator figures only)

`bench` and `msgping` in the same runs, protection off (the kernel made `MMU=OFF`) -> on. FS-UAE in warp mode, Amiberry
at the machine's own speed; both time with the E-clock. Neither emulates the 040/060 caches or the ATC's real cost,
so these are no estimate of a real CPU; the ratios show where the cost lies.

| | FS-UAE 030 | FS-UAE 040/060 | Amiberry 030 | Amiberry 040/060 |
|---|---|---|---|---|
| a null system call | 35 -> 35 us | 17 -> 17 us | 14 -> 14 us | 5 -> 5 us |
| `stat` through the filesystem server (a system process) | 5,001 -> 5,219 us (+4%) | 2,623 -> 2,716 us (+4%) | 1,235 -> 1,262 us (+2%) | 906 -> 919 us (+1%) |
| a call to a protected server and back (`msgping`: two retenants and a lend) | 559 -> 1,695 us (x3.0) | 239 -> 941 us (x3.9) | 183 -> 415 us (x2.3) | 67 -> 311 us (x4.6) |
| a message round trip between two protected programs (`bench`: two retenants) | 544 -> 2,265 us (x4.2) | 243 -> 1,397 us (x5.7) | 180 -> 555 us (x3.1) | 66 -> 477 us (x7.2) |
| fast RAM free after one login | 7,559 -> 7,528 KB | 73,094 -> 72,999 KB | 8,070 -> 8,039 KB | 73,606 -> 73,510 KB |

So a call to a system server (every file, disk, network and display request) costs a few per cent, and a switch
between two protected programs (a pipe, a protected server) costs several times what it did: the page flips and the
ATC refills of section 9. On a real 50 MHz 68060 section 9 estimates 40-80 us per retenant. The two later steps of
section 9 (flipping whole 256 KB page tables at the pointer level; keeping a client flipped in while a protected server
holds its request) are where that cost would be won back; not built (they need the partner's word, being speed work
on the golden path).

### 19.5 The tests

- `tools/mmu-test.sh`: every model above (`020 030 ec030 040 lc040 ec040 060 lc060 ec060`, each also `+on`); the EC
  models say SKIP on FS-UAE. `make check` runs `020 030 lc040` and `040 060 lc060` (two jobs).
- `tools/mmu-suite.sh 030|040|060`: the regular suites with protection on; `make check` runs it for each CPU.
- The matrix's 68030 machine has FS-UAE's MMU model now, so `matrix` boots the 030, 040 and 060 with protection on.
- `zine-emu-test`: its 040 and 060 machines boot with protection on; after their ZINE sessions a stray write is still
  stopped (`prot`).
- `bootmem-test`: protection off now means a kernel made `MMU=OFF`; on is the kernel as built.
- `zenixboot-test a1200-030mmu`: the A6-14 hand-over (a 68030 left with an identity translation and `TT1` on by
  `tools/tests/mmuleft.S`; with the old hand-over the kernel had to switch it off itself).
- Not in the emulators: a real EC part's answer to the MMU instructions (the reason for 19.1), the 040/060 caches'
  pushes, the real cost of a retenant: the 1260 (section 18).
