# D56: the tracker music player (`modplay`, grown)

Revision 1, 2026-11-17, built on branch `tracker` (in FS-UAE and on the PC). Approved by the partner on 2026-11-17
(docs/decisions.md, "New approvals", item 1: "We want a mod/tracker music player": a tracker music player beyond
`modplay`, more module formats, a player UI in the terminal); the details delegated. It sits on D40's sound server
and changes nothing in it: no kernel change, no new system call, message, port or claimed number.

## 1. In plain words

- **One player, `modplay`, for the Amiga's modules and the PC trackers' ones:** ProTracker and its kin (the old
  15-sample Soundtracker, NoiseTracker, `M.K.`, `M!K!`, `4CHN`, `FLT4`), FastTracker's MODs of 6 to 32 channels
  (`6CHN`, `8CHN`, `xxCH`, `OCTA`, `CD81`), MED and OctaMED (MMD0, MMD1, MMD2, MMD3), Scream Tracker 3 (`.s3m`) and
  FastTracker 2 (`.xm`). The format is found by the file's contents, never its name.
- **Paula first.** A module Paula can play as it is (four channels or fewer, Amiga periods: ProTracker modules and
  4-track MED songs) goes to Paula exactly as before (D40 section 8: the samples in chip RAM, one frame of registers a
  tick): no CPU spent on the sound, the module's own sound. Everything else (more channels, 16-bit samples, ping-pong
  loops, panning, envelopes) is **mixed by the CPU** into one 8-bit stereo stream on Paula's channels 0 and 1.
- **A screen of its own on a terminal**, 80 columns first: the song's name and format, the position, pattern, row,
  speed and tempo, the time, VU meters (one a channel), and one of four views: the pattern being heard, the
  instruments, the list, the keys. **A list:** files and folders on the command line (a folder: its files in name
  order), next and previous song, repeat (the list, the song), shuffle. Every key can be changed in
  `~/.config/modplay/keys` (the format and parser of `files` and `show`).
- **Lean:** one program of 64 KB on the disk (it was 16 KB), nothing resident; the mixer's tables (44 KB) only
  while a mixed module plays. `modplay -q` and `-s` keep the old ways for scripts and serial terminals.

## 2. The program: `modplay`, grown (not a second one)

The name stays: "module" is the word for every one of these files, `modplay` is already the plain name partners and
scripts know, and a second player would be a copy (D13). What changes:

```
modplay [-v VOL] [-l] [-s] [-q] [-z] [-r RATE] FILE|FOLDER...      modplay -K      modplay -B SECONDS [-r RATE] FILE
```

- `-l` repeats the song (as before), `-z` shuffles the list, `-r` the mixing rate (4,000-28,000 Hz; by default
  16,000 up to 8 channels, 11,025 above: section 5), `-s` the old one-line status instead of the screen, `-q` nothing on the terminal and no keys,
  `-K` prints the default keys, `-B` measures the mixer (section 5).
