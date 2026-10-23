#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The audit D (permissions) checks on a users-test log, shared by tools/users-test.sh and tools/zroot-test.sh.
# Every match is a WHOLE line (the log also holds the typed keystrokes, which contain most of these words).
# usage: source it, then  users_d_checks LOG [prefix]   -> PASS/FAIL lines; returns 1 if any failed
users_d_checks() {
    local log=$1 pre=$2 bad=0 L
    L=$(mktemp); tr -d '\r' < "$log" | sed 's/\x1b\[K//g' > "$L"
    line() { if grep -aqxF -- "$2" "$L"; then echo "PASS $pre$1"; else echo "FAIL $pre$1"; bad=1; fi; }
    rx() { if grep -aqxE -- "$2" "$L"; then echo "PASS $pre$1"; else echo "FAIL $pre$1"; bad=1; fi; }
    none() { if grep -aqxF -- "$2" "$L"; then echo "FAIL $pre$1"; bad=1; else echo "PASS $pre$1"; fi; }
    line "autologin warns that root has no password" "login: warning: root has no password and /etc/autologin logs in without asking; set passwords with passwd and remove /etc/autologin before trusting this system"
    line "raw messages, reserved ports, system processes, clock, serial copy, O_EXCL (badptr ipc)" "badptr: ipc all ok"
    line "a user cannot kill a system process (kill 1, the shell's kill)" "sh: 1: permission denied"
    line "... and kill says so in its status" "kill-1=1"
    line "a user cannot kill another user's process" "kill-other=1"
    rx   "... with permission denied" "sh: [0-9]+: permission denied"
    line "a user cannot open the raw disk" "dd: cannot open ide0"
    line "a user cannot set the clock" "date: cannot set the clock: permission denied"
    line "su resets PATH and HOME" "P=/bin H=/"
    line "su drops the caller's IFS" "ifs=1"
    line "a user's ~/.shrc runs for that user" "rc-ran-as-user"
    none "a root shell does not run a user's ~/.shrc" "rc-ran-as-root"
    line "tar x refuses a name with .." 'tar: ../a/f: refused: a ".." part could reach outside the directory'
    line "... and exits 1" "tar-dots=1"
    line "tar x puts an absolute name under the directory" "inside"
    line "... and exits 0" "tar-abs=0"
    line "tar x refuses to write through a link from the same archive" "tar: lnk/f: refused: it goes through the symbolic link lnk from this archive"
    line "... the file behind the link is unchanged" "changed"
    line "find -delete does not follow a link out of the tree" "keep"
    line "... and succeeds" "find-delete=0"
    rx   "a here-document file is 0600 with a random name" '-rw------- 1 root root +2 .* /tmp/\.sh[0-9]+_[a-z0-5]{8}'
    rx   "mktemp makes a 0600 file with a random name" '-rw------- 1 root root +0 .* /tmp/tmp\.[A-Za-z0-9]{6}'
    rx   "a set-group-id directory gives a user's new file its group; chmod cannot set set-group-id for another group" '-rwxr-xr-x 1 user root +0 .* /tmp/sgd/f'
    rx   "... and a new directory its group and the bit" 'drwxr-sr-x +[0-9]+ user root .* /tmp/sgd/d'
    rx   "a write by a non-owner clears set-user-id and set-group-id" '-rwxrwxrwx 1 root root +7 .* /tmp/sgd/prog'
    line "a user cannot move a directory it may not write to another parent" "mv-dir=1"
    line "... it is still where it was" "mv-dir-stays=0"
    line "the user part ran to the end" "perm-user-done"
    line "the root part ran to the end" "perm-root-done"
    line "Amiga+Return at the login prompt opens no shell and says why" "Nobody is logged in: log in first; Amiga+Return then opens a shell."
    rm -f "$L"
    return $bad
}
