# SPDX-License-Identifier: GPL-2.0-only
# Waits until the wall clock (the file times' clock: whole seconds) has moved two seconds on. sleep counts the system's
# ticks, which run ahead of the PC's clock in FS-UAE's fast mode, so two slept seconds can end in the same second of
# the clock the file times come from (tools/make-test.sh).
t=$(date +%s)
while [ "$(date +%s)" -lt $((t + 2)) ]; do sleep 1; done