- **The screen** (stdout a terminal, not `-s` or `-q`): line 1 the title, format, channels and whether Paula plays
  it or it is mixed, the song's number in the list; line 2 position, pattern, row, speed, BPM, volume, time, repeat
  and shuffle, a message; line 3 the meters (block characters ▁..█, two cells a channel, one when they do not fit);
  line 4 the views' tabs; the rest the view. The pattern view shows a page of rows with the row heard in inverse; it
  turns the page when the row leaves it, so a row change redraws two lines, not the screen (the console's time is
  the stock 68020's time). Cells are `C-4 01 48 H81` (note, instrument, volume column, effect) where they fit,
  `C-4 01` or `C-4` where many channels must fit; `[` and `]` move the channels shown.
- **The row heard, not the row made:** native modules count frames the server reports played (D40 8.4, as before);
  mixed ones subtract the server's ring (two halves of 100 ms) from what was written.
- **Keys** (the defaults; every one in `~/.config/modplay/keys`, written with the defaults when missing, the
  `files`/`show` format: `KEY ACTION`, `<Name>` for special keys, `none` takes one away; Ctrl-C always quits):
  `q`/Esc quit, Space pause, `n`/`p` next/previous song, `.`/`,` and Right/Left the next/previous pattern
  (position), `+`/`-` volume, `r` repeat (off, the list, the song), `s` shuffle, Tab the next view, `1` `2` `3` `?`
  the views, Up/Down/`k`/`j` and Return in the list, `g` back to the song playing, `[` `]` the channels.
- Not taken: the partner's `tui.library` (ports/tui). It is a C-library program's toolkit (tuitest is 41 KB) and the
  player's screen is a few lines of escape sequences; libz and the shared key parser keep it at the size above.

## 3. The formats, and what of each is played

All replayers are plain C with no system calls (malloc and free only), behind one face (`user/trk.h`): a module is
loaded from the file in memory, then played one tick at a time into each channel's registers for that tick (a Paula
period or a rate, a volume 0-64 with every volume applied, a panning, a sample start, its place and loop). Every
format's samples become 8-bit signed at load, in place in the file's memory (16-bit ones by their high byte; Paula
is 8-bit, and this halves their memory); a ping-pong loop is laid out forward and backward once in a copy, so the
mixer only loops forward. Everything read is checked: a damaged file is refused with a reason or plays what is there.

- **MOD** (`user/modrep.c`, D40's replayer, now 4 to 32 channels): every effect D40 lists. 6-32 channels pan as
  Paula's sides repeat (left, right, right, left). FLT8 (StarTrekker's patterns in pairs) is refused, said, and so is
  a 31-sample module whose tag is not known. A looped sample plays up to its loop's end, then the loop (ProTracker's
  length is the loop's start plus its length; D40's replayer played the whole sample first). The old 15-sample format
  has no mark, so its 15 headers must look like headers (volume 0-64, finetune 0-15, names without control bytes):
  a text file in a folder is not played as noise.
- **MED/OctaMED** (`user/medrep.c`): MMD0 (3-byte notes), MMD1-MMD3 (4-byte notes, blocks of up to 3,200 lines),
  MMD2/MMD3's play sequences and sections flattened into one order of up to 1,024 blocks (a `0B` jump is into the
  current play sequence). Sample instruments (8- and 16-bit, the left half of a stereo one); synthetic, hybrid and
  multi-octave (IFF 2-7 octave) instruments are silent, MIDI ignored. Commands 00-0F (0F's F1-F5 and FD-FF too),
  11, 12, 14, 15, 16, 18, 19, 1A, 1B, 1D, 1E, 1F, 2E; not 08 (hold and decay), 0E (synth jump), the mix mode's 20-2F
  but 2E. Timing: SPD mode tempo 1-10 is the trackers' speed at 50 ticks a second; 11-240 a tick of 0.66 / tempo s
  (MED's manual: tempo 33 is the trackers' 125 BPM); BPM mode a tick of 10 / (BPM x lines a beat) s (OctaMED's
  manual: BPM mode assumes 6 ticks a line). Slides act on every tick, the first too, unless the song's
  `FLAG_STSLIDE` asks for ProTracker's way; volumes are decimal unless `FLAG_VOLHEX`; `04`'s depth is twice `14`'s
  (the manual). A sample at an odd place in the file is moved down a byte at load (Paula's places are even; the
  byte it covers is its header's, already read). A BPM-mode tick is held to 0.25 s at most. Notes above B-3 on Paula are played an octave lower for each period below 113 (Paula's DMA cannot go
  faster; the mixer plays them where they are). A song of 4 tracks or fewer (and not in the 5-8 channel mode) goes to
  Paula.
- **S3M** (`user/s3mrep.c`): the 32 channel slots' sample channels (Adlib ones silent), mono or stereo, global volume,
  8- and 16-bit and signed (format 1) or unsigned (format 2) samples, the order's 254 (skipped) and 255 (the end).
  Effects A-V: A B C D (with its fine forms) E F (fine, extra fine) G H I J K L O Q R S (2 finetune, 3/4 waveforms, 8
  panning, B loop, C cut, D delay, E pattern delay) T U V; the memory of D E F I J K L Q is one value a channel. The
  period's octave shift is done after the multiplication (TECH.DOC's formula shifts first, which detunes the high
  octaves).
