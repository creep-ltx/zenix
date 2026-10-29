/* SPDX-License-Identifier: GPL-2.0-only */
/* The C library's test program (DESIGN D27 section 11): a test in /tests, not a command. Each check compares what
 * the library gives with the expected text or value and prints "ok NAME" or "FAIL NAME: got ..."; the last line
 * counts them. tools/libc-test.sh runs it in FS-UAE, also in the modes that need the shell around it:
 *   libctest            every check that runs inside one process
 *   libctest order      stdout's buffering: a printf line, then a dprintf line (on a console the printf line comes
 *                       first: line-buffered; into a pipe it comes last: fully buffered)
 *   libctest epipe      writes to stdout until the reader is gone (the shell sees status 141)
 *   libctest stdin      prints a prompt without a newline, reads a line: the prompt is out before the read waits;
 *                       then the stream buffers' memory (inside the allocator's first chunk) */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <malloc.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <zenix.h>
#define ZENIX_LIBC
#include "usys.h"                       /* SYS_procinfo, for the memory checks */

static int nchk, nfail;
static void check(const char *name, int ok, const char *got)
{
    nchk++;
    if (ok) printf("ok %s\n", name);
    else { nfail++; printf("FAIL %s: got %s\n", name, got ? got : "(no text)"); }
}
static void checks(const char *name, const char *got, const char *want) { check(name, !strcmp(got, want), got); }
static void checki(const char *name, long long got, long long want)
{
    char b[32];
    snprintf(b, sizeof b, "%lld", got);
    check(name, got == want, b);
}

/* the memory this process owns (SYS_procinfo's mem: its regions and load block) */
static unsigned mymem(void)
{
    struct uproc p;
    for (u32 i = 0; i < 256; i++)
        if (sys_procinfo(i, &p) >= 0 && p.state && (int)p.pid == getpid()) return p.mem;
    return 0;
}

static void t_formats(void)
{
    char b[128];
    snprintf(b, sizeof b, "[%5d|%-5d|%05d|%+d|% d|%x|%X|%#o|%#x]", 42, 42, 42, 42, 42, 255, 255, 8, 255);
    checks("printf ints", b, "[   42|42   |00042|+42| 42|ff|FF|010|0xff]");
    snprintf(b, sizeof b, "[%s|%10s|%-6s|%.2s|%c|%%]", "abc", "right", "left", "xyz", 'Q');
    checks("printf strings", b, "[abc|     right|left  |xy|Q|%]");
    snprintf(b, sizeof b, "%lld %llu %llx", -1234567890123LL, 18446744073709551615ULL, 0x123456789abcdefULL);
    checks("printf long long", b, "-1234567890123 18446744073709551615 123456789abcdef");
    snprintf(b, sizeof b, "%ld %lu %zu %hhd", -2147483647L - 1, 4294967295UL, (size_t)7, (signed char)-3);
    checks("printf lengths", b, "-2147483648 4294967295 7 -3");
    checki("snprintf returns the full length", snprintf(b, 4, "%s", "abcdefgh"), 8);
    checks("snprintf cuts and ends the text", b, "abc");
    int n = -1;
    snprintf(b, sizeof b, "x%ny", &n);
    check("%n is not supported (nothing stored)", n == -1, b);
    snprintf(b, sizeof b, "%f", 1.5);
    checks("%f without a float build is visible", b, "*float*");
}

static void t_scan(void)
{
    int a = 0, c = 0; char w[16] = ""; unsigned x = 0; long long ll = 0;
    int r = sscanf("12 word 0x1f -9000000000 7", "%d %15s %x %lld %d", &a, w, &x, &ll, &c);
    char b[96];
    snprintf(b, sizeof b, "%d %d %s %x %lld %d", r, a, w, x, ll, c);
    checks("sscanf", b, "5 12 word 1f -9000000000 7");
    char *e;
    errno = 0;
    long v = strtol("2147483648", &e, 10);
    check("strtol overflow: LONG_MAX and ERANGE", v == LONG_MAX && errno == ERANGE && !*e, "");
    errno = 0;
    v = strtol("  -0x7fZ", &e, 16);
    check("strtol base 16, the end", v == -127 && *e == 'Z', "");
    v = strtol("0755", 0, 0);
    checki("strtol base 0 octal", v, 493);
    checki("strtoul -1", (long long)strtoul("-1", 0, 10), 4294967295LL);
    checki("atoi", atoi("  -42x"), -42);
}

