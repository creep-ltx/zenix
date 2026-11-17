# D57: ZINE's guest mode, AmigaOS window programs on the Zenix desktop (design study)

Status: **proposed, not approved to build.** A design study written 2026-11-17 for the partner's dream of that day:
"Installing and running an Amiga gui program on the desktop using zine is the dream ... Kinda how wine does it in
Linux." Nothing here is built and no number is claimed; every estimate is *proposed*. Facts about Zenix are *read* from
main (`6bc369f`); facts about AmigaOS come from the public autodocs and manuals (knowledge base `manuals/`), the NDK
3.2's interface files (names and numbers only), the knowledge base's AmigaOS 3.1 study notes (aos31-study/, ideas
only), and Wine's public design documents (ideas only). No AmigaOS source was read for this. Marks: *read*,
*verified*, *proposed*, as in D29.

## 1. In plain words

- **Today ZINE takes over the machine** (D29): Zenix pauses, the program gets the chips, the interrupts and nearly all
  memory, and Zenix comes back when it ends. That is right for demos and games, and it stays exactly as it is.
- **Guest mode is the second way**, the Wine way: the Amiga program runs **inside an ordinary Zenix process**, next to
  the shell, the file view and the other programs. Its windows are desktop windows that zdesk moves, raises and
  minimises like any other; its menus are in the desktop's bar; its files are Zenix files; it can be closed from the
  desktop. Nothing of Zenix pauses.
- **Simpler than Wine in one big way:** Wine has to stand in for Windows on the same CPU but a different OS; we are
  on the same CPU *and* the programs were written for an OS whose interfaces are documented function by function
  (the autodocs). No CPU emulation, no translation of code: the program's 68k code runs at full speed, and every
  library call is a `jsr` into our own code in the same process.
- **What it is not:** a way to run demos and hardware-banging games on the desktop (they keep take-over mode), or an
  AmigaOS inside Zenix (we never boot or ship AmigaOS code; the user's own copies of closed libraries, such as MUI,
  are run, never shipped).
- **The verdict of this study:** feasible, and much of it already exists in ZINE's library code. Roughly 140-200 of
  the helper's hours for steps (a) to (e) of section 9, about three small kernel changes, and one new displayd
  request. The first milestone (a hello-world Intuition window with text and a close gadget, on the desktop) is about
  25-30 hours including the groundwork.

## 2. What exists, and what others did (checked first)

### 2.1 In Zenix (*read*)

- **ZINE's small AmigaOS** (`user/zine/`, about 490 KB of source; the blob's code about 117 KB): exec (memory with
  MemHeaders and pools, lists, ports, signals, semaphores, tasks with switching since 16.16, libraries with
  `LoadSeg`/`InitResident` of a program's own disk libraries since 16.8), dos (a volume, assigns in `zpath.c`, names
  without case, packets, `Execute`, `CreateNewProc`), graphics (RastPort drawing by the CPU into a BitMap: `Text`,
  `RectFill`, `SetRast`, `Move`, pens and draw modes since 16.12; fonts and `diskfont.library` through the desktop's
  `amifont` reader since 16.13), intuition (screens in depth order, windows as structures, IDCMP from input since 16.17),
  gadtools (structures and messages, no drawing), keymap, console/input/keyboard/gameport/timer/audio devices,
  lowlevel, and the hunk loader with overlays. All of it runs **in supervisor mode with the machine taken over**; most
  of the library logic is plain C that does not care.
- **The desktop** (D38): displayd draws everything; programs send batches of drawing orders (`DPY_DRAW`), register
  planar images (`DPY_IMAGE`, depth 1-4, "the server keeps a copy"), get events in a ring (`EV_KEY`, `EV_BUTTON`,
  `EV_MOTION`, `EV_RESIZE`, `EV_EXPOSE`, `EV_FOCUS`, `EV_CLOSE`, `EV_MENU`); a window's menus go to zdesk's bar with
  `DPY_MENU` (a text of at most 512 bytes, "Title\tItem\tItem\n..."); window kinds `WK_NORMAL`, `WK_DIALOG`, `WK_BAR`,
  `WK_DESKTOP` (display/proto.h). Amiga bitmap fonts (amifont, D38 "Fonts"), TrueType drawn into Amiga bitmap fonts
  (D51).
- **Screens of a program's own** (D55, `DPY_SCREEN`): a mode, a palette and planes in the program's own chip RAM it
  draws into directly, a checked Copper list in Zenix's own form, front and back with Left Amiga+N, planes held by the
  kernel while shown (`CON_HOLD`). Exactly the shape of an AmigaOS custom screen.
- **Processes** (architecture.md 6, limits.md): one address space, `NPROC` 32, up to `NREG` 16 memory regions a
  process, **no threads**, the kernel never preempted, user processes preempted. Signals with handlers through one
  entry point (D16), `alarm`, `poll`/`evwait` on event bits. The kernel saves **no FPU state** (Zenix is
  `-msoft-float`; nothing in `kernel/` saves FP registers).
- **Memory protection** (D41, optional, 68030/040/060 with an MMU): identity mapped, one tree; a protected process
  sees only its own blocks; page 0 (address 4 included), the ROM and the custom chips are not its own; a fault ends
  only that process (status 139).
- **Archives** (D33): LhA and LZX unpacking; packages (D22, zpkg).

### 2.2 Elsewhere (searched 2026-11-17)

