# Block 6's closing audit, 2026-11-12

Scope: roadmap block 6, "sound and hardware". Everything merged since `0cb5677` up to `9b85174`: D40 sound
(`/sbin/sndd`, `play`, `modplay`, `kernel/audio.c`, `DRV_PAULA`), D41 optional memory protection (`kernel/mmu.c` and
its hooks, `zenixboot MMU=`), D42 faster IDE (`kernel/ide.c`'s long moves, ZenixFS whole-block runs, ext2
read-ahead), D43 RTG (`display/server/fb.c`, displayd's card, `zenixboot RTG`) and D44 CD (the ATAPI code in
`kernel/ide.c`, `SYS_atapi`, diskd's `cd0`, `user/iso9660d.c`, `mount`). ZINE's work in the same span (d29 16.15-16.17)
is not covered.

Two independent read-only reviewers took D40 and D41; this audit read D42-D44 and the sizes itself and checked each
reviewer finding again in the source before it went in. "Verified" names the test that showed it; "read" means from
the source only.

**Result:** 0 high, 5 medium, 14 low, 8 info; and 4 size proposals (section 2). Fixed with tests or corrected:
A6-1 to A6-7, A6-9 to A6-13, A6-15, A6-16, A6-22. Open: A6-14, A6-17, A6-23, A6-27 (small, listed with their fixes).
Noted: A6-8, A6-18 to A6-21, A6-24 to A6-26.

## 1. Findings

| Id | Severity | Where | Problem | Fix | Status |
|---|---|---|---|---|---|
| A6-1 | medium | `user/modrep.c:103` | A 15-sample module's loop start counts bytes, so `loop = off + ls` could be odd; sndd's `place_ok` refuses odd places, so the whole `SND_FRAMES` got -EINVAL and modplay stopped with "Invalid argument" (status 2, "no sound" by its page). Verified (host: a 15-sample module with loop start 9) | The loop start rounded down to even | fixed (test: sound-host-test, "15-sample module with an odd loop start: rounded down to even (8)"; fails on the old code) |
| A6-2 | medium | `user/modrep.c:400` (the output), `user/modplay.c:74` | Finetunes +2..+7 give B-3 periods of 108-112, and a vibrato on a high note goes below 113; modplay then left `SNDC_PER` off, so the channel kept its previous period (on a first note whatever AUDxPER held). Verified (host: finetune 7, note 113 gave 108) | The replayer's output period held at 113 (the server's lowest, `SND_FMINPER`) instead of dropped | fixed (test: sound-host-test, "finetune 7, B-3 (108): held at the server's lowest period, 113") |
| A6-3 | medium (test) | `tools/sound-host-test.py` (the 200 damaged modules) | The fuzz checked only "inside the bank" and the volume, not sndd's own refusals (even places and lengths, periods 113-4,095): A6-1 and A6-2 passed every test. Read | The fuzz checks both on every tick and channel | fixed (it found 2 such ticks in the old code's run: the same seed now passes) |
| A6-4 | low (test) | `tools/sound-test.sh:32` | "... and again after a restarted sndd" matched `^.?0$`: any line "0" anywhere in the log passed it. Read | The count printed as `b1c=N` and checked as `b1c=0$` (the bell's BEL byte starts that line) | fixed (test: sound-test at stock speed, every other check passed; the new check passes on that run's log and fails with the count changed to 1) |
| A6-5 | low | `user/modplay.c:127`, `:54` | A module with more than 1 MB of samples got the server's -EINVAL, said as "Invalid argument", exit 2. Also `(fsize - off + 1) & ~1u` read one byte past the file's buffer for an odd bank (malloc's padding in practice). Read | Said before the start ("its samples are over 1 MB", exit 1, as its page now says); the buffer one byte longer and that byte 0 | fixed (read; the page updated) |
| A6-6 | low | `user/sndd.c:122` wait_bits | A hit wait limit (Paula not taking LC/LEN in a period and a millisecond) was only counted (`play -S`), never said; CLAUDE.md: every hardware wait says what it waited for. Read | Said once on sndd's output | fixed (read; sound-test unchanged) |
| A6-7 | low (docs) | `docs/d40-sound.md` sections 6, 7, 11 | Three places did not match the code: the refusal's text ("in use by PID", the code says "by another program"); "a long word at a time" (stereo goes byte by byte, halves are cleared); a call arriving already cancelled "answered -EINTR at the next look" (it is served normally: no POLLPRI for it). Read | The text corrected | fixed |
| A6-8 | info | `docs/man/play.txt:58`, `docs/features.md:106` | Emulator speed figures for decoding (labelled as FS-UAE's) | The project's practice labels them; kept | noted |
| A6-9 | low | `user/play.c:91` | `snd_info`'s result unchecked: a failure printed the stack's bytes in the format line. Read | `hi` zeroed first | fixed (read) |
| A6-10 | medium | `kernel/mmu.c:300` view | The update bundle was flipped in and out by the tenant's uid *at the time of the flip*: a root tenant that called setuid kept the bundle's pages valid (read only, with the kernel's neighbouring blocks in its partial end pages) for every later protected process until `UPD_RELEASE`. Read | Flipped out whatever the uid (`!in`); in only for root as before | fixed (test: mmu-test 030 and 060 unchanged; the case itself read) |
| A6-11 | medium | `kernel/mmu.c:489` mmu_setup | `mmu_regs_save` (MOVEC of TC, the TTRs, URP) ran before the "asked for?" test, so every 68040/060 boot without `MMU=ON` executed MMU register reads; D41 section 3 promises the EC parts none unless asked (MC68060UM 1.1.2.2: URP/TCR "must not be used" on an EC060). Read | A 68040/060 not asked returns before the save; the 68030's cleanup still runs on every boot | fixed (test: mmu-test 030 060, and `MMU_TEST_OFF=1 mmu-test 060`: "protection off") |
| A6-12 | low | `kernel/mmu.c:404` flipf, `:392` mmu_lend | These flipped with every interrupt masked, where retenant lowers to level 4 so the serial receive keeps running: freeing a 4 MB block or lending a big view flipped about 1,000 descriptors with the serial port deaf (D41 section 9's own concern). Read | Both lower to level 4 like retenant (`sr_to4`) | fixed (test: mmu-test 030 060) |
| A6-13 | low (docs) | `kernel/ipc.c:405`, `docs/architecture.md`, D41 section 8 | The client's view stays lent until the next retenant, not "ipc_recv to ipc_finish": when `replyrecv` finds another request queued, the server serves it still seeing the earlier client's memory. Read | Docs corrected; doing it at once costs a retenant per message (D41 section 15) | docs fixed |
| A6-14 | low | `kernel/main.c:472-477`, `kernel/mmu.c` mmu_setup | A 68030 left with AmigaOS's translation on (Enforcer, CPU FASTROM) is switched off only in mmu_setup, after the kernel copy, `vectors_init`, `mem_init` and `mem_selftest` ran under the old tables, which may lie in memory Zenix takes as free. Older than D41. Read | Switch the 030's translation off in the hand-over, as `handoff-c.S` does for the 040/060 | open (a hand-over change: the main session's, with a 68030 test) |
| A6-15 | low (test) | `tools/mmu-test.sh:85` | "a write into the kernel's image" used the same loose pattern as the 0x1000 line before it, so a wrong or empty address (strtoul 0) still passed. Read | The kernel's address taken from its boot line and matched exactly | fixed (test: mmu-test 030 060) |
| A6-16 | low (docs) | `docs/d41-memory-protection.md` sections 5, 10, 12, 15; `kernel/mmu.h:4` | `CRP` "limit 0x7FFF" (the code: 0x80000002); log texts that are not the code's ("68EC030?", "not memory"); "machines without protection run none of it" (mmu_setup's head, the 030 cleanup, the system mask and alloc_in/carve run everywhere, with the same cuts); mmu.h's list of callers. Read | Text corrected | fixed |
| A6-17 | low | `kernel/mem.c:360` mem_owner_of | An address inside a used block's 16-byte header answers "free memory" in the fault line; any user-mode bus error with protection on is called "memory protection", also a real one in a system process. Read | Header addresses as the kernel's; "protection" only for a protected process | open (the fault line's words only) |
| A6-18 | info (docs) | `kernel/proc.c:443` fill_v2 | A service's code copy also gets `MEM_USER` (whole pages) with protection on, where D41 section 6 says a service "gets its blocks as before". Needed: a segment may later be shared with a protected process | Noted here; the D41 text stays (it means the data block) | noted |
| A6-19 | info | `kernel/ipc.c:64` port_create | Marks the caller a system process before it knows a port slot is free. Root only, harmless | - | noted |
| A6-20 | info | `user/snddec.c:150-159` | An 8SVX stereo file of several octaves is split at the middle of the whole body, so the left channel gets the wrong data (no memory error; rare files) | Split each channel's first octave | noted |
| A6-21 | info | `user/sndd.c:598` (`a[3] != SNDFRAME_SIZE`) | Right today; when `struct sndframe` grows, the next sndd must still take today's size and step the client's array by `a[3]` | A line in frozen-interfaces.md when it grows | noted |
| A6-22 | low | `user/modplay.c:54` | (A6-5's second half, listed alone) the odd bank's extra byte | - | fixed with A6-5 |
| A6-23 | low | `kernel/ide.c:608-612` ide_atapi | After DEVICE RESET (08h) the driver writes the device register (the master again) 1.4 us later, while the CD is still busy resetting; ATA forbids register writes while BSY, so a real drive may miss the select. FS-UAE never takes this path. Read | A bounded wait for BSY to clear (`ide_wait(ST_BSY, 0, T_START_MS, ...)`, about 20 bytes) before the master is selected again | open (needs a real drive to test: add to d44 section 9) |
| A6-24 | info | `kernel/ide.c:595`; `user/diskd.c` cd_look | A CD that stalls pauses the whole machine up to 30 s per phase (polled, as an IDE disk's spin-up), and diskd answers no one while it waits up to 10 s for a disc spinning up. Both said in limits.md | - | noted |
| A6-25 | info | `zenixfs/zenixfs.c:1208` (readn), `kernel/ext2.c:443` bfill | The new whole-block runs ignore the device's `maxsect` (fatd, ffsd and iso9660d honour it). Harmless today: every driver splits by itself (IDE 256 sectors, the A3000's bounce buffer, the SD card 65,535, the floppy per track) and neither filesystem can be on `cd0` (mount refuses) | Clamp to `maxsect` if a device with a hard limit ever carries them | noted |
| A6-26 | info | `user/diskd.c:77` cd_look | READ CAPACITY's block length is not checked: an audio CD or a 2,352-byte disc then fails every read with EIO (safe, never misread) instead of a clear "not a data disc" | A word in mount's message | noted |
| A6-27 | low | `display/server/displayd.c:420` card_take | For a test's pretend card (`HWFB_TEST`, root's `dpytest -fb`), `bpr * height` is allocated and cleared before `bpr` is range-checked: a huge `bpr` wraps the product (the same wrapped size cleared, so nothing outside), then the record is refused but the block stays allocated. Test path only. Read | Check `bpr` (as below it) before the allocation | open (one line; needs rtg-test) |

Found sound (one line each): sndd's requests (every client pointer through `sys_peer`, sizes and counts, owners and
the handle generation, `place_ok` without overflow, `SND_SILENT`), channels off and the two-period wait before chip
RAM is freed, Paula released at sndd's end and at ZINE, `audio_hold` under `irq_save`, the bell silent while sndd holds
Paula; snddec's chunk bounds and 80-bit rates; modrep's header, order list, sample bounds and effect limits; no
interrupt code in the sound work. D41's descriptors and TTRs per CPU, MOVEC/PMOVE/PFLUSHA encodings, every table change
pushed and flushed, the probe's restore paths, the fault frames of the 030/040/060, every hot hook gated by `mmu_on`
on a 68EC020. D42's long moves only after IDENTIFY hashes the same both ways, writes word by word, every wait timed on
the E-clock; ZenixFS's run reads stop at a cached (newer) block, check the volume's range, and the one-block `sc_r`
copy is dropped by every write, allocation and the rebuild walk; ext2's read-ahead never puts a block in two slots
and its slots are one piece of memory. D43's card record checked (sizes, row bytes, the base's wrap), the pointer and
every rectangle clipped (the host test's 7,028 checks with guard rows). D44's kept window (`skip`, `len`: whole pieces
only inside it, single words otherwise), 64 MB and zero-length phases refused, every wait timed; diskd's `cd0` (the
generation, `ESTALE` only for the mounting server, 128 sectors at most, writes `EROFS`); iso9660d's record, SUSP, `CE`,
`SL`, `CL`/`PL` and directory checks, the link depth, the listing's cookie, the static `cur` in walk's recursion
reassigned before it is used again; mount's read-only rule. The claims (`SYS_atapi` 98, `DISKCAP_RO`, `DRV_PAULA`,
`KOPT_MMU_ON`/`OFF`, `HWFB_*`, `DPYM_RTG`, port `audio`, `SND_*`) match `docs/claims.md`,
`docs/frozen-interfaces.md` and the headers.

## 2. Sizes, and what could leave the always-loaded path

Measured (`make`, this branch): `kernel.bin` 193,112 bytes (budget 193,150; 1,448 bytes left on the mount tests'
floppy). What block 6 put in every machine:

| Part | Where | Bytes | Used by |
|---|---|---|---|
| ATAPI (`cd_probe` 446, `ide_atapi` 694, texts, `SYS_atapi` 58) | kernel | about 1,330 | machines with a CD drive (the probe: every Gayle machine, once at boot) |
| `cd0` in the built-in diskd | kernel (embedded diskd) and diskd's own copy in RAM | 812 + 812 | the same |
| D41 (`mmu.o` 5,546; mem/proc/ipc/... hooks about 1,300) | kernel | about 6,850 | 68030/040/060 with `MMU=ON` |
| D43 (fb.c 1,976, the chunky branches, the card's start) | every displayd | 5,352 (3,584 packed on the DD floppy) | a PiStorm or a card |

The proposals, each with what it saves (none built here):

1. **A span given back after the boot (RAM on every machine, about 15 KB).** The built-in `boot` program (`boot.zx`,
   7,756 bytes) is run once by kmain (`proc_spawn_image` copies it) and then lies in the kernel's memory for good;
   the boot-only code (ide_init and the CD probe, emmc_init, a3000_init, mmu_setup, cache_setup, klog_init, rnd_init,
   kmain's own body: about 8-9 KB by the symbol table, to be confirmed one by one) likewise. Linked into a section at the
   end of the image and handed to the memory pool at the end of kmain, that is about 15 KB more free RAM on the stock
   A1200. No floppy bytes. Needs: a linker-script section, `mem` taking a range after `mem_init`, and care that nothing
   after the boot calls into it (ZINE's way back, `gfx_init`). A deep core change: a design note and two reviews.
2. **Code kept only when its hardware is there (RAM).** With the same mechanism, the parts used only when found:
   `ide_atapi` and `SYS_atapi`'s body (about 0.8 KB) when no CD drive answered; D41's flips, lends, fault decoding and
   switch (about 4.3 KB, the reviewer's measure) when protection is off - i.e. on every machine but a 68030/040/060
   with `MMU=ON`. About 5 KB more on a stock A1200. Same cost and risk as 1, one step further.
3. **displayd without RTG on the demo floppy (floppy and RAM).** The floppy's bootblock never writes a card record
   (D43 section 3), so its desktop can never take a card: build displayd twice from the same files (`-DNO_RTG` leaving
   out fb.c, the chunky branches and the card's start) and put the small one on the DD floppy: 3,584 bytes of the
   floppy back and 5.3 KB less in every desktop booted from it. The disk image and the package keep the full one (or
   add `displayd-rtg`, chosen by displayd at its start when the record is there). Small, low risk; touches the
   Makefile (the main session's).
   **Built 2026-11-12 (branch bootmem):** `displayd-nortg` (`-DNO_RTG`) on both demo floppies; measured 65,236 bytes
   on the disk (-5,300), 96,076 in memory (-4,956); the DD floppy's stream -3,044 bytes, 57,856 -> 60,928 free; the
   HD one's stream -4,100. Tests: demo-test, dpy-test and zdesk-test (stock speed; the full displayd is byte for byte
   the same).
4. **The CD's kernel bytes on the floppy (kernel.bin).** Moving the packet mover into diskd saves nothing on
   `kernel.bin` (diskd is embedded) and would need diskd to do PIO in user mode beside the kernel's ATA commands (the
   reset-warning flush, ZINE): not worth it. The only way to take the 2.1 KB off `kernel.bin` is a loadable kernel
   part (none exists); with proposal 1's section, the probe at least stops costing RAM. Recommended: no move now.

Recommended order: 3 (small, now), then 1 and 2 together as one design note when the kernel next needs room.

## 3. For the partner (one line each, the recommendation in brackets)

1. **A smaller desktop on the demo floppy (section 2, 3):** about 3.5 KB of the floppy and 5 KB of RAM back, the
   graphics-card support kept on hard-disk installs. [Yes]
2. **Giving back the boot's memory (section 2, 1 and 2):** about 15-20 KB more free RAM on the stock A1200, a careful
   kernel change with two reviews. [Yes, when the kernel next needs room]
3. **The CD drive's reset (A6-23) and a 68030 left with Enforcer on (A6-14):** two small fixes best tried on real
   hardware (a CD drive, a 68030). [Yes, with the first real-drive test]

Established 2026-11-12: the reviewers' reading and host builds, this audit's reading; the tests named per item:
`tools/sound-host-test.sh` (passes; its three new checks fail on the old code), `tools/sound-test.sh` (stock speed),
`tools/mmu-test.sh 030 060` and `MMU_TEST_OFF=1 tools/mmu-test.sh 060`, the kernel's size check (`ksize-check`).
