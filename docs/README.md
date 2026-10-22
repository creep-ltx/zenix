# Zenix documentation

| Document | What it answers |
|---|---|
| [architecture.md](architecture.md) | How Zenix is put together: the layers, the kernel, memory, processes, messages, the servers, the filesystem, the terminal and display, and how the image is built. |
| [how-it-works.md](how-it-works.md) | Walk-throughs: from power-on to a shell prompt, what happens inside `cat file | grep x`, what happens when a program crashes. |
| [vs-amigaos.md](vs-amigaos.md) | Zenix next to AmigaOS: what is the same, what is different, what Zenix gives up and gains. |
| [vs-unix-linux.md](vs-unix-linux.md) | Zenix next to Unix and Linux (and no-MMU Linux, Fuzix, Minix 3, QNX): where it behaves like a Unix and where it cannot. |
| [commands.md](commands.md) | Reference for every command: what it does, how to use it, its options (manual pages in `man/`, ready for `man`). |
| [limits.md](limits.md) | What is missing, what is unmeasured, and the honest no-MMU limit. |
| [ROADMAP.md](ROADMAP.md) | Vision, principles and the phased plan. |
| [zenfs.md](zenfs.md) | The proposed native filesystem (nothing built). |
| [../DESIGN.md](../DESIGN.md) | The numbered design decisions D1-D8. |

How these documents were written: by reading the source tree (`kernel/`, `user/`, `boot/`, `tools/`) and the
existing notes on 2026-10-22. Statements about this code are *read* from the source unless a sentence says
*verified* (it was run in FS-UAE, see the knowledge-base note `amiga/zenix-m0-m3-findings.md`). Statements about
AmigaOS, Linux and other systems are from general knowledge, not measured here. Nothing has run on a real Amiga yet.