| Project | What it does | Licence | What we take |
|---|---|---|---|
| **vamos** (amitools, Christian Vogelgsang; [lallafa.de](https://lallafa.de/blog/amiga-projects/amitools/vamos), github.com/cnvogelg/amitools) | "the virtual AmigaOS runtime (aka Wine for Amiga)": runs AmigaOS **CLI** programs on Linux/macOS with a 68k emulator (Musashi), exec and dos replaced by Python, paths mapped to host folders, volumes and assigns configured per user | GPL-2.0 | The proof that exec + dos stand-ins carry real programs (compilers, LhA, xvs.library: knowledge base amiga/xvs-virus-scanning-on-linux.md, *verified* there); its config idea (volumes, assigns, a "prefix" in a config file). No GUI at all. Ideas only; we need no code (different language, emulated CPU). |
| **Janus-UAE** (sourceforge janus-uae, GPL-2.0) and AmiBridge | a UAE whose AmigaOS windows appear as host (AROS) windows ("coherency"/rootless mode) | GPL-2.0 | The look of the goal; but it runs the real Kickstart and Workbench in an emulated machine and patches Intuition from inside, the opposite of our approach. Lesson: per-window integration needs window-level hooks, which we have natively. |
| **Emumiga** (AROS) | runs 68k AmigaOS programs on x86 AROS by emulating the CPU and calling AROS's native libraries | not found | Same idea as ours on another CPU; confirms the library-level approach. |
| **AROS m68k** | an open AmigaOS 3.1 work-alike: "system friendly" 68k binaries run on it | AROS Public License | A reference for behaviour only. Not a base (the partner's view, and it is a whole OS, not a guest runtime). |
| **MorphOS ABox/Trance, AmigaOS 4 Petunia** | 68k programs inside a PPC AmigaOS, the 68k calls routed to native libraries | closed | Ideas: the "library jump table points at native code" trick is what we do too. |
| **Wine** (winehq.org design documents) | Windows programs on Linux | LGPL | Ideas, below. |

**Ideas taken from Wine's design** (*read*: its public architecture and configuration documents):
1. **The prefix**: a folder holding a whole pretend system (`drive_c`, the registry), so programs install and break
   things there and nowhere else. Here: section 5.2.
2. **Built-in versus native libraries** (Wine's "DLL overrides"): each library is either Wine's own or the real one
   from disk, chosen per library and per program. Here: section 6.
3. **Managed and unmanaged windows**, and the **virtual desktop** fallback (all of a program's windows inside one
   host window when it misbehaves with the window manager). Here: sections 4.1 and 4.7.
4. **wineserver**: one small process holding what Windows programs share (named objects, messages between
   programs). Here: section 3.5, later.
5. **Separate projects installed on demand** (wine-mono, wine-gecko): here OpenMUI as the MUI provider (section 6.3).

## 3. The model: a guest process

### 3.1 The shape

`zine -g PROGRAM` (and, from the desktop, double-clicking an Amiga program or its icon) starts **one Zenix process
per Amiga program**, the guest. In its own memory it holds:

- **The guest runtime**: ZINE's library code built a second time, for user mode, without the hardware parts (section
  3.3). A ZEX2 program with shared code (D13), so a second guest shares the code and pays only for its data.
- **Its own ExecBase and library bases**, with jump tables pointing into the runtime. Every library call is a `jsr
  -LVO(a6)` that lands in a small assembler stub (`blobs.S`'s are the model): it saves the program's `a5`, loads the
  runtime's own data base (Zenix's pure programs keep it in `a5`, D13) from a private field of the library base, calls
  the C function with the registers the autodoc names, and puts `a5` back. No trap, no system call: a library call
  costs what it costs on AmigaOS.
- **The program**, loaded by ZINE's hunk loader (overlays too) into memory from the runtime's allocator.
- **Its windows' canvases** (section 4.1) in chip RAM where there is room.

**What changes against take-over mode:**

| | Take-over (D29, today) | Guest (this note) |
|---|---|---|
| Zenix | paused, its state checksummed | runs on, untouched |
| CPU mode of the library code | supervisor | user |
| Who may run it | root and the `zine` group, at the console | any user with a desktop session (or a terminal for CLI programs) |
| The chips, interrupts, vectors | the program's | Zenix's; the program never touches them |
| `Forbid`/`Disable` | stop everything | stop only the guest's own task switching and its pretend interrupts (3.2) |
| Memory | nearly all chip, all free fast | what the guest asks Zenix for, like any process |
| Display | the program's own Copper lists | desktop windows (or a D55 screen) |
| Files | an in-memory copy of the program's folder, writes gone afterwards | real Zenix files through the prefix (5), writes kept |
| A crash | a cold start into Zenix, the disks safe | the guest ends; with D41 nothing else is touched (7) |

### 3.2 Tasks, signals and ports in one process

Zenix has no threads, and needs none here: **exec's tasks become the guest runtime's own coroutines inside the one
process**, exactly the bookkeeping 16.16 already built (the ready and waiting lists, `tc_SPReg`, `TS_*`, priorities,
the quantum), with the switch done in user mode (save the registers on the task's stack, load the next one's; the
supervisor trick of 16.16 is not needed, because nothing preempts the runtime's own code except a Zenix signal, which
it holds off while it changes the lists).

- **Waiting:** when every guest task waits (`Wait`, `WaitPort`, `WaitIO`, `Delay`, `WaitTOF`), the runtime calls
  `evwait` (D16) on what can wake one: the display's event bit (input for its windows), a timeout (the earliest
  `timer.device` request or `Delay`), the vertical blank (`CON_VBLANK`, D55) when a task waits in `WaitTOF` or a
  `VERTB` server is installed. So an idle Amiga program costs nothing, as on AmigaOS.
- **Preemption** between the guest's own tasks: at first only at those waits (cooperative), which is enough for
  programs with one task and for the common "main task plus a helper that waits" pattern. Later, if a program needs
  it, a D16 `alarm` every 4 frames (exec's quantum) whose handler switches tasks when the interrupted code is the
  program's (never the runtime's). Between the guest and other Zenix programs Zenix's own scheduler preempts as for
  any process.
- **Pretend interrupts:** `AddIntServer(INTB_VERTB)` servers and soft interrupts (`Cause`, `PA_SOFTINT` ports) are
  called by the runtime at its switch points and after each vertical blank it waited for, with "interrupt" state
  (`Disable` holds them off: it is a counter in the runtime, nothing more). Other levels (`AddIntServer` on PORTS,
  EXTER, the audio channels; `SetIntVector`) answer as on a system with the slot taken: the program's own error path,
  or the "wants the whole machine" message (3.4).
- **Signals and messages** are exec's own (AllocSignal, Signal, PutMsg, ReplyMsg), inside the process: no Zenix
  message per Amiga message.
- **Input is pumped at switch points**: the runtime reads the display's event ring when a task waits, and also in
  `GetMsg` on a window's port (programs that poll without waiting). A program that computes for seconds without asking
  gets its clicks when it next asks; zdesk still moves and raises its windows meanwhile (displayd does that, 4.1). This
  is the one visible difference from AmigaOS, where Intuition runs in input.device's task (aos31-study/
  intuition-and-console.md: "Intuition has no task", a state machine fed by input.device, *read*). The same state
  machine idea fits the runtime: one function fed events, called at switch points.

### 3.3 Reusing ZINE's code

The blob mixes three kinds of code: (1) **pure library logic** (lists, memory headers, tasks, dos names and assigns,
the hunk loader, RastPort drawing into a BitMap, fonts, gadtools structures, keymap); (2) **hardware** (Copper lists,
`LoadView`, the CIA resources, interrupts, the blitter, audio.device's DMA, input from the chips); (3) **the session**
(memory evacuation, checksums, the ways back). Guest mode links (1) with a new (2') of desktop calls and none of (3).
The split is mostly by file already (`blibs.c`, `bmore.c`, `btask.c`, `bgad.c`, `bfont.c` against `blob.c`,
`binput.c`, the `.S` files); where a function mixes, it calls a small `zhw_*` interface with two builds. **One source,
two builds**, so a fix in a library function helps demos and window programs alike. The cost is discipline: every new
take-over function must keep the line.

### 3.4 What breaks the model

- **Programs that touch the hardware** (`$DFF000`, the CIAs, `OwnBlitter` and then the blitter's registers, their own
  interrupt vectors, `LoadView` of their own View, `Supervisor` for anything but reading the VBR or the cache
  registers). With D41 on, the first touch faults and the guest ends with a line the runtime turns into "NAME touches
  the custom chips: run it with zine -t (the whole machine)". **Without an MMU** (the stock A1200, the PiStorm's Emu68)
  nothing can stop the touch: a poked colour register flickers the desktop, a poked `DMACON` can blank it. The runtime
  catches what goes through libraries (`OwnBlitter`, `LoadView`, `SetIntVector`, `AddICRVector`, `Disable` followed
  by a busy loop is invisible) and stops with the same message; the rest is the no-MMU limit as everywhere in Zenix
  (D8). A list of known take-over programs (`/etc/zine/takeover`, by name and size) can choose the mode before the
  first instruction.
- **Forbid-heavy code**: `Forbid` only stops the guest's own tasks, which is what the program means by it (no other
  Amiga task runs in between). A program that uses `Forbid` to protect a *system* list it walks (`SysBase->TaskReady`,
  the port list) sees the guest's own lists, consistent. A program that spins on a variable set by another program's
  task (a different guest) waits for ever: guests do not share memory (3.5).
- **Poking other tasks' memory or the system's structures:** inside the guest it works (it is all the guest's); across
  guests it does not, by design. Programs that patch the OS (`SetFunction` on intuition or dos to change every
  program's behaviour: commodities like "MagicMenu", Workbench enhancers) only change their own guest: harmless and
  useless. Monitors that list "all tasks" see their own guest.
- **Address 4.** Every Amiga program reads `ExecBase` with `move.l 4.w,a6`. Zenix has one address space, so address
  4 is shared by every process. **Kernel change K1** (section 8): a guest process carries the longword it wants at
  address 4, and the context switch writes it there when it switches *to* a guest (one compare and one store; a
  non-guest switch costs one compare). Without an MMU the program reads its own ExecBase; with D41 page 0 (`0`-`0xFFF`:
  the vectors' old place, unused since VBR moved, and the copier at `0x500`) becomes readable, read only, for guest
  processes (a write to it faults, as one should). The guest's ExecBase has a deliberately wrong `ChkBase`, so a reset
  during a guest's turn never believes it (D29 2.2: the ROM checks that sum) and Zenix's reset path is unchanged.
- **Privileged and missing instructions.** A user-mode `move sr,dx` (68010+), `movec`, `Supervisor()`, the 68040/060's
  missing FPU and integer instructions (ZINE's `zsp040.S`/`zsp060.S` stand in for them in supervisor mode today):
  in a Zenix process each one is a fault and the process is killed (D16: faults have no handlers). **Kernel change K2**:
  for a guest, the privilege violation, line-A, line-F, illegal-instruction and the FPU's unimplemented-instruction
  vectors are delivered to the guest's own handler (D16's one entry point, with the frame) instead of killing it; the
  runtime emulates `Supervisor()` for the few things programs do with it (read VBR: 0; cache registers: through
  Zenix's cache flush), `move sr` (the user byte and S clear), and reuses the 040/060 stand-in code in user mode.
- **The FPU.** Programs built for a 68881/2 (and the math libraries on an FPU machine) keep live values in FP
  registers across a Zenix context switch. One guest alone is safe (no Zenix program touches the FPU), two are not.
  **Kernel change K3**: processes marked as FPU users (every guest, on a machine with an FPU) have their FPU state
  saved and restored by the switch (`fsave`; if not a null frame, `fmovem` of the eight registers and the control
  registers; 16.16's per-task code shows the shape), lazily: only when the previous FPU user was another process.
- **Stacks:** the guest's tasks run on stacks in its own regions, not in the stack block Zenix gave the process. D16's
  signal delivery checks the stack pointer against the process's stack (512 bytes above the guard); for a guest it
  must accept any of the process's own regions (part of K2).

### 3.5 One process per program, or one for all?

**Proposed default: one process per program.** Programs sit side by side, a crash ends one (with D41), memory is
freed when it ends, and K1's address-4 switch makes each one's ExecBase its own. What it gives up: Amiga programs
talking to each other (an ARexx port another program finds with `FindPort`, a public screen another program opens
on, the clipboard through `clipboard.device`, AppWindows dropped on from Workbench). Those are later and come through
**a small broker** (Wine's wineserver idea): `zguestd`, a Zenix server that holds the names of public ports and
screens and passes messages between guests by copying them (an ARexx message is a small structure and strings), and
the clipboard as the desktop's own clipboard (D38 "later"). The alternative, all Amiga programs as tasks of one shared
guest (AmigaOS's own model: one ExecBase, one memory), makes sharing free but ties every Amiga program's fate to the
worst one, and is a question for the partner (11).

## 4. Graphics, layers and Intuition on top of displayd

### 4.1 A window is a desktop window with a canvas

- **`OpenWindow`/`OpenWindowTagList` on a public screen** (the Workbench screen, `LockPubScreen(NULL)`, or no screen
  named) opens a desktop window (`DPY_WIN_NEW`, `WK_NORMAL`; requesters and `WFLG_BORDERLESS` pop-ups `WK_DIALOG`) of
  the Amiga window's size, and gives the program a real `struct Window` with its `RPort`, `UserPort`, `BorderLeft/Top/
  Right/Bottom`, `MouseX/Y`, `IFont`.
- **The canvas:** the window's RastPort draws into a **planar BitMap of the window's own** (the desktop's depth: 4
  planes on AGA, 3 on OCS/ECS), allocated by the guest. All of graphics' drawing is done by the CPU into it (ZINE's
  `Text`, `RectFill` and friends are already that code), then the changed rectangles are shown. The canvas holds
  everything the window shows, so for the program **every window is effectively smart refresh**: raising, moving or
  uncovering it never asks the program to draw again (Intuition's smart refresh, which D38 already chose for the
  desktop). `WFLG_SIMPLE_REFRESH` windows simply get fewer `IDCMP_REFRESHWINDOW` messages: one after a resize (the new
  area) and none for being uncovered. `WFLG_SUPER_BITMAP` is the same thing with the program's own BitMap.
- **New displayd request, `DPY_CANVAS`** (D1-style: the client's memory read in place): "this window's inside is this
  planar bitmap in my chip RAM; these rectangles changed". displayd blits the changed rectangles onto the screen,
  clipped to what is visible, with the blitter, and for a window whose canvas is complete it need not keep hidden
  pieces of its own (saving the chip RAM D38 6 budgets). The planes are held by the kernel while shown, as D55's
  screens' planes are (`CON_HOLD`), so a guest that dies cannot free them under the blitter. On a machine short of
  chip RAM the canvas lives in fast RAM and displayd copies it by the CPU (the console's speed). The existing
  `DPY_IMAGE` ("the server keeps a copy") would cost a copy per change: not used for windows.
- **When to show:** the runtime collects changed rectangles and sends one `DPY_CANVAS` at each switch point (a wait, a
  `GetMsg`), and at most every frame while a program draws without waiting (from the vertical-blank pump), so a
  program that draws a hundred lines then waits costs one request.
- **Frames: the program's own Amiga border, by default.** Intuition programs place gadgets in and around their borders
  (`GZZ` windows, border gadgets, `BorderTop` from the screen's font) and expect the depth, zoom, drag and size
  gadgets where Intuition puts them. The runtime draws the classic frame into the canvas (the pens of 4.4), and the
  desktop window is undecorated (a new window kind, `WK_GUEST`: no frame of zdesk's, but zdesk still places it,
  minimises it and lists it in the bar). A press on the drag bar asks displayd to drag (`DPY_WM_DRAG`-like, through
  zdesk); the depth gadget raises or lowers through zdesk; the close gadget sends `IDCMP_CLOSEWINDOW` (the program
  decides, as `EV_CLOSE` already works). **The other choice**, zdesk's frame around a borderless Amiga window (Wine's
  "let the window manager decorate"), makes Amiga windows look like every other desktop window but breaks border
  gadgets; a per-program setting, and a question for the partner (11).

### 4.2 Layers inside a canvas

Because every window has its own canvas, **no other window ever overlaps it inside the canvas**: overlapping windows
are displayd's business. That shrinks layers.library to what one window needs: a layer for the window (two for a
`GZZ` window: the border and the inside), the program's clip regions (`InstallClipRegion`, used by listviews, MUI and
many 3.x programs), requesters' layers inside the window (`Request`), `BeginRefresh`/`EndRefresh` with the window's
damage, `LockLayer`/`LockLayerInfo` as counters (only the guest's tasks contend), `ScrollRaster` and `ClipBlit` inside
the canvas. Real overlapping layers are needed only for custom screens (4.5).

### 4.3 IDCMP from the desktop's events

| displayd event | Intuition message |
|---|---|
| `EV_KEY` | `IDCMP_RAWKEY` (the raw code and qualifiers), or `IDCMP_VANILLAKEY` for a key-down that gives one character (the desktop's keymap, so the Swedish keys come out right); ZINE's 16.17 code makes both, with the previous keys in `im_IAddress` for dead keys |
| `EV_BUTTON` | `IDCMP_MOUSEBUTTONS` (`SELECTDOWN`/`UP`, `MENUDOWN`/`UP` with `WFLG_RMBTRAP`), or the runtime's own gadget handling: `IDCMP_GADGETDOWN`/`GADGETUP`, the system gadgets |
| `EV_MOTION` | `IDCMP_MOUSEMOVE` with `WFLG_REPORTMOUSE` (the window asks displayd for motion only then); at most 5 unreplied, as Intuition (aos31-study, *read*) |
| `EV_RESIZE` | `IDCMP_NEWSIZE` (and `IDCMP_CHANGEWINDOW`), the canvas reallocated with its old content kept, `IDCMP_REFRESHWINDOW` for the new part |
| `EV_FOCUS` | `IDCMP_ACTIVEWINDOW`/`INACTIVEWINDOW`, `WFLG_WINDOWACTIVE`, the frame redrawn active or inactive |
| `EV_CLOSE` (zdesk's close) | `IDCMP_CLOSEWINDOW` |
| `EV_MENU` | `IDCMP_MENUPICK` with the `FULLMENUNUM` of the item |
| the runtime's 10 Hz tick | `IDCMP_INTUITICKS` (only after the last one was replied) |

`keymap.library`'s `MapRawKey`/`MapANSI` need an Amiga `KeyMap` for the desktop's layout: the runtime builds it from
Zenix's own `sv`/`us` tables (we write them; AmigaOS's keymap files are not ours to ship).

### 4.4 Screens, pens and fonts

- **The public screen is the desktop.** `LockPubScreen(NULL)`/`"Workbench"` gives a `struct Screen` of the desktop's
  size, depth and font (the desktop's UI font through amifont; Topaz 8 the fallback), with a `DrawInfo` whose pens
  (`DETAILPEN`, `BLOCKPEN`, `TEXTPEN`, `SHINEPEN`, `SHADOWPEN`, `FILLPEN`, `FILLTEXTPEN`, `BACKGROUNDPEN`,
  `HIGHLIGHTTEXTPEN`, the bar pens) are the desktop look's colour numbers. Its own `RastPort` draws into nothing (an
  empty clip): programs that draw straight onto the Workbench screen lose that drawing, harmlessly.
- **Colours:** the desktop has 16 colours on AGA and 8 on OCS/ECS, set by the look. `ObtainBestPen`/`FindColor` give
  the nearest of them; `ObtainPen` with `PEN_EXCLUSIVE` and `SetRGB4`/`LoadRGB32` on the Workbench's ViewPort are
  refused or ignored, as on a full Workbench screen, and programs fall back. Old programs that assume pens 0-3 are the
  Workbench 2.x grey, black, white and blue look right only if the look's first four colours are close to those: a
  question for the partner (11).
- **Fonts:** `OpenFont`/`OpenDiskFont` read `FONTS:` (the prefix's fonts plus the desktop's font folder, a
  multi-directory assign), with ZINE's `bfont.c` and the desktop's amifont reader; D51's TrueType fonts are Amiga bitmap
  fonts too, so they come free. Text is drawn by the guest into its canvas, so the guest's fonts never need displayd.

### 4.5 Custom screens

`OpenScreen` with the program's own mode and palette becomes a **D55 screen** (`DPY_SCREEN`): planes in the guest's
chip RAM, `SetRGB*`/`LoadRGB*` as `SS_PALETTE`, double buffering (`ChangeScreenBuffer`, `ChangeVPBitMap`) as
`SS_PLANES`, Left Amiga+N and the program's `ScreenToFront`/`Back` as `SS_FRONT`, keys and the mouse as `EV_*` with
`win` 0. On a custom screen there is no desktop: the runtime's Intuition draws the screen's title bar, its windows,
their frames and the menus itself into the screen's planes, and windows on it **overlap**, so this needs real layers
(smart refresh's hidden pieces, simple refresh's damage). That is the larger half of layers.library, and it comes
after the public-screen steps (9, step f). Copper effects through `UCopList` map onto D55's checked instructions
where they are waits, colours and plane pointers.

### 4.6 Gadgets, menus, requesters

- **Intuition's own gadgets** (boolean, string with editing and `IDCMP` on Return, proportional with the knob), the
  system gadgets, `RefreshGList`, `AddGList`, `ActivateGadget`, drawn into the canvas in the classic look from the
  `DrawInfo` pens; hit-tested by the runtime from button events.
- **GadTools** (ZINE has the structures; guest mode adds the drawing and the behaviour): `BUTTON`, `CHECKBOX`,
  `INTEGER`, `LISTVIEW`, `MX`, `CYCLE`, `NUMBER`, `PALETTE`, `SCROLLER`, `SLIDER`, `STRING`, `TEXT`; `GT_SetGadgetAttrs`,
  `GT_GetGadgetAttrs`, `DrawBevelBox`, `GT_RefreshWindow`, `GT_BeginRefresh`.
- **Menus: in the desktop's bar.** Amiga menus live in the screen's title bar and open with the right button, which is
  where zdesk's bar shows a window's menus (D38 revision 2). `SetMenuStrip` sends the menu to zdesk; a pick comes back
  as `EV_MENU`. `DPY_MENU`'s text form today has no sub-items, check marks, mutual exclusion, disabled items or
  Amiga-key shortcuts and is capped at 512 bytes, so it grows (a new request with those flags, **displayd/zdesk change
  D2**). The Amiga-key shortcuts are handled by the runtime from `EV_KEY` (the right Amiga qualifier) without zdesk.
  The fallback, menus drawn by the runtime in a pop-up `WK_DIALOG` window with the pointer grabbed, is what custom
  screens use anyway (4.5).
- **Requesters:** `EasyRequest`, `AutoRequest`, `BuildEasyRequest`/`SysReqHandler` as small `WK_DIALOG` windows drawn by
  the runtime; `Request` (a requester inside a window) in the window's canvas; `DisplayAlert` as a dialog.
- **ASL requesters** (`asl.library`, built in): the file requester as a guest dialog in the familiar ASL layout,
  listing the prefix's volumes and assigns and Zenix folders through them (5.1), with patterns (`#?.txt`, dos's
  `MatchPattern`); the font requester over `FONTS:`; the screen-mode requester over the D55 modes. A later choice is to
  show the desktop's own file dialog instead (consistency against the Amiga look): a question (11).

### 4.7 The fallback: a virtual Workbench window

For a program that does not behave as separate windows (it draws on the Workbench screen, opens windows that overlap
on purpose, uses a backdrop window as its whole interface), the per-program setting `screen = window` puts the whole
pretend Workbench screen into **one desktop window** of 640 x 256 (Wine's "virtual desktop"): the runtime's Intuition
manages the windows inside it with the custom-screen layers of 4.5, drawn into one canvas. It costs that code path
only, which custom screens need anyway.

## 5. dos.library and files

### 5.1 Amiga paths on Zenix folders

Guest mode reads and writes **real Zenix files** with the user's own permissions (no in-memory volume, no lost
writes). Each Amiga name maps to a Zenix folder:

| Amiga name | Zenix folder (default) |
|---|---|
| `SYS:` (and `C:` `S:` `L:` `LIBS:` `DEVS:` `FONTS:` `LOCALE:` `ENVARC:` `Prefs` as its folders) | the prefix's `SYS` folder (5.2) |
| `PROGDIR:` | the program's own folder |
| `Work:` | the prefix's `Work` folder (where programs are installed) |
| `Home:` | the user's home folder |
| `ENV:` | the prefix's `ENV` folder, filled from `ENVARC:` at the guest's start (no RAM disk needed) |
| `RAM:`, `T:` | a per-user folder under `/tmp` |
| `Zenix:` | `/`, so any Zenix file has an Amiga path (`Zenix:home/ann/doc.txt`) |
| a mounted Amiga disk (D31 FFS, D32 PFS3) | its own volume name, so `Workbench3.2:` or `DH1:` paths in a program's settings still work |

ZINE's `zpath.c` (assigns with up to three folders each, followed through each other) is the base; guest mode lifts
its limits (8 assigns of 95-byte paths are too few for a prefix) and reads them from the prefix's `assigns` file;
`Assign` from the program (ZINE's `Execute` already does one) changes them for this run, `Assign` in the prefix's
`S:User-Startup` (read once, only its `Assign` lines) permanently. Names: AmigaDOS's rules as ZINE has them (without
case, Latin-1 names as UTF-8 on the disk: 16.10); protection bits from the Unix mode; file comments (`SetComment`) and
the bits Unix lacks (`P`, `S`, `A`, `H`) in a small hidden file per folder (`.zine-attr`), or dropped: a question of
effort, not of design. `Lock`, `Examine`/`ExNext`/`ExAll`, `Seek`, `Rename`, `CreateDir`, `DeleteFile`, `SetProtection`,
`Info` (sizes from Zenix's `statfs`), `NameFromLock`, `ParentDir`, `MatchFirst`/`MatchNext`, notifications
(`StartNotify`, polled or from D16 events later) map onto Zenix calls one to one.

### 5.2 The prefix

A **prefix** is one folder holding a whole pretend Amiga system for one user, `~/.zine/default` unless a program's
setting names another (Wine's `~/.wine`):

```
~/.zine/default/
    SYS/        C/ S/ L/ Libs/ Devs/ Fonts/ Locale/Catalogs/ Prefs/ Envarc/   (empty at first: our own libraries are built in)
    ENV/
    Work/       one folder per installed program
    assigns     NAME: FOLDER [FOLDER...] lines
    programs    per-program settings: mode (guest, take-over, ask), frame (amiga, desktop), screen (window, own),
                library overrides (6.1), stack size, the program's icon for the desktop
```

`zine -g` creates it on first use. A second prefix keeps, for example, a user's own MUI 3.8 setup apart from an
OpenMUI one.

### 5.3 Installing from an .lha

`zine install ARCHIVE.lha [--prefix P]` (and "Install" on an archive in the file view, D48):
1. D33 unpacks it into a temporary folder; nothing is run.
2. The usual Aminet layouts are recognised: the program's folder (with its `.info` icons) goes to `Work:NAME`;
   `Libs/*.library` and `Libs/*/` to `SYS:Libs` **only if newer** than what is there (the version from the file's
   `$VER:` string or its resident tag, as AmigaOS's Installer compares them, `workbench-libraries.md` *read*);
   `Fonts/` to `SYS:Fonts`; `Catalogs/LANG/` to `SYS:Locale/Catalogs/LANG`; `Prefs/Env-Archive` to `ENVARC:`; `C/`
   to `SYS:C`. A list of what went where is kept (`Work/NAME/.zine-installed`) so `zine uninstall NAME` can undo it
   (libraries others still use stay).
3. The program appears in zdesk's "Amiga" menu and as a file the file view runs.
4. **Archives with an Installer script**: the script language (a small Lisp) is public through its documentation;
   the first version reads the script only for the file copies it can do safely and shows the rest; running the real
   Installer program in a guest, as Wine runs a real setup.exe, comes when guest mode can run it (it is a GadTools
   program of its own; Commodore's binary is not ours to ship, the user may have it).

## 6. Libraries the program brings

### 6.1 Built-in and native

Because the program's 68k code runs as it is, **a third-party disk library is just more 68k code**: ZINE already
loads one with `LoadSeg` and `InitResident` (16.8) and it calls our exec, graphics and intuition like the program
does. So `reqtools.library`, `xpkmaster.library`, `iffparse`-using libraries, `muimaster.library` and their classes
run as they are, **as far as our libraries under them behave like AmigaOS's**. That is the whole game: every library
we make complete lets a whole family of disk libraries work unchanged.

The prefix's `programs` file chooses per library (Wine's overrides): **built-in** (ours), **native** (the file in
`LIBS:`), or **native, then built-in**. Defaults:
- built-in, always: exec, dos, graphics, layers, intuition, keymap, input/console/timer/keyboard/gameport devices
  (they are the runtime itself);
- built-in, native allowed: gadtools, asl, utility, diskfont, locale, icon, workbench, iffparse, commodities,
  datatypes, mathffp/mathieee*;
- native by default: everything else (third-party libraries).

**The user's own AmigaOS libraries:** the partner owns AmigaOS 3.1 and 3.2. Like Wine using native DLLs from a real
Windows, a user may point the override at their own copies (`asl.library` from their Workbench disk; even ROM
modules such as `gadtools.library` read from their Kickstart in the machine). That is the user running their own
licensed code, never shipped by us, and a strong cross-check of our built-ins. With D41 the ROM's pages would need to
be readable by the guest (part of K1's mapping). Clean room is unaffected: we run that code, we never read it.

### 6.2 What a disk library needs from us

From the autodocs, what typical libraries call beyond a simple program: `MakeLibrary`/`SetFunction`/`SumLibrary`
(ZINE has), semaphores between tasks (has), `AllocVec`/pools (has), `ObtainSemaphoreShared`, `utility.library`'s
tags and hooks (`CallHookPkt`, `NextTagItem`, `GetTagData`, `FindTagItem`, `PackBoolTags`), BOOPSI (`MakeClass`,
`AddClass`, `NewObject`, `DoMethod` through the class hook, `SetAttrs`/`GetAttr`, the public classes `rootclass`,
`imageclass`, `gadgetclass`, `propgclass`, `strgclass`, `buttongclass`, `frbuttonclass`, `groupgclass`, `icclass`,
`modelclass`, `sysiclass`, `frameiclass`, `fillrectclass`, `itexticlass`), layers' clip regions, `AllocBitMap` with a
friend bitmap, `ObtainPen`/`ReleasePen`, `BltBitMapRastPort`, `BltMaskBitMapRastPort`, `locale.library`'s
`OpenCatalog`/`GetCatalogStr` (catalogs from `LOCALE:Catalogs`, falling back to the built-in English strings),
`iffparse.library` (MUI's clipboard and settings), `timer.device`, `commodities.library` (an application's hotkeys:
`CxBroker` working inside the guest only) and `workbench.library` (`AddAppWindow`/`AddAppIcon`: accepted, nothing
dropped on them until the broker of 3.5).

### 6.3 MUI

- **MUI 3.8** (Stefan Stuntz; shareware, closed source): the user's own registered copy goes in the prefix
  (`SYS:Libs/muimaster.library`, `MUI:Libs/mui/*.mcc`, the key file, `ENVARC:mui`). We never ship it and never read
  its code; it is the hardest native library to run and the best proof that our layer is right. It needs everything
  in 6.2, BOOPSI in full, and behaves best on a custom screen or the Workbench with enough pens.
- **OpenMUI** (the partner's open MUI 3.8 work-alike, `creep-ltx/openmui`, AROS Public License): the **MUI provider
  guest mode can offer** (Wine's wine-mono idea): `zine install-mui` puts it in a prefix, and any MUI program runs
  without the closed original. Its licence lets it be shipped as a separate package (a 68k library loaded by the
  guest is a separate work from Zenix's GPL code; to confirm with the partner). It gives both projects a shared test
  bed: a program that runs with MUI 3.8 and not with OpenMUI in the same guest is an OpenMUI bug, and the reverse is a
  guest-mode bug.

## 7. Memory and safety

- **Chip and fast RAM:** the runtime keeps exec's own `MemHeader`s over a few large regions it gets from Zenix
  (`SYS_memalloc`: a fast arena of 512 KB that grows in 512 KB steps; a chip arena that grows in 64 KB steps when
  `MEMF_CHIP` is asked for), because Zenix gives a process at most 16 regions (`NREG`) and Amiga programs allocate
  thousands of small blocks. `AvailMem` answers what the arenas have plus what Zenix's free memory would give, so
  programs that check for room before a big allocation are not refused needlessly. `MEMF_CHIP` is honoured (canvases,
  sprites, audio data, images for the blitter); the program's own chip memory is real chip RAM, so a program that
  hands it to `audio.device` (through `sndd` later) or to displayd works.
- **On a machine without fast RAM** (the stock A1200) every arena is chip RAM and the canvases compete with the
  desktop's; it works for small programs. The minimum to aim at: a stock A1200 with 4 MB fast (the Zenix target,
  feedback "Zenix speed and size").
- **A guest's cost** (*proposed*): the runtime's code about 120-180 KB once (shared code, D13) with all of section
  4-6; its data and bases about 40 KB a guest; a 400 x 200 window's canvas 40 KB of chip RAM at 4 planes. A small
  GadTools tool in all: about 150 KB of fast RAM and 40-80 KB of chip RAM.
- **With D41 on** (a 68030, 040 or 060 with an MMU, protection switched on): the guest is a protected process; a stray
  write inside the program or a library ends that guest only, with D41's line; the desktop and other programs go on.
  A touch of the custom chips ends it with the "run it with zine -t" message (3.4). The only extra exposure is page 0,
  read only (K1).
- **Without an MMU:** as for every Zenix program, a bad pointer can damage anything (D8, limits.md "The no-MMU limit");
  Amiga programs were written for exactly that world and are, on the whole, careful with memory. The runtime adds
  exec's own checks (a `FreeMem` of memory it never gave stops the guest with a message, as ZINE does now).
- **No privilege:** unlike take-over mode, a guest needs no root and no `zine` group: it can do nothing a Zenix
  program cannot.

## 8. Changes outside the runtime

| | Where | What | Size (*proposed*) |
|---|---|---|---|
| **K1** | kernel: `proc.c` switch, D41's page 0 | a guest's longword at address 4 written on the switch to it; page 0 (and, if the user's own ROM modules are used, the ROM) readable to guests under D41 | about 100-200 bytes |
| **K2** | kernel: the vectors, D16's delivery | privilege violation, line-A, line-F, illegal, FPU unimplemented delivered to a guest's handler; the signal stack check accepts the guest's regions | about 400-600 bytes |
| **K3** | kernel: the switch | FPU state saved and restored for processes marked as FPU users, lazily | about 200-300 bytes |
| **D1** | displayd and libdisplay | `DPY_CANVAS` (a window shown from the client's planar bitmap, changed rectangles), held planes as D55's, window kind `WK_GUEST` | 3-5 KB of displayd |
| **D2** | displayd, zdesk | menus with sub-items, check marks, disabled items, shortcuts, more than 512 bytes | 1-2 KB |
| | `zine` | `-g` (guest), `-t` (take-over), the mode chosen by the prefix or by where it was started; `install`, `uninstall` | a few KB |

**The kernel's size is the tight part:** the mount tests' floppy has 856 bytes left after D55 (DESIGN D55 "As built"),
and K1-K3 together are about 0.7-1.1 KB. Either K1-K3 are left out of the floppy's kernel (`-DNO_GUEST`, as the demo
floppies build displayd `-DNO_SCREEN`), or the budget is found elsewhere first. K1-K3 need the usual two independent
reviews (they touch the context switch and the fault path).

**How the mode is chosen:** started from the desktop (an icon, the file view, zdesk's menu), a program runs as a
guest; typed at a text console, as today, it takes over the machine. The prefix's `programs` file and the options
`-g`/`-t` override both. A guest that turns out to want the machine says so in one line and ends; it never takes the
machine by itself (that still needs the `zine` group and the console).

## 9. A realistic order of steps

Each step is a milestone the partner can see on the desktop, tested in FS-UAE with the cage rig (`tools/zine-*-test.sh`
style) and then on the A1200 and the 1260. Test programs are **our own** (written in C with the NDK's names, built by
the project's 68k compiler) or freely licensed Aminet programs fetched by a test script, never committed as binaries.
Hours are the helper's own, *proposed*.

| Step | What the partner sees | Work | Test programs | Kernel | Hours |
|---|---|---|---|---|---|
| **0. Groundwork** | `zine -g hello` prints "hello" in the terminal, from a guest | split the runtime (3.3), user-mode task switch, the arenas, real-file dos with the prefix and assigns, K1-K3 | our `ghello` (dos `Printf`), ZINE's existing CLI tests in guest mode | K1, K2, K3 | 12-16 |
| **(a) A window** | an Amiga window on the desktop with text, an Amiga frame and a close gadget that works | `OpenWindowTagList` on the public screen, the canvas and `DPY_CANVAS`, the frame, IDCMP from events (`CLOSEWINDOW`, `RAWKEY`, `VANILLAKEY`, `MOUSEBUTTONS`, `NEWSIZE`), `WK_GUEST` | our `gwin` (draws text and boxes, echoes keys, resizes) | none more | 12-16 |
| **(b) GadTools** | buttons, check boxes, cycle, string and slider gadgets that respond; a menu in the desktop's bar | Intuition's gadgets, GadTools' 12 kinds drawn and handled, menus (D2), Amiga-key shortcuts, `EasyRequest` | our `ggad` (one of each gadget kind, a menu with sub-items and check marks) | none | 25-35 |
| **(c) A real tool** | a real Aminet program running side by side with Zenix programs | graphics completeness (`Area*`, `Draw`, `PolyDraw`, `BltTemplate`, `ScrollRaster`, `ClipBlit`, `BltBitMapRastPort`), clip regions, `timer.device`, `icon.library` and the Workbench start message (tooltypes), `locale` with built-in strings, the 10 Hz tick | **VClock 1.9a** (`util/time/vclock-1.9a`: "VCLOCK and source is in the PUBLIC DOMAIN", a resizable seven-segment clock using `timer.device` and graphics' `Area` functions); then a calculator and a text viewer chosen from Fred Fish disks or Aminet with source and a free licence (each licence read from its readme before use: candidates are listed for the partner, none fetched yet) | none | 20-30 |
| **(d) Installing** | `zine install` of an .lha, the program in zdesk's Amiga menu, an ASL file requester picking a Zenix file | the prefix tools, the install rules of 5.3, `asl.library` (file, font, screen mode), `reqtools.library` as a native library test (freely distributable; its readme decides whether a test may fetch it) | the step (c) programs installed from their archives; our `gasl` | none | 20-30 |
| **(e) MUI** | a MUI program running, first with OpenMUI, then with the partner's own MUI 3.8 | BOOPSI in full, the classes of 6.2, `utility`, `iffparse`, `commodities`/`workbench` inside the guest, `ObtainPen`, `AllocBitMap` friends, layers' clip regions everywhere | OpenMUI's own demo programs; a small free MUI program; the partner's MUI programs by hand | none | 50-80 |
| **(f) Custom screens** | a program on its own screen (a paint program, a game's options screen), Left Amiga+N back to the desktop | D55 screens from `OpenScreen`, full overlapping layers, the runtime drawing title bar and menus there, the virtual Workbench window (4.7) | our `gscreen` (two overlapping windows on a 32-colour screen) | none | 20-30 |

**In all about 160-240 hours** for (0) to (f), 140-200 without (f). Each step ends merged and tested before the next
starts (one helper at a time). The first milestone is (0) + (a), about 25-30 hours.

Calls missing at any step are found the way ZINE finds them today: the stop line names the library and the function
("NAME called graphics.library `AreaEllipse` (offset -186), which guest mode does not have yet"), and `/var/log/zine`
collects them; that list drives the order of work after (c), as the Top 100 drove the demos.

## 10. Risks

- **The long tail.** Intuition, layers and BOOPSI are where AmigaOS behaviour is subtle (refresh order, gadget
  activation, the exact message sequence MUI and others rely on). Our built-ins follow the autodocs; where a program
  relies on behaviour the autodocs do not state, only testing finds it. The native-library cross-check (6.1, the
  partner's own OS 3.x libraries) is the best defence, and the reason to keep it possible.
- **No MMU, no isolation.** On the stock A1200 and on the PiStorm (Emu68 has no 68k MMU), a guest that pokes the chips
  or writes through a bad pointer can damage the desktop or Zenix. On the PiStorm A1200 (one of the three test machines)
  guests run as trusted as any other program. Known take-over programs are caught by name or by their library
  calls first.
- **The kernel's size** on the mount tests' floppy (section 8).
- **Speed on a 14 MHz 68020:** drawing by the CPU into chip RAM canvases, then a blit to the screen, is about what
  Intuition itself costs for smart-refresh windows, plus the blit; fine for tools, possibly slow for programs that
  redraw large windows every frame. To be measured at step (a) on the real A1200, not claimed from an emulator.
- **Two copies of the library code** (take-over and guest builds) could drift apart if the split of 3.3 is not kept.
- **Colours:** a 16-colour desktop with fixed pens limits programs written for 8-colour Workbench screens with their own
  palettes (icon-heavy or picture-heavy tools look wrong). Custom screens (f) and per-program screen settings help.

## 11. Questions for the partner

1. **Is guest mode the direction** for "Amiga GUI programs on the desktop", next to take-over mode for demos? (This
   note's default: yes, both, chosen by where a program is started.)
2. **One process per Amiga program** (crash isolation with D41, no sharing between Amiga programs until a broker), or
   **one shared Amiga session** where all Amiga programs live together as on AmigaOS (sharing ARexx ports, public
   screens and the clipboard from the start, one crash ends all)? Default: one process each.
3. **Frames:** Amiga windows with their own Amiga frames and gadgets (they look like AmigaOS and every program works),
   or zdesk's frames like every other desktop window (consistent, but border gadgets break)? Default: Amiga frames,
   zdesk's as a per-program setting.
4. **File requesters:** the ASL look (Amiga programs look like Amiga programs), or the desktop's own file dialog?
   Default: the ASL look.
5. **The desktop's first four colours:** should the looks keep colours 0-3 close to the Workbench 2.x grey, black, white
   and blue, so old programs look right on the desktop?
6. **MUI:** is OpenMUI meant to be the MUI that guest mode offers (installed on demand, shipped as its own package
   under its own licence), with the user's own MUI 3.8 as the alternative?
7. **The user's own AmigaOS libraries** (from their Workbench disks or Kickstart ROM, never shipped): may guest mode
   offer to use them, as Wine uses native Windows DLLs? It is the best compatibility check we can have.
8. **Which programs** would you like on the desktop first? A short list (a clock, a calculator, a text viewer, an
   editor, a mod player, IBrowse, a paint program...) decides the order after step (c) better than a guess.
9. **Approval:** may this be built in the order of section 9, step (0) + (a) first (about 25-30 hours, three small
   kernel changes with two independent reviews before building)?

*Written 2026-11-17 as a design study: Zenix facts read from main `6bc369f`, AmigaOS facts from the public autodocs,
the NDK 3.2's interface files and the knowledge base's AmigaOS 3.1 study notes, other projects from their public pages
(vamos, Janus-UAE, Emumiga, AROS, Wine) the same day. Nothing measured; every number proposed.*
