#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# DESIGN D53: the process name is the script's; the test makes it set-user-id and owned by uid 1000
ps | grep --color=never psname
id
