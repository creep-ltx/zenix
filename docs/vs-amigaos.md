# Zenix compared with AmigaOS

AmigaOS facts below are from general knowledge of Exec, DOS and Intuition (the ROM Kernel Manuals are in the
knowledge base under `manuals/`); Zenix facts are read from this source tree. Nothing was benchmarked against
AmigaOS.

## What they share (and why Zenix was built this way)

- **Same machine model:** one flat address space, no memory protection, chip and fast RAM as separate pools,
  custom chips driven directly. The Amiga is the reason Zenix has no MMU requirement: most Amigas have none.
- **Message passing as the core idea.** Exec's message ports are the model for Zenix's `port_create / call / recv
  / reply`. Messages carry pointers, not data, because everything shares one address space - the Amiga's famous
  speed trick, kept deliberately (ROADMAP principle 9).
- **Servers instead of a fat kernel.** AmigaOS filesystems are handler *processes* reached by DOS packets.
  Zenix's `fsd` and `diskd` are the same idea.
- **Relocating executables.** AmigaOS HUNK files carry relocation tables and are loaded wherever memory is free;
  Zenix's ZEXE does the same with a much simpler format (one image, one table).
- **Preemptive multitasking** with a small kernel and fast context switches.
- **Hardware used natively:** Copper and blitter draw the display; Topaz comes from the ROM.

## Where they differ

| | AmigaOS | Zenix |
|---|---|---|
| Role of the OS | Runs on the ROM; applications and the OS coexist; you can leave it | Takes the machine over from the bootblock, kills AmigaOS; no way back |
| Message semantics | Asynchronous: `PutMsg` returns at once, the receiver `GetMsg`s/`ReplyMsg`s, signals wake tasks | Synchronous call: the client blocks until the reply (QNX/Minix style); simpler, no signal bits |
| Messages and memory | Message is a struct in sender memory; ownership by convention | Same, but the kernel tracks *who owns what* per block |
| Scheduling | Priority-based preemptive, with time slicing among equals | Round robin, 50 ms slice, no priorities yet |
| Kernel preemption | `Forbid()` / `Disable()` let tasks turn off multitasking/interrupts | Kernel code is never preempted: no locks anywhere in the kernel |
| Memory cleanup | A task must free what it allocated; leaks survive a crash | Every block has an owner pid; death frees everything automatically |
| Crash | A bad pointer or illegal instruction raises a Guru Meditation and usually takes the whole machine | The offending process is killed, the rest continues |
| Program model | `exec` tasks and DOS processes; startup from a CLI or Workbench | Unix: `spawn(path, argv, stdio)`, exit status, `wait` |
| File naming | `DH0:dir/file`, volumes and assigns, case-insensitive | One tree rooted at `/`, case-sensitive, mount-based |
| Filesystem | OFS/FFS/PFS3/SFS, `DosType` in the RDB selects the handler | ext2 today (readable by Linux), RDB partition table shared; ZenFS planned |
| I/O model | Packets/IORequests to devices; ReadArgs templates; `Wait()` on signals | Everything is a file descriptor: `read/write` on files, ttys, pipes |
| Pipes | Not a core concept (`PIPE:` handler) | Kernel pipes, first-class, `|` in the shell |
| Libraries | Shared libraries opened at run time with a jump table (`OpenLibrary`) | None: static linking, a tiny libc per program; shared libs left open in the file format |
| GUI | Intuition, windows, screens, gadgets, ReAction/MUI | None yet: text terminal, workspaces and a bar; display server planned |
| Drivers/devices | `.device` and `.library` files, loaded on demand, resident modules in ROM | Built-in IDE, serial, keyboard drivers; more drivers planned as servers |
| CPU | 68000 and up | 68020 and up (68000 refused: not a target) |
| Real hardware | Decades of it | **Only FS-UAE so far**; not yet on a real Amiga |

## What Zenix gives up

- All existing Amiga software, libraries and the GUI stack: Zenix is not binary compatible and has no Exec,
  Intuition or `dos.library`. (The design explicitly says "not AmigaOS-based".)
- Coexistence: AmigaOS programs can run alongside each other and call a thousand libraries. Zenix has one
  statically linked world.
- The 68000 and 68010: Zenix needs a 68020 or better (ROADMAP: 68000 is not a target).
- Hardware reach: AmigaOS drivers cover Zorro cards, RTG, network and USB. Zenix has only what its authors write.

## What Zenix gains (intended; not yet shown on real hardware)

- A **Unix environment**: `/`, pipes, a real shell with scripting, small composable tools, `man`.
- **Containment:** a crashing program does not bring down the machine, and its memory comes back automatically.
- **A filesystem that Linux can read** and fsck; ZenFS planned to be crash-safe by atomic commit.
- A small, readable kernel (about 5,000 lines) with no hidden global state: no `Forbid` bugs, no lock ordering.
- Predictable behaviour: one mechanism per job (messages), no parallel APIs for the same thing.

## The one thing they have in common that matters most

Neither can *prevent* a wild write. AmigaOS cannot protect programs from each other; Zenix without an MMU cannot
either. The difference is what happens next: AmigaOS leaves the damage and the leaked memory; Zenix at least
cleans up after a program that crashes visibly. An MMU port could later add real protection (DESIGN: "an MMU may
be used later for optional protection").