static int cmpint(const void *a, const void *b) { int x = *(const int *)a, y = *(const int *)b; return x < y ? -1 : x > y; }
static void t_qsort(void)
{
    int v[200], ok = 1;
    for (int i = 0; i < 200; i++) v[i] = (i * 7919) % 211 - 100;
    qsort(v, 200, sizeof v[0], cmpint);
    for (int i = 1; i < 200; i++) if (v[i - 1] > v[i]) ok = 0;
    check("qsort 200 ints", ok, "");
    int k = 5, *f = bsearch(&k, v, 200, sizeof v[0], cmpint);
    check("bsearch", f && *f == 5, "");
}

static void t_files(void)
{
    const char *p = "/tmp/libct.txt";
    struct stat st;
    unlink(p);
    FILE *f = fopen(p, "w");
    check("fopen w creates", f != 0, strerror(errno));
    if (!f) return;
    fprintf(f, "line one\nline two\n");
    fclose(f);
    check("w wrote 18 bytes", !stat(p, &st) && st.st_size == 18, "");
    f = fopen(p, "a"); fputs("three\n", f); fclose(f);
    check("a appends", !stat(p, &st) && st.st_size == 24, "");
    char b[64];
    f = fopen(p, "r");
    fgets(b, sizeof b, f);
    checks("r and fgets", b, "line one\n");
    checki("ftell", ftell(f), 9);
    int c = fgetc(f);
    ungetc(c, f);
    checki("ungetc gives it back", fgetc(f), 'l');
    fseek(f, -6, SEEK_END);
    fgets(b, sizeof b, f);
    checks("fseek from the end", b, "three\n");
    check("EOF after the end", fgetc(f) == EOF && feof(f), "");
    fclose(f);
    f = fopen(p, "r+");
    fseek(f, 5, SEEK_SET); fputs("ONE", f); fclose(f);
    f = fopen(p, "r"); fgets(b, sizeof b, f); fclose(f);
    checks("r+ writes in place", b, "line ONE\n");
    f = fopen(p, "w+");
    fputs("new", f); rewind(f);
    fgets(b, sizeof b, f); fclose(f);
    check("w+ truncates and reads back", !strcmp(b, "new") && !stat(p, &st) && st.st_size == 3, b);
    check("fopen of a missing file: ENOENT", !fopen("/tmp/nonexistent/x", "r") && errno == ENOENT, strerror(errno));
    /* O_EXCL is Zenix's 0x80 and O_NONBLOCK 0x800 (picolibc's own header has O_EXCL 0x800) */
    check("O_EXCL and O_NONBLOCK kept apart", O_EXCL == 0x80 && O_NONBLOCK == 0x800 && O_EXCL != O_NONBLOCK, "");
    int fd = open(p, O_WRONLY | O_CREAT | O_EXCL, 0644);
    check("O_EXCL on an existing file: EEXIST", fd < 0 && errno == EEXIST, strerror(errno));
    unlink(p);
    fd = open(p, O_WRONLY | O_CREAT | O_EXCL, 0600);
    check("O_EXCL creates, with its mode", fd >= 0 && !stat(p, &st) && (st.st_mode & 0777) == 0600, "");
    close(fd);
    unlink(p);
    check("access of a missing file: ENOENT", access(p, F_OK) < 0 && errno == ENOENT, "");
    check("isatty of a file is 0, ENOTTY", (fd = open("/etc/passwd", O_RDONLY)) >= 0 && !isatty(fd) && errno == ENOTTY, "");
    close(fd);
}

static void t_dirs(void)
{
    const char *d = "/tmp/libct.d";
    char p[64];
    mkdir(d, 0755);
    for (int i = 0; i < 200; i++) { snprintf(p, sizeof p, "%s/f%03d", d, i); close(open(p, O_WRONLY | O_CREAT, 0644)); }
    DIR *dp = opendir(d);
    int n = 0, sum = 0;
    struct dirent *e;
    while (dp && (e = readdir(dp)))
        if (e->d_name[0] == 'f') { n++; sum += atoi(e->d_name + 1); }
    char b[48];
    snprintf(b, sizeof b, "%d names, sum %d", n, sum);
    checks("opendir: 200 names", b, "200 names, sum 19900");
    rewinddir(dp);
    n = 0;
    while (dp && (e = readdir(dp))) if (e->d_name[0] == 'f') n++;
    checki("rewinddir reads them again", n, 200);
    if (dp) closedir(dp);
    for (int i = 0; i < 200; i++) { snprintf(p, sizeof p, "%s/f%03d", d, i); unlink(p); }
    check("rmdir after", rmdir(d) == 0, strerror(errno));
    check("opendir of a file: ENOTDIR", !opendir("/etc/passwd") && errno == ENOTDIR, "");
}

