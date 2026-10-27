/* SPDX-License-Identifier: GPL-2.0-only */
/* The stand-in for libcheck (check.h). */
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "check.h"

struct TCase { const char *name; SFun setup, teardown; const TTest **t; int n; TCase *next; };
struct Suite { const char *name; TCase *first, **last; Suite *next; };
struct SRunner { Suite *first, **last; int run, failed; };

static jmp_buf env;
static struct skip { const char *test, *why; } skips[64];
static int nskip, skipped;
void ck_skip(const char *test, const char *why)
{
    if (nskip < 64) { skips[nskip].test = test; skips[nskip].why = why; nskip++; }
}
static const char *skipwhy(const char *test)
{
    int i;
    for (i = 0; i < nskip; i++) if (!strcmp(skips[i].test, test)) return skips[i].why;
    return 0;
}
static int in_test, failed_now;
static const char *cur;

void ck_fail_at(const char *file, int line, const char *expr, ...)
{
    va_list ap;
    const char *fmt;
    printf("FAIL %s: %s:%d: %s", cur ? cur : "?", file, line, expr);
    va_start(ap, expr);
    fmt = va_arg(ap, const char *);
    if (fmt) { printf(": "); vprintf(fmt, ap); }
    va_end(ap);
    printf("\n");
    failed_now = 1;
    if (in_test) longjmp(env, 1);
}
static void *xcalloc(size_t n) { void *p = calloc(1, n); if (!p) { perror("check"); exit(2); } return p; }
Suite *suite_create(const char *name) { Suite *s = xcalloc(sizeof *s); s->name = name; s->last = &s->first; return s; }
TCase *tcase_create(const char *name) { TCase *t = xcalloc(sizeof *t); t->name = name; return t; }
void tcase_add_checked_fixture(TCase *tc, SFun setup, SFun teardown) { tc->setup = setup; tc->teardown = teardown; }
void tcase_add_test(TCase *tc, const TTest *t)
{
    tc->t = realloc(tc->t, (tc->n + 1) * sizeof *tc->t);
    if (!tc->t) { perror("check"); exit(2); }
    tc->t[tc->n++] = t;
}
void suite_add_tcase(Suite *s, TCase *tc) { *s->last = tc; s->last = &tc->next; }
SRunner *srunner_create(Suite *s) { SRunner *r = xcalloc(sizeof *r); r->last = &r->first; if (s) srunner_add_suite(r, s); return r; }
void srunner_add_suite(SRunner *sr, Suite *s) { *sr->last = s; sr->last = &s->next; }
void srunner_set_fork_status(SRunner *sr, int mode) { (void)sr; (void)mode; }
void srunner_set_xml(SRunner *sr, const char *file) { (void)sr; (void)file; }
void srunner_run_all(SRunner *sr, int mode)
{
    Suite *s;
    TCase *tc;
    int i;
    (void)mode;
    setvbuf(stdout, NULL, _IOLBF, 0);
    for (s = sr->first; s; s = s->next)
        for (tc = s->first; tc; tc = tc->next)
            for (i = 0; i < tc->n; i++) {
                cur = tc->t[i]->name;
                if (getenv("CHECK_ONLY") && strcmp(getenv("CHECK_ONLY"), cur)) continue;
                if (skipwhy(cur)) { printf("skip %s: %s\n", cur, skipwhy(cur)); skipped++; continue; }
                failed_now = 0;
                if (tc->setup) tc->setup();
                in_test = 1;
                if (!setjmp(env)) tc->t[i]->fn(0);
                in_test = 0;
                if (tc->teardown) tc->teardown();
                sr->run++;
                if (failed_now) sr->failed++;
                else if (getenv("CHECK_VERBOSE")) printf("ok %s/%s\n", s->name, cur);
            }
    printf("lwip-unit: %d tests, %d failed, %d skipped (not Zenix's configuration)\n", sr->run, sr->failed, skipped);
}
int srunner_ntests_failed(SRunner *sr) { return sr->failed; }
void srunner_free(SRunner *sr) { (void)sr; }
