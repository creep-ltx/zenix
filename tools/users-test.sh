#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Users and permissions in the emulator: boots as the autologin root, switches users with su, checks what a normal user may
# and may not do (write to /, remove another user's file, read a 0600 file, sticky /tmp), umask, owner names in ls -l,
# passwd with a wrong and a right password, chown/chgrp.
# Then the audit D checks (tools/users-d-checks.sh, whole-line matches): raw messages to the servers, kill/stop of
# others' and system processes, the raw disk, the clock, su's environment, ~/.shrc ownership, tar x and find -delete
# safety, exclusive here-document and mktemp files, the autologin warning; and batch E's: a login on console 2 (as
# that user), less in a pipe reading console 2's keys, type-ahead, the consoles' login prompts, permission texts.
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}       # the build directory (make check gives every test a copy of its own)
make -s BUILD=$BD all >/dev/null 2>&1 || { echo "build failed"; exit 1; }
rm -f $BD/zenix.hdf; make -s BUILD=$BD $BD/zenix.hdf >/dev/null 2>&1
OUT=${USERS_OUT:-${ZENIX_OUT:-/tmp/zenix-users}}; mkdir -p $OUT
tools/run-test.sh fsuae/a1200-020-fast.fs-uae 320 tools/tests/users-test.txt 2>&1 | sed 's/\x1b\[[0-9;:]*[mK]//g' > $OUT/run.log
pass=1
want() { if grep -aqF -- "$2" $OUT/run.log; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
want "root identity" "uid=0(root) gid=0(root)"
want "su to a user" "uid=1000(user) gid=100(users)"
want "user cannot write to /" "sh: /nofile: permission denied"
want "owner names in ls -l" "-rw-r--r-- 1 user users 5"
want "umask 077 gives mode 600" "-rw------- 1 user users 2"
want "set-user-id shown in ls" "-rwsr-xr-x 1 root root"
want "user cannot remove a root file" "rm: /etc/motd: permission denied"
want "user cannot read a 0600 root file" "cat: /tmp/r1: permission denied"
want "sticky /tmp protects other users' files" "rm: /tmp/r1: permission denied"
want "running a file without x says permission denied" "/etc/passwd: permission denied"
want "passwd stores a password" "passwd: password for guest changed"
want "su with a wrong password fails" "su: wrong password"
want "su with the right password works" "uid=1001(guest) gid=100(users)"
want "chown by name" "-rw-r--r-- 1 guest users 5"
want "sha256sum matches the host" "$(sha256sum rootfs/etc/group | cut -d' ' -f1)"
. tools/users-d-checks.sh
users_d_checks $OUT/run.log || pass=0
exit $((1 - pass))
