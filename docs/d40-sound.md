# D40. Sound: the Paula server `sndd`, `play` and `modplay`

Revision 2, 2026-11-12: revision 1 after its independent design review, every finding folded in (section 15).
Roadmap block 6, the sound part ("a Paula server, `play`, a MOD player"), approved in the order of work; the details
were delegated (docs/decisions.md, 2026-11-12). Sources: the Hardware Reference Manual (3rd edition, "Audio Hardware":
the period, the minimum of 124, the back-up registers and the audio interrupt's timing, stopping a channel), the RKM
Devices manual's audio.device chapter for the channel allocation idea, EA's IFF 8SVX and Apple's AIFF descriptions,
Microsoft's RIFF WAVE description, and ProTracker's documented MOD format and effect behaviour (the knowledge base's
`amiga/paula-player.md` for the documented quirks). No player's source was read.

## 1. What and why

Zenix has one sound today: the kernel's bell (`kernel/audio.c`, a square wave on channels 0 and 1). Block 6 asks for
sound as the rest of Zenix is built (ROADMAP principle 1, D19's rule "every driver not needed to reach the root is a
user-mode process"): a server owns Paula; programs talk to it by messages; `play` plays sound files; `modplay` plays
ProTracker modules on Paula's four channels.

What it must be: lean (a stock A1200 with 4 MB of fast RAM is the machine; the kernel's binding room is the mount
tests' floppy: 1,500 bytes over the size of 2026-11-11, of which `ROOM` keeps 800), correct on PAL and NTSC, safe when a program dies or ZINE takes the machine, and no CPU work where Paula's DMA
can do it.

## 2. The shape

```
 play  modplay  (other programs)          user programs, each linked with user/sndlib.c
   \      |      /
    port "audio"  (a D11 service: /sbin/sndd, started by the first call, again after it ends)
         |
       sndd   ---- writes Paula's registers itself (user mode reaches the chips: no MMU), polls INTREQR
         |
   kernel: SYS_drv's DRV_CLAIM of DRV_PAULA (one owner; the bell steps aside; quiesce at the end and for ZINE)
```

- **The kernel's part is a claim, nothing else** (section 4): `DRV_CLAIM` with the base `DRV_PAULA`, which only the
  process the service manager runs for port `audio` may take. While it is held the bell is silent (the kernel does not
  touch Paula); when the holder ends, or before ZINE takes the machine, the kernel stops the four channels' DMA and
  zeroes their volumes before any of the server's memory is freed (D19 2.4's rule for DMA: the device is stopped
  before its memory goes). No new system call, no interrupt code, no new message the kernel sends. (There is no MMU:
  the claim does not stop a program from writing Paula's registers; it silences the bell and guarantees the stop.)
- **sndd** (`user/sndd.c`, `/sbin/sndd`, `/etc/svctab`: `audio -10 /sbin/sndd`) owns the channels, the chip RAM
  buffers and the timing. Nice -10: above the display server (-5), so drawing never starves the sound.
- **The client library** (`user/sndlib.c`, `user/sndproto.h`): a dozen small functions over `sys_call`.
- **`play`** (`user/play.c` + the decoders `user/snddec.c`): WAV, 8SVX, AIFF and raw bytes, from a file or standard
  input.
- **`modplay`** (`user/modplay.c` + the replayer `user/modrep.c`): ProTracker modules; the replayer computes each
  tick's registers in the client, the server applies them on time (section 8).

## 3. Paula, the facts used

- Four channels, 8-bit signed samples fetched by DMA from **chip RAM** (AUDxLC, a word address; AUDxLEN, words; one
  word = two samples). AUDxPER is colour clocks per sample: 3,546,895 Hz on PAL, 3,579,545 Hz on NTSC; the least
  period is 124 (HRM: the DMA slot per line), about 28.6 kHz. AUDxVOL 0-64. Channels 0 and 3 are left, 1 and 2 right.
- **The back-up registers:** Paula reads LC and LEN when a channel starts and each time its length runs out; right
  after the read it raises the channel's interrupt request (INTREQ bits 7-10). Writing the next block's LC/LEN after
  that request makes the sound continue without a gap; not writing them repeats the block (HRM "Joining tones").
- **The request bit is set whether or not the interrupt is enabled** in INTENA. sndd never enables it: it reads
  INTREQR and clears its bits itself (section 6).
- **The E-clock is exactly a fifth of Paula's clock** (709,379 Hz PAL, 715,909 NTSC: crystal / 40 against crystal / 8),
  and `sys_hrtime` reads it. So the time a block plays is known exactly in the clock the process can read: LEN x 2 x
  PER / 5 E-clock ticks.
- **A new sample on a channel** (a MOD note): the channel's DMA off, LC/LEN/PER/VOL written, a wait until Paula has
  let the channel go (longer than one sample period), DMA on, a wait until Paula has read LC/LEN (its request bit),
  then the loop's LC/LEN written behind it. The waits count raster lines on VHPOSR, never a CPU loop (Emu68), each with
  a limit (section 8.3).
- **Stopping a channel takes two sample periods** (HRM, "Stopping the audio DMA": a channel off for less "may stay on
  and thus continue from where it left off"). So every DMA-off before a new start (a MOD note, a stream's start) and
  before its memory is freed waits two periods of the longest period Paula may still be counting (the old one or the
  new), timed on the E-clock (a period is PER colour clocks, PER / 5 E-clock ticks), not on raster lines (which depend
  on the display mode).
- **ADKCON**'s bits 0-7 (one channel modulating another) are cleared when sndd starts: a program before it (ZINE's)
  may have left them set. INTENA's audio bits stay off.
- **The least period:** the HRM gives 123 on PAL and 124 on NTSC; a PCM stream uses 124 on both. A MOD's frames may use
  113-4,095 (ProTracker's B-3 is 113, below Paula's DMA rate as ProTracker itself plays it).
- **AGA and OCS/ECS have the same Paula:** nothing in the sound differs between them. PAL or NTSC is read once at the
  start from VPOSR bit 12 (the Agnus/Alice NTSC bit, the kernel's own test in `kernel/main.c`).
- The audio filter (CIA-A PRA bit 1, the power light) is left as the machine has it.

## 4. The kernel's part (`kernel/drv.c`, `kernel/audio.c`, `kernel/netproto.h`)

- `DRV_PAULA` = 0xDFF0A0 (AUD0LC: the four channels' registers, 0xDFF0A0-0xDFF0DF, are its window), a new base of
  `DRV_CLAIM` (D19 step 7's call). `DRV_CLAIM` with this base is handled **before** SYS_drv's network gate (which
  answers -ECONNRESET without netd and -EPERM to anyone but netd's children) and has a gate of its own:
  `svc_pid("audio") == current->pid`, the process the service manager started for the port (-EPERM for anyone else,
  root or not). `svc_pid` names a running entry only once its port exists, so sndd creates port `audio` first and
  claims after. Every other op of SYS_drv keeps the network gate, so the sound server gets -EPERM (or -ECONNRESET)
  for `DRV_DMAMEM`, `DRV_QUIESCE`, `DRV_CACHE`, `DRV_PARACK` and the rest: it has only the claim. The frozen wording
  of SYS_drv in docs/frozen-interfaces.md says so. The claim is one of the four slots of `claims[]` (`NCLAIM`), the
  ones the network's drivers use; a machine has one Paula, so it takes one.
- "audio" is a **reserved port name** (as `fs`, `disk`, `net`): only root creates it, so a user's port by that name
  cannot block the service's registration or stand in for it.
- The Paula claim has no DMA region: sndd's chip RAM is its own (`sys_memalloc_chip`), which is safe because the
  claim's stop runs in `proc_exit` before the process's memory is freed (`drv_exit`).
- **The stop** is the claim base's own step, as D39's parallel port has its interrupt switched off beside the quiesce
  program (`stop_base` in drv.c, one place for both): `audio_hold` writes `AUD0VOL`..`AUD3VOL` = 0, then DMACON clear
  of AUD0-3, with interrupts masked (so the tick's `audio_tick` never sees half of it). It runs at the claim (a beep in
  progress is stopped), when the holder ends (`drv_exit`), and before ZINE takes the machine (`drv_zine`). A quiesce
  program is not used: DMACON (0xDFF096) lies outside the claim's window, and a program written by the kernel for
  itself would be a second place for the same four writes.
- **netd's end leaves it alone:** `drv_exit` ends the claims of netd's children when netd ends; the Paula claim is not
  one of them and is skipped.
- **The bell steps aside:** `beep()` does nothing while Paula is claimed; `audio_tick` does nothing then. The terminal's
  BEL is silent while sndd runs. sndd ends by itself after 30 s with no stream (section 11), so the bell comes back.
- Cost (measured): drv.o +156 bytes, audio.o +86 bytes and one byte of bss, ipc.o +22 bytes (the reserved name); kernel.bin
  182,820 -> 183,088 bytes, 1,232 bytes left on the mount tests' floppy (`ROOM` 800). The budget lines of
  `tools/ksize-budget` say it.

**Not taken: a level 4 interrupt route.** The kernel could take the audio interrupts and wake sndd (as it counts the
parallel port's ACK for the plipbox, D39). It costs kernel code the floppy has no room for, and the wake would still
reach sndd only at the next return to user mode or scheduling point; polling the request bits at times computed from
the E-clock (section 6) gives the same continuous sound with buffers of a few ticks. If a measurement on the stock
A1200 shows the polling's latency matters, the route is a small later step (a `DRV_AUDWAIT`), with its own review.

## 5. The protocol (port `audio`, `user/sndproto.h`, frozen once merged)

Synchronous calls (`sys_call`); a request's numbers are in `a[0..3]`; the answer is the result (>= 0, or -errno).
Pointers name the client's own memory and are checked with `sys_peer(PEER_RD/PEER_WR)` before use. A stream belongs
to the process that opened it (its pid is checked on every request; root may use any stream).

**A stream's handle** is `(start << 8) | slot`: the slot 1-8, and sndd's start count (D11's `SVC_SELF`, 15 bits). When
sndd ends (ZINE, a crash, its idle end), only the request it was serving learns it from the kernel (-ECONNRESET);
queued and later requests reach the next sndd. That one answers a handle of an earlier start with -ECONNRESET
itself, so a client always learns that its stream is gone and opens a new one, and never reaches someone else's
stream in a reused slot.

| Type | Name | a[0] | a[1] | a[2] | a[3] | Result |
|---|---|---|---|---|---|---|
| 1 | `SND_OPEN` | rate in Hz | `SNDF_*` flags | volume 0-64 | a half's ms (20-1000; 0: 60) | handle |
| 2 | `SND_WRITE` | stream | buffer | bytes | - | the bytes (all of them; -EINTR if cancelled) |
| 3 | `SND_DRAIN` | stream | - | - | - | 0 when everything written has played |
| 4 | `SND_CLOSE` | stream | - | - | - | 0; the channels stop at once |
| 5 | `SND_VOLUME` | stream | volume 0-64 | - | - | the volume before |
| 6 | `SND_INFO` | `struct sndinfo *` | its size (>= 32) | a handle, or 0 | - | bytes written (32) |
| 7 | `SND_BANK` | stream | sample data | bytes | - | 0: copied into the stream's chip RAM bank |
| 8 | `SND_FRAMES` | stream | `struct sndframe *` | count, 1-4096 | `SNDFRAME_SIZE` (100) | frames played so far |
| 9 | `SND_PAUSE` | a frames stream | 1 pause, 0 go on | - | - | 0 (a PCM stream: -EINVAL) |
| 10 | `SND_STATE` | `struct sndstate *` | its size | - | - | bytes written: **the tests' state block, not frozen** |

- **`SND_OPEN` flags:** `SNDF_STEREO` 1 (the data interleaves left and right bytes: channels 0 and 1, else one pair
  plays the same byte on a left and a right channel), `SNDF_FRAMES` 2 (the four channels at once, played by frames:
  `SND_BANK` and `SND_FRAMES`, section 8; no `SND_WRITE`). The rate is turned into a period by the server
  (round(clock / rate), at least 124); `SND_INFO` says the period and the real rate.
- **Channels, not mixing (section 7):** a PCM stream takes a pair (0+1 or 3+2), a frames stream all four. A request
  that cannot have its channels gets -EBUSY. Up to 8 handles exist (the slots), but the channels allow at most two
  PCM streams or one frames stream at a time.
- **`SND_WRITE` blocks the client** until all its bytes are in the stream's chip ring (the server keeps the message
  and answers it when the last byte is copied): flow control without a second buffer. The data is 8-bit signed. A
  client writes in pieces of about a half (play: 1,024 frames), so Ctrl-C is felt at once (section 11).
- **`SND_VOLUME`** on a frames stream scales every frame's volumes (volume x the frame's / 64).
- **`SND_BANK`** may come again (a new module): the four channels are stopped first (two periods), the old bank
  freed, the queue emptied.
- **`struct sndinfo`** (frozen, first size 32): `size`, `clock` (Paula's Hz), `eclock` (Hz), `flags` (`SNDI_NTSC` 1),
  `period`, `rate` (of the stream named in a[2], or 0), `underruns` (of all streams since sndd started), `freech`
  (channels free, a mask). The state block of the tests is a request of its own, `SND_STATE` (DMACONR, each channel's
  last period, volume, block, its request bits seen, its stream), outside the frozen protocol.
- Errors: -EINVAL (a bad number), -EFAULT (a pointer not the caller's), -EBUSY (no channels), -EPERM (another's
  stream), -ENOMEM (no chip RAM), -EINTR (the client was killed while waiting), -ECONNRESET from the kernel when the
  server is gone (the library says so; `play` and `modplay` open again once, section 10).

## 6. PCM streams: the double buffer and the timing

- A stream's ring is two halves of H bytes per channel in chip RAM, H = the rate x the half's time (`SND_OPEN`'s
  a[3], 60 ms by default; `play -b MS`), even, at least 256 bytes (22,030 Hz: 1,322 bytes; a stereo stream
  4 x 1,322 = 5.2 KB of chip RAM). Paula plays half A while sndd fills half
  B; when B starts (its request bit) sndd writes A's LC/LEN behind it and fills A.
- **Timing without interrupts:** when a half starts at time T (seen by its request bit), it ends at T + H x PER / 5
  E-clock ticks. sndd sleeps (`sys_evwait` on its port with a timeout in ticks) until about one tick before the next
  half is due, then looks every tick; a request comes in meanwhile wakes it at once. With halves of 60 ms and ticks of
  10 ms, the next half is written 40-50 ms before Paula needs it. Wakes: about two per half, 30-40 a second for a
  stream, none when nothing plays.
- **Underrun:** if a half is due and nothing is written into it, sndd writes silence (zeroes) and counts an underrun.
  If sndd itself is late by a whole half, Paula has already started the half it queued, not yet filled: what is in it
  (stale or partial sound) plays, and the underrun is counted. The request bit stays correct (Paula repeats the half
  whose place is in the registers), so the halves never get out of step.
- **When sndd is late:** the kernel is never preempted, and some of its work runs long: the floppy driver waits inside
  the kernel for a track (up to about 300 ms), a big program's load, IDE PIO. While a process is in such a call,
  sndd does not run, whatever its priority (an interrupt-driven wake would wait for the same system call). So floppy
  activity on a floppy-only machine makes underruns with 60 ms halves; `play -b 500` (half-second halves, 1 s of chip
  buffer) rides over a track read. The worst kernel section is to be measured on the stock A1200 and the default
  chosen from it (section 16).
- The CPU writes each sample to chip RAM once (mono with memcpy; stereo de-interleaved byte by byte on
  the way); a half is cleared once when a stream opens or ends early (`do_open`, `pcm_seal`). No other chip RAM access
  (audit 2026-11-12 A6-7: the text said long words throughout).
- **The end:** `SND_DRAIN` answers when the last written byte has played (the time is known); `SND_CLOSE` stops the
  pair at once (volume 0, DMA off) and frees its chip RAM.
- **Idle:** four silent halves in a row with nothing pending and the stream stops its DMA; the next write starts it
  again (the start procedure of section 3).
- Latency (the time from a write to the speaker) is at most two halves, 120 ms: right for `play`, and for a beep
  wanted at once a program can write a short sound to an idle stream, which starts at the next tick.

## 7. Several programs at once

Paula has four channels; mixing in software would cost the 14 MHz 68020 a few per cent per stream and chip RAM
reads. The first model is **allocation, as audio.device does it**: two PCM streams at once (each a stereo pair,
mono played on both sides) or one frames stream (a MOD) alone. A program that finds no channels is told -EBUSY at
`SND_OPEN` and says so ("sound is in use by another program"). No priorities and no stealing in this step: the first comes first.
Mixing (several sounds on one pair) and priorities are later steps if wanted; the protocol has room (`SNDF_*` bits
grow at the end).

## 8. Frames streams: the MOD player through the server

### 8.1 The bank
`SND_BANK` copies the module's sample data (one block, at most 1 MB) into chip RAM owned by the stream. Frames name
places in the bank by byte offsets; the server checks every offset and length against the bank (an offset past it
is -EINVAL at `SND_FRAMES`, so a broken module cannot point Paula at other memory). A 4-byte silent word at the
bank's end is the loop of a sample that does not loop (ProTracker's "plays the first word as its loop": silence when
that word is zero, as trackers write it).

### 8.2 The frame
```c
struct sndch { unsigned short flags, period, volume, pad; u32 start, len, loop, looplen; };   /* bytes, even */
struct sndframe { u32 ticks; struct sndch ch[4]; };            /* 100 bytes; ticks: E-clock ticks to the next frame */
```
`flags`: `SNDC_TRIG` 1 (start `start`/`len`, then loop at `loop`/`looplen`), `SNDC_PER` 2, `SNDC_VOL` 4, `SNDC_LOOP`
8 (only the next loop's LC/LEN: ProTracker's instrument change without a note), `SNDC_STOP` 16 (the channel's DMA
off). One frame is one ProTracker tick; `ticks` = E-clock x 2.5 / BPM (14,187 at 125 BPM on PAL: 20 ms). The limits,
checked for every frame (-EINVAL): a period 113-4,095; a place even and inside the bank, or `SND_SILENT`
(0xFFFFFFFF: the bank's silent word); a length 2-131,070 bytes (AUDxLEN counts 16-bit words). `SND_FRAMES` passes the
frame's size in a[3], so the struct can grow at its end later.

### 8.3 Applying a frame
At the frame's time: the triggered channels' DMA off in one DMACON write; every channel's PER and VOL; the triggered
channels' LC/LEN; a wait of two of the old periods (the longest Paula was counting on those channels: section 3; at
period 1,712 under 1 ms), timed on the E-clock; their request bits cleared; DMA on in one write; a wait for their
request bits (at most a period and a millisecond); then their loop LC/LEN. Every wait has its limit; a limit hit is
counted (the state block's `waits`). About 1 ms a frame with notes, a few register writes without.

### 8.4 The timing
sndd keeps the stream's own clock: frame k is due at the stream's start plus the sum of the earlier frames' `ticks`,
in E-clock ticks (modular differences: the E-clock count wraps after about 1.7 hours). The start is the E-clock time
read when the first frame is applied, which sndd does when woken by a tick, so the frames' times sit on the ticks'
phase. sndd's timeout is the next due frame; it applies every frame due within half a tick. So **the tempo is exact
on average and, while sndd gets the CPU at its tick, each frame is within 5 ms of its time**; at 125 BPM (most
modules) a frame falls on every second tick. Other tempos move each frame by up to 5 ms. sndd runs at nice -10:
a program at a higher priority (a root program at -20, or a server boosted by priority inheritance) or a long kernel
section (section 6) delays it. A queue that runs dry is an underrun (the notes ring on); after a stall of more than
100 ms the clock starts again from now rather than rushing. A finer timer (CIA-B's, through the kernel) is a later
step if wanted.
- `SND_FRAMES` copies the frames into the stream's queue (16 frames, 320 ms at 125 BPM) and answers when the last
  is in; its answer is the count of frames played, which `modplay` uses to show the row being heard, not the row
  computed. A queue that runs dry keeps the last state (the notes ring on) and counts an underrun.

## 9. The decoders (`user/snddec.c`, host-tested)

One reader for `play`, from a file or standard input (no seeking: chunks are read in order, unknown ones skipped):
- **WAV** (RIFF WAVE): PCM 8-bit unsigned and 16-bit signed little-endian, `WAVE_FORMAT_EXTENSIBLE` with the PCM
  subformat, 1 or 2 channels (more: refused, said); any rate.
- **8SVX** (IFF): VHDR's rate and volume, `sCompression` 0 and 1 (Fibonacci delta), the `CHAN` chunk's stereo (the
  left half of BODY, then the right); the one-shot part, then the repeat part once.
- **AIFF** and **AIFF-C** with `NONE` or `sowt`: COMM's channels, sample size (8 or 16 bits) and the 80-bit rate; SSND's
  offset. A COMM after SSND is refused on a stream (it is rare; said).
- **Raw** (`play -r`): 8-bit signed (or `-u` unsigned) bytes at `-R RATE` (default 8,000), mono or `-2` stereo.
- To Paula: 16-bit samples are rounded to 8 bits; a rate above Paula's (28,604 Hz PAL) is divided by the smallest
  whole number that brings it below (44.1 kHz -> 22,050, 48 kHz -> 24,000), averaging the samples it joins; more than
  two channels are refused; `-m` mixes stereo to mono (one pair, the second free for another program).

## 10. `play` and `modplay`

- `play [-v VOL] [-m] [-q] [-b MS] [-r [-u] [-2] [-R RATE]] [FILE... | -]`, and `play -S` (the server's state, for
  tests and curiosity): plays each file in turn; prints its format line unless `-q`; Ctrl-C ends it at once (play
  catches no signal: the kernel cancels its waiting `SND_WRITE`, sndd stops the pair, section 11); exit status 0, 1
  for a file it could not play (it goes on with the next), 2 for sound not available.
- `modplay [-v VOL] [-l] [-q] FILE`: ProTracker / NoiseTracker 4-channel modules (`M.K.`, `M!K!`, `4CHN`, `FLT4`, and
  the old 15-sample format); a status line (position, pattern, row, speed, tempo, each channel's note) updated in
  place; `-l` loops the song; keys: `q` or Ctrl-C quit, space pauses, `n`/`p` the next or previous position (`-q`:
  no status line and no keys, the terminal left alone, for scripts). Its loop: poll the keyboard without waiting, make
  4 frames, `SND_FRAMES` (which waits while the server's queue of 16 is full); keys are felt within the queue's
  length, 320 ms at 125 BPM. Paused, it waits on the keyboard alone. The
  replayer is `user/modrep.c`, a plain C module with no system calls, so the PC's tests run it as it is.
- Effects (ProTracker's, as documented): 0 arpeggio, 1/2 portamento up/down, 3 tone portamento, 4 vibrato, 5 and 6
  (3 or 4 with a volume slide), 7 tremolo, 9 sample offset, A volume slide, B position jump, C set volume, D pattern
  break, F speed (< 32) and tempo (>= 32, CIA timing; F00 ends the song); E1/E2 fine portamento, E3 glissando, E4/E7
  waveforms, E5 finetune, E6 pattern loop, E9 retrigger, EA/EB fine volume, EC note cut, ED note delay, EE pattern
  delay. Not done: E0 (the filter, which belongs to the machine), E8 and EF (unused by trackers). ProTracker's
  quirks that only matter for broken modules (an arpeggio reading past its table, a period of 0) are clamped instead,
  and said in the page.
- **After ZINE, or when sndd is restarted:** the library returns -ECONNRESET; `play` opens a new stream and goes on with
  the next block (a gap), `modplay` opens again, sends its bank again and goes on at the row it had.

## 11. Ends, kills and ZINE

- **A client killed while it waits in `SND_WRITE`/`SND_DRAIN`/`SND_FRAMES`** (Ctrl-C): the kernel marks the message
  `UMSG_CANCEL` and wakes sndd's `evwait` (POLLPRI, D23); sndd stops that stream at once and answers -EINTR. (A
  caught SIGINT would not mark the call: play and modplay catch none. A call made after its client was already killed carries
  `UMSG_CANCEL` without raising the port's POLLPRI, so it is simply served like any other: harmless, the client is
  gone; audit 2026-11-12 A6-7.)
- **A client that ends between calls** (with a stream open): sndd checks each open stream's pid every half-second
  (`getpriority(PRIO_PROCESS, pid)`, -ESRCH when it is gone or a zombie) and closes the streams of the dead. Pids only
  count up (`next_pid`), so a pid is never someone else's. (displayd learns of a dead client from a failed post to its
  port; sndd's clients have no port of their own, so the pid is what there is.)
- **sndd is killed:** the kernel holds a server's death while it serves requests (up to 5 s, so it can answer them),
  and sndd keeps requests nearly always. So sndd acts itself: its `evwait` answers -EINTR at once to a killed process
  (sndd catches no signal, so -EINTR means killed), and then it silences Paula, answers every kept request (-EINTR for
  a cancelled one, -ECONNRESET for the others) and exits. The kernel's stop (section 4) runs again at the end, before
  its chip RAM is freed. The next call starts a new sndd (D11).
- **sndd's idle end:** after 30 s with no stream sndd exits with status 0 (a clean end, which D11 does not count
  against its restart limit); its claim ends and the bell sounds again; the next call starts it.
- **ZINE**: `drv_zine` stops the channels and kills sndd before the hand-over (ZINE has the hardware to itself, its
  audio.device included). Nothing of Zenix runs during the session; at the resume sndd sees the kill at its next
  `evwait` and ends as above before it touches Paula again (it was asleep in `evwait` or about to return to it: no
  frame is applied between). The DMA state ZINE restores was read after `drv_zine`, so audio DMA is not turned on
  again. The next call starts a new sndd; the clients get -ECONNRESET and open again. (The display server is
  different: it refuses ZINE because ZINE's way back restores the text display, D29 16.7. Sound has no screen state
  to keep, so it steps aside.)

## 12. Tests

- **Host** (`tools/sound-host-test.sh`, in `make check`'s host list): the decoders against files the generator makes
  (`tools/mksound.py`: WAV 8/16-bit mono/stereo at 8, 22.05, 44.1 and 48 kHz, WAV_EXTENSIBLE, 8SVX plain, Fibonacci
  and stereo, AIFF 8/16 and AIFF-C `sowt`, truncated and lying headers), each decoded byte compared with the
  generator's own expectation; the replayer against small modules the generator writes (`tools/mkmod.py`), one per
  effect, each tick's period, volume and trigger compared with values worked out by hand from the format documents
  (e.g. arpeggio 037 on C-2: 428, 360, 285, 428, 360, 285; tone portamento 3 04 from C-2 to D-2: 428 -> 424 ... -> 381),
  and the order list's jumps, breaks, loops and delays as row sequences. No third-party music in the repository.
- **Emulator** (`tools/sound-test.sh`, FS-UAE A1200 020 + 4 MB fast): `play` generated WAV, 8SVX and AIFF files,
  `modplay` the generated module; the state block (`play -S`, `SND_STATE`) taken while each plays shows audio DMA on in
  DMACONR, the expected periods and volumes on the expected channels; two `play`s at once take the two pairs and a
  third gets "busy"; Ctrl-C stops a play (DMA off after); a killed sndd's claim is released and play goes on with a
  new one; the bell is silent while sndd holds Paula and sounds again after its idle end. Recording FS-UAE's sound
  (the OpenAL file writer, knowledge base `demoscene/fs-uae-notes.md`) to check the pitch is left for a later round
  (section 16).

## 13. Size and speed (measured sizes 2026-11-12; the rest estimated)

Kernel +268 bytes (section 4); on the disk sndd 11.1 KB (13 KB in memory), play 23.3 KB, modplay 18.8 KB. Estimated before the build: sndd about 8-10 KB on the disk and its RAM, plus the chip RAM of what plays (a stereo
22 kHz stream about 5 KB; a module its samples plus 4 bytes); `play` about 10 KB, `modplay` about 14 KB. CPU: a PCM
stream costs the copy of each byte once and about 35 wakes a second; a module about 50 frames a second of a few
register writes, plus the replayer's tick in `modplay` (well under a raster line's worth a tick on a 68020 for the
effect logic in C, to be measured on the A1200).

## 14. Build order

1. This note, its review; the claims (`docs/claims.md`); the decisions.
2. The kernel's claim (`kernel/drv.c`, `kernel/audio.c`, `kernel/netproto.h`), the budget line.
3. `sndd`, `sndproto.h`, `sndlib.c`; `/etc/svctab`'s line; the Makefile (its own commit).
4. `snddec.c` and `play`; the host test of the decoders.
5. `modrep.c` and `modplay`; the host test of the replayer.
6. The emulator test; the documents (features, commands, man pages, architecture, ROADMAP).

## 15. The review (2026-11-12) and what changed

One independent read of revision 1 against DESIGN.md, architecture.md, frozen-interfaces.md and the code it builds on
(by a helper with no part in writing it). Its findings and what was done:
1. SYS_drv's network gate came before the claim: the Paula claim is handled before it, with its own gate; the other
   ops answer the sound server -EPERM; frozen-interfaces says so (section 4).
2. netd's end released every claim, Paula's too: skipped now (section 4).
3. A killed sndd lived on up to 5 s (the kernel holds a serving server's death) and could start DMA after ZINE: sndd
   ends itself at evwait's -EINTR (section 11).
4. Stream ids meant nothing across a restart: handles carry sndd's start count (section 5).
5. The timing ignored the kernel's long sections (the floppy): said, the half's length an option, the default to be
   measured on the stock A1200; "never stale" dropped (section 6).
6. The DMA-off wait was one period of the new period, capped: two periods of the old (or new) one, on the E-clock,
   also before memory is freed; periods range-checked (sections 3, 8.3).
7. The built-in stop beside D19's quiesce program: kept as the base's own step (D39's way, one place), the reasons
   said; the claim's slot said (section 4).
8. The claim never ended while sndd lived, so the bell stayed silent: sndd's idle end after 30 s; idle streams stop
   their DMA (sections 6, 11).
9. The bell could race the claim: `audio_hold` masks interrupts (section 4).
10. The frame timing's promise: its conditions said (priorities, long kernel sections), the clock's start on a tick
    (section 8.4).
11. Protocol gaps: SND_INFO's stream argument, SND_PAUSE for frames only, SND_VOLUME and a second SND_BANK on frames,
    SND_FRAMES' count and frame size, the frame's limits, the write's answer, the stream count (sections 5, 8.2).
12. The test state block in a frozen struct's growth path: a request of its own, `SND_STATE`, not frozen (section 5).
13. Ctrl-C: play and modplay catch no SIGINT, write in pieces; modplay's loop said (sections 10, 11).
14. ADKCON cleared at sndd's start (section 3).
15. The kernel's room: the binding figure is the mount tests' floppy; measured (section 4).
16. The claims list spelt out (docs/claims.md).
17. The half's bounds said again (section 6). 18. The least period 123/124 cited right; the E-clock's wrap (sections 3,
    8.4). 19. Pid reuse: none, said (section 11). 20. sndd makes its port before it claims; "audio" reserved (section
    4). 21. The claim's protection overstated: said (section 2). 22. `play -S` in the synopsis (section 10).

## 16. Left for later

- play's reader: 44.1 kHz 16-bit stereo decodes at about half real time on FS-UAE's stock A1200 (14 MHz 68020, 4 MB
  fast RAM; emulator time, measured 2026-11-12 with `time play`), after its loops were made one per sample kind (it
  was a fifth before); 11,025 Hz 8-bit mono plays 10 s with no underrun. Faster code (asm for the 16-bit pair
  averaging) only if the owner wants CD files on a stock A1200.
- Measure on the stock A1200: the worst kernel section (a floppy track read, a program's load) against the halves'
  length, and choose the default from it; the CPU a PCM stream and a module cost.
- Record FS-UAE's sound in a test (the OpenAL file writer) and compare a played tone's pitch and a module's rendering
  with one computed on the PC.
- Mixing several sounds on one pair, priorities between programs, the bell played by sndd while it holds Paula, a
  14-bit mode (two channels a side at volumes 64 and 1), a finer timer for frames.