- **XM** (`user/xmrep.c`, version 1.04): linear and Amiga frequency tables, instruments with up to 16 samples and
  their note map, volume and panning envelopes (sustain, loop), fadeout, the instrument's auto-vibrato (its
  depth and sweep our reading: the document leaves it "not finished"), relative notes and finetunes, the volume
  column, effects 0-F with E1-EE, G H K L P R T X1 X2, each with the memories the document gives. E5x is
  FastTracker's (x - 8) x 16 (8 is none), applied to the row's own note. The fadeout starts at 65,536 and loses the
  instrument's fadeout each tick after the key is let go, as the document says (the reviewer remembered FastTracker
  starting at 32,768, twice as fast; the document is followed until a listening says otherwise).
- **Not in this round: Impulse Tracker (`.it`).** Its new-note actions keep old notes sounding beside new ones, so a
  32-channel IT asks for more voices than channels, and its compressed samples and filters cost more again; the
  stock 68020 does not mix that (section 5). It is the next format if the partner wants it, for the 030/060.

## 4. Paula or the mixer

`trk.native`: at most 4 channels, Amiga periods (MOD, MED), one block of samples of at most 1 MB (the server's bank).
A native module whose samples are over the server's 1 MB bank is mixed instead. Native modules use D40's frames
stream unchanged (`SND_BANK`, `SND_FRAMES`, `SND_PAUSE`, `SND_VOLUME`): the four
channels, ProTracker's quirks, the exact sound. Everything else opens one PCM stream, stereo, at the mixing rate,
with halves of 100 ms (`SND_OPEN`, `SND_WRITE`): channels 0 (left) and 1 (right). The second pair stays free for
`play`. Pause stops writing (the server plays silence after its ring and stops the DMA, D40 section 6).

## 5. The mixer (`user/trkmix.c`)

- Each channel steps through its 8-bit sample with a 16.16 place: `step = clock x 65536 / rate / period` for Amiga
  periods, `rate16 x 4096 / rate` for the PC formats' rates (in 1/16 Hz). No interpolation, no volume ramps (the
  Amiga's own sound, and the cheapest).
- `sample x volume x gain / 256` from a table of 65 x 256 words (33 KB, made at the start of a mixed song) is added
  into one of three sums: left (panning below 64), right (above 192), or the middle (both); then `left + middle` and
  `right + middle` go through an 8 KB clipping table into the output's bytes. The gain: half the channels at full
  volume on one side reach full scale, then twice that (modules seldom peak on every channel at once); with it any
  number of channels sums to at most about 3,000, inside the clipping table. Only the sums some channel uses are
  cleared and read (an XM's channels are all in the middle by default, a MOD's left and right).
- The inner loop on the 68020 is five instructions a frame, four frames a pass (a byte, a word from the table, an
  add into the sum, the 16.16 step as `add.l` and `addx.l`); C on the PC. The two make the same bytes: the FS-UAE
  test compares an FNV-1a of 5 s mixed by `modplay -B` with the PC's (section 7). A channel steps at most 64 bytes a
  frame (a megahertz), so no product overflows.
- **What a 68020 at 14 MHz mixes (FS-UAE's cycle-exact A1200 with 4 MB fast RAM, emulator time, measured with
  `modplay -B`; not a real machine's figure):** the table in section 9. In short: a channel costs about 34 cycles
  a frame, the output about 60; 4 channels at 16 kHz take a quarter of the CPU, 8 channels 41 %, 16 channels at
  11 kHz 56 %, 32 channels do not keep up at any rate (107 % at 11 kHz). So mixing is for the 020 up to about 16
  channels; 32-channel modules want a 030/060.
  So the default rate is 16,000 Hz up to 8 channels and 11,025 Hz above; `-r` sets it (lower on a busy machine,
  higher on a 030/060). Real figures: the partner's A1200 (section 10).

## 6. Memory

The whole file is read into memory (at most 8 MB). MOD and MED play from the file's own bytes; S3M and XM's samples
are made 8-bit in place; ping-pong loops get a copy of themselves (the loop twice). A native module's samples go to
chip RAM through the server (as before); a mixed one's stay in fast RAM, with the mixer's 33 KB table and its sums.
The pattern view decodes packed patterns (S3M, XM) from an index of each row's place (2 bytes a row) made at load.

## 7. Tests

- **The PC** (`tools/trk-host-test.sh`, in `make check`'s host list): modules generated by `tools/mktrk.py` (every
  format, written from the same documents; no third-party music in the repository), each replayer's registers tick by
  tick against values worked out by hand from the documents (e.g. S3M's C-4 at 8,363 Hz is 14,317,056 x 16 / 1712 =
  133,804 sixteenths of a Hz; XM's linear C-5 16,726 Hz; MED's 0D01 lowering the volume on every tick, the first
  too, and not the first with `FLAG_STSLIDE`; an XM volume envelope held at its sustain point, then faded); the mixer
  byte for byte against a model of its arithmetic written in the test, at 11, 16 and 22 kHz; 150 damaged files of
  each format (MOD8, MED, MMD2, S3M, XM, XM16) under ASan and UBSan, every place a channel reads checked inside the
  file. `tools/sound-host-test.sh` (D40's) still passes unchanged for the 4-channel replayer.
- **FS-UAE** (`tools/tracker-test.sh`, the A1200 with 4 MB fast RAM): an 8-channel MOD, an S3M and an XM mixed (the
  server's state while they play: one stream on channels 0 and 1 at the rate's period), a MED on all four channels by
  frames, `-r 11025`, a folder with a damaged file in it, the screen's views and keys, a keys file, `-B`, a missing
  file. `tools/sound-test.sh` (D40) runs `modplay -s` now.

## 8. Clean room: the documents used

Written from format documents and trackers' manuals only; no player's or tracker's source was read:
- ProTracker: as D40 (the format's documentation and its documented effect behaviour).
- MED/OctaMED: Teijo Kinnunen, "MED/OctaMED MMD0 and MMD1 file formats" (revision 1, 25.4.1992, public domain);
  MED V3.21's manual (the player commands, the two tempos); OctaMED SoundStudio 1.03c's manual (Aminet
  docs/help/OctamedSSManual.lha: appendix A, the player commands; the Tempo window); MMD2's song structure from the
  field list of the `med-module` Haskell package's published documentation (field names and types only).
- Scream Tracker 3: Sami Tammilehto's "Scream Tracker 3.01 BETA File Formats And Mixing Info" (TECH.DOC) and Kalle
  Kaivola's ST 3.0 user's manual (the effects), both from "The Computer Music File Formats Collection 1.0".
- FastTracker 2: Matti Hamalainen's "The 'Complete' XM module format specification" v0.81 (2001; Triton's XM
  description with its corrections and FT2's behaviour).
Where a document is silent or vague (XM's auto-vibrato depth, MED's notes above B-3, MED 16-bit samples' loop units,
the order in which a row's effects meet), the choice is ours and said here; a difference from the trackers found by
ear on the A1200 is a fix, not a design change. Our code is GPL-2.0-only.

## 9. Sizes and measurements

- **Disk:** `modplay` 63,968 bytes (was 16,132): the replayers 31 KB (MOD 7.0, XM 9.2, MED 6.8, S3M 6.0, the face
  1.8), the program 11.6 KB, the mixer 2.2 KB, the shared key parser 3.0 KB. 30 KB of bss (the mixer's output buffer
  16 KB). Nothing else grows: no kernel, server or library change; not on the demo floppies.
- **RAM while playing:** the file; a mixed song also the mixer's 44 KB (volume table 33 KB, clipping table 8 KB,
  sums 3 KB) and ping-pong copies.
- **The mixer in FS-UAE's A1200 (68EC020 14 MHz, 4 MB fast RAM; emulator time, `modplay -B 10`, 2026-11-17).**
  Busy modules (every channel playing a looped 2 KB sample, a note every 4 rows, `tools/mktrk.py` busy*). The share
  is of the CPU's time for the sound's length; the replayer's share is in brackets:

  | channels | 11,025 Hz | 16,000 Hz | 22,050 Hz |
  |---|---|---|---|
  | 4 (XM) | 18 % (2.7) | 24 % (2.7) | 31 % (2.7) |
  | 8 (XM) | 30 % (4.6) | 41 % (4.7) | 54 % (4.6) |
  | 8 (MOD) | | 42 % (2.8) | |
  | 16 (XM) | 56 % (8.6) | 75 % (8.6) | 99 % (8.6) |
  | 32 (XM) | 107 % (16.5) | 144 % (16.5) | 189 % (16.5) |

  The first build (C output stage with multiplies, 64-bit rate arithmetic) took 48 % for 4 channels at 16 kHz;
  the volume table with the gain in it, the clipping table, the sums cleared only where used and the 4-frame asm loop
  brought it to 24 %. A native (Paula) module costs only the replayer's tick (D40 section 13).
- A real 68020's figures are to be taken with `modplay -B` on the A1200 (section 10).
## 10. Questions for the partner

1. Impulse Tracker next (030/060 only, or a 4-channel subset on the 020)? Section 3.
2. The mixing rate: 16 kHz by default; on the A1200 with the 1260 it could be 28 kHz. A per-machine default needs
   the CPU type from the kernel (not there today for programs): fine to leave as `-r`?
3. Hard Amiga panning (left/right) for 6-32 channel MODs, as Paula plays them, or a softer stereo?
4. Listening on the A1200: the formats sound right? (No real-hardware run yet; section 5's figures are emulator ones.)
   `modplay -B 10 FILE` there gives the real figures for the table in section 9.

## 11. The review (2026-11-17, one independent read and fuzz of the code)

It found no memory fault (its own fuzzer: 63,000 damaged files of seven kinds under ASan and UBSan, every voice's
places checked inside the file and a native one's inside the bank). Its findings, and what was done:
1. A looped MOD sample played its whole length before the loop: now up to the loop's end (section 3; test added).
2. Any file with a plausible byte 470 played as a 15-sample MOD (a README in a folder): the 15 headers are checked,
   a 31-sample module with an unknown tag refused (tests added).
3. `mod_goto` during an EE repeat skipped the new row's notes: fixed.
4. A native module over the 1 MB bank was refused with status 0: it is mixed instead.
5. MED samples at odd places were rounded a byte early for Paula: moved down a byte at load.
6. A MED BPM tick longer than the mixer's buffer played too fast: ticks held to 0.25 s, the buffer 8,192 frames.
7. A silent channel's skip looped once per loop length: by a remainder now; steps capped at 64 bytes a frame.
8. The screen's line writer could read past a name cut inside a UTF-8 character: it stops at the end.
9. An escape sequence cut short (Alt+[) stalled the player: each byte waits 30 ms at most.
10. XM's E5x used ProTracker's signed nibble: FastTracker's (x - 8) x 16 now, for the row's note too (test added).
    The fadeout's start (65,536 by the document, 32,768 by the reviewer's memory of FastTracker) follows the document.
11. The time shown after a tempo change, positions over 255 (MED) on the screen: fixed.
12. Section 5's arithmetic said what an earlier build did: rewritten.
The driver's check now also holds a native module's places inside its bank.
