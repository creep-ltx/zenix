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