static void t_malloc(void)
{
    static void *v[1500];
    int ok = 1;
    for (int i = 0; i < 1500; i++) {
        size_t n = (size_t)(1 + (i * 37) % (i % 50 == 0 ? 8000 : 600));
        if (!(v[i] = malloc(n))) { ok = 0; break; }
        memset(v[i], i & 255, n);
        if ((uintptr_t)v[i] & 7) ok = 0;
    }
    check("malloc 1500 blocks of every class, 8-aligned", ok, "");
    for (int i = 0; i < 1500; i += 2) free(v[i]);
    for (int i = 1; i < 1500 && ok; i += 2) {
        size_t n = (size_t)(1 + (i * 37) % (i % 50 == 0 ? 8000 : 600));
        for (size_t k = 0; k < n; k++) if (((unsigned char *)v[i])[k] != (i & 255)) { ok = 0; break; }
    }
    check("the blocks between freed ones kept their bytes", ok, "");
    for (int i = 1; i < 1500; i += 2) free(v[i]);
    char *r = malloc(10);
    strcpy(r, "grow");
    for (size_t n = 16; n <= 200000 && r; n *= 3) r = realloc(r, n);
    check("realloc growth keeps the text", r && !strcmp(r, "grow"), r);
    free(r);
    void *a = 0;
    check("posix_memalign 64", !posix_memalign(&a, 64, 100) && !((uintptr_t)a & 63), "");
    check("malloc_usable_size of it", a && malloc_usable_size(a) >= 100, "");
    a = realloc(a, 5000);
    check("realloc of an aligned block", a != 0, "");
    free(a);
    char *z = calloc(1000, 4);
    int zero = z != 0;
    for (int i = 0; z && i < 4000; i++) if (z[i]) zero = 0;
    check("calloc zeroes", zero, "");
    free(z);
    volatile size_t big = 0x10000, huge = 0x80000000u;  /* volatile: the compiler would refuse them at compile time */
    check("calloc overflow: NULL", calloc(big, big) == 0, "");
    check("malloc of 2 GB: NULL", malloc(huge) == 0, "");
    fflush(stdout);
    char *volatile bad = (char *)&nchk;
    free(bad);                          /* reported on standard error, ignored */
    check("a bad free is survived", 1, "");
}

static jmp_buf jb;
static volatile int depth;
static void deep(void);
static void (*volatile again)(void) = deep;     /* through a pointer: frames that longjmp leaves */
static void deep(void) { if (--depth <= 0) longjmp(jb, 7); again(); depth++; }
static void t_setjmp(void)
{
    volatile int seen = 0;
    int r = setjmp(jb);
    if (!r) { seen = 1; depth = 20; deep(); }
    check("setjmp/longjmp", r == 7 && seen == 1, "");
}

static void t_time(void)
{
    char b[64];
    time_t t = 1798761599;              /* 2026-12-31 23:59:59 UTC */
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S %a %j", &tm);
    checks("gmtime and strftime", b, "2026-12-31 23:59:59 Thu 365");
    t = 4102444800LL;                   /* 2100-01-01: past 2038 */
    gmtime_r(&t, &tm);
    strftime(b, sizeof b, "%Y-%m-%d %A", &tm);
    checks("past 2038 (64-bit time_t)", b, "2100-01-01 Friday");
    tm.tm_year = 126; tm.tm_mon = 9; tm.tm_mday = 29; tm.tm_hour = 12; tm.tm_min = 0; tm.tm_sec = 0; tm.tm_isdst = 0;
    setenv("TZ", "UTC0", 1); tzset();
    checki("mktime in UTC", (long long)mktime(&tm), 1793275200LL);
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
    t = 1782907200;                     /* 2026-07-01 12:00 UTC */
    localtime_r(&t, &tm);
    strftime(b, sizeof b, "%H:%M %Z", &tm);
    checks("TZ summer time", b, "14:00 CEST");
    t = 1767268800;                     /* 2026-01-01 12:00 UTC */
    localtime_r(&t, &tm);
    strftime(b, sizeof b, "%H:%M %Z", &tm);
    checks("TZ winter time", b, "13:00 CET");
    struct timeval tv;
    time_t now = time(0);
    gettimeofday(&tv, 0);
    check("time() and gettimeofday agree", tv.tv_sec - now >= 0 && tv.tv_sec - now <= 1, "");
    struct timespec a, c;
    int back = 0;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (int i = 0; i < 2000; i++) {
        clock_gettime(CLOCK_MONOTONIC, &c);
        if (c.tv_sec < a.tv_sec || (c.tv_sec == a.tv_sec && c.tv_nsec < a.tv_nsec)) back = 1;
        a = c;
    }
    check("CLOCK_MONOTONIC never goes back", !back, "");
    struct timespec ts = { 0, 30000000 };
    clock_gettime(CLOCK_MONOTONIC, &a);
    nanosleep(&ts, 0);
    clock_gettime(CLOCK_MONOTONIC, &c);
    long ms = (long)(c.tv_sec - a.tv_sec) * 1000 + (c.tv_nsec - a.tv_nsec) / 1000000;
    snprintf(b, sizeof b, "%ld ms", ms);
    check("nanosleep 30 ms sleeps 30 ms or more", ms >= 30, b);
}

