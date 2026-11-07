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

