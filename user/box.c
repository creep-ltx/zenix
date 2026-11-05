/* SPDX-License-Identifier: GPL-2.0-only */
/* The box (DESIGN D25 revision 3, 2.2 and 10 step 1): the everyday commands linked into one program, BusyBox's
 * idea, for the demo floppy. Each command is built from its own unchanged source; tools/mkbox.sh renames its main to
 * NAME__main and writes the table boxtab.h (BOX(NAME) for every command, ALIAS("NAME", COMMAND) for a second name).
 * The command is picked by the name the program was started as (argv[0]'s last part), and gets argv as given, so a
 * command that looks at its own name (more, gunzip, ifup, halt) behaves as the separate program does. An unknown name
 * lists the names and exits 2. */
#include "libz.h"
#include "boxtab.h"

#define BOX(n) int n##__main(int argc, char **argv);
#define ALIAS(s, n)
BOX_TABLE
#undef BOX
#undef ALIAS

struct boxent { const char *name; int (*fn)(int, char **); };
#define BOX(n) { #n, n##__main },
#define ALIAS(s, n) { s, n##__main },
static const struct boxent box_tab[] = { BOX_TABLE };
#undef BOX
#undef ALIAS
#define BOX_N (sizeof box_tab / sizeof box_tab[0])

static void err(const char *s) { sys_write(2, s, strlen(s)); }

int main(int argc, char **argv)
{
    const char *me = argc > 0 && argv[0] ? argv[0] : "";
    const char *slash = strrchr(me, '/');
    if (slash)
        me = slash + 1;
    for (unsigned i = 0; i < BOX_N; i++)
        if (!strcmp(box_tab[i].name, me))
            return box_tab[i].fn(argc, argv);
    err("box: no command named '"); err(me); err("'; this program answers to:\n");
    unsigned col = 0;
    for (unsigned i = 0; i < BOX_N; i++) {
        unsigned n = strlen(box_tab[i].name);
        if (col && col + 1 + n > 76) { err("\n"); col = 0; }
        if (col) { err(" "); col++; }
        err(box_tab[i].name); col += n;
    }
    err("\n");
    return 2;
}
