# SPDX-License-Identifier: GPL-2.0-only
# awk's wide printf fields (audit A4: they read past a 128-byte buffer), run by cmds.sh: a script of its own, as
# cmds.sh is near the shell's 256 KB parse arena. Each prints the length and what is left without the padding.
awk 'BEGIN { printf "%300d|\n", 7 }' | wc -c; awk 'BEGIN { printf "%300d|\n", 7 }' | tr -d ' '
awk 'BEGIN { x = sprintf("%200c", "a"); print length(x); gsub(/ /, "", x); print x }'
awk 'BEGIN { x = sprintf("%-500.3f|", 3.14159); print length(x); gsub(/ /, "", x); print x }'
awk 'BEGIN { x = sprintf("%*x|%-*o|%0300i", 300, 255, 150, 8, -5); print length(x); gsub(/ /, "", x); sub(/^ff\|10\|-0+/, "", x); print x }'
awk 'BEGIN { printf "%4096s|\n", "e" }' | wc -c; awk 'BEGIN { printf "%4096u|\n", 3 }' | tr -d ' '