static void t_utf8(void)
{
    check("setlocale C.UTF-8", setlocale(LC_ALL, "C.UTF-8") != 0, "");
    mbstate_t st;
    memset(&st, 0, sizeof st);
    wchar_t w = 0;
    size_t n = mbrtowc(&w, "\xc3\xa5", 2, &st);         /* å */
    check("mbrtowc of a Swedish letter", n == 2 && w == 0xe5, "");
    n = mbrtowc(&w, "\xe4\xb8\x80", 3, &st);             /* 一 */
    check("mbrtowc of a wide character", n == 3 && w == 0x4e00, "");
    checki("wcwidth of a Swedish letter", wcwidth(0xe5), 1);
    checki("wcwidth of a wide character", wcwidth(0x4e00), 2);
    memset(&st, 0, sizeof st);
    errno = 0;
    n = mbrtowc(&w, "\xc3\x28", 2, &st);
    check("bad UTF-8: EILSEQ", n == (size_t)-1 && errno == EILSEQ, "");
    char b[8];
    memset(&st, 0, sizeof st);
    n = wcrtomb(b, 0xf6, &st);
    check("wcrtomb of ö", n == 2 && (unsigned char)b[0] == 0xc3 && (unsigned char)b[1] == 0xb6, "");
    check("toupper stays ASCII", toupper('a') == 'A' && isalpha('Z') && !isdigit('x'), "");
    setlocale(LC_ALL, "C");
}

static void t_errors(void)
{
    checks("strerror(ENOENT): libz's text", strerror(ENOENT), "no such file or directory");
    checks("strerror(EILSEQ)", strerror(EILSEQ), "invalid or incomplete multibyte character");
    check("strerror and zstrerror are one table", !strcmp(strerror(EACCES), zstrerror(EACCES)) &&
          !strcmp(strerror(ENOTSOCK), zstrerror(ENOTSOCK)), "");
    checks("strerror of an unknown number", strerror(999), "unknown error 999");
    check("the C library's numbers are Zenix's", ENOTSOCK == 88 && EDOM == 33 && EOVERFLOW == 75 && EILSEQ == 84 &&
          ENOTSUP == EOPNOTSUPP && EOPNOTSUPP == 95 && SIGUSR1 == 10 && SIGCHLD == 17 && PATH_MAX == 64, "");
    check("getenv of PATH", getenv("PATH") != 0, "");
    setenv("LIBCTEST", "yes", 1);
    checks("setenv then getenv", getenv("LIBCTEST") ? getenv("LIBCTEST") : "", "yes");
    unsetenv("LIBCTEST");
    check("unsetenv", getenv("LIBCTEST") == 0, "");
    char cwd[64];
    check("getcwd", getcwd(cwd, sizeof cwd) && cwd[0] == '/', "");
    check("getpid", getpid() > 1, "");
}

static int atexit_ran;
static void at_end(void) { if (!atexit_ran++) dprintf(1, "atexit ran\n"); }

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "order")) {
        printf("printf line\n");
        dprintf(1, "dprintf line\n");
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "epipe")) {
        for (;;) printf("a line that goes nowhere once the reader has gone\n");
    }
    if (argc > 1 && !strcmp(argv[1], "stdin")) {
        free(malloc(16));               /* the allocator's first chunk */
        unsigned m0 = mymem();
        printf("prompt> ");
        char b[64] = "";
        fgets(b, sizeof b, stdin);
        b[strcspn(b, "\n")] = 0;
        printf("\nread: %s\n", b);
        fflush(stdout);
        unsigned m1 = mymem();
        printf("stream buffers in the first chunk: %s (%u -> %u)\n", m1 == m0 ? "yes" : "no", m0, m1);
        return 0;
    }
    atexit(at_end);
    t_formats();
    t_scan();
    t_qsort();
    t_files();
    t_dirs();
    t_malloc();
    t_setjmp();
    t_time();
    t_utf8();
    t_errors();
    printf("libctest: %d checks, %d failed\n", nchk, nfail);
    return nfail ? 1 : 0;
}
