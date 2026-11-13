# ZINE and the Top 100 Amiga productions (measured 2026-11-07)

What stops the 500 most-liked Amiga productions on Pouet (five lists of 100: AGA demos, OCS demos, 64k, 40k
and 4k intros) under ZINE (DESIGN D29, [d29-zine.md](d29-zine.md)), so the most common gaps get fixed first. Every
production with an AmigaOS executable was run by `zine` in FS-UAE, once on the stock-style A1200 and, if it was
blocked, again on a bigger machine. *Verified* in FS-UAE (not on real hardware); ZINE was not changed for this.
The demos themselves, the harness and the per-production results (ZINE's lines, serial logs, screenshots) are kept
outside the repository, on the owner's PC; only this summary is here. Production numbers are the rank in their list
(e.g. "Demos-AGA 010" = rank 10 of the AGA demos).

## How it was run

- Per production: the main executable picked from the folder (`.exe` first, a name matching the production, top
  level, not a WHDLoad/install/C: tool, the plain version before `_uae`, `_uncrunched`, `_060`...), the folder
  written into a copy of the root disk (`/demo`, names in UTF-8), `cd` to its folder and `zine ./PROGRAM` typed
  at the console. Folders with only an ADF/DMS image are counted as "disk image only" and not run (ZINE runs
  executables, not disk images: owner decision 2026-10-30).
- Outcomes: *running* = still going at the limit (a screenshot about 20 s emulated after the hand-over and one at
  the limit: "changing" if they differ, "unchanged", or "black"); *ended cleanly* = return code 0, or ended by
  itself after 60 s or more of emulated time (intros often return whatever d0 held); otherwise the line ZINE
  printed (a missing call, a crash, a refusal).
- Four FS-UAEs at once in the shared emulator slots; about 1.5 hours for pass 1 and 25 minutes for pass 2.

## What it says, in short

- **The real ZINE gaps** (they stop the same productions on the bigger machine too): `exec SuperState` (20), the
  "damaged" refusal of an executable whose header counts more hunks than come before its `HUNK_OVERLAY` (15: 14 of
  them have the overlay right after the first hunk, a cruncher's form ZINE does not accept; Boogietown, Love, Fullmoon,
  Real, faktory, switchback...), `dos IsInteractive` (12), `graphics OpenFont` (8), `exec RawIOInit` (8),
  `utility.library` (6), `intuition LockPubScreen` (5), the math libraries (`mathffp`, `mathieeedoubbas`,
  `mathieeedoubtrans`: 11 together), `dos FWrite`, `GetProgramName`, `ReadArgs`, `exec CreateIORequest`, `AddPort`.
- **Memory**: the biggest single cause on the 2 + 8 MB A1200 (49 "does not fit", 14 "could not be loaded", 14
  "cannot be read"). With 16 MB fast, 9 of those ran and 37 got further, to another blocker; 47 are still too big:
  `zine` reads the whole executable into its own memory first ("cannot be read" for the 7-26 MB AGA demos), and the
  file plus its hunks must fit at once. 41 productions also had their data left out of ZINE's volume (more than
  half the free fast RAM), so 4-7 of them ended with an error code.
- **No FPU** on the 68EC020: 24 crashed on an FPU instruction; with a 68882, 15 of them ran and 9 got further.
  Four need a 68040/060 (MOVE16).
- **ZINE printed no function name** for `graphics.library` -888 (SetChipRev), -960 (GetBitMapAttr), -1050
  (BestModeIDA), `intuition.library` -768 (AllocScreenBuffer) and private `exec.library` -912: the names are added
  here from the NDK 3.2 `.fd` files.

## Pass 1: the stock-style A1200

**Pass 1.** Productions measured: 500 of 500. Machine: FS-UAE A1200 (AGA, 68EC020 14 MHz, no FPU, no MMU), 2 MB chip + 8 MB Zorro II fast RAM, Kickstart 3.1 ROM, Zenix main at 1879c57 in warp mode; each program got about 50 s of PC time in warp mode (several minutes emulated; demos that ended by themselves report up to 287 s) after the hand-over.

### What blocks the most productions

Each production counts once, under the first thing that stopped it. Disk-image-only productions are not counted here (see the table below).

| # | Blocker | Productions | Examples |
|---|---|---|---|
| 1 | does not fit in memory (file + hunks > free RAM) | 49 | Demos-AGA 002, Demos-AGA 007, Demos-AGA 008, Demos-AGA 010 ... |
| 2 | crashed: line F, an FPU instruction (this 68EC020 has no FPU) | 24 | Demos-AGA 004, Demos-AGA 017, Demos-AGA 069, Intros-64k 016 ... |
| 3 | missing: exec.library SuperState (-150) | 18 | Demos-OCS 018, Demos-OCS 059, Demos-OCS 076, Demos-OCS 083 ... |
| 4 | zine: "is damaged (it ends before its last hunk)" (a HUNK_OVERLAY right after the first hunk) | 15 | Demos-AGA 024, Demos-AGA 048, Demos-AGA 057, Demos-AGA 072 ... |
| 5 | zine: "cannot be read" (an executable of 7-26 MB: zine cannot hold the file) | 14 | Demos-AGA 020, Demos-AGA 022, Demos-AGA 023, Demos-AGA 038 ... |
| 6 | could not be loaded: (not enough memory) | 14 | Demos-AGA 039, Demos-AGA 056, Demos-AGA 082, Intros-64k 042 ... |
| 7 | black screen at the limit (still in precalc, or hung) | 13 | Demos-AGA 088, Demos-OCS 045, Demos-OCS 091, Intros-64k 007 ... |
| 8 | missing: graphics.library OpenFont (-72) | 8 | Demos-OCS 046, Intros-64k 017, Intros-64k 053, Intros-4k 014 ... |
| 9 | data left out of ZINE's volume (folder over 500 entries, 16 MB or half the free fast RAM), then ended with an error code | 7 | Demos-AGA 012, Demos-AGA 015, Demos-AGA 019, Demos-AGA 027 ... |
| 10 | missing: dos.library IsInteractive (-216) | 6 | Demos-AGA 001, Demos-AGA 003, Demos-AGA 006, Demos-AGA 052 ... |
| 11 | no utility.library (no library) | 5 | Demos-AGA 009, Demos-AGA 013, Demos-AGA 032, Demos-AGA 073 ... |
| 12 | crashed: illegal instruction | 5 | Intros-64k 097, Intros-40k 012, Intros-40k 043, Intros-40k 044 ... |
| 13 | missing: dos.library GetProgramName (-576) | 4 | Demos-AGA 028, Demos-AGA 071, Demos-OCS 031, Intros-40k 089 |
| 14 | missing: dos.library FWrite (-330) | 3 | Demos-AGA 026, Demos-AGA 037, Demos-AGA 046 |
| 15 | missing: dos.library ReadArgs (-798) | 3 | Demos-AGA 034, Demos-OCS 041, Intros-40k 008 |
| 16 | crashed: line F, another instruction | 3 | Demos-AGA 040, Demos-AGA 084, Intros-64k 088 |
| 17 | missing: graphics.library SetChipRev (-888; ZINE printed no name) | 3 | Demos-AGA 085, Intros-64k 019, Intros-40k 039 |
| 18 | missing: exec.library AddPort (-354) | 3 | Demos-AGA 093, Demos-OCS 069, Intros-40k 026 |
| 19 | no mathffp.library (no library) | 3 | Intros-64k 024, Intros-64k 027, Intros-64k 041 |
| 20 | no mathieeedoubbas.library (no library) | 2 | Demos-AGA 021, Demos-AGA 060 |
| 21 | ended early after: no file (data file not found) | 2 | Demos-AGA 051, Demos-OCS 070 |
| 22 | no mathieeedoubtrans.library (no library) | 2 | Demos-AGA 078, Intros-40k 019 |
| 23 | ended early with return code 1 | 2 | Demos-AGA 091, Intros-64k 003 |
| 24 | crashed: line F, MOVE16 (a 68040/060 instruction) | 2 | Intros-64k 031, Intros-64k 059 |
| 25 | missing: exec.library CreateIORequest (-654) | 2 | Intros-64k 062, Intros-40k 053 |
| 26 | missing: exec.library RawIOInit (-504) | 2 | Intros-64k 084, Intros-40k 095 |
| 27 | crashed: line F at $FFFF (ran into erased memory) | 2 | Intros-40k 055, Intros-40k 092 |
| 28 | missing: dos.library Lock (-84) | 1 | Demos-AGA 053 |
| 29 | missing: intuition.library QueryOverscan (-474) | 1 | Demos-AGA 077 |
| 30 | ended early with return code -1 | 1 | Demos-OCS 049 |
| 31 | crashed: line A instruction | 1 | Demos-OCS 052 |
| 32 | missing: intuition.library DisplayAlert (-90) | 1 | Demos-OCS 054 |
| 33 | crashed: privilege violation | 1 | Demos-OCS 082 |
| 34 | missing: intuition.library ActivateWindow (-450) | 1 | Demos-OCS 085 |
| 35 | missing: graphics.library SetAPen (-342) | 1 | Demos-OCS 096 |
| 36 | missing: intuition.library AllocRemember (-396) | 1 | Intros-64k 054 |
| 37 | missing: intuition.library RethinkDisplay (-390) | 1 | Intros-64k 079 |
| 38 | missing: intuition.library LockPubScreen (-510) | 1 | Intros-40k 007 |
| 39 | crashed: address error | 1 | Intros-40k 023 |
| 40 | missing: intuition.library RemakeDisplay (-384) | 1 | Intros-40k 070 |
| 41 | missing: exec.library -912 (private; ZINE printed no name: a call through a null base?) | 1 | Intros-40k 072 |
| 42 | missing: graphics.library TextLength (-54) | 1 | Intros-4k 020 |
| 43 | missing: intuition.library AllocScreenBuffer (-768; ZINE printed no name) | 1 | Intros-4k 057 |
| 44 | ended early with return code 18 | 1 | Intros-4k 073 |

### Outcomes per category

