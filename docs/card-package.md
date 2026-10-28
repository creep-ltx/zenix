# The card package: putting a new Zenix on the owner's card, and the DoM

The owner's first card package of DESIGN D15 (docs/d15-self-updates.md section 12, step 6): it moves the kernel to
`BOOT:Zenix/`, adds the "Zenix (previous)" line to the boot menu, brings the fixed bootmenu, and puts a new Zenix root
on the card. `tools/mkcardpkg.sh` builds it from a tree into `build/cardpkg/` (never onto a device) and copies the
part below the line into the package as its `README.md`. It also carries the DoM image (`tools/mkdom.sh`).

**Why the root comes as an image this once:** the root on the owner's card (Zenix of 2026-10-24) has no `/sbin/zup`,
so the new kernel cannot install a bundle's root half there (D15 section 8: "the first D15 update has no menu way
back; the way back then is the card on the PC"). So the card goes into the PC one last time and
`tools/write-root.sh` writes partition 3 only, after saving the old root and carrying its settings across
(`tools/cardroot.py`: users and passwords, groups, the `/home` line in fstab, autologin, the random seed, any other
file in `/etc`). From then on every update is a bundle copied to `BOOT:Zenix/` from AmigaOS.

**Why the DoM is Zenix only:** it is for the stock A1200 and the Apollo 060, where none of the owner's AmigaOS lives
on it; a dual-boot layout needs the AmigaOS partitions, which only the card has. It has the same boot menu and the same
`Zenix/` place as the card, so updates work the same way there.

Tested in FS-UAE by `tools/cardpkg-test.sh` (the steps below on an emulated copy of the card's layout, the trial
update and "Zenix (previous)", the DoM on a stock A1200 and a 68060). Not yet done on the real card or a real DoM.

<!-- package -->
## What this package does

1. Brings the new Zenix to the boot partition (BOOT:): the kernel in a folder `Zenix`, a new `zenixboot`, the fixed
   boot menu, and a new menu line "Zenix (previous)" (going back after an update).
2. Puts the new Zenix root on the card's partition 3, from the PC. Your users, passwords and `/home` stay. Partitions
   1 (Emu68), 2 (AmigaOS) and 4 (`/home`) are not touched; the old root is saved first.
3. Optional: tries a real update the new way (copy one file, reboot), and the way back.
4. The DoM: a disk image for the IDE disk-on-module, for the stock A1200 and the 060.

This is the last time the card goes into the PC for an update. About 30 minutes, the DoM another 15.

On the Swedish keyboard: `>` is Shift and the key left of Z, `"` is Shift+2. In Zenix `|` is Shift and the key left
of Backspace, `~` is Shift and the key left of 1. No command here needs `{` or `}`.

## Step 1: the boot partition, from AmigaOS

Start AmigaOS as every day. Get the folder `amiga/zenixpkg` of this package to the Amiga as `RAM:zenixpkg` (the
session can send it with Wasabi when you say so, or copy it from a USB stick or the NAS as you usually do). Then in a
Shell:

```
Info BOOT:
```
The line for BOOT must show at least the free space this README's first lines name. If it does not, stop and tell
the session.

Keep the old files (the way back), then copy the new ones:
```
Copy BOOT:zenixboot BOOT:zenixboot.pre-d15
Copy BOOT:bootmenu BOOT:bootmenu.pre-d15
Copy BOOT:S/BootMenu.conf BOOT:S/BootMenu.conf.pre-d15
MakeDir BOOT:Zenix
Copy RAM:zenixpkg/Zenix/zenix BOOT:Zenix/zenix
Copy RAM:zenixpkg/zenixboot BOOT:zenixboot
Copy RAM:zenixpkg/bootmenu BOOT:bootmenu
Copy RAM:zenixpkg/bootmenu BOOT:bootzenix
Echo >>BOOT:S/BootMenu.conf "Zenix (previous) = run SYS:zenixboot PREVIOUS"
Type BOOT:S/BootMenu.conf
```
The last command shows the menu file: the new line must be the **last line, on a line of its own**, after the
`Zenix = run SYS:zenixboot` line. `RAM:zenixpkg/BootMenu.conf` shows how the whole file should look (if you never
changed yours, the same). If you copied `bootzenix` into your AmigaOS as well (`Which bootzenix` says where), copy the
new one there too: `Copy RAM:zenixpkg/bootmenu C:bootzenix` (with the place `Which` named).

`BOOT:zenix` and `BOOT:zenix.old` (the old kernels) stay where they are: the new zenixboot takes the kernel from
`BOOT:Zenix/` now, and the old files are the way back. Do not start Zenix yet: the root comes first (step 2).

Switch the Amiga off (wait until the drive light is out).

## Step 2: the root, on the PC

Take the SD card out of the PiStorm and put it into the PC's card reader. Tell the Claude session on the PC "write
the Zenix root"; it runs the steps with you (`sudo` asks for your password):

1. **Find the card's name, never guess it:** `lsblk -o NAME,SIZE,MODEL,TRAN` before and after putting the card in.
   The new line, about 16 GB (shown as 14.x G), is the card, for example `sdb` or `mmcblk0`. Your PC's own disks are
   there before the card goes in.
