# D33: LhA and LZX unpacking (design note)

Status: **built on branch lha (2026-11-01), the fourth item of block 4 (ROADMAP "The order of work", item 4).** It also settles a
public-release blocker: Ahiru's old readers were derived from LHa for UNIX and unlzx (docs/audit-2026-10-29-block2.md, "Licences to
settle"). These are a clean rewrite. Read "Where the facts came from" before relying on that.

## What it is
Two commands, `unlha` and `unlzx`, one program by its name as gzip and gunzip: `unlha l|t|x [-f] [-q] [-d folder] [-L megabytes] archive
[name...]`. Unpack only; making archives is out of scope. The decoders are `user/arcread.c`, `user/arclha.c` and `user/arclzx.c`,
compiled into the commands and into Ahiru (its `arcfs`, through `core/arc.h`). No copies.

## Decisions (made here, the owner delegated the details)
1. **Names.** `unlha` and `unlzx`. `lha` would promise `lha a`; a real `lha` may come later. No clash: neither name was in use in
   UAPPS, the ports or the docs. `unlzx` is also a known name on other systems, and does the same job there.
2. **Commands, not a filesystem.** The roadmap asks for unpacking; Ahiru gives the "open as a folder" view.
3. **Methods.** LhA: `-lh0-`, `-lz4-` (stored), `-lh1-`, `-lh4-` to `-lh7-`, `-lhd-`; levels 0, 1, 2. `-lh2-`, `-lh3-`, `-lzs-`, `-lz5-`,
   `-pm?-`, `-lh8-` and up are listed, and answer "method is not supported" with exit status 1: real Amiga archives do not use them
   (12,600 members of the owner's archives: only `-lh0-`, `-lh5-`, `-lh6-`, `-lhd-`). Level 3 stops the listing with the same word.
   LZX: pack modes 0 (stored) and 2 (normal), merged or not; another mode is listed and refused.
4. **Names in archives.** Valid UTF-8 stays, anything else is Latin-1 and becomes UTF-8. Control bytes become `_`. `\` and 0xFF in LhA's
   level 0 and 1 name fields and in its directory field are `/`.
5. **What is refused, member by member, with exit status 1:** a name starting with `/`, a `..` part, a link (`-lhd-` with `|`, or the Unix
   link mode), a name that does not fit 255 bytes of UTF-8. No link is ever made; a soft link on the way to a target is not followed;
   nothing is overwritten without `-f`, and `-f` never writes through a link (the name is removed, then made anew with `O_EXCL`).
6. **The limit.** `x` and `t` add up the members first; over 512 MB unpacked (`-L` sets it, at most 4000 MB) nothing is done and it says
   so. So a small archive claiming gigabytes costs a few header reads. A member stops at the size in its header, a stream that says more
   is damage; every token takes input, or (a one-symbol LhA table) at most a block of them, so damage cannot hang.
7. **What follows LhA's end mark** may be anything (real archives have a BBS's junk there), but a member right there is damage, or a flipped byte would hide the rest and call it success. A
   file of only zeros, or an empty one, is no archive. A real archive may reach past the file's end only if cut short, which is damage. **A damaged merged group** answers its later members at once, with the same error: it is decoded once. **The limit** counts all of a group's
   bytes before the member asked for: they are decoded, written or not. A name cut by a buffer is refused, not unpacked under what is left. A directory that is only `./` is nothing to do.
   Valid UTF-8 for U+0080-U+009F becomes `_` as control bytes do. Files of 2 GB or more cannot be read (Zenix's `lseek`), an error.
7a. **A failed member does not stay:** a checksum error or damage removes the partial file. The exit status is 1; the archive's structure
   being damaged is 2, with what came before it unpacked. Dates: as stored, read as UTC. Modes: LhA's Unix field `& 0777`, else from `rwed`
   (r gives 444, w 200, e 111), nothing at all gives 600; never set-user-id. Filenotes are listed, not stored (Zenix has no such thing).
8. **Ahiru** is strict: a member that may not be written makes the list call fail (`arc_list_refused` 0). Ahiru then opens the archive as damaged and read-only, with
   only the members before it (`arcfs_load`); if it is the first, the archive does not open. It is never shown a name that leads out.
9. **Block type 1 in LZX** was never seen: real LZX 1.21 wrote only types 2 and 3 for text, code, random data and all its compression
   levels, and none of 1,647 real archives has one. It answers "not supported" instead of a guess.

## Memory (no fast RAM needed; sizes from `sizeof`, measured 2026-11-01)
| What | Window | Tables and input | Total |
|---|---|---|---|
| `-lh0-`, `-lz4-` | none | 2.1 KB | 2.1 KB |
| `-lh1-` | 4 KB | 7.2 KB | 11.2 KB |
| `-lh4-` | 4 KB | 6.6 KB | 10.6 KB |
| `-lh5-` | 8 KB | 6.6 KB | 14.6 KB |
| `-lh6-` | 32 KB | 6.6 KB | 38.6 KB |
| `-lh7-` | 64 KB | 6.6 KB | 70.6 KB |
| LZX stored | none | 4 KB | 4 KB |
| LZX normal | 64 KB | 9.3 KB | 73.3 KB |

All of it fits a 2 MB chip-only A1200 (1,132 KB free with one login). The window is 64 KB because the format's largest distance is 65535: it
cannot be smaller. The listing adds a header buffer (LhA 2.5 KB, LZX 0.9 KB), the checksum tables 1.5 KB. An empty member takes none of it. The program: 37.7 KB on the disk (`/bin/unlha`; `unlzx` is a link).

## The LZX format, as this reader reads it (*verified* against LZX 1.21's own output)
All of this was worked out by experiment, in the order below; each step's test is named. Nothing here was read from another reader.

**The archive** (from hex dumps of archives whose contents and listing were known). 10 bytes starting `LZX`, then entries. An entry: 31
bytes, then the name, then the filenote.
| Offset | Size | What | How established |
|---|---|---|---|
| 0 | 1 | attributes: r 1, w 2, d 4, e 8, a 16, h 32, s 64, p 128 | `protect` on one file each, packed, dumped |
| 2 | 4 LE | unpacked size | known files |
| 6 | 4 LE | packed size; in a merged group 0 but in the last member | known files; a group of known total |
| 10 | 1 | machine (10) | always 10 here |
| 11 | 1 | pack mode: 0 stored, 2 normal | `-0` against `-2` |
| 12 | 1 | bit 0: merged | `lzx v`'s word "merged" against the byte |
| 14 | 1 | filenote length | a `filenote` of known length |
| 15 | 1 | version (10) | always 10 here |
| 18 | 4 BE | date: 5 bits day, 4 month from 0, 6 year, 5 hour, 6 minute, 6 second | files of known dates |
| 22 | 4 LE | CRC-32 of the data | computed on known files |
| 26 | 4 LE | CRC-32 of the header (31 bytes with this field 0, the name, the filenote) | computed; a file with a filenote told name-only from name + note |
| 30 | 1 | name length; a name ending in `/` is a directory | known names |

**Years:** archive `Test_LZX.lzx` in the owner's files (made with the year-2000 patch) has empty files named after their years: the field reads
8-29 = 1978-1999, 58-63 = 2000-2005, 30-57 = 2006-2033, 0-7 = 2034-2041. An unpatched LZX 1.21 writes a wrong year after 1999 (2026 as 20, read
as 1990); nothing can tell.

**A merged group** is the members' headers one after another, then one stream, after the last header. Its bytes are the members' bytes in
order. A group is a stream of its own (tested with `-M1`, many groups).

**The stream** (each found by searching on a block of known size, then checked on all streams): 16-bit big-endian words, each taken from its low
bit up. A block: 3 bits type; type 3 then has 8 lengths of 3 bits (the aligned table); then 3 pieces of 8 bits, high first: the block's unpacked
size; then the lengths of the main table. **Type 2 has no aligned table, type 3 has.** Found on `-2` output: the size field was located by search
over every bit order; the order "aligned before size" by checking both (the wrong one gave a Kraft sum under 1 and a size past the file).

**Main table:** 768 symbols, 256 literals + 512 matches, in two halves, each with its own small table: 20 lengths of 4 bits, canonical code
(shorter first, by symbol; the code's first bit is the first read). In the halves a symbol is: 0-16 a change, new = (old - symbol) mod 17, where
old is the block before's (all 0 at a group's start); 17 a run of zeros, 18 a longer run, 19 a run of the same change (the next code says which).
**The two halves differ:** first half 17 = 4 + 4 bits, 18 = 20 + 5 bits, 19 = 4 + 1 bit; second half 17 = 3 + 4 bits, 18 = 19 + 6 bits, 19 = 3 + 1
bit. Found by searching every base and width with the sum of the code's weights (Kraft) exactly 1 and no overrun on four streams: one answer
only. 19's first-half base: the other value makes real archives fail (tested by building that variant).

**A match symbol** (minus 256): the low 5 bits are the distance slot, the high 4 the length index. Found from a grid of the table (rows of 32), then
from a designed file: a de Bruijn sequence (no 3 bytes repeat) with copies planted at chosen lengths and distances, packed by the real LZX, so each
match token could only be that plant. Distance bits come first, then length bits.
- Slot 0: the last distance again, 1 at a group's start. Slot n: base + extra bits, bases 0, 1, 2, 3, 4, 6, 8, 12, 16, ... doubling every second, extra
  bits 0, 0, 0, 0, 1, 1, 2, 2, ... 14, 14. Largest distance 65535. **A distance before the group's first byte reads the window as it starts, all zeros:** real archives need it (found by a sweep of the owner's archives: 70-odd failed at this check, all of the older archive header `4C 5A 58 00 00 00 0A 00 00 00`; with zeros every checksum came right).
- Type 3, 3 or more extra bits: the top ones raw, the low 3 through the aligned table.
- Length index 0-15: base 3, 4, 5, 6, 7, 9, 11, 15, 19, 27, 35, 51, 67, 99, 131, 195, extra bits 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6. Largest 258.

## The LhA format, as this reader reads it
**Headers** (*read* from the public descriptions below, then *verified* against the PC's LHa as a program and the real Amiga LhA 2.15): level 0: byte 0 the
header's size less 2, byte 1 the sum of the rest, 2-6 the method, 7 size packed, 11 size, 15 a DOS date, 19 attribute, 20 level, 21 name length, name,
CRC-16 of the data. Level 1 adds the OS byte, and last the next extended header's size; the packed size includes the extended headers. Level 2: bytes 0-1
the whole header's size, a Unix date, 21 CRC-16, 23 OS, 24 next size. An extended header: size (2 bytes, all of it), type, data, next size: 1 name, 2 directory
(0xFF between parts), 0x3F and 0x71 a filenote, 0x50 Unix mode, 0x54 Unix date. The end: a zero byte or the file's end.
**From experiments with the Amiga's LhA:** a level 0 or 1 name field may hold `name\0filenote`; the attribute is AmigaDOS's protection bits exactly when the OS
byte is `A` or absent (level 0), elsewhere it means something else (0x20 from MS-DOS and Unix); a directory at level 0 is a `-lh0-` of size 0 and name `x\`.
**From experiments with LHa as a program:** level 0 and 1 name fields can hold 0xFF between parts; a link is `-lhd-` with `name|target`.

**`-lh4-` to `-lh7-`** (*read*: blocks of three tables, codes 0-255 literal, 256 and up a length, a position code, windows 4, 8, 32, 64 KB, from the descriptions
below; the bit layout from my general knowledge of the format, then *verified* against archives from LHa and the Amiga's LhA, every CRC): most significant bit
first. A block: 16 bits count of codes; a small table (5 bits count, up to 19 lengths, 3 bits each, 7 continued by ones; after the third a 2-bit run of zeros; count
0: one symbol in 5 bits); the main table of 510 (9 bits count; through the small table: 0 one zero, 1 run of 3 + 4 bits, 2 run of 20 + 9 bits, n length n - 2; count 0:
one symbol in 9 bits); the position table (4 bits count, 14 symbols for `-lh4-` and `-lh5-`; 5 bits, 16 for `-lh6-`, 17 for `-lh7-`). Length: code - 253. Distance: code 0
is 1, code n is 2^(n-1) + (n - 1) bits + 1. The window starts as spaces.
**`-lh1-`** (*read*: 4 KB window, longest match 60, adaptive Huffman, fixed position code; the tree's details from my general knowledge, *verified* on files that pass the
limit 0x8000): 314 symbols; counts start at 1, the tree is built by pairs; after each symbol its count and each ancestor's go up by one, with a node moving past equal
counts; at 0x8000 at the root the leaves' counts halve (rounded up) and the inner nodes are built anew. A position: its top 6 bits in a fixed code of 64 (lengths 3 to 8:
1, 3, 8, 12, 24, 16 codes), the low 6 raw.

## Where the facts came from
**Never opened:** Zenix's old `core/lha.c`, `lha.h`, `lzx.c`, `lzx.h`, the owner's CFile copies, LHa for UNIX's source, unlzx's source, lhasa, libmspack, 7-Zip,
the owner's BBX Go readers, and the leaked AmigaOS sources.
**Run as programs only:** LHa for UNIX 1.14i (to make test archives), unlzx (to list archives while learning the layout), LZX 1.21 and LhA 2.15 on a private FS-UAE test Amiga
(to make archives from designed files); their output was read as data. None was disassembled.
**Public descriptions read:**
- [LHA.TXT, P. Schepers](https://ist.uwaterloo.ca/~schepers/formats/LHA.TXT): header layout, method table.
- [Kaitai Struct's lzh spec](https://formats.kaitai.io/lzh/): the field table, levels 0 to 2 (the page's field table, not its generated parser).
- [Old File Viewer's LZH page](https://old-file-viewer.bobpritchett.com/docs/lzh-archive/): levels, the block structure in words, `-lh1-` in words.
- [PSX SPX on LHarc](https://problemkaputt.de/psxspx-cdrom-file-compression-larc-lharc-lha-lzs-lzh.htm): the method and window table.
- Microsoft [MS-PATCH](https://learn.microsoft.com/en-us/openspecs/exchange_server_protocols/ms-patch/cc78752a-b4af-4eee-88cb-01f4d8a4c2bf) (a relative of the Amiga's LZX): the pretree of 20, mod 17 changes,
  the two halves of the main tree, the aligned table of 8: these gave the first hypotheses. Amiga's LZX then differs (the second half's runs, the 16 by 32 symbols, the type numbers), each found above.
- A forum thread saying entries start `0f 00` then sizes (it had no more, and pointed at a source that was not opened).
**Knowledge base notes:** `amiga/lzx-format.md` was **not opened** (its layout was read from unlzx.c). `amiga/archive-file-listings.md` and `amiga/pc-archive-formats.md` were checked by a search for their
origin lines only and not used; the search showed a few lines, among them "a merged group is one stream" from a section derived from a port of unlzx: that fact was
established again above from an archive of known contents.
**What cannot be proved:** my own general knowledge. I know of these formats in general from what I have read over time, including possibly descriptions drawn from those programs; for `-lh1-`, the LhA block
layout and the first LZX hypotheses (LSB-first words, a length-and-slot split, a table of bases) that knowledge suggested what to try. Facts are not protected as expression, every fact above was tested
against real output, and the code was written anew here, in its own shape: but this is a clean rewrite by experiment and public description, not a two-team clean room. The owner should decide with that said plainly.

## Against real archives (*verified* 2026-11-01, read-only, the owner's NAS and files)
Every `.lha`, `.lzh`, `.lzx` of the owner's Amiga folder on the NAS (11,408 files: 9,761 LhA, 1,647 LZX) under ASAN and UBSAN: no crash. 11,070 pass every checksum. 189 have a member refused for its name (BBS ads such as `../free.exe`, `/LIBS/x`). 133 are found bad by the reference programs too (those looked at were cut-off files; LHa by its exit status, unlzx by its exit status or a "crc bad"). The last 16 pass LHa's exit status: 7 are not archives (empty files, macOS `._` files), and 9 are copies of 3 archives holding a forged member, a 7-byte stream claiming 4,310 bytes with a stored checksum of 0: its stream does not hold the file, so it answers "damaged", and the rest of the archive unpacks. Block type 1 did not occur: nothing answered "not supported".

## Tests
`arc-host-test` (make check's host list): every method and level from `tools/tests/archives/` (small archives made with LHa from test data) and `tools/mkarc.py` (this project's own small LZX and LhA writer, written
from this note), checked byte for byte; the refusals; a fuzz run under ASAN and UBSAN that prints its count. `ZENIX_ARCHIVES=FOLDER`: every `.lzx`, `.lha`, `.lzh` in a folder too (real ones, SKIP without it). `arc-test`: FS-UAE, the stock A1200: both
commands on Zenix. `ahiru-test`: Ahiru over the new readers.
