# SPDX-License-Identifier: GPL-2.0-only
# zdb's commands with a pause (tools/zdb-test.sh, section 10's proof): held at leaf while the other copies run
printf 'info\nb leaf\nc\n'
sleep 25
printf 'p counter\nd leaf\nc\n'
