/* SPDX-License-Identifier: GPL-2.0-only */
/* A small stand-in for the "check" unit test framework (libcheck), just what lwIP's test/unit uses, so the tests
 * build on a PC without that package (tools/host/net-test.sh). Tests run in one process, one after another; a
 * failed check ends its test (longjmp), the test's teardown still runs. */
#ifndef ZENIX_CHECK_SHIM_H
#define ZENIX_CHECK_SHIM_H
#include <stddef.h>

#define CHECK_MAJOR_VERSION 0
#define CHECK_MINOR_VERSION 15
#define CK_NORMAL 0
#define CK_VERBOSE 1
#define CK_NOFORK 0
#define CK_FORK 1

typedef void (*TFun)(int);
typedef void (*SFun)(void);
typedef struct TTest { const char *name; TFun fn; const char *file; int line; } TTest;
typedef struct Suite Suite;
typedef struct TCase TCase;
typedef struct SRunner SRunner;

#define START_TEST(t) static void t##_fn(int _i CK_UNUSED); \
    static const TTest t##_ttest = { #t, t##_fn, __FILE__, __LINE__ }; \
    static const TTest *t = &t##_ttest; \
    static void t##_fn(int _i CK_UNUSED)
#define END_TEST
#define CK_UNUSED __attribute__((unused))

void ck_fail_at(const char *file, int line, const char *expr, ...);
#define fail_unless(e, ...) ((e) ? (void)0 : ck_fail_at(__FILE__, __LINE__, "'" #e "' is false", ##__VA_ARGS__, (const char *)NULL))
#define fail_if(e, ...) ((e) ? ck_fail_at(__FILE__, __LINE__, "'" #e "' is true", ##__VA_ARGS__, (const char *)NULL) : (void)0)
#define fail(...) ck_fail_at(__FILE__, __LINE__, "failed", ##__VA_ARGS__, (const char *)NULL)
#define ck_assert(e) fail_unless(e)
#define mark_point() ((void)0)

Suite *suite_create(const char *name);
TCase *tcase_create(const char *name);
void tcase_add_checked_fixture(TCase *tc, SFun setup, SFun teardown);
void tcase_add_test(TCase *tc, const TTest *t);
void suite_add_tcase(Suite *s, TCase *tc);
SRunner *srunner_create(Suite *s);
void srunner_add_suite(SRunner *sr, Suite *s);
void srunner_set_fork_status(SRunner *sr, int mode);
void srunner_set_xml(SRunner *sr, const char *file);
void srunner_run_all(SRunner *sr, int mode);
int srunner_ntests_failed(SRunner *sr);
void srunner_free(SRunner *sr);
/* Zenix: a test that does not apply to Zenix's configuration is named here with the reason, and not run */
void ck_skip(const char *test, const char *why);
#endif
