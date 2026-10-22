# Zenix documentation

| Document | What it answers |
|---|---|
| [architecture.md](architecture.md) | How Zenix is put together: the layers, the kernel, memory, processes, messages, the servers, the filesystem, the terminal and display, and how the image is built. |
| [how-it-works.md](how-it-works.md) | Walk-throughs: from power-on to a shell prompt, what happens inside `cat file | grep x`, what happens when a program crashes. |
| [vs-amigaos.md](vs-amigaos.md) | Zenix next to AmigaOS: what is the same, what is different, what Zenix gives up and gains. |
| [vs-unix-linux.md](vs-unix-linux.md) | Zenix next to Unix and Linux (and no-MMU Linux, Fuzix, Minix 3, QNX): where it behaves like a Unix and where it cannot. |
| [commands.md](commands.md) | Reference for every command: what it does, how to use it, its options (manual pages in `man/`, ready for `man`). |
| [features.md](features.md) | The one list of what Zenix can do, with its status (verified / planned). Kept in step with the code. |
| [limits.md](limits.md) | What is missing, what is unmeasured, and the honest no-MMU limit. |
| [ROADMAP.md](ROADMAP.md) | Vision, principles and the phased plan. |
| [zxfs.md](zxfs.md) | The proposed native filesystem (nothing built). |
| [DESIGN.md](DESIGN.md) | The numbered design decisions D1-D8. |

How these documents were written: by reading the source tree (`kernel/`, `user/`, `boot/`, `tools/`) and the
existing notes on 2026-10-22. Statements about this code are *read* from the source unless a sentence says
*verified* (it was run in FS-UAE, see the knowledge-base note `amiga/zenix-m0-m3-findings.md`). Statements about
AmigaOS, Linux and other systems are from general knowledge, not measured here. Nothing has run on a real Amiga yet.

## Where things live (and the rule)
- All documentation is in `docs/`. The only others are `README.md` (short, points here), `CLAUDE.md` (the rules for
  whoever works on the project) and the manual pages' sources, which are also in `docs/man/`.
- Manual pages: `docs/man/NAME.txt`, one per program. Command index: `commands.md`. Features: `features.md`.
- Code and data that look like text but are not documentation stay where they are: `rootfs/` (files that end up on
  the Zenix disk), `tools/tests/` (scripts typed into the emulator), `archive/` (old code kept for reference).
- **When a command is added, changed or removed, or a feature appears, the same commit updates its manual page,
  `commands.md` and `features.md`.** `tools/docs-check.sh` checks this; `--staged` checks a commit before it is made.