| Outcome | Demos-AGA | Demos-OCS | Intros-64k | Intros-40k | Intros-4k | All |
|---|---|---|---|---|---|---|
| running (picture, changing) | 3 | 9 | 24 | 36 | 16 | 88 |
| running (picture, unchanged) | 0 | 0 | 1 | 2 | 15 | 18 |
| running (black screen) | 1 | 2 | 3 | 3 | 4 | 13 |
| ended cleanly (code 0, or after 60+ s) | 1 | 10 | 21 | 35 | 20 | 87 |
| ended (error code) | 17 | 2 | 4 | 1 | 1 | 25 |
| missing call | 15 | 12 | 14 | 13 | 10 | 64 |
| crashed | 5 | 2 | 9 | 7 | 16 | 39 |
| stopped (other) | 53 | 7 | 18 | 2 | 12 | 92 |
| disk image only | 4 | 56 | 6 | 1 | 6 | 73 |
| no Amiga executable | 1 | 0 | 0 | 0 | 0 | 1 |
| total | 100 | 100 | 100 | 100 | 100 | 500 |

### Other things ZINE said (on any production, also ones that ran)

| Line (program name and paths left out) | Productions |
|---|---|
| only the program goes into ZINE's volume (its folder too big: > 500 entries, 16 MB or half the free fast RAM) | 41 |
| no file ... | 6 |
| no library utility.library | 5 |
| no library mathffp.library | 5 |
| no library mathieeedoubtrans.library | 4 |
| no library mathieeesingbas.library | 4 |
| no library mathieeedoubbas.library | 2 |
| no library mathieeesingtrans.library | 2 |
| no library cybergraphics.library | 1 |
| (program) asked dos Execute to run "...": no program TBL-TINT.E01 in its folder or C: (a Shell says "TBL-TINT.E01: Unknown command"), answered as fa | 1 |
| no device input.device | 1 |
| no library asl.library | 1 |
| no library icon.library | 1 |
| no library gadtools.library | 1 |

## Pass 2: the blocked ones again on a bigger machine

220 productions that neither ended cleanly nor ran to the limit in pass 1 (disk-image-only ones left out) ran again on FS-UAE's A1200 with a **68030 + 68882 FPU, 2 MB chip + 16 MB fast RAM** (8 MB Zorro II + 8 MB Zorro III; the CPU's clock is not pinned: warp mode). Same Zenix, same limit.

Per pass-1 blocker: how many the bigger machine got past (a test-machine limit), how many stopped at the same thing again (a real ZINE gap, or a program that needs still more), and how many got further and stopped at something else.

| Pass-1 blocker | Productions | Fine on the bigger machine | Same blocker | Other blocker |
|---|---|---|---|---|
| does not fit in memory (file + hunks > free RAM) | 49 | 5 | 18 | 26 |
| crashed: line F, an FPU instruction (this 68EC020 has no FPU) | 24 | 15 | 0 | 9 |
| missing: exec.library SuperState (-150) | 18 | 0 | 18 | 0 |
| zine: "is damaged (it ends before its last hunk)" (a HUNK_OVERLAY right after the first hunk) | 15 | 0 | 15 | 0 |
| zine: "cannot be read" (an executable of 7-26 MB: zine cannot hold the file) | 14 | 0 | 13 | 1 |
| could not be loaded: (not enough memory) | 14 | 4 | 0 | 10 |
| missing: graphics.library OpenFont (-72) | 8 | 0 | 8 | 0 |
| data left out of ZINE's volume (folder over 500 entries, 16 MB or half the free fast RAM), then ended with an error code | 7 | 0 | 3 | 4 |
| missing: dos.library IsInteractive (-216) | 6 | 0 | 6 | 0 |
| no utility.library (no library) | 5 | 0 | 5 | 0 |
| crashed: illegal instruction | 5 | 2 | 2 | 1 |
| missing: dos.library GetProgramName (-576) | 4 | 0 | 4 | 0 |
| missing: dos.library FWrite (-330) | 3 | 0 | 3 | 0 |
| missing: dos.library ReadArgs (-798) | 3 | 0 | 3 | 0 |
| crashed: line F, another instruction | 3 | 0 | 0 | 3 |
| missing: graphics.library SetChipRev (-888; ZINE printed no name) | 3 | 0 | 3 | 0 |
| missing: exec.library AddPort (-354) | 3 | 0 | 3 | 0 |
| no mathffp.library (no library) | 3 | 0 | 3 | 0 |
| no mathieeedoubbas.library (no library) | 2 | 0 | 2 | 0 |
| ended early after: no file (data file not found) | 2 | 0 | 1 | 1 |
| no mathieeedoubtrans.library (no library) | 2 | 0 | 2 | 0 |
| ended early with return code 1 | 2 | 0 | 0 | 2 |
| crashed: line F, MOVE16 (a 68040/060 instruction) | 2 | 0 | 2 | 0 |
| missing: exec.library CreateIORequest (-654) | 2 | 0 | 2 | 0 |
| missing: exec.library RawIOInit (-504) | 2 | 0 | 2 | 0 |
| crashed: line F at $FFFF (ran into erased memory) | 2 | 0 | 0 | 2 |
| missing: dos.library Lock (-84) | 1 | 0 | 1 | 0 |
| missing: intuition.library QueryOverscan (-474) | 1 | 0 | 1 | 0 |
| ended early with return code -1 | 1 | 0 | 1 | 0 |
| crashed: line A instruction | 1 | 1 | 0 | 0 |
| missing: intuition.library DisplayAlert (-90) | 1 | 0 | 1 | 0 |
| crashed: privilege violation | 1 | 0 | 0 | 1 |
| missing: intuition.library ActivateWindow (-450) | 1 | 0 | 1 | 0 |
| missing: graphics.library SetAPen (-342) | 1 | 0 | 1 | 0 |
| missing: intuition.library AllocRemember (-396) | 1 | 0 | 1 | 0 |
| missing: intuition.library RethinkDisplay (-390) | 1 | 0 | 1 | 0 |
| missing: intuition.library LockPubScreen (-510) | 1 | 0 | 1 | 0 |
| crashed: address error | 1 | 0 | 0 | 1 |
| missing: intuition.library RemakeDisplay (-384) | 1 | 0 | 1 | 0 |
| missing: exec.library -912 (private; ZINE printed no name: a call through a null base?) | 1 | 0 | 0 | 1 |
| missing: graphics.library TextLength (-54) | 1 | 0 | 0 | 1 |
| missing: intuition.library AllocScreenBuffer (-768; ZINE printed no name) | 1 | 0 | 1 | 0 |
| ended early with return code 18 | 1 | 1 | 0 | 0 |

### What blocks the most on the bigger machine (the real ZINE gaps first)

Each production under its pass-2 blocker if it ran again, else its pass-1 one.

| # | Blocker | Productions | Examples |
|---|---|---|---|
| 1 | missing: exec.library SuperState (-150) | 20 | Demos-AGA 010, Demos-AGA 067, Demos-OCS 018, Demos-OCS 059 ... |
| 2 | does not fit in memory (file + hunks > free RAM) | 19 | Demos-AGA 007, Demos-AGA 016, Demos-AGA 025, Demos-AGA 041 ... |
| 3 | could not be loaded: (not enough memory) | 15 | Demos-AGA 002, Demos-AGA 070, Demos-AGA 081, Demos-AGA 086 ... |
| 4 | zine: "is damaged (it ends before its last hunk)" (a HUNK_OVERLAY right after the first hunk) | 15 | Demos-AGA 024, Demos-AGA 048, Demos-AGA 057, Demos-AGA 072 ... |
| 5 | black screen at the limit (still in precalc, or hung) | 15 | Demos-AGA 084, Demos-AGA 088, Demos-OCS 045, Demos-OCS 091 ... |
| 6 | zine: "cannot be read" (an executable of 7-26 MB: zine cannot hold the file) | 13 | Demos-AGA 020, Demos-AGA 022, Demos-AGA 023, Demos-AGA 038 ... |
| 7 | missing: dos.library IsInteractive (-216) | 12 | Demos-AGA 001, Demos-AGA 003, Demos-AGA 006, Demos-AGA 008 ... |
| 8 | missing: graphics.library OpenFont (-72) | 8 | Demos-OCS 046, Intros-64k 017, Intros-64k 053, Intros-4k 014 ... |
| 9 | missing: exec.library RawIOInit (-504) | 8 | Intros-64k 042, Intros-64k 052, Intros-64k 058, Intros-64k 061 ... |
| 10 | no utility.library (no library) | 6 | Demos-AGA 009, Demos-AGA 013, Demos-AGA 032, Demos-AGA 039 ... |
| 11 | missing: intuition.library LockPubScreen (-510) | 5 | Demos-AGA 012, Demos-AGA 017, Demos-AGA 029, Intros-64k 016 ... |
| 12 | no mathffp.library (no library) | 5 | Demos-AGA 097, Intros-64k 024, Intros-64k 027, Intros-64k 041 ... |
| 13 | missing: dos.library FWrite (-330) | 4 | Demos-AGA 004, Demos-AGA 026, Demos-AGA 037, Demos-AGA 046 |
| 14 | data left out of ZINE's volume (folder over 500 entries, 16 MB or half the free fast RAM), then ended with an error code | 4 | Demos-AGA 018, Demos-AGA 019, Demos-AGA 027, Demos-AGA 066 |
| 15 | missing: dos.library GetProgramName (-576) | 4 | Demos-AGA 028, Demos-AGA 071, Demos-OCS 031, Intros-40k 089 |
| 16 | crashed: line F, MOVE16 (a 68040/060 instruction) | 4 | Demos-AGA 075, Intros-64k 031, Intros-64k 059, Intros-64k 073 |
| 17 | crashed: illegal instruction | 4 | Intros-40k 043, Intros-40k 044, Intros-40k 072, Intros-40k 092 |
| 18 | missing: exec.library CreateIORequest (-654) | 3 | Demos-AGA 015, Intros-64k 062, Intros-40k 053 |
| 19 | no mathieeedoubbas.library (no library) | 3 | Demos-AGA 021, Demos-AGA 060, Demos-AGA 091 |
| 20 | missing: dos.library ReadArgs (-798) | 3 | Demos-AGA 034, Demos-OCS 041, Intros-40k 008 |
| 21 | no mathieeedoubtrans.library (no library) | 3 | Demos-AGA 036, Demos-AGA 078, Intros-40k 019 |
| 22 | missing: graphics.library SetChipRev (-888; ZINE printed no name) | 3 | Demos-AGA 085, Intros-64k 019, Intros-40k 039 |
| 23 | missing: exec.library AddPort (-354) | 3 | Demos-AGA 093, Demos-OCS 069, Intros-40k 026 |
| 24 | ended early with return code 1 | 2 | Demos-AGA 035, Demos-AGA 051 |
| 25 | missing: intuition.library RethinkDisplay (-390) | 2 | Demos-OCS 082, Intros-64k 079 |
| 26 | missing: graphics.library SetAPen (-342) | 2 | Demos-OCS 096, Intros-64k 064 |
| 27 | crashed: line A instruction | 2 | Intros-40k 012, Intros-4k 020 |
| 28 | missing: exec.library -912 (private; ZINE printed no name: a call through a null base?) | 2 | Intros-40k 055, Intros-40k 081 |
| 29 | missing: graphics.library GetBitMapAttr (-960; ZINE printed no name) | 2 | Intros-4k 085, Intros-4k 086 |
| 30 | back without a ZINE reason line (rc 1) | 1 | Demos-AGA 033 |
| 31 | missing: dos.library Lock (-84) | 1 | Demos-AGA 053 |
| 32 | missing: intuition.library QueryOverscan (-474) | 1 | Demos-AGA 077 |
| 33 | ended early with return code -1 | 1 | Demos-OCS 049 |
| 34 | missing: intuition.library DisplayAlert (-90) | 1 | Demos-OCS 054 |
| 35 | ended early after: no file (data file not found) | 1 | Demos-OCS 070 |
| 36 | missing: intuition.library ActivateWindow (-450) | 1 | Demos-OCS 085 |
| 37 | missing: intuition.library DrawBorder (-108) | 1 | Intros-64k 003 |
| 38 | missing: dos.library DateStamp (-192) | 1 | Intros-64k 025 |
| 39 | missing: intuition.library AllocRemember (-396) | 1 | Intros-64k 054 |
| 40 | crashed: line F, another instruction | 1 | Intros-40k 023 |
| 41 | missing: intuition.library RemakeDisplay (-384) | 1 | Intros-40k 070 |
| 42 | missing: intuition.library AllocScreenBuffer (-768; ZINE printed no name) | 1 | Intros-4k 057 |
| 43 | missing: graphics.library BestModeIDA (-1050; ZINE printed no name) | 1 | Intros-4k 084 |

