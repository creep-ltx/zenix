/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* filecol - the family's colour file, ENVARC:FileColours (csh's ls reads
 * it too; see CShell docs/design.md "Colours"):
 *   KIND CTERM,PLAIN   SGR numbers in CTerm and in a plain Shell window
 *   .EXT KIND          a name ending in .ext is of that kind
 * Kinds: drawer link program script archive picture sound source text
 * icon hidden. A kind given a colour takes it in Ahiru too; '-' (or no
 * colour number, only bold or italic) leaves Ahiru.theme's pen.
 *
 * The CTERM column is ANSI colours (90-97 the bright ones), which is the
 * order of Ahiru's own palettes and of CTerm: used on Ahiru's screen and
 * inside CTerm. The PLAIN column is the Workbench's pens (30 grey, 31
 * black, 32 white, 33 blue): used in a Workbench window and in a plain
 * console. Written with csh's defaults when missing. */
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include "ahiru.h"
#include "core/util.h"

enum { FK_DRAWER, FK_LINK, FK_PROGRAM, FK_SCRIPT, FK_ARCHIVE, FK_PICTURE, FK_SOUND,
       FK_SOURCE, FK_TEXT, FK_ICON, FK_HIDDEN, FK_COUNT };
static const char *const kindname[FK_COUNT] = { "drawer", "link", "program", "script",
    "archive", "picture", "sound", "source", "text", "icon", "hidden" };
static const int kindrole[FK_COUNT] = { RO_DIR, RO_LINK, RO_EXEC, RO_SCRIPT, RO_ARCH,
    RO_IMG, RO_AUD, RO_SRC, RO_TEXT, RO_INFO, RO_HIDDEN };

static signed char sgrcol[2][FK_COUNT];     /* [CTERM, PLAIN]: 30-37 -> 0-7, 90-97 -> 8-15, -1 none */

#define MAXEXT 64
static char extname[MAXEXT][12];
static unsigned char extkind[MAXEXT];
static int nexts;

/* the same text csh's ls writes (CShell tools/ls.c) */
static const char deffile[] =
    "; File kinds and their colours, for csh's ls, Ahiru and Maki.\n"
    "; KIND CTERM,PLAIN: SGR numbers in CTerm and in a plain Shell window; '-' = none.\n"
    "; In a CTerm Shell tab 30-37 are the Amiga's pens: its theme's colours are 90 grey\n"
    "; 91 red 92 green 93 yellow 94 blue 95 magenta 96 cyan 97 white. A plain window on\n"
    "; a stock Workbench has pens 30 grey 31 black 32 white 33 blue. 1 bold, 3 italic.\n"
    "; .EXT KIND puts a name ending in .ext into a kind (after the built-in list).\n"
    "drawer 94,33\n"
    "link 96,3\n"
    "program 92,1\n"
    "script 92,1\n"
    "archive 91,-\n"
    "picture 93,-\n"
    "sound 95,-\n"
    "source 96,-\n"
    "text -,-\n"
    "icon 90,32\n"
    "hidden 90,32\n";

/* the last colour number in "1;94" (30-37 or 90-97), -1 when none */
static int sgrcolour(const char *s, int n)
{
    int c = -1, v = 0, i, any = 0;
    for (i = 0; i <= n; i++) {
        if (i < n && s[i] >= '0' && s[i] <= '9') {
            v = v * 10 + s[i] - '0';
            any = 1;
            continue;
        }
        if (any && v >= 30 && v <= 37) c = v - 30;
        else if (any && v >= 90 && v <= 97) c = v - 90 + 8;
        v = any = 0;
    }
    return c;
}

static void addext(const char *e, int kind)
{
    int i;
    if (!*e || strlen(e) > 11) return;
    for (i = 0; i < nexts; i++)
        if (nocase(extname[i], e) == 0) {
            extkind[i] = (unsigned char)kind;
            return;
        }
    if (nexts == MAXEXT) return;
    strcpy(extname[nexts], e);
    extkind[nexts++] = (unsigned char)kind;
}

static void parse(char *t)
{
    char *line = t;
    while (line && *line) {
        char *nl = strchr(line, '\n'), *k, *v, *e;
        int i;
        if (nl) *nl = 0;
        k = line;
        while (*k == ' ' || *k == '\t') k++;
        if (*k && *k != ';' && *k != '#') {
            for (e = k; *e && *e != ' ' && *e != '\t'; e++) ;
            v = e;
            while (*v == ' ' || *v == '\t') v++;
            *e = 0;
            for (e = v; *e && *e != ' ' && *e != '\t' && *e != '\r'; e++) ;
            *e = 0;
            for (i = 0; i < FK_COUNT && nocase(k[0] == '.' ? v : k, kindname[i]) != 0; i++) ;
            if (i < FK_COUNT) {
                if (k[0] == '.') addext(k + 1, i);
                else {
                    char *comma = strchr(v, ',');
                    sgrcol[0][i] = (signed char)sgrcolour(v, comma ? (int)(comma - v) : (int)strlen(v));
                    sgrcol[1][i] = comma ? (signed char)sgrcolour(comma + 1, (int)strlen(comma + 1)) : sgrcol[0][i];
                }
            }
        }
        line = nl ? nl + 1 : NULL;
    }
}

static char *readall(const char *path)
{
    BPTR fh = Open((STRPTR)path, MODE_OLDFILE);
    char *t;
    LONG n;
    if (!fh) return NULL;
    Seek(fh, 0, OFFSET_END);
    n = Seek(fh, 0, OFFSET_BEGINNING);
    if (n < 0 || n > 32768 || !(t = AllocVec(n + 1, MEMF_ANY))) {
        Close(fh);
        return NULL;
    }
    n = Read(fh, t, n);
    Close(fh);
    t[n > 0 ? n : 0] = 0;
    return t;
}

void filecol_load(void)
{
    static char d[sizeof deffile];
    char *t;
    memcpy(d, deffile, sizeof deffile);
    parse(d);
    if (!(t = readall("ENV:FileColours")) && !(t = readall("ENVARC:FileColours"))) {
        APTR old = quietreq();      /* no requester on a write-protected disk */
        BPTR fh;
        if ((fh = Open((STRPTR)"ENVARC:FileColours", MODE_NEWFILE))) {
            Write(fh, (APTR)deffile, sizeof deffile - 1);
            Close(fh);
        }
        if ((fh = Open((STRPTR)"ENV:FileColours", MODE_NEWFILE))) {
            Write(fh, (APTR)deffile, sizeof deffile - 1);
            Close(fh);
        }
        loudreq(old);
        return;
    }
    parse(t);
    FreeVec(t);
}

/* the theme's pens with the file kinds' colours over them; plain: the
 * PLAIN column as pens, else the CTERM column through ansipen */
const int *filecol_apply(const int *theme, const int *ansi, int plain, int ncolours)
{
    static int pens[RO_COUNT];
    int i;
    memcpy(pens, theme, sizeof pens);
    for (i = 0; i < FK_COUNT; i++) {
        int c = sgrcol[plain][i];
        if (c < 0) continue;
        /* grey (90) is the theme's dim pen: the 8-colour screen has no grey */
        pens[kindrole[i]] = plain ? (c & 7) % ncolours : c == 8 ? theme[RO_DIM] : ansi[c];
    }
    return pens;
}

/* the role of a name ending in an .ext the file lists, or -1 */
int filecol_extrole(const char *name)
{
    const char *e = extof(name);
    int i;
    if (!*e) return -1;
    for (i = 0; i < nexts; i++)
        if (nocase(extname[i], e) == 0) return kindrole[extkind[i]];
    return -1;
}
