/* SPDX-License-Identifier: GPL-2.0-only */
/* Settings files written by the preference programs (deskprefs, conprefs: docs/d38-desktop.md "Preferences",
 * docs/d28-console-text.md section 22): a file replaced whole (a new file beside it, then renamed over it, so a cut
 * leaves the old file or the new one, never half), and a file's lines of some settings replaced with the rest kept. */
#pragma once
int cfg_write(const char *path, const char *text);     /* -> 0, or -errno (the old file kept) */
/* path's lines whose first word is key[i] become line[i] (the first such line; later ones are dropped), every other
 * line (comments, other settings) kept as it is, a setting it lacks added at the end -> 0, or -errno. A missing file
 * is made. At most 4 KB is kept: a longer file is refused (-EFBIG) rather than cut. */
int cfg_set(const char *path, const char *const *key, const char *const *line, int n);