Pass-2 outcomes: running (picture, changing) 12, running (picture, unchanged) 1, running (black screen) 2, ended cleanly (code 0, or after 60+ s) 15, ended (error code) 25, missing call 91, crashed 11, stopped (other) 63.

## After the fixes (2026-11-07, branch zinefix, d29 16.12)

The blockers above were fixed in this order: exec `SuperState`/`UserState`; memory (zine loads a program whose file
and hunks do not fit together from its file itself; the volume's rule: half the fast RAM the program leaves free,
archives and then the largest files left out and named, instead of all or nothing); the cruncher's overlay form;
the missing calls (IsInteractive, OpenFont and text, RawIOInit/RawMayGetChar/RawPutChar, utility.library,
LockPubScreen, the six math libraries, FWrite, GetProgramName, ReadArgs, CreateIORequest, AddPort, SetChipRev,
GetBitMapAttr, BestModeIDA, AllocScreenBuffer, and more of the same families) with every stopping call named from
the NDK's `.fd` files; Latin-1 file names. Then the 188 affected productions (every one whose blocker, or whose
lines, named one of these) ran again on both machines, with the same harness and limit. *Verified* in FS-UAE.

**How many run now** (running at the limit, or ended cleanly: return code 0 or after 60 s and more):

| Machine | Before | After |
|---|---|---|
| A1200, 68EC020, 2 MB chip + 8 MB fast (pass 1) | 206 | 263 |
| A1200, 68030 + 68882, 2 + 16 MB (pass 2; a production that did not run again there counts with its pass-1 result) | 244 | 312 |

Of the 426 productions with an AmigaOS executable, 263 run on the stock-style A1200 and 312 on the bigger machine.
None that ran before stops now: three that ended at once before (no math library, an error code 20) now get
further and stop at a call ZINE still lacks (Massive: `Allocate`; Trauma: `QueryOverscan`; Sunglasses at Night:
`Lock`).

What became of each fixed blocker (each production under its old blocker; "further": it now stops at another one):

Pass 1 (68EC020, 8 MB):

| Blocker before | Productions | Run or end cleanly now | Same blocker | Further, another blocker |
|---|---|---|---|---|
| SuperState | 18 | 18 | 0 | 0 |
| overlay form | 15 | 12 | 1 | 2 |
| cannot be read | 14 | 0 | 0 | 14 |
| does not fit | 49 | 4 | 34 | 11 |
| could not be loaded | 14 | 0 | 13 | 1 |
| IsInteractive | 6 | 2 | 0 | 4 |
| OpenFont | 8 | 7 | 0 | 1 |
| RawIOInit | 2 | 2 | 0 | 0 |
| LockPubScreen | 1 | 0 | 0 | 1 |
| FWrite | 3 | 3 | 0 | 0 |
| GetProgramName | 4 | 2 | 0 | 2 |
| ReadArgs | 3 | 3 | 0 | 0 |
| CreateIORequest | 2 | 0 | 0 | 2 |
| AddPort | 3 | 0 | 0 | 3 |
| SetChipRev | 3 | 2 | 0 | 1 |
| AllocScreenBuffer | 1 | 0 | 0 | 1 |
| TextLength | 1 | 0 | 0 | 1 |
| SetAPen | 1 | 0 | 0 | 1 |

Pass 2 (68030 + 68882, 16 MB):

| Blocker before | Productions | Run or end cleanly now | Same blocker | Further, another blocker |
|---|---|---|---|---|
| SuperState | 20 | 20 | 0 | 0 |
| overlay form | 15 | 13 | 1 | 1 |
| cannot be read | 13 | 1 | 0 | 12 |
| does not fit | 19 | 2 | 15 | 2 |
| could not be loaded | 15 | 0 | 15 | 0 |
| IsInteractive | 12 | 6 | 0 | 6 |
| OpenFont | 8 | 8 | 0 | 0 |
| RawIOInit | 8 | 7 | 0 | 1 |
| LockPubScreen | 5 | 0 | 0 | 5 |
| FWrite | 4 | 4 | 0 | 0 |
| GetProgramName | 4 | 0 | 0 | 4 |
| ReadArgs | 3 | 3 | 0 | 0 |
| CreateIORequest | 3 | 0 | 0 | 3 |
| AddPort | 3 | 0 | 0 | 3 |
| SetChipRev | 3 | 1 | 0 | 2 |
| GetBitMapAttr | 2 | 0 | 0 | 2 |
| BestModeIDA | 1 | 1 | 0 | 0 |
| AllocScreenBuffer | 1 | 1 | 0 | 0 |
| SetAPen | 2 | 1 | 0 | 1 |

- `SuperState` was the whole story for its 20: all run now. The cruncher's overlay form: 13 of 15 run, Animotion
  (a truly truncated file) stays refused, one gets further.
- **Memory:** the 7-26 MB executables no longer stop at "cannot be read"; on these test machines most still do not
  fit (their hunks alone are 7-26 MB), and some find no free fast block in one piece for their largest hunk (Zenix's
  memory, 8 + 8 MB in two boards on the 030 machine). A real 1260 with 64 MB is where they belong. "Does not fit"
  now means the hunks alone are too big.
- The demos whose data was left out of the volume (Starstruck, Ocean Machine, Silkcut, Rift, Lightshaft...: 10-35 MB
  of `.dat`/`.wav` files) now get past the calls they lacked and stop at the FPU on the 020 or at their data on
  smaller machines; zine names each file left out.

