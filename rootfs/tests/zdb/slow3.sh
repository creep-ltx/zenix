# SPDX-License-Identifier: GPL-2.0-only
# a breakpoint, then zdb waits to be killed (tools/zdb-test.sh: the debugger dies while its program is held)
printf 'b leaf\nc\n'
sleep 60