2. Write the root (with the card's name in place of `sdX`):
   ```
   sudo bash tools/write-root.sh /dev/sdX build/cardpkg/pc/root.img ~/zenix-backups
   ```
   It checks that this is the Zenix dual-boot card (an Amiga partition 2, a Zenix root on partition 3), saves the
   root that is on it now to `~/zenix-backups/zenix-root-DATE.img.zst` and checks that copy, carries your settings
   into the new root (it prints one line for each: your users and passwords, `/etc/fstab` with the `/home` line,
   autologin), waits 5 seconds (Ctrl-C stops it), writes partition 3, reads it back and compares. It ends with
   "done" and the name of the saved copy. **Note** the lines it printed.
3. Eject the card (`sudo eject /dev/sdX`), put it back into the PiStorm.

## Step 3: the first start of the new Zenix

Switch the A1200 on and choose **Zenix** in the boot menu (or `bootzenix` from AmigaOS). The menu now has four lines:
AmigaOS, Zenix, AmigaOS rescue shell, Zenix (previous).

**Photo** the screen while zenixboot runs if it stops. When the login comes (or the autologin's prompt), type:
```
uname
mount
grep -c : /etc/passwd
/sbin/zup -l
free
```
Expected: `uname` says `Zenix` and the version this README's first line names; `mount` shows `/` and `/home`; the
number of users is what you had (3, or more if you added some); `/sbin/zup -l` lists the packages zup, base, utils and man
with this version; `free` shows the memory. **Note** the `free` line. If `/home` is missing or the login fails, **photo** and tell the session.

## Step 4 (optional, 10 minutes): an update the new way, and back

This tries what every later update will do: copy one file from AmigaOS, reboot.

1. In AmigaOS (choose AmigaOS in the menu, or `reboot amigaos` in Zenix):
   ```
   Copy RAM:zenixpkg/try/zenix-update.zup BOOT:Zenix/zenix-update.zup.part
   Rename BOOT:Zenix/zenix-update.zup.part BOOT:Zenix/zenix-update.zup
   bootzenix
   ```
   (If RAM: was emptied by a restart, copy the `try` folder to the Amiga again first.)
2. zenixboot says it installs the new version, flushes after each step, and starts it; Zenix installs the rest
   (`zup: installed Zenix ...`). At the prompt: `uname` must name the trial version (it ends in `-try`). **Photo**.
3. `reboot zenix`: zenixboot says the version "came up" and renames the bundle `zenix-update.done`. **Photo**.
4. The way back: `reboot`, and choose **Zenix (previous)** in the menu. zenixboot says it goes back; Zenix puts the
   old files back. `uname` must name this README's version again (no `-try`). **Photo**.

You are then on the package's version, as after step 3. If anything stops on the way, **photo** the screen and tell
the session: nothing is lost, the previous version is kept at each step.

## The way back (only if the new Zenix does not work)

1. In AmigaOS:
   ```
   Copy BOOT:zenixboot.pre-d15 BOOT:zenixboot
   Copy BOOT:bootmenu.pre-d15 BOOT:bootmenu
   Copy BOOT:bootmenu.pre-d15 BOOT:bootzenix
   Copy BOOT:S/BootMenu.conf.pre-d15 BOOT:S/BootMenu.conf
   Rename BOOT:Zenix BOOT:Zenix-off
   ```
2. On the PC, with the card in the reader (the saved copy's name was printed in step 2):
   ```
   sudo bash tools/write-root.sh /dev/sdX --restore ~/zenix-backups/zenix-root-DATE.img.zst
   ```
The card is then exactly as before this package.

## The DoM (disk-on-module)

The image `dom/zenix-dom.img` is Zenix only: a boot partition with the menu (Zenix starts after 3 seconds; "Zenix
(previous)" and the Kickstart shell are the other lines), the Zenix root and a small `/home`. The rest of a bigger DoM
stays empty. Everything on the DoM is replaced.

1. **On the PC:** connect the DoM with the IDE adapter. Find its name the same way as the card's:
   `lsblk -o NAME,SIZE,MODEL,TRAN` before and after connecting it; the new line with the DoM's size (and `usb` under
   TRAN) is the DoM. **Never guess the name: the wrong one erases a disk of this PC.** Then (with that name):
   ```
   sudo dd if=build/cardpkg/dom/zenix-dom.img of=/dev/sdX bs=4M conv=fsync status=progress
   sudo cmp -n $(stat -c %s build/cardpkg/dom/zenix-dom.img) build/cardpkg/dom/zenix-dom.img /dev/sdX && echo same
   sudo eject /dev/sdX
   ```
   `cmp` must end with `same`.
2. **The stock A1200:** the PiStorm out, the DoM on the IDE port, switch on. The boot menu, then Zenix. Kickstart 3.2
   boots the DoM's boot partition by itself.
3. **The A1200 with the Apollo 060:** the same DoM. If the menu or zenixboot does not come up on the 060 (no
   68060.library is loaded before them), **photo** what the screen shows and tell the session.

The weekend checklist (docs/weekend-2026-10-30.md) says what to measure on each.

Last checked against the code: 2026-10-28.