**What blocks the most now** (the full tables are in the results folder's `summary.md`): on the stock-style A1200
the machine itself (no FPU: 37; hunks larger than its memory: 47; not enough memory in one piece: 23); on the
bigger machine the next ZINE gaps are graphics `GetVPModeID` (9), dos `Lock` (6), exec `Allocate` (6),
intuition `QueryOverscan` (5), graphics `VideoControl` (4), intuition `RethinkDisplay` (3), 4 MOVE16 (68040/060
only), and 29 black screens at the limit (still in precalc, or hung: not yet looked at one by one).

## Round 3 (2026-11-10, branch zine2, d29 16.13)

Fixed in this round: the calls the bigger machine stopped at after round 2 and their neighbours (exec `Allocate`,
dos `Lock` and the lock and directory calls, `DateStamp`, `FilePart`, `PrintFault`; graphics `GetVPModeID`,
`VideoControl`, `BltClear`, `ScrollVPort`, `FreeSprite`, `GetColorMap`, `WaitBOVP`, `AllocDBufInfo`, `WritePixel`
and a View of the program's own; intuition `QueryOverscan`, `RethinkDisplay`, `RemakeDisplay`, `MoveScreen`,
`WBenchToFront`, `ActivateWindow`, `DisplayAlert`, `EasyRequestArgs`, `AllocRemember`, `DrawBorder`; exec
`FindSemaphore`, `SumKickData`), diskfont.library with Zenix's /usr/share/fonts/amiga, a disk's name taken as the
program's folder, the math libraries on the FPU, timer.device's units. Disk-image-only productions are boot disks,
not ZINE targets, and are not counted as failures.

**Targeted reruns** (the 68030 + 68882, 16 MB machine, the same harness, limits of 50-120 s of PC time): of the 47
productions round 2 left stopped at a missing call and the 29 with a black screen at the limit (68 productions, some
in both), **38 now run or end cleanly** (Love, sumea, cathode, superkewl, supercool, Trauma, Klone, Neonsky, Last Train
To Danzig, switchback, New World Order, Fortocalypse, Rubbur, blumen kraft, Second Rodeo, Colombia, D-Funk, Undated,
Inside the Machine...).

**The full rerun** (all 500 productions, both machines, the same harness; the 68030 pass ran the blocked ones again,
the 68020 pass the whole list; "before" is round 2's result, "after" is this round's):

- **68EC020, 8 MB** (FS-UAE A1200, 2 MB chip + 8 MB fast): ran or ended cleanly **242 before, 259 after**.
- **68030 + 68882, 16 MB**: ran or ended cleanly **285 before, 311 after** (where round 2 had no result for a
  production, its 68020 result is used).
- **Boot disk, not a ZINE target: 73** (the same on both machines; not counted as failures).
- **Top 10 blockers on the 68030** (productions each one stopped):
  1. black screen at the limit, still in precalc or hung (29)
  2. could not be loaded: not enough memory (18)
  3. does not fit in memory, file plus hunks bigger than the free RAM (18)
  4. ended early with return code 1 (8)
  5. ended early, a data file not found (7)
  6. could not be loaded from its file: not enough memory in one piece for its hunks (7)
  7. crashed: line F, MOVE16 (a 68040/060 instruction) (7)
  8. crashed: illegal instruction (6)
  9. missing: exec.library `execPrivate33` (-912) (2)
  10. crashed: line F, another instruction (2)

**The black screens, one by one** (the trace blob: the calls, the names opened, the PC and its bytes every 5 s):

| Production | What it was | ZINE's fault? |
|---|---|---|
| Love, sumea (Virtual Dreams) | Open of `Love:...`/`sumea:...` (their disk's name) in an endless retry | yes: fixed (the disk's name is its folder); Love plays to its end |
| cathode (Unique), superkewl, supercool (Supergroup); Trauma (Unique) | precalc through the math libraries, in software on a 68882 machine | yes: fixed (FPU); all end cleanly |
| Second Rodeo, Colombia, D-Funk, Undated, Doodle-Doo, Inside the Machine | precalc longer than the 68020 run's limit | no: they run on the 68030 (Second Rodeo after 4,067 s emulated) |
| Software Failure's 7 intros (Electromathesis, Antelogium, Insomniac Contemplation, Ephemeral Permanence, Dual Panoply, Binary Roads, Blue Artifact) | "Please wait while I count electric sheep...": precalc in their own integer loops (interrupts off after Disable), still going at 120 s | no: precalc made for 68060s |
| Point of No Return, Cruisin', Bantam, Hex Junkie, Tunnel Vision, 4k-Wavy | their own decrunch or precalc loops with their own interrupts | no (as far as seen): still busy at the limit |
| Fetish (Ozone) | OldOpenLibrary("CrM.library") in an endless retry: the folder does not carry it | no: needs CrM.library in its LIBS |
| Beam Riders, Signals (Haujobb/Ghostown), Serenity (Elude), Deep, lux aeterna | loading hundreds of files and precalc, with their music or data left out of the volume (7-12 MB) | no: the test machine's memory |
| J (Ephidrena) | waits in `Wait` for a signal its own interrupt code should send (after a CIA resource vector and a device request) | not settled |
| Lola (Fresh Prince) | its own loop right after `AllocAbs`, interrupts on | not settled |
| total triple trouble (Rebels) | took the machine at once (no calls at all) | not settled |
| killer (Carillon & Cyberiad) | now a crash: illegal instruction | not settled |

**What blocks the rest** (of the reruns): data left out of the volume on this machine (Elude's demos, 7-24 MB of
`.dat`/`.wav`), 4 MOVE16 (68040/060 only), dos `CreateNewProc` (a second task: Dust), graphics `UCopperListInit`
(user Copper lists: The Morning Trip), a call through a null base (Gift), cybergraphics.library (Shake Off The Dust),
and the black screens above that are not ZINE's. Alcatraz's Odyssey shows an alert "Execbase in chip memory !" and
quits: ZINE keeps ExecBase at $800 by design.

## Round 4 (2026-11-11, branch zine4, d29 16.14)

**The early quits on the 68030** (the 50 productions round 3 left "ended by itself" with an error code; traced with the
trace blob, the program's own output read from the logs, which the harness's result lines leave out):

| Cause | Productions | ZINE's fault? |
|---|---|---|
| ended after its full run (82-2,849 s emulated) with a return code left in d0: Ikanim, the tetris intro and Gagrakacka traced to their normal end (the View put back, the libraries closed); lapsuus says "Thank You."; the 4k and 40k intros of Ephidrena, Spaceballs, Loonies, Razor 1911, Passion, Dual Crew Shining, TBL and Logicoma by their length, ZINE saying nothing | 31 | no: clean ends |
| memory on the 16 MB test machine: AllocMem of 6-49 MB refused (Metropolice, Swansong, Kioea, Fetish 2), parts of 3-4 MB not loadable (Tint, Captured Dreams, Norwegian Kindness), data files of 2-15 MB left out of the volume (Elude's five, Chips don't lie), more than 500 files and 21 MB (Dolor Sit Amet) | 14 | no: the machine's memory (ZINE now says the refused AllocMem) |
| "Processor: 68030 [68040+ needed]" (Capsule's Tornado: Renouncetro, Brutalism) | 2 | no: they run on the 68060 now |
| The Juggler: console.device, then intuition ViewAddress | 1 | yes: fixed; it shows its text and waits for a key (ZINE gives programs no keys) |
| HAM Eager's HD loader reads its own .adf, which the harness's staging left out | 1 | no: the harness (staging now keeps a disk image the program names; it then plays 860 s to a clean end) |
| Human Target: its "executable" is an installer's slave, not a program | 1 | no |

Fixed in ZINE: console.device on a window, intuition ViewAddress, input.device (seen when TBL's Ocean Machine got
enough memory on the 68060: it then plays 2,148 s to its end), and the first refused large AllocMem said with the
largest free block.

**The 68040/060 FPU** (d29 16.14): Motorola's packages emulate what the CPU lacks. The test machine was the cause of the
"no FPU" crashes in round 3b: FS-UAE's `uae_cpu_model = 68060` alone is the EC/LC 68060 (PCR $0431); `cpu = 68060`
with `fpu = 68060` (or `uae_fpu_model = 68060`) is the full part, its FPU switched off in PCR at the start as on a real
68060 before 68060.library. The harness's 060 machine is now that, with 64 MB of fast RAM like the Apollo 1260.
Targeted reruns on it (60 s of PC time): of 8 productions that crashed on an FPU instruction on the 68020 and ran on
the 68030 (Starstruck, Ocean Machine, 021, 039 of the AGA demos; 4ks 007 and 010; 64k 025; 40k 053), none crashes:
7 run or play to their end (Ocean Machine 2,148 s), Starstruck stops at an AllocMem of 48 MB (43 MB free in one
piece); Renouncetro and Brutalism (Tornado, 68040+) run. Not yet on a real 68060.

### Round 4's full rerun (2026-11-11, main 2026111101's code, all 500 on three machines, 50 s each)

Counted with the harness's own rule (running with a picture at the limit, or ended cleanly); the 74 boot disks and
non-programs are left out, so each machine is out of 426:

| Machine | Before round 4 | After |
|---|---|---|
| 68EC020, 8 MB fast | 257 | 258 |
| 68030 + 68882, 16 MB fast | 311 | 312 |
| 68060 with its FPU, 64 MB fast (new) | - | 348 |

Round 4's early-quit work mostly showed the old counts were already right for the 020/030 (the "quits" played to
their end; the rule counts a run of 60 s or more as clean). The new 68060 machine runs 36 more than the 030, from the
memory and from the CPU support packages. Three productions changed for the worse between runs (Elysium on the 020,
The Fall and No Sleeping! on the 030: a line F crash and a black screen where the round before had them working): to
be checked, likely timing. Top blockers on the 68060: black screen at the limit 42 (6 of them work on the 030: look at
the 060's copyback data cache and chip RAM), illegal instruction 9, dos CreateNewProc 5, early return codes 5, line A 2,
exec -912 2. On the 030: black screen 30, memory 43, illegal instruction 9, MOVE16 7, data file not found 6.

## Round 5 (2026-11-11, branch zine5, d29 16.15)

Targeted reruns only (the same harness; the 68060 machine with its FPU and 64 MB unless said; traced runs with the
trace blob). The full 500 rerun is still to do.

**The 68060's 42 black screens** (run again with 120 s of PC time instead of 50): **14 now run or end cleanly**,
none of them through a ZINE change: Nexus-7, A500 Homage and Talking Heads show their picture; Ikadalawampu,
Luminagia, Noxie (Loonies), Second Rodeo (Fnuque), Discotto, Tribute to Acid (Nature), Higher State of Resolution,
superkewl and three of Software Failure's intros (Supahfunkers, Umbilicus Mundi, Binary Roads) end after 2,300-4,600 s
emulated. The guess that the 060's copyback data cache hid the CPU's chip RAM writes is wrong: ZINE runs programs with
the data cache off, Zenix's DTT0 keeps the first 16 MB uncached even when a program switches it on, and FS-UAE does
not model that cache against the chips at all (d29 16.15, the `zcache` test). The six that "worked on the 68030":
Silkcut and Little Nell only ended cleanly there because a 28 MB / 14 MB AllocMem was refused (on the 68060 they get
it and are still loading at 120 s); superkewl (ends now) and Supercool precalc for 734 and 1,933 s emulated; A500
Homage and Talking Heads run (6 of 6 and 3 of 4 reruns at 50 s; Talking Heads' picture comes near the 50 s limit).

**The 28 still black at 120 s**, compared with the 68030's results and shots: precalc (the program's own loops, no
calls, as on the 68030): Software Failure's Electromathesis, Omnimetatheorem, Antelogium, Insomniac Contemplation,
Ephemeral Permanence, Dual Panoply, Blue Artifact; Supercool; cathode and Trauma (Unique); Hex Junkie and Tunnel
Vision (Fnuque); Point of No Return, Cruisin', Bantam, 4k-Wavy, gush. Loading or memory: Silkcut, Little Nell, lux
aeterna, Twenty, Deep, Chips don't lie. Not settled (as in round 3): J (Ephidrena), Lola, total triple trouble, Fetish
(CrM.library), The Fall (below).

**A second process** (dos CreateNewProc, 5 on the 68060, all Elude's: a sound process): now refused with AmigaOS's
answer (0, IoErr 103) and said. Soliloquy plays 2,235 s to a clean end; Dust, Serenity, kakao and Shake Off The Dust
run with a picture to the 120 s limit (silent). Lightshaft, which said "This program requires a math co-processor" on
the 68060, now loads (AttnFlags say the FPU); Machinist stops at an AllocMem of 11.7 MB (9 MB in one piece).

**The data files not found on the 68030** (6): Elude's We come in peace, Lightshaft, Soliloquy and Machinist, and
Ephidrena's Chips don't lie: their 12-15 MB `.dat` archives (and wavs) are left out of the volume on the 16 MB
machine, so their files are missing: memory, not ZINE (on the 68060 the first four get their data). Dolor Sit Amet
has 671 files and folders, over the volume's 500 entries, so only the program went in: the limit is now 2,000; on the
68030 its 21 MB still do not fit, on the 68060 they do.

**Illegal instruction, line A, exec -912** (traced on the 68060): Cobalt (Paradox) wrote MOVEC CAAR, which a
68040/060 lacks: now a no-op, and it runs. deeppan opens gadtools.library (ZINE has none) and then calls through a base
that is no library (exec's private -912 slot): needs gadtools. Gift needs ahi.device ("Couldn't open ahi.device") and
crashes after. Not settled (each crashes in its own code, mostly after a long run; crash PCs reading `454D xxxx` are
the tail of a Zenix memory header, "ZMEM", i.e. a jump into free memory): Psychedelic (after 342 s emulated),
Virtual Dreams' 40k intro (CHK, illegal or line F in chip RAM, a different place each run), Leisner, Cubic
Meditation (after "Calculating cubic capacity", 482 s), scifi (inside its own level 3 interrupt, at $82), SoftWired,
1000%, Piramida (line A), My Kingdom (line A at $A), Cebit 90 (a jump to 0 after 890 s). The three that "work on the
68020 and crash on the 68030" (Cebit 90, Psychedelic, the 40k intro) crash the same on a 68030 with only Zorro II RAM
and with no FPU, so it is not 32-bit RAM or the FPU; traced, they crash after 300-900 s emulated, beyond where the
68020's 50 s runs stopped. Superautodrome ran 292 s to its end on one 68030 run and crashed on another: flaky.

**The three that did worse in round 4's rerun** (each rerun 3 times, this branch's code): Elysium (68020) crashes
with line F at $130 every time; rounds 1 and 2 crashed at the same address (line A), only round 3's one run reached
the limit: a crash that is usually there, not a regression. The Fall (68030) runs to the limit every time with a
picture early and a black screen late; round 3's "clean end" was a return after 0 s: it runs further now, the black
part not settled. No Sleeping! (68030) crashes every time, at a different place each run (line F, address error,
illegal instruction); round 2 crashed too, round 3 once ran: an unstable crash in the intro's own code, not a
regression.

## Round 6 (2026-11-12, branch zinetask, d29 16.16)

Targeted reruns only (the same harness, `OUTBASE` a folder of its own; the 68060 machine with its FPU and 64 MB,
the trace blob, 120 s of PC time). The full 500 rerun is still to do.

**Elude's five now run their sound process.** ZINE switches the program's tasks (d29 16.16), so dos CreateNewProc
gives the "14bit" sound process its own task; the engine loads its CyberSound `14bit.driver` (from the demo's
folder), opens audio.device for the four channels and plays on Paula itself, with its own level 4 handlers
(SetIntVector). How it was checked: the trace build writes, every 250 frames, DMACONR, audio.device's own counts,
the task switches, ThisTask and the level 4 interrupts it dispatched (an `A` line); the demo's own lines say
"[Sound]: Create process:14bit", "Created!", "Started!" and, at the music's end, "Flushing buffers... Done.
Finished!". All five: the process made and started, task switches counted, the four audio channels' DMA (DMACONR
`$000F`) seen on in the samples, the driver's AUD3 interrupt dispatched about 18 times a second. Serenity reaches
its "THE END" screen; Soliloquy ends by itself after 3,351 s emulated; Dust's sound process plays its whole music
and ends ("Finished!") after 845 s emulated.

**But in FS-UAE the sound comes in bursts:** the channels' DMA is on in about 30 % of the samples (Dust; 4-5 % in
Serenity, Shake Off The Dust and kakao). Read from Dust's code (its symbols: `snd_wav_read_msadpcm`, `SoundPlay`,
`SoundReply`): the process decodes the MS-ADPCM `.wav` into 8 KB buffers (2,048 stereo frames at 27,429 Hz, 75 ms
each) and gives each to the driver; the driver's interrupt calls `SoundReply`, which signals the process for the next.
Measured in the trace: 2,987 buffers in 820 s emulated, 3.6 a second where the music needs 13.4. The process never
waits (each Wait finds its signal already there) and has the CPU to itself (ThisTask is it in every sample; the first
task, of lower priority, waits in TaskReady): the emulated 68060 decodes the music at about a quarter of real time,
so the driver runs dry and stops between buffers. That is the emulated CPU's speed (FS-UAE's 68060 in the harness's
cycle-exact set-up; `accuracy = 0` with `uae_cpu_speed = max` in a copy of the harness changed nothing measurable),
not the task switching. A real 68060 at 50 MHz is several times faster; it has not been tried (the Apollo 1260 has
not run ZINE's 68060 package yet either).

## Round 7 (2026-11-12, branch zine7, d29 16.17)

Targeted reruns only (the same harness in a folder of its own; traced runs with the trace blob; keys pressed on the
emulated keyboard where a demo waits for one). The full 500 rerun is still to do.

**Input, gadtools and the screens:**
- **The Juggler** (68020): shows its text, starts its HAM movie when a key is pressed (console.device's read) and
  ends at Esc with return code 0. The movie needed intuition's View's Copper lists (d29 16.17: it double-buffers by
  keeping two lists RemakeDisplay made and switching `View.LOFCprList` and `RasInfo->BitMap` each frame; showing the
  RasInfo's BitMap showed the frame being decoded) and `FreeCprList` at its end.
- **J** (Ephidrena, 68060): was black to the limit; its vertical-blank code signals the main task only while
  IntuitionBase's `FirstScreen` is its screen. ZINE keeps the screens' order now: part 1 ends by itself after 253 s
  emulated (return code 0). The demo's other parts (`EXECUTE_ME` runs part2 and fades) are not part of the harness.
- **deeppan** (Efreet, 68060): opens its window through gadtools and runs to the limit (a 640 x 256 screen, drawing
  each frame; black at 300 s in FS-UAE, its own slow rendering on the emulated 68060). Before: a crash through base 0.

**Round 5's unsettled ones, rechecked (traced):**
- **Lola** (68020): after AllocAbs and its unpacking it waits in `STOP` at `$6080` for a flag its own interrupt code
  sets; ZINE's level 3 vector is still in place (the trace's samples come from it), so its handler was never
  installed where the CPU looks. Not settled.
- **total triple trouble** (68020) and **The Fall** (68030): make no call after the first ones (The Fall's last is
  `Supervisor`) and run their own interrupts; nothing of ZINE's runs (no trace samples). Black late is their own.
- **Fetish** (Ozone, 68060): loops on `OldOpenLibrary("CrM.library")` (Crunch-Mania's decruncher library, not in
  its folder). Not ZINE's; it runs with the library in its LIBS:.
- **Gift** (Potion, 68060): needs AHI (an `ahi.device` mode requester and a mixing player); without it says
  "Couldn't open ahi.device" and its cleanup calls offset -66 through a base of 0. An AHI is a project of its own;
  ZINE keeps answering `IOERR_OPENFAIL`.

**The crashes after minutes (traced):** most are **calls through a library base of 0**: `jsr -N(a6)` with a6 = 0
jumps to `$FFFFFFxx`, the CPU wraps to 0 and runs the vector table as code until an illegal instruction (`$0A`,
`$66`, `$82`, `$D2`, "instruction 0000 0000") or into exec's jump table below ExecBase (then read as a call of exec's
offset -912). The trace's R line (USP and the stacked return address) names the call. **My Kingdom** part 1 runs its
exit routine twice: the first closes graphics and clears its GfxBase, the second calls LoadView through it (its own
code; the return address on the stack is that call). **Cebit 90** and **scifi** stop at exec -912 the same way (a
jump to 0), **Cubic Meditation** (68030) jumps into free memory (line F at a `ZMEM` header). No ZINE cause found in
these; the rest of round 5's list (Psychedelic, the 40k intro, Leisner, SoftWired, 1000%, Piramida) not traced
this round.

**Spot checks (this branch's final code):** on the 68020 Interparallactic, Paul the Kinder Penguin, Way Too Rude and
Griffin end by themselves as in the rerun (205, 208, 286, 252 s emulated); on the 68060 Soliloquy plays to its end
(3,375 s), Dust, Shake Off The Dust and Cobalt run to the limit, Dust's sound process says "Started!" and
"Finished!". On the way, keeping IntuitionBase's `FirstScreen` made Elude's 14bit driver ask graphics'
`GetDisplayInfoData` for the front screen's monitor (it found no screen before and skipped it): the display
database's calls came with this round (d29 16.17).

### Round 7's full rerun (2026-11-12, main 8b8b06d's code, all 500 on three machines, 50 s each for 020 and 030, 120 s for 060)

Counted with the harness's own rule (running with a picture at the limit, or ended cleanly); the boot disks and
non-programs are left out, so each machine is out of 426:

| Machine | Before round 7 | After | Regressions |
|---|---|---|---|
| 68EC020, 8 MB fast | 261 | 259 | 4 |
| 68030 + 68882, 16 MB fast | 314 | 314 | 1 |
| 68060 with its FPU, 64 MB fast | 362 | 359 | 12 |

The 68060 regressions are the ones to look at first: 11 of the 12 ran to their end (return code 0) in the last
rerun and now crash (five on an illegal instruction) or stop on a missing call. Whether this is round 7's change or
the 68060 setup is not yet investigated. The full
breakdown is in `~/Amiga/Top100-zine/summary.md` under "Round 7: full rerun".

## Round 8 (2026-11-12, branch zine8, d29 16.18)

**Round 7's regressions, explained.**
- **68060 (12):** not round 7's doing. The 68060's "before" was round 4's rerun, made before round 5 made AttnFlags
  say the FPU (d29 16.15). Eleven of the twelve had then printed why they quit and ended with 0, which the counting
  rule takes as working: Jesus Christ Motocross "error: fpu required (if you have 040/060, run setpatch first)", Beam
  Riders (both copies) "68020+ and FPU required!", Impossible "FPU required", The Morning Trip "FPU required, try the
  (bigger) nonfpu version!", The Martini Effect, Sojourn of the Soul, Dead Ahead, Hexel and Knarkzilla "FPU: None ...
  These were not detected on your system", Lupus Reditus "reqires minimum MC68882 FPU". Now they get past the check and
  meet ZINE's next gaps: lowlevel.library (Sojourn, Dead Ahead: a call through base 0), sprites (Impossible:
  AllocSpriteDataA), user Copper lists (The Morning Trip: UCopperListInit), and crashes after long runs (below).
  We come in peace (the twelfth) was "ran until the limit" before and stopped on a call through base 0 after 4,743 s
  emulated in round 7; it works again in this round's rerun.
- **68020 (4):** Rubbur was never stable on the 68020 (privilege violation, return code 1, a reset into Zenix in
  rounds 1-7; traced twice this round: "Could not open display" once, a jump into the ROM at $F8000A the other time;
  it runs to its end on the 68030 and 68060). HAM Eager crashes at a different low address each untraced run and ran
  without a crash when traced: timing, not settled. Dual Panoply missed a 2,349 KB AllocMem by 13 KB after round 7's
  blob grew: fixed by giving the program the ZINE block's unused end (about 180 KB). destroy fascism: below.
- **68030 (1):** Boogietown's "Zenix did not boot to its prompt" was the harness (an empty serial log after 530 s, as
  for Demos-AGA 099 in the same rerun); rerun below.

