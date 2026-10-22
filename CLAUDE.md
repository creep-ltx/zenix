# Zenix: rules for working on this project

Zenix is a small Unix-like OS for 68020+ Amigas without an MMU. Start with `docs/README.md`.

## Where files go
- **Documentation lives in `docs/`, nowhere else.** No `.md`/`.txt` notes in `kernel/`, `user/`, `boot/`,
  `tools/` or the project root (only `README.md` and this file). Design decisions: `docs/DESIGN.md`. Plan:
  `docs/ROADMAP.md`. Findings that help other projects: the shared knowledge base, not here.
- Manual pages: `docs/man/NAME.txt` (plain text, at most 78 columns, the format `man` reads). Command index:
  `docs/commands.md`. Feature list: `docs/features.md`.
- Test input scripts: `tools/tests/`. Old code kept for reference: `archive/`. Files copied onto the Zenix disk:
  `rootfs/`.

## Documentation stays in step with the code (same commit, always)
- **New command** (`user/NAME.c` added to `UAPPS`): write `docs/man/NAME.txt`, add it to `docs/commands.md`, add or
  adjust a line in `docs/features.md`.
- **Changed behaviour of a command** (an option, a default, an output format, an exit status, a limit): update its
  page, including the NOTES section about what it does not do.
- **New or changed kernel feature, system call, message protocol, limit** (`kernel/*.h`): update
  `docs/architecture.md` (and `docs/features.md`, and `docs/limits.md` if a number changed).
- **Removed feature**: remove it from the same places; never leave a page for something that is gone.
- Write what the code *does*, read from the source. Say *verified* only for what was run; emulator timing is not
  hardware timing.
- Before committing run `tools/docs-check.sh --staged`; it must say `docs-check: ok`.

## Other rules
- Licence: GPL-2.0-only; new files carry `SPDX-License-Identifier: GPL-2.0-only`.
- Several sessions work in parallel (main, and the `drivers` branch in `~/Projects/zenix-drivers`). Commit your own
  files by path. Never `git stash`, `git checkout .` or `git reset --hard` in a shared working tree.
