# Block 3's closing audit, 2026-10-30

Scope: roadmap block 3, "the C library, UTF-8, the owner's tools and its audit". Everything merged since block 2's
audit (`42ef547`) up to `63d65e4`: D27 steps 0-4 (picolibc vendored, the port in `libc/zenix`, the pure build and the
64-bit division, termios and processes, the bridge), the allocator (`user/zmalloc.c`, NREG 16), D28 (the console's
UTF-8 decoder, the cells, `CON_UFONT`, the keys, init's font line, telnet's modes, sh/edit/less/ls), the ports
(`ports/common`, the text-UI library, Sumi, Maki, Markdown, Ahiru, `ctop`), Shift+Tab and the kernel's shrink. D27
step 5 was measured first (`docs/d27-c-library.md` "Step 5: the measurement": not a clear win, nothing switched).

Three independent read-only reviewers, one angle each: (a) security and robustness (untrusted input: font files,
UTF-8 from the network, archives in Ahiru, the files the ports open; privileges; allocation failures; stacks; the
frozen interfaces and the claims); (b) the ports' correctness on Zenix (the AmigaOS layer's mappings, Ahiru's file
operations never losing data, settings paths, the licences in the images); (c) size and speed on the stock A1200
(each program's memory, the kernel's budget, the hot paths). Each finding was read again from the source before it
was fixed; "verified" names the test that showed it, "read" means from the source only.

**Result:** 1 high, 7 medium (one of them speed, one the licences), 8 low (three of them speed or memory), 9 info.
Fixed with tests: B3-1 to B3-5, B3-7 to B3-13, B3-15, B3-16 and B3-21. Not bugs: B3-14, B3-22. Open: B3-6 (a fix in
the owner's Ahiru), B3-17 to B3-20 (proposals), B3-23 (the licences, the owner's); B3-24 and B3-25 noted. Block 2's
B2-16 is fixed (its own file).

## 1. Findings

