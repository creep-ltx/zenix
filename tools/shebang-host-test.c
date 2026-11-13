/* SPDX-License-Identifier: GPL-2.0-only */
/* DESIGN D53: the #! line's parser (kernel/shebang.h) on the PC, every rule of docs/d53-shebang.md section 3. */
#include <stdio.h>
#include <string.h>
#include "../kernel/shebang.h"

static int bad;

/* parse the file content S (L bytes, as spawn1 reads it: at most SHEBANG_MAX); WANT_I/WANT_A the expected interpreter
 * and argument (A 0: none), or WANT_ERR the start of the refusal */
static void t(const char *what, const char *s, unsigned l, const char *wi, const char *wa, const char *werr)
{
    char b[SHEBANG_MAX];
    unsigned n = l < SHEBANG_MAX ? l : SHEBANG_MAX;
    memset(b, 0, sizeof b);              /* as spawn1 reads it */
    memcpy(b, s, n);
    char *i = 0, *a = 0;
    const char *e = shebang_parse(b, &i, &a);
    int ok;
    if (werr)
        ok = e && !strncmp(e, werr, strlen(werr));
    else
        ok = !e && i && !strcmp(i, wi) && (wa ? a && !strcmp(a, wa) : !a);
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        printf("     got err=%s interp=[%s] arg=[%s]\n", e ? e : "-", i ? i : "-", a ? a : "-");
    bad += !ok;
}
#define T(w, s, i, a, e) t(w, s, sizeof s - 1, i, a, e)

int main(void)
{
    T("a plain interpreter", "#!/bin/sh\necho hi\n", "/bin/sh", 0, 0);
    T("spaces after #!", "#!  /bin/lua\n", "/bin/lua", 0, 0);
    T("a tab after #!", "#!\t/bin/lua\n", "/bin/lua", 0, 0);
    T("one argument", "#!/bin/make -f\nall:\n", "/bin/make", "-f", 0);
    T("the argument keeps its inner spaces (Linux)", "#!/bin/env lua -i\n", "/bin/env", "lua -i", 0);
    T("spaces and tabs at the end removed", "#!/bin/make -f \t \n", "/bin/make", "-f", 0);
    T("spaces after the interpreter only: no argument", "#!/bin/sh   \n", "/bin/sh", 0, 0);
    T("tabs between", "#!/bin/echo\t\tx\n", "/bin/echo", "x", 0);
    T("no newline: the file's end ends the line", "#!/bin/sh", "/bin/sh", 0, 0);
    T("a 0 byte ends the line", "#!/bin/sh\0 -x\n", "/bin/sh", 0, 0);
    T("a carriage return is kept (Linux)", "#!/bin/sh\r\n", "/bin/sh\r", 0, 0);
    T("a carriage return after an argument is kept", "#!/bin/make -f\r\n", "/bin/make", "-f\r", 0);
    T("a relative interpreter", "#!bin/x a\n", "bin/x", "a", 0);
    T("an empty line: refused", "#!\n", 0, 0, "#! line without an interpreter");
    T("only spaces: refused", "#!   \t \nfoo\n", 0, 0, "#! line without an interpreter");
    T("only #!: refused", "#!", 0, 0, "#! line without an interpreter");
    T("a 3-byte file", "#!a", "a", 0, 0);
    T("no #! any more (the file changed): refused", "\177ELF", 0, 0, "changed while starting");
    T("an escape byte stays in the name (never printed by the kernel)", "#!/bin/\x1b[2J\n", "/bin/\x1b[2J", 0, 0);

    char s[300];
    /* a line of exactly 127 bytes (then the newline): the longest allowed */
    memset(s, 'a', sizeof s);
    memcpy(s, "#!/bin/echo ", 12);
    s[127] = '\n';
    s[128] = 'z';
    {
        char wa[200];
        memset(wa, 'a', 115);
        wa[115] = 0;
        t("a 127-byte line with its newline at byte 128", s, 200, "/bin/echo", wa, 0);
        t("a 127-byte line that ends the file (127 bytes)", s, 127, "/bin/echo", wa, 0);
    }
    s[127] = 'a';
    s[128] = '\n';
    t("a 128-byte line: refused", s, 200, 0, 0, "#! line over 127 bytes");
    t("a 128-byte file with no newline: refused", s, 128, 0, 0, "#! line over 127 bytes");
    memset(s, 'b', sizeof s);
    memcpy(s, "#!/bin/", 7);
    s[126] = '\n';
    {
        char wi[200] = "/bin/";
        memset(wi + 5, 'b', 119);
        wi[124] = 0;
        t("a 126-byte line, the interpreter alone", s, 200, wi, 0, 0);
    }
    printf(bad ? "shebang-host-test: %d FAILED\n" : "shebang-host-test: ok\n", bad);
    return bad != 0;
}
