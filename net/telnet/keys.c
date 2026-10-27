/* SPDX-License-Identifier: GPL-2.0-only */
/* telnet's keys (DESIGN D20 3.3): what the console gives -> what the board gets, per mode. */
#include "telnet.h"

long tk_keys(int mode, const unsigned char *in, long n, unsigned char *out, int *bell)
{
    long i, k = 0;
    for (i = 0; i < n; i++) {
        unsigned char c = in[i];
        if (mode == TN_UNIX) {
            if (c < 0x80)
                out[k++] = c;           /* Backspace 0x7F, Del ESC[3~, everything as given */
            else {
                out[k++] = (unsigned char)(0xC0 | (c >> 6));   /* Latin-1 -> UTF-8 */
                out[k++] = (unsigned char)(0x80 | (c & 0x3F));
            }
            continue;
        }
        if (c == 0x7F) {
            out[k++] = 0x08;            /* Backspace: BS (boards expect it) */
            continue;
        }
        if (c == 0x1B && i + 3 < n && in[i + 1] == '[' && in[i + 2] == '3' && in[i + 3] == '~') {
            out[k++] = 0x7F;            /* Del (ESC[3~, whole in this read): DEL */
            i += 3;
            continue;
        }
        if (c >= 0x80 && mode == TN_PC) {
            unsigned char p = tn_latin1_cp437[c];
            if (p)
                out[k++] = p;
            else if (bell)
                (*bell)++;              /* no CP437 form: not sent */
            continue;
        }
        out[k++] = c;                   /* arrows, F-keys, Help, controls, Latin-1 to an Amiga board: as given */
    }
    return k;
}