| Id | Severity | Where | Problem | Fix | Status |
|---|---|---|---|---|---|
| B3-1 | high | `ports/common/dos.c` Rename (used by Ahiru's `r` and `:rename`, `ports/ahiru/upstream/fs/ops.c:1152`, `:1197`) | Rename was rename(2), which replaces an existing file silently; upstream relies on AmigaOS's refusal (ERROR_OBJECT_EXISTS) and asks nothing: renaming `a` to an existing `b` lost the old `b`, and the `.info` after it could overwrite another file the same way. Verified (reviewer, on the PC) | Rename refuses an existing other object at the new name (EEXIST, ERROR_OBJECT_EXISTS); the same file under another name may be renamed. Upstream's saves (Sumi, Markdown, Ahiru's copy) move the old file aside first, as on AmigaOS | fixed (test: dos-host-test, "Rename onto another existing file is refused, both files kept") |
| B3-2 | medium | `ports/common/dos.c` SystemTags/Execute; callers `ports/ahiru/upstream/ahiru/src/ops.c:352`, `:451`, `more2.c` (open with), `ports/maki/upstream/src/main.c:1050` | Upstream quotes a name AmigaDOS's way (`ed "NAME"`), where nothing is expanded; Zenix's sh expands `$(...)`, backquotes and `$VAR` inside double quotes: a file named `x$(rm -rf ~)` ran the command when it was edited or opened with Ahiru or Maki. Read | Each quoted part becomes a single-quoted one for sh (`'` as `'\''`). Left: a name holding a `"` still ends its quotes early (proposal 3.4) | fixed (test: dos-host-test, "a quoted name holding $(...), backquotes, $VAR and ' is passed as it is") |
| B3-3 | medium | `ports/common/dos.c:168` Open | MODE_OLDFILE opened read only; AmigaOS's is read and write, and Ahiru's core writes disk images (ADF) in place through it: every write into an ADF failed (EBADF; nothing was half-written, the crash-safe order held). Verified (reviewer, on the PC) | MODE_OLDFILE opens O_RDWR, read only when writing is not allowed | fixed (test: dos-host-test, "MODE_OLDFILE opens for writing in place", "a read-only file still opens") |
| B3-4 | medium | `ports/common/dos.c` SameLock; `libc/zenix/sys.c:64` (no `st_dev`) | Zenix's stat has no device, so every pair of paths was "the same volume": Ahiru's moves to another mount always tried rename and failed (EXDEV, never reaching copy-then-delete); "the same file" and "inside itself" were refused for unrelated files with equal inode numbers on two mounts; Sumi and Markdown switched to an unrelated tab. Read | SameLock (and Rename's same-file test) tell mounts apart by the mount table, links resolved (`zmount`). A folder moved to another parent on ext2 still fails (the kernel's EXDEV for directories): proposal 3.5 | fixed (test: dos-host-test, "two files on different mounts are LOCK_DIFFERENT") |
| B3-5 | medium | `ports/ahiru/upstream/fs/ops.c:30` (icons on), `:1034`, `:970`, `:1157` | On AmigaOS `NAME.info` is NAME's icon and travels with it; on Zenix it is a file of its own (Texinfo's): deleting NAME removed `NAME.info` unasked, moving or copying NAME could overwrite a `NAME.info` at the target without the collision question. Read | Ahiru's Zenix layer starts with icons off (`ports/ahiru/zenix/os.c` os_init); ahiru(1) says so | fixed (read; the setting has one reader, `icons`) |
| B3-6 | medium | `ports/ahiru/upstream/fs/ops.c:664` (resolveone), `fs/arcw.c:1131` (memberof), `:1165` (delstage) | An archive member or ADF folder named `..` (a plain name on AmigaOS) is the parent on Zenix: pasting it writes its contents into the target's parent; deleting it from an archive (the rebuild path, blocked today because `C:lha`/`C:zip` are never there) would wipe the folder holding the archive. Read | In upstream (the owner's Ahiru): the `safename` rule in resolveone and memberof, or `..` listed as `_..`. Zenix carries upstream unchanged (the owner's decision of 2026-10-28) | open (for the owner's Ahiru: section 3.1) |
| B3-7 | low | `ports/common/dos.c` (T: and RAM: as /tmp) | The programs' fixed names in the shared /tmp (mode 1777): `Sumi-N.save`, `Markdown-N.save` (the rescue when the terminal goes away), `CFileV`/`CFileE` plus a fixed address, `Ahiru.log`: another user's file of that name made the rescue fail and the text was lost; a soft link planted there made root's Sumi write through it (`/etc/passwd`). Read | T: and RAM: are `/tmp/T-UID`, the user's own folder, mode 700; one of that name that is not the user's own or is a link is refused | fixed (test: dos-host-test, "T: is /tmp/T-UID, mode 700", "refused when it is a planted soft link") |
| B3-8 | low | `ports/ahiru/zenix/os.c:69` (ds_next), `ports/common/dos.c:401` (ExNext) | A name whose path is 64 bytes or more was left out of listings, so copying its folder left it out without a word (CLAUDE.md: a limit says so). Read | Listed as an empty file nobody may read: a copy or move fails on it ("too long") | fixed (test: dos-host-test, "ExNext lists a name whose path is too long") |
| B3-9 | low | `ports/common/dos.c:448` DeleteFile | rmdir was tried only after EISDIR; POSIX also allows EPERM for unlink of a folder (an NFS server's), and then a folder could never be deleted. Read | rmdir after EISDIR or EPERM | fixed (test: dos-host-test, "DeleteFile removes an empty folder"; the EPERM case read) |
| B3-10 | low | `user/zmalloc.c` memalign1 (`aligned_alloc`, `posix_memalign`) | `n > MALLOC_MAX - align - 8` wrapped for an alignment above MALLOC_MAX: a 16-byte block came back and the header went gigabytes past it. Verified (reviewer: SIGSEGV on the PC) | An alignment above MALLOC_MAX / 2 is refused | fixed (test: libctest, "memalign with a huge alignment refused") |
| B3-11 | medium (speed) | `user/zmalloc.c` free, free_med, free_slow (`binmap >> BIN_LARGE`) | Any free block of 64 KB or more made every free flush the quick lists, and a heap that has just grown always has one (the new chunk's untouched rest; chunks 9-10, Ahiru's and Markdown's range, are 64 KB and more): every malloc took the slow path until the chunk was used up, 7 times slower on the PC. Verified (reviewer: 12.2 against 1.7 ns a call) | The newest chunk's untouched rest is not "memory given back": the flush rule skips it (`fresh`, `given_back()`); memory really given back still flushes at once, so chunks go back as before | fixed (test: zmalloc-test, "a grown heap keeps its quick lists"; fails with the old code; the replays' budgets and the stress runs' "given back" unchanged) |
| B3-12 | low | `user/zenv.c:16` zenv_load, `kernel/syscall.c:357` SYS_envset | A parent could store 1,024 bytes without a final 0; every child (also the set-user-id `su`, `sudo`, `passwd`) ran strlen past the buffer. Verified (reviewer: AddressSanitizer) | The buffer has one byte more and is ended after the read. The kernel refusing such a block is an interface change: proposal 3.6 | fixed (test: libctest, "an environment block without its last 0: the child's strings end inside it"; the old code passed it only because the bytes after the buffer happened to be 0) |
| B3-13 | info | `user/zenv.c` setenv, env_save | setenv returned -1 at 63 variables without an error number, accepted a name with `=`, and a variable past the kernel's 1 KB was dropped by env_save without a word | EINVAL for a bad name, ENOMEM (the old value kept) when it would not fit | fixed (test: libctest, "setenv of a name with = refused", "setenv too big refused, the old value kept") |
| B3-14 | info | `libc/zenix/spawn.c` (`err = errno` after SYS_envget/envset) | Asked whether those set errno | They do: sys3 sets it (zerrno is errno in the C library) | not a bug |
| B3-15 | info | `ports/tui/zenix/core.c` zreadsome | A full input buffer would read 0 bytes and look like the end of input (the program's rescue save and exit); no caller lets it fill today | A full buffer reads nothing and is not the end | fixed (read) |
| B3-16 | info | `user/zpoll.c` select | 512 bytes on the stack for the C library's FD_SETSIZE (64) where a process has 16 descriptors; a timeout over about 24 days wrapped into "for ever" | The array sized by NFD, a descriptor past it EBADF, a negative timeout EINVAL, a long one held at about 23 days | fixed (test: libctest, three select checks) |
| B3-17 | low (speed) | `kernel/term.c:217-233` hrow_take | Each scrolled line holding a character above U+00FF scans the high-row pool bit by bit from row 0 (up to 200 bits): about 800-1,600 instructions per scroll once the scrollback is full of such lines (box-drawing output). Read | Hand the leaving line's row to the new one (O(1)) | open (proposal 3.2) |
| B3-18 | low (speed) | `kernel/term.c:952` put1 | The plain ASCII path is about 45 instructions to gfx_cell against 38 before D28 (the `>= 0x80` test, a spill, the A_HI read): about 4-5% per character. Measured (objdump) | Load `at[o]` once; keep put1's printable path at -O2 | open (proposal 3.2) |
| B3-19 | low (memory) | `ports/tui/upstream/src/core.c:61`, `:308` | Each text-UI program keeps two grids of 6-byte cells (30 KB at 80 x 32); the console holds only the BMP, so 4-byte cells would save about 10 KB per program. Measured | An upstream change (the owner's library) | open (proposal 3.3) |
| B3-20 | info | libz's `cw_*` tables, picolibc's wcwidth tables, the kernel's | Three copies of the width tables (about 2.6 KB in each width-counting libz program, 3.5 KB in each port, 2.5 KB in the kernel). Measured | One format shared by libz and the C library would save about 1.5 KB per port | open (accepted for now) |
| B3-21 | info | `libc/picolibc/sources.mk` | libc.a and libc-pure.a held two members each of memcpy, memset, strcpy and strlen (machine/m68k's and the generic byte loops) and twice localedata and frexpl (listed twice): the fast ones won only by their order | The generic four and the doubled lines left out; the archives depend on the list; libc-host-test checks one member of each name | fixed (test: libc-host-test, "one member of each name"; the ports link the same machine/m68k code) |
| B3-22 | info | `kernel/proc.c:283` user_ok_wr | With NREG 16 a buffer outside the process block scans up to 16 slots (about 80 instructions a call) | None: small against the system call | not a bug |
| B3-23 | medium (licence) | `Makefile:759` AHIRU_CORE (`core/lha.c`, `lzx.c`, `dms.c`) | Block 2's "Licences to settle": the LHA and LZX readers (from LHa for UNIX and unlzx, not GPL-compatible or no licence) are built into `/bin/ahiru` on every image and in the ahiru package, with only the owner's MIT notice | A build without them (Ahiru then says "not supported"), or the clean rewrite block 2 recommends; DMS (public domain) may stay | open (the owner's: section 3.7; before any public release) |
| B3-24 | info | `ports/common/dos.c:148` FGets | One read-ahead buffer for every handle: switching FGets between two handles would drop bytes read ahead (no caller does, all eight checked) | Noted in ports/common/README | noted |
| B3-25 | info | `tools/ksize-check.sh`, the mount tests' floppy | kernel.bin 166,876 bytes: 5,124 under the kernel budget, 4,132 bytes left on the mount tests' floppy (ROOM 1,024); D28's per-console memory comes only with a used console. Measured | Watch: the next 3 KB of kernel growth takes the mount floppy's RAM disk down again | noted |

Found sound (one line each): `CON_UFONT` (root only, every header field, glyph count, page and size checked on the
kernel's own copy) and the glyph offsets in `gfx_ucell`; the UTF-8 decoder (Unicode's table 3-7, broken sequences as
byte cells, a character never split across writes); wide characters at the last column, in ICH/DCH/ECH/EL, scroll
regions and the scrollback; `A_HI` not settable by SGR, `CON_READ`'s bounds, `CON_FONT`/`CON_RULES` only on the
caller's own console; `kbd.c`'s UTF-8 keys and Shift+Tab, tty's erase; telnet's key buffer and filter; init's font
line; the allocator's overflow checks in calloc, realloc and malloc; glob, zsig, zpoll, zout, zpw; Markdown's depth
limit on its 32 KB stack, Ahiru's tree depth and its paths on the heap; Ahiru's copyfile (an existing target never
emptied, short writes and close checked, a failed copy deleted), copy-then-delete only after a checked copy, deltree
(no soft link followed, stops at the first failure), copy onto itself or into its own folder refused; Seek's old
position, ExNext's end, IoErr's numbers and the inverted rwed bits; the settings without HOME; every port's MIT
notice in `/usr/share/licenses`; the ZEX2 programs share their code; consoles never used cost nothing; the
allocator's fast paths are O(1); the stacks measured. The claims (`CON_UFONT` op 4, `CONR_8BIT`, `CONRF_CELL4`) are in
`docs/claims.md` and `docs/frozen-interfaces.md`.

## 2. The new programs' memory (reviewer c, measured from the built files)

| Program | Shared code | Data + BSS per process | Stack | Text-UI grids at start (80 x 32) | First one running |
|---|---|---|---|---|---|
| sumi | 92,848 | 16,922 | 16 KB | 38.8 KB | about 165 KB |
| maki | 69,648 | 19,524 | 16 KB | 38.8 KB | about 144 KB |
| markdown | 143,088 | 43,728 | 32 KB | 38.8 KB | about 258 KB |
| ahiru | 230,608 | 49,782 | 16 KB | 38.8 KB | about 336 KB |
| ctop | 58,432 | 46,122 | 16 KB | 38.8 KB | about 160 KB |

Each fits alone in the stock A1200's 1,132 KB; Ahiru crowds it most (about 30%, its traced peak 179-196 KB more), and
Ahiru with Sumi opened from it about 500 KB (44%). Each program carries its own 28-38 KB of the C library and the
text-UI library (static, D27 section 8).

## 3. For the owner (one line each, the recommendation in brackets)

1. **Ahiru's `..` members (B3-6):** a small fix in your Ahiru (the `safename` rule for a selected archive member and
   an ADF folder named `..`), then Zenix takes the new version. [Yes]
2. **The console's speed (B3-17, B3-18):** two small kernel changes for faster scrolling of box-drawing lines and
   about 4% faster plain text; timed on the stock A1200 before and after. [Yes, with your A1200 timing]
3. **Smaller text-UI programs (B3-19):** 4-byte cells in your text-UI library save about 10 KB per program on Zenix
   (and on AmigaOS). [Yes, in tui.library]
4. **Names holding a `"` (B3-2's rest):** your programs pass a file name to a command inside the command's text; a
   way to pass it apart (Zenix would then run it with no shell) closes the last hole. [Yes, in your programs]
5. **Moving folders on the same disk (B3-4's rest):** Zenix's ext2 cannot move a folder to another folder (it says
   "cross-device"), so Ahiru cannot either; teaching the kernel's ext2 to do it is a new feature. [Later, approve when
   wanted]
6. **The kernel refusing a broken environment (B3-12):** the C library is fixed; the kernel could also refuse a block
   without its last 0, a frozen-interface change that only grows. [Yes]
7. **The LHA and LZX readers in Ahiru (B3-23):** before any public release, either Ahiru without them or a clean
   rewrite from the formats' descriptions; DMS stays. [Clean rewrite later; leave them out of a public build]
8. **libz smaller by 290 KB (D27 step 5):** building libz's cold parts smaller saves about 290 KB over the whole
   system and 1.8 KB in the kernel, with no change to any program; the hot string code keeps its speed. [Yes, the main
   session's to do with a speed check]

Established 2026-10-30: the reviewers' reading and their host-side tests (named above); the fixes' tests
`tools/dos-host-test.sh` (new), `tools/zmalloc-test.sh`, `tools/libc-host-test.sh`, `/tests/libctest`, and the full
suite (`make check`).