**Fixed in ZINE (d29 16.18):** lowlevel.library (Flex's and Ephidrena's demo system: Software Make The Dance Foam
Oil and fake elektronik lightshow now run to their end on the 68060, about an hour emulated each, where they quit
after 70-90 s; Sojourn of the Soul gets through its loading), dos CompareDates (Reborn), the DOS list and
GetDeviceProc (THE CURE), user Copper lists (The Morning Trip), the ZINE block's unused end given to the program
(about 180 KB on the 68020 machine), a big plain program's file no longer kept beside its hunks (the 68020's 3-4 MB
AGA demos), a faster copy for Read and CopyMem, RectFill a byte at a time (deeppan).

**The black screens at the limit (the 68060's 34, traced at 120 s).** No common ZINE cause. About 24 take the machine
(their own level 3 vector: the trace's sampler goes quiet) and compute past the limit: Software Failure's seven 64k
intros (Forbid, Disable, Supervisor, then precalc), Luminagia, Hex Junkie, Tunnel Vision, Cruisin', Point of No
Return, Bantam, 4k-Wavy, Cracktro 4k, Lola, scifi, Kilofix, Twenty, Massive, Deep, No-CPU Challenge, The Fall, total
triple trouble. The rest spend the time in ZINE's calls or loops of their own: loading in small pieces (The Martini
Effect: a Read and a thousand CopyMem calls), 900,000 mathffp calls in a decrunch (gush), RectFill every frame
(deeppan), a loop re-opening CrM.library (Fetish: not in its folder), out of memory while loading (Dolor Sit Amet:
an AllocMem of 129 KB with 127 KB in one piece), a string-compare loop after its CIA set-up (Silkcut, not settled),
the sound process waiting on the emulated CPU (Shake Off The Dust, round 6). The limit also decides what the late
screenshot shows: No-CPU Challenge has a picture at 50 s on the 68020 and is black at 120 s on the 68060.

**Crashes after long runs (68060):** Beam Riders (both) at $100 in its own level 3 interrupt code at the end of the
demo (the supervisor stack holds its saved d2-d6; d1 = $100: a jump through a computed pointer); Jesus Christ
Motocross a CHK at $0A at 3 % of its precalc, Lupus Reditus line F at $0A after 2,129 s, Hexel at $5FE after 1,537 s:
calls through a base of 0, not traced to their call this round.

### Round 8's reruns (2026-11-12, branch zine8's code, the same harness: 50 s for the 020 and 030, 120 s for the 060)

Not a full rerun: on each machine every production that did not work in round 7 (on the 68020 without the 47 that
do not fit in its 10 MB and the 42 that need an FPU it lacks), 15 working ones picked at random, the lowlevel and DOS
list users, and every production whose file is big enough for the new path-B rule; 377 runs, laid over round 7's
results and counted with the same rule (boot disks and non-programs left out, out of 426):

| Machine | Round 7 | Round 8 | Now working | No longer working |
|---|---|---|---|---|
| 68EC020, 8 MB fast | 259 | 262 | Smoke Bomb (its 3.3 MB file no longer kept beside its hunks; plays 164 s to its end), Cyboman 6 (black at the limit before), fake elektronik lightshow (see below) | none |
| 68030 + 68882, 16 MB fast | 314 | 316 | Boogietown (the harness), Cebit 90, Fullmoon, Binary Roads (black at the limit before) | sumea, cathode (see below) |
| 68060 with FPU, 64 MB fast | 359 | 366 | We come in peace, Sojourn of the Soul (lowlevel.library), Reborn (CompareDates), Norwegian Kindness, Kilofix, Luminagia, gush (black or a crash before) | none |

How far to trust the 50 s and 120 s limits: the harness runs FS-UAE in warp mode, so the emulated time a run gets in its
limit depends on how busy the PC is (the load was 15-17 all evening, other sessions' emulators included). Several of
the "now working" black screens are that, and so are the two the 68030 lost: sumea now shows its tunnel part in the
early shot and is between parts (black) in the late one, where round 7 showed its first effect and then its title;
cathode, which round 7 saw end after 2,263 s emulated within 73 s, had not ended at the limit. fake elektronik
lightshow counts as working on the 68020 only because it ends with return code 0: its music file was left out of the
volume (it says "no file musikk/p61.dhreph2"), so it quits at once. Dual Panoply gets its 2,349 KB now and is in its
precalc (black) at the limit, as on the 68030 and 68060. The Morning Trip gets past UCopperListInit and stops at
AllocSpriteDataA (sprites), as Impossible does.

Path B for big plain programs (d29 16.18) is limited to hunks that land below 16 MB: on the 68030 machine, where
Zenix's allocator gave the hunks Zorro III RAM, it had made four more work (lux aeterna, You are Lucy, Outside Planet
Earth, Concrete: their AllocMem got the file's memory) and sumea lose its picture; with the limit they are as in
round 7 there. Giving those four the memory without moving old programs into 32-bit RAM needs Zenix's allocator to
give a low block on request (a kernel change, for the main session).

Top blockers now: 68020: does not fit (47), FPU instructions (42), black at the limit (23), not enough memory (15);
68030: black (31), not enough memory (18), does not fit (18), illegal instruction (8); 68060: black (31), illegal
instruction (11), return code 1 (3), sprites (2), calls through base 0 (exec -912, 2).

## Round 9 (2026-11-13, branch zine9, d29 16.19)

**Fixed or added in ZINE:** low memory on request (the kernel's `MEMA_LOW`; path B's hunks below 16 MB when they fit);
the line that sends a big plain program to path B, lost in round 8's last commit, is back, now only when memory is
short (file and hunks leave less than 16 MB); hardware sprites (ChangeSprite, MoveSprite, AllocSpriteDataA,
GetExtSpriteA, ChangeExtSpriteA, FreeSpriteData); dos DoPkt and the handler's lock and volume actions; 64 open files
(was 16).

**The null-base crashes after long runs, traced (68060):**
- **Hexel**: not a null base at first. It opens its precalculated tables and does not close every one; after 16
  handles every Open failed ("Unable to open ram:packscript for write or append"), its loader went on with null
  pointers and crashed 25 minutes later at $5FE. Fixed by the 64 handles; it now runs until the limit.
- **Jesus Christ Motocross**: no library call between its last Flush and the crash (CHK at $0A, a jump into the
  vector page) during its precalc's sine and square-root tables; the same instructions in a test program run
  correctly under the 68060 package. Not settled.
- **Lupus Reditus** and **Beam Riders**: both crash right after Forbid and two Supervisor calls, inside their own
  supervisor or interrupt code (Lupus at $4000000C, Beam Riders at $100 with SR $2300); both decrunch into BSS hunks,
  so their code cannot be read from the file. Not settled.
- **destroy fascism (68020)**: an absolute-address intro (it decrunches itself to $50000 and jumps there); at the
  crash its user stack is at $FD0, inside the page where ZINE keeps ExecBase. Not fixed.

**Loonies' Impossible and The Morning Trip** get past the sprites and now print "CIA-timer could not be allocated!"
and end with 0 (not counted as working). They do not call OpenResource or AddICRVector before saying it (a debug build
showed neither); between Forbid and Permit they look for the timer without a library call, so they read a CIA
resource's private fields or the resource list directly. Putting the two CIA resources on ExecBase's ResourceList did
not change it. Not settled.

**Also seen:** Nerve Axis' Relic says "AGA chipset ... Failed" in its own hardware check and ends with 0 (on the 68020
and 68030 machines; GfxBase's ChipRevBits0 has the AA bits); an early quit the counting rule takes as working, in
every round so far.

### Round 9's reruns (2026-11-13, the same harness: 50 s for the 020 and 030, 120 s for the 060)

On each machine: every lean candidate, every production whose round 8 result was a memory, loading, missing-call,
crash or early-end failure (not "does not fit", FPU or MOVE16), 15 working ones picked at random, and the named ones
above; 289 productions (305 runs with the 68060 repeats), laid over round 8's results and counted with the same rule (out of 426). The 68060's lean runs were
repeated with the final rule (memory short). The "before" is round 8's merged result as counted at the start of this
round.

| Machine | Round 8 | Round 9 | Now working | No longer working |
|---|---|---|---|---|
| 68EC020, 8 MB fast | 262 | 262 | none | none |
| 68030 + 68882, 16 MB fast | 316 | 323 | lux aeterna, You are Lucy, Outside Planet Earth, Concrete (low memory and path B), Nexus-7, Norwegian Kindness, cathode | none |
| 68060 with FPU, 64 MB fast | 366 | 365 | THE CURE (DoPkt) | Norwegian Kindness, sumea (below) |

The counting rule gives 324 and 367; The Morning Trip (68030, 68060) and Impossible (68060) are taken out by hand:
they end with 0 after "CIA-timer could not be allocated!". Norwegian Kindness on the 68060 now reaches its end within
the limit and crashes there (address error after 3,057 s emulated, on path A as in round 8, which stopped earlier);
sumea on the 68060 is on path A as before and black at the limit (between parts: timing, as in round 8). Lucy ends
with return code 1 after 314 s, its full length, on the 68030; the counting rule takes that as working.

Top blockers now: 68020: does not fit (47), FPU instructions (43), black at the limit (23), not enough memory (13);
68030: black (28), does not fit (18), not enough memory (15), "not enough memory in one piece" (7), MOVE16 (7); 68060:
black (33), illegal instruction (10), return code 1 (3).

## Round 10 (2026-11-13, branch zine10, d29 16.20)

**The count made honest first.** The harness's `report.py` (beside the results, not in this repository) took a run
that ended with code 0 (or after 60 s) as working even when the program had printed its own failure. Its `reason()`
now reads the program's own lines in the serial log (ZINE's lines left out) and takes the run as failed when one of
these reviewed phrases is among its last 6 lines, or anywhere when it ended within 60 s emulated: "CIA-timer could not
be allocated"; "FPU required" / "requires a FPU" / "requires a math co-processor"; a checklist's "..... Failed";
"needed] : FAILED"; "Requirement test failed"; "not supported" / "does not seem to meet"; "system_start() failed",
"demo_init() failed", "cstart() failed"; "requires N mb fast", "Not enough memory", "out of memory", "Memory error",
"Failed to allocate ... Aborting"; "Can't open X.library", "Could not initialize"; "Can't get image w name",
"error in res_img", "Unknown layout". Never for a run that went on to the limit.

On round 9's merged results this took out:
- **68020** (16): Multiverse, Impossible, the castle, The Morning Trip, Grid 2, Beam Riders (AGA and OCS) (FPU);
  Magia, Perfect Circle, Rift, Little Nell (The Black Lotus' engine out of memory); Relic ("Relic.dat ... Failed");
  live axxion ("Memory error"); Pesso ("AGA not supported."); superautodrome 2 ("requires 4mb fast"); Melba
  ("Can't open asl.library V39").
- **68030** (22): Silkcut, Magia, Perfect Circle, Starstruck, Ocean Machine, Rift, Little Nell (TBL engine);
  Software Make The Dance Foam Oil, Neonsky, Sojourn of the Soul, Dead Ahead, The Martini Effect (the Flex/Ephidrena
  engine's start failed); Relic ("AGA chipset ... Failed"); Multiverse, Impossible, the castle (Loonies' "Not enough
  memory"); The Morning Trip (CIA timer); live axxion (AHI); Pesso; superautodrome 2; Melba; Renouncetro ("[68040+
  needed] : FAILED").
- **68060** (13): Impossible, The Morning Trip (CIA timer); Starstruck, Rift, Brutalism, superautodrome 2 (memory);
  Software Make The Dance Foam Oil, Sojourn of the Soul, Chips don't lie (a data file); live axxion (AHI); For real
  (its speed test); Pesso; Melba.

The screenshots of round 8's runs at the limit were gone (their folder was deleted after round 9), so 24, 28 and 30
of those runs could not be judged again; their black ones are known by number (round 9's own count): 18, 21 and 21.
**Round 9's honest baseline: 68020 246, 68030 302, 68060 354** (of 426; was 262, 324, 367 by the old rule).

**Fixed in ZINE:** the CIA resources' own fields as Loonies' replayer reads them (the in-use mask 40 bytes in, the
resource's Interrupt first in its chain 42 bytes in, the vectors 64 bytes in, a handler written there called);
`ChipRevBits0` `$1F` on AGA (MLISA, as a real A1200 after SetPatch). The trace build dumps the program's memory.

**What the dumps settled** (d29 16.20): Loonies' CIA check (fixed; The Morning Trip's clouds are hardware sprites,
gone in a build with only the null sprite); Relic's AGA check (fixed); destroy fascism (the 68020's instruction
cache, not memory: a 1993 intro copying code without a flush; runs with the cache off; a question for the owner);
Lupus Reditus (plays its whole show and its end part, then its own timeline walk runs off its table about 7 minutes
later: its own bug; screenshots show the show). **Not settled:** Beam Riders on the 68060 (a jump to `$100` from a
callback in its own level 3 handler; the callback fell in a gap of the dump) and Jesus Christ Motocross on the 68060
(an `rts` to 0, the user stack 12 bytes short at the end of its sine/sqrt precalc; the same routine in a test program
runs right).

### Round 10's reruns (2026-11-13, the same harness: 50 s for the 020 and 030, 120 s for the 060)

On each machine: the named productions (Impossible, The Morning Trip, Relic, destroy fascism, Lupus Reditus, Beam
Riders twice, Jesus Christ Motocross), every run the new rule takes out, every run at the limit whose screenshots were
gone, and 15 working ones at random (57, 69 and 63 runs), laid over round 9's merged results.

| Machine | Round 9 (honest) | Round 10 | Now working | No longer working |
|---|---|---|---|---|
| 68EC020, 8 MB fast | 246 | 246 | none | none |
| 68030 + 68882, 16 MB fast | 302 | 303 | Relic, The Morning Trip | Cebit demo 90 (a call through a null base), No Sleeping! (illegal instruction): both fail with main's (round 9's) code too; their round 8 runs were at the limit |
| 68060 with FPU, 64 MB fast | 354 | 356 | Impossible, The Morning Trip, Software Make The Dance Foam Oil (got past its data file this time) | Goon Royale (zero divide; the same with main's code) |

Of the runs without screenshots, 18, 22 and 21 are black or failing now, as counted. Lupus Reditus on the 68060 is
counted as crashed by the rule but plays its whole show. Impossible on the 68030 machine asks for 8,790 KB of fast RAM
in one piece (that machine has two blocks of 8 MB); Relic on the 68020 machine lacks the memory for its data file.

Also found: `tools/zine-input-test.sh`'s check "the input.device handler saw the key events (10 or more)" fails now
and then with 9 on main as on this branch (2 of 4 runs on main): a flaky test. Settled in round 11: zinput quit at q's key-down and removed its handler
before q's key-up came (both the handler and lowlevel's keyboard interrupt counted 9 in every failed run); it now
quits at q's key-up (RAWKEY 0x90), which reaches the window only after the handler (priority 60, before intuition)
has counted it. 14 runs in a row passed (10 in warp mode, 4 at stock speed), each with exactly 10.

## Round 11 (2026-11-13, branch zine11, d29 16.21)

**The flaky input test** settled first (Round 10's last paragraph): the test program, not ZINE.

**The blockers by reason** (round 10's merged results, the honest rule): 68020: does not fit (47), FPU instructions
(43), black at the limit (23), not enough memory (13), return code 1 (11); 68030: black (27), does not fit (18), not
enough memory (15), not enough memory in one piece (7), MOVE16 (7), data files left out of ZINE's volume (6), return
code 1 (6); 68060: black (33), illegal instruction (10), return code 1 (3). Read one by one:
- **68030, not enough memory (15) and in one piece (7)**: AllocMem of 9-16 MB in one piece, on a machine whose 16 MB
  are two blocks of 8 MB (the harness's FS-UAE configuration, d29 16.21). Not ZINE's; left.
- **Data files left out, return code 1 on the 68030 (Elude's, Metropolice, Kioea, Swansong...)**: directories of 9-23
  MB that do not fit beside the program in 16 MB with ZINE's volume in memory (5.6); some ask for 14-25 MB at once.
  ZINE's design; left.
- **The Black Lotus' engine "Out of memory"** (68020, 68030; Starstruck and Rift on the 68060 too): it asks for one
  pool of 48-56 MB; on the 68060 machine Starstruck's 18.7 MB data file in the volume leaves 43.6 MB in one piece.
  Left (as above).
- **Tint and Captured Dreams, return code 1 on all three**: their parts are run with Execute and each part's hunks
  stayed after it ended (CrunchMania hands the decrunched hunks over in cli_Module). **Fixed** (d29 16.21).
- **scifi, Cebit demo 90, Gift** (a call through a base of 0 at -912): the trace build is too slow to reach scifi's
  call within the limit (black at 120 s); not settled.
- **Rubbur** (68020 only, "Could not open display"): crashes into the ROM's area under the trace build instead; not
  settled.
- **68060 illegal instructions** (10): most fail on the 68020 and 68030 machines too (1000%, No Sleeping!, Leisner,
  SoftWired, scifi); Lupus Reditus is its own bug (round 10); Beam Riders below.

**The trace dump made whole**: the PC's serial tap lost the lines (d29 16.21); with it fixed, a 512 KB dump of Beam
Riders and of Jesus Christ Motocross arrived in both passes whole.

**The unsettled 68060 crashes, further**:
- **Jesus Christ Motocross**: the 68060 package takes the same entries in the demo and in a test program with its
  precalc: one line F (the first FPU instruction, the FPU switched on), nothing else; FS-UAE's 68060 does `fsin` and
  `fsqrt` itself. The stack's 12 bytes are not lost in the package. Not settled.
- **Beam Riders**: its level 3 handler, its replayer's frame routine and its callback lists read whole (d29 16.21);
  none jumps to `$100`, and no computed jump in the dumped code could. Not settled.

### Round 11's reruns (2026-11-13, the same harness: 50 s for the 020 and 030, 120 s for the 060)

On each machine: Jesus Christ Motocross, Beam Riders (AGA and OCS), Tint, Captured Dreams, and 15 working ones at
random (20 runs each), laid over round 10's merged results.

| Machine | Round 10 (honest) | Round 11 | Now working | No longer working |
|---|---|---|---|---|
| 68EC020, 8 MB fast | 246 | 246 | none | none |
| 68030 + 68882, 16 MB fast | 303 | 304 | Captured Dreams | none |
| 68060 with FPU, 64 MB fast | 356 | 357 | Captured Dreams | none |

Tint now plays all seven parts on the 68030 and 68060 machines (23 minutes 45 seconds, its own "Exit Info" at the
end) and ends with return code 1; the counting rule takes that as failed (a long run with notes, here its Execute
lines); counted by hand it works, which gives 305 and 358. On the 68020 machine both still fail: their parts do not
fit beside the program in ZINE's volume on 8 MB (Tint: "no program TBL-TINT.E01"; Captured Dreams: its own check,
"Fast-RAM needed: 3200kb .. failed"). The 45 working ones at random still work.

## Round 12: one-block memory and full rerun (2026-11-13, main at the round 11 merge, d29 16.21)

**Fast RAM in one block, as on real A1200s.** The harness (`run1.py`, beside the results, not in this repository) gave
the 68030 machine two blocks (FS-UAE's 8 MB `fast_memory` plus 8 MB of Zorro III) and the 68060 machine 8 + 64 MB. Now:
- **68030**: `fast_memory` left out, `zorro_iii_memory = 16384`. Zenix's boot log: `ram 0x40000020..0x41000000 fast`,
  `zorro: 0: ... 16384 KB, RAM, Zorro III`, `mem: 2015 KB chip, 16147 KB fast free`. One block of 16 MB.
- **68060**: `zorro_iii_memory = 65536` only: `ram 0x40000020..0x44000000 fast`, `mem: 2015 KB chip, 65299 KB fast free`.
  One block of 64 MB, as the Apollo 1260.
- **68020**: unchanged (8 MB `fast_memory`).
- Tried and failed: FS-UAE's CPU board options (`uae_cpuboard_type = Blizzard1230IV`, `uae_cpuboard_memory = 16384`):
  Kickstart saw no fast RAM at all (0 KB fast free), the same trap as `uae_mbresmem_size` in the 2026-11-07 notes. The
  Zorro III block works on a 030 and 060 because FS-UAE's `cpu = 68030/68060` makes the Z3 board appear.
- The Z3 block sits at `$40000000` (32-bit space), not in the 24-bit `$200000` area, as on a real 030/060 card.

**report.py's notes problem.** A run that ended by itself after 60 s or more emulated, but had "notes" from ZINE, was
counted as failed (the "ran its course" rule needed `not notes`). Execute lines ("... asked dos Execute to run ...")
and "differ only in case" are ZINE telling what it did, not a sign of failure: they no longer count (`failnotes()`).
The notes that stay: no file / no library / no device / no resource, left out data, "goes into ZINE's volume". The
round 10 phrase list is unchanged. On round 11's results on disk the change moves:
- **68030**: Tint (Execute lines only; it plays all seven parts), Captured Dreams (the same), Norwegian Kindness (its
  Execute lines; it ends at return code 1 after more than 60 s) to working.
- **68060**: Tint and Captured Dreams to working.
- **68020**: none (Tint and Captured Dreams still fail there on memory, as in round 11).
On round 12's own results it moves Tint on the 68030 and 68060 machines. Norwegian Kindness is a long run that ends
with return code 1, so it counts as working by the rule "ran its course"; that is the rule's honest limit.

**The rerun** (all 500 productions on each machine, headless, 3 emulators at a time, 50 s for the 020 and 030, 120 s
for the 060, main's build in `~/.cache/zrerun12/build`). Counted of 426 (disk image only and no executable left out),
with the new notes rule on both sides. The old files on disk, recounted, give 243, 298 and 352; round 11's own table
gave 246, 304 and 357 (merged results; those on-disk files differ from them by a few runs).

| Machine | Round 11 (its table) | Round 11 files, recounted | Round 12 | Now working | No longer working |
|---|---|---|---|---|---|
| 68EC020, 8 MB fast | 246 | 243 | **243** | 4 | 4 |
| 68030 + 68882, 16 MB in one block | 304 | 298 | **310** | 19 | 7 |
| 68060 with FPU, 64 MB in one block | 357 | 352 | **353** | 8 | 7 |

Part of the 030's gain is the one block ("could not be loaded" 25 -> 6, "does not fit in memory" as before); the rest of
the changes on the 020 (same config) and the 060 (an A1200-like block of the same size) are run to run: black screens
at the limit (still in precalc, load 15-23 on the PC while it ran) came and went. Black at the limit: 020 23 -> 28,
030 30 -> 36, 060 34 -> 40; those are the ones to look at before trusting a "newly failing".

Newly working (a production is listed on each machine where it changed):
- 020: Smoke Bomb, fake elektronik lightshow, 1000%, Leisner.
- 030: Jesus Christ Motocross, lux aeterna luceat eis, You are Lucy, Surfing Great Victoria, Master Control Program,
  Fullmoon, entropy chamber, subside, Outside Planet Earth, Concrete, THE CURE, Boogietown, 1000%, Impossible, The
  Morning Trip, Leisner, Planet Loonies, Lvis, Perspiration.
- 060: We come in peace, Shake Off The Dust, THE CURE, Reborn, 1000%, Impossible, The Morning Trip, Leisner.

Newly failing:
- 020: Psychedelic and Clubisque (black at the limit), Elysium (a missing call), Three Men and a Glenz Vector (line F
  at $FFFF).
- 030: Software Make The Dance Foam Oil ("demo_init() failed"), Real (Complex), sumea, Supercool, Talking Heads (all
  black at the limit), Zeon (illegal instruction), Three Men and a Glenz Vector (line F).
- 060: Software Make The Dance Foam Oil, Serenity, sumea, the castle, Noxie (black at the limit), Signals (alert
  8100000f), Goon Royale (zero divide: it did so in round 10 too).

## Round 13 (2026-11-13, branch zine13, d29 16.22)

**Noise or real.** Every production round 12 listed as newly failing ran twice more with round 12's build (passes
started at PC load 10-12, which rose to 15-30 while they ran; 3 emulators at a time), and once more in the final
rerun below (started at load 2-12, median 10.5).
- **Noise** (ran in this round's runs): 68020 Psychedelic, Clubisque; 68030 Real (Complex), Supercool, Talking Heads,
  Zeon; 68060 Noxie (ran in one of three runs).
- **Real, fixed in ZINE** (d29 16.22): Three Men and a Glenz Vector (68020, 68030: the user vectors, read through a
  base of 0 the program made itself); Signals on the 68060 (ZINE's pools kept a size word beside each block, which the
  program overwrote; alert `8100000F` at its end).
- **Real, not ZINE's or not settled**: Elysium on the 68020 machine (black in six runs, a CHK crash in a seventh; plays
  with FS-UAE's full 68020 and on a 68030 with the same memory); sumea on the 68030 and 68060 (all fast RAM above 16
  MB, round 12's one block; plays with round 11's 8 MB below 16 MB); Serenity on the 68060 (its pool needs about 55
  MB; ZINE leaves 53.5 of the 64 MB, its 6 MB data file being in ZINE's volume; plays with round 11's 72 MB); Goon
  Royale on the 68060 (a zero divide after 3,608 s in every run, round 10's too).
- **Not newly failing**: Software Make The Dance Foam Oil (its round 11 files on disk were older than round 11; on the
  68030 its two 1 MB tables are left out of ZINE's volume and "demo_init() failed" follows; on the 68060 its precalc
  is still running at 400 s); the castle on the 68060 (stuck on a dark screen in every run; round 11's dark brown
  was just above report.py's black threshold).

**The biggest remaining blockers by reason** (round 12's merged results): 68020: does not fit (47) and not enough
memory (13+6) on 8 MB with ZINE's volume in memory, FPU instructions (42, a real 68EC020 fails too), black at the limit
(28); 68030: black (36), does not fit (18), memory messages (10), MOVE16 (9, a real 68030 fails too); 68060: black (40),
illegal instructions (8, most fail on the other machines too, round 11). The black screens were looked at first: of
round 12's newly black ones about half were load, and the rest memory (Serenity), high memory (sumea) or long
precalc (Foam Oil). The memory ones come from ZINE's volume keeping data files in RAM (5.6); left as designed.

### Round 13's reruns (2026-11-13, branch zine13's code, the same harness: 50 s for the 020 and 030, 120 s for the 060)

On each machine: Three Men and a Glenz Vector, Signals, the named productions of each machine, 15 working ones picked
at random (seed 13), and the newly failing ones of round 12 (25, 28 and 30 runs, 66 in all), laid over round 12's
merged results.

| Machine | Round 12 (honest) | Round 13 | Now working | No longer working |
|---|---|---|---|---|
| 68EC020, 8 MB fast | 243 | 246 | Three Men and a Glenz Vector; Psychedelic, Clubisque (noise) | none |
| 68030 + 68882, 16 MB in one block | 310 | 315 | Three Men and a Glenz Vector; Real, Supercool, Talking Heads, Zeon (noise) | none |
| 68060 with FPU, 64 MB in one block | 353 | 354 | Signals | none |

The 45 working ones at random still work. Still failing as above: Elysium (68020), Foam Oil (68030, 68060), sumea
(68030, 68060), Signals on the 68030 (black at the limit, as in round 11), Serenity, Noxie (this run), Goon Royale,
the castle (68060).

