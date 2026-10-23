# SPDX-License-Identifier: GPL-2.0-only
# Audit D checks run as root by tools/users-test.sh (the output is compared there, line by line):
# tar x of absolute and ".." names and through a link from the same archive, find -delete and a link out of the
# tree, here-document and mktemp files (exclusive, mode 0600, random names).
rm -rf /tmp/tz; mkdir /tmp/tz; cd /tmp/tz; mkdir a victim x
echo inside > a/f
echo orig > victim/f
cd a; tar cf /tmp/tz/dots.tar ../a/f; cd /tmp/tz
tar cf abs.tar /tmp/tz/a/f
ln -s /tmp/tz/victim lnk
tar cf sl.tar lnk lnk/f
rm lnk
echo changed > victim/f
tar xf dots.tar -C x; echo tar-dots=$?
tar xf abs.tar -C x; echo tar-abs=$?
cat x/tmp/tz/a/f
tar xf sl.tar -C x; echo tar-link=$?
cat victim/f
mkdir -p fd/tree; echo keep > fd/keep; ln -s /tmp/tz/fd fd/tree/up
find /tmp/tz/fd/tree -delete; echo find-delete=$?
cat fd/keep
sh -c 'ls -l /tmp/.sh*' <<END
x
END
ls -l $(mktemp)
cd /; rm -rf /tmp/tz /tmp/tmp.*
echo perm-root-done
