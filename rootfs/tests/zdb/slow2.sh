# SPDX-License-Identifier: GPL-2.0-only
# attach to the only copy of a ZEX2 program: its code becomes its own, the breakpoint works (tools/zdb-test.sh)
printf 'info\nb leaf\ninfo\nc\n'
sleep 12
printf 'd leaf\nc\n'
