/* SPDX-License-Identifier: GPL-2.0-only */
/* The C library's test program (DESIGN D27 section 11): a test in /tests, not a command. Each check compares what
 * the library gives with the expected text or value and prints "ok NAME" or "FAIL NAME: got ..."; the last line
 * counts them. tools/libc-test.sh runs it in FS-UAE, also in the modes that need the shell around it:
 *   libctest            every check that runs inside one process
 *   libctest order      stdout's buffering: a printf line, then a dprintf line (on a console the printf line comes
 *                       first: line-buffered; into a pipe it comes last: fully buffered)
 *   libctest epipe      writes to stdout until the reader is gone (the shell sees status 141)
 *   libctest stdin      prints a prompt without a newline, reads a line: the prompt is out before the read waits;
 *                       then the stream buffers' memory (inside the allocator's first chunk)
 *   libctest tty        the terminal (DESIGN D27 5.4) with the keys the test types after each prompt: the console's
 *                       size, raw mode and back, ICANON without ECHO (the console's grid shows nothing typed), the
 *                       C library's line editing, getpass
 *   libctest sigdfl     (run by system's check) status 0 when SIGINT and SIGQUIT have their default actions
 *   libctest hold N     every check, then N seconds more (the ZEX2 build on two consoles at once: free shows one
 *                       copy of its code) */
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
#include <glob.h>
#include <grp.h>
#include <poll.h>
#include <pwd.h>
#include <regex.h>
#include <spawn.h>
#include <termios.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <zenix.h>
#define ZENIX_LIBC
#include "usys.h"                       /* SYS_procinfo, for the memory checks */

static int nchk, nfail;
static const char *myname = "/tests/libctest";      /* argv[0]: the ZEX2 build runs as /tests/libctestpure */
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

/* a name in /tmp of this process's own (two may run at once: the ZEX2 build on two consoles) */
static void tmpname(char *out, const char *what) { snprintf(out, 48, "/tmp/lc%d%s", getpid(), what); }

static void t_files(void)
{
    char p[48];
    tmpname(p, ".txt");
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
    char d[48];
    tmpname(d, ".d");
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
    /* audit B3-10: an alignment over the allocator's limit is refused, not wrapped into a 16-byte block */
    {
        volatile size_t big = 0xBFFFFFF8u, al = 0x40000000u;       /* (volatile: no compile-time size warning) */
        check("memalign with a huge alignment refused", !memalign(al, big) && !aligned_alloc(al * 2, 16), "");
    }
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
    /* DESIGN D28 11: setlocale(LC_ALL, "") reads LC_ALL, LC_CTYPE, then LANG (picolibc alone would not) */
    static const struct { const char *all, *ctype, *lang; int utf8; } env[] = {
        { 0, 0, "C.UTF-8", 1 }, { 0, 0, "sv_SE", 1 }, { 0, 0, "sv_SE.UTF-8", 1 }, { 0, 0, "C", 0 }, { 0, 0, 0, 0 },
        { "C", 0, "C.UTF-8", 0 }, { 0, "C.UTF-8", "C", 1 }, { "", 0, "C.UTF-8", 1 } };
    for (unsigned i = 0; i < sizeof env / sizeof env[0]; i++) {
        if (env[i].all) setenv("LC_ALL", env[i].all, 1); else unsetenv("LC_ALL");
        if (env[i].ctype) setenv("LC_CTYPE", env[i].ctype, 1); else unsetenv("LC_CTYPE");
        if (env[i].lang) setenv("LANG", env[i].lang, 1); else unsetenv("LANG");
        char what[96];
        snprintf(what, sizeof what, "setlocale(LC_ALL, \"\") with LC_ALL=%s LC_CTYPE=%s LANG=%s: %s", env[i].all ? env[i].all : "-",
                 env[i].ctype ? env[i].ctype : "-", env[i].lang ? env[i].lang : "-", env[i].utf8 ? "UTF-8" : "C");
        const char *r = setlocale(LC_ALL, "");
        check(what, r && (MB_CUR_MAX > 1) == env[i].utf8, r ? r : "NULL");
    }
    setenv("LANG", "sv_SE.KOI8-R", 1); unsetenv("LC_ALL"); unsetenv("LC_CTYPE");
    setlocale(LC_ALL, "C.UTF-8");
    check("an unknown charset: NULL, the locale as it was", !setlocale(LC_ALL, "") && MB_CUR_MAX > 1, "");
    /* the console's decoder's cases (term-test): mbrtowc agrees on what is a character */
    static const struct { const char *s; int n; unsigned w; } dec[] = {
        { "\xc2\xa0", 2, 0xA0 }, { "\xdf\xbf", 2, 0x7FF }, { "\xe0\xa0\x80", 3, 0x800 }, { "\xef\xbf\xbd", 3, 0xFFFD },
        { "\xf0\x90\x80\x80", 4, 0x10000 }, { "\xf4\x8f\xbf\xbf", 4, 0x10FFFF }, { "\xe2\x94\x80", 3, 0x2500 },
        { "\xc0\x80", -1, 0 }, { "\xe0\x80\x80", -1, 0 }, { "\xed\xa0\x80", -1, 0 }, { "\xf0\x80\x80\x80", -1, 0 },
        { "\xf4\x90\x80\x80", -1, 0 }, { "\xf5\x80\x80\x80", -1, 0 }, { "\x80", -1, 0 } };
    int agree = 1;
    for (unsigned i = 0; i < sizeof dec / sizeof dec[0]; i++) {
        memset(&st, 0, sizeof st);
        w = 0;
        n = mbrtowc(&w, dec[i].s, strlen(dec[i].s), &st);
        if (dec[i].n < 0 ? n != (size_t)-1 : n != (size_t)dec[i].n || (unsigned)w != dec[i].w) {
            agree = 0;
            printf("libctest: mbrtowc case %u: %d, U+%04X\n", i, (int)n, (unsigned)w);
        }
    }
    check("mbrtowc agrees with the console's decoder (valid lengths, overlong, surrogates, above U+10FFFF)", agree, "");
    unsetenv("LANG");
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
          ENOTSUP == EOPNOTSUPP && EOPNOTSUPP == 95 && SIGUSR1 == 10 && SIGCHLD == 17 && PATH_MAX == 256, "");
    check("getenv of PATH", getenv("PATH") != 0, "");
    setenv("LIBCTEST", "yes", 1);
    checks("setenv then getenv", getenv("LIBCTEST") ? getenv("LIBCTEST") : "", "yes");
    /* audit B3-13: a bad name and a variable too big for the kernel's block are refused, the old value kept */
    errno = 0;
    check("setenv of a name with = refused (EINVAL)", setenv("A=B", "x", 1) == -1 && errno == EINVAL, "");
    {
        static char big[1100];
        memset(big, 'v', sizeof big - 1);
        errno = 0;
        check("setenv too big for the block refused (ENOMEM), the old value kept", setenv("LIBCTEST", big, 1) == -1 &&
              errno == ENOMEM && getenv("LIBCTEST") && !strcmp(getenv("LIBCTEST"), "yes"), "");
    }
    /* audit B3-12: the kernel refuses a block without its last 0 (EINVAL) and keeps the old one; the C library's
     * extra byte (user/zenv.c) stays for safety */
    {
        static char saved[1024], junk[1024];
        int sl = sys_envget(saved, sizeof saved), r = 0, r1 = 0, sl2 = -1, sl3 = -1, same2 = 0, same4 = 0;
        memset(junk, 'A', sizeof junk);
        if (sl > 0) {
            r = sys_envset(junk, sizeof junk);
            sl2 = sys_envget(junk, sizeof junk);
            same2 = sl2 == sl && !memcmp(junk, saved, (size_t)sl);
            r1 = sys_envset("Z=1", 4);  /* one 0 at its end is enough */
            sl3 = sys_envget(junk, sizeof junk);
            sys_envset(saved, (u32)sl);
            same4 = sys_envget(junk, sizeof junk) == sl && !memcmp(junk, saved, (size_t)sl);
        }
        checki("an environment block without its last 0 is refused (EINVAL)", r, -EINVAL);
        checki("the refused block leaves the old one, byte for byte", same2, 1);
        checki("a block ending in one 0 is taken", r1 == 0 && sl3 == 4, 1);
        checki("the kernel's environment is back after the test", same4, 1);
    }
    unsetenv("LIBCTEST");
    check("unsetenv", getenv("LIBCTEST") == 0, "");
    char cwd[64];
    check("getcwd", getcwd(cwd, sizeof cwd) && cwd[0] == '/', "");
    {   /* an older program's small buffer, and the limit to the byte (docs/architecture.md section 7, "Paths") */
        static char lp[PATH_MAX + 2], big[PATH_MAX];
        char small[16];
        memset(lp, 'x', sizeof lp - 1);
        lp[0] = '/'; lp[sizeof lp - 1] = 0;             /* 257 bytes */
        check("a path of 257 bytes: ENAMETOOLONG", open(lp, O_RDONLY) < 0 && errno == ENAMETOOLONG, "");
        lp[PATH_MAX] = 0;                               /* 256 bytes */
        check("a path of 256 bytes: ENAMETOOLONG", open(lp, O_RDONLY) < 0 && errno == ENAMETOOLONG, "");
        lp[PATH_MAX - 1] = 0;                           /* 255 bytes: taken, and not there (its one name is over 63) */
        check("a path of 255 bytes: asked for", open(lp, O_RDONLY) < 0 && errno != EFAULT, "");
        check("getcwd with too small a buffer: ERANGE, nothing past it",
              chdir("/tests") == 0 && getcwd(small, 3) == 0 && errno == ERANGE && getcwd(big, sizeof big) && !strcmp(big, "/tests") && chdir("/") == 0, "");
    }
    check("getpid", getpid() > 1, "");
}

/* ---- step 3 (DESIGN D27 section 13): signals, poll and select, sockets, libzenix's helpers, users, glob, regcomp,
 * processes */
static volatile int got_sig;
static void on_usr1(int s) { got_sig = s; }
static void t_signals(void)
{
    struct sigaction a, o;
    memset(&a, 0, sizeof a);
    a.sa_handler = on_usr1;
    check("sigaction", sigaction(SIGUSR1, &a, &o) == 0 && o.sa_handler == SIG_DFL, "");
    raise(SIGUSR1);
    checki("raise: the handler ran", got_sig, SIGUSR1);
    sigset_t s, old;
    sigemptyset(&s);
    sigaddset(&s, SIGUSR1);
    got_sig = 0;
    sigprocmask(SIG_BLOCK, &s, &old);
    raise(SIGUSR1);
    check("blocked: not yet", got_sig == 0, "");
    sigprocmask(SIG_SETMASK, &old, 0);
    checki("unblocked: it ran", got_sig, SIGUSR1);
    check("signal() gives the handler back", signal(SIGUSR1, SIG_IGN) == on_usr1, "");
    raise(SIGUSR1);
    check("an ignored signal", 1, "");
    signal(SIGUSR1, SIG_DFL);
    checks("zsig_name", zsig_name(SIGTERM) ? zsig_name(SIGTERM) : "", "TERM");
    checki("zsig_num", zsig_num("SIGHUP"), SIGHUP);
    check("kill(pid, 0): it exists", kill(getpid(), 0) == 0, "");
    check("getpgrp is ours", getpgrp() > 0 && getpgid(0) == getpgrp(), "");
}

static void t_poll(void)
{
    int fd[2];
    check("pipe", pipe(fd) == 0, "");
    struct pollfd p = { fd[0], POLLIN, 0 };
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    int r = poll(&p, 1, 50);
    clock_gettime(CLOCK_MONOTONIC, &b);
    long ms = (long)(b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
    check("poll of an empty pipe: 0 after the timeout", r == 0 && ms >= 50, "");
    write(fd[1], "x", 1);
    r = poll(&p, 1, -1);
    check("poll: POLLIN once there is a byte", r == 1 && (p.revents & POLLIN), "");
    fd_set rs;
    FD_ZERO(&rs);
    FD_SET(fd[0], &rs);
    struct timeval tv = { 1, 0 };
    r = select(fd[0] + 1, &rs, 0, 0, &tv);
    check("select: readable", r == 1 && FD_ISSET(fd[0], &rs), "");
    /* audit B3-16: a descriptor past the process's limit is EBADF, a negative timeout EINVAL; a huge timeout is no
     * wrap into "for ever" (the byte is there: at once) */
    FD_ZERO(&rs);
    FD_SET(40, &rs);
    errno = 0;
    check("select: a descriptor past the limit is EBADF", select(41, &rs, 0, 0, &tv) == -1 && errno == EBADF, "");
    struct timeval neg = { -1, 0 }, huge = { 0x7FFFFFFF, 0 };
    FD_ZERO(&rs);
    FD_SET(fd[0], &rs);
    errno = 0;
    check("select: a negative timeout is EINVAL", select(fd[0] + 1, &rs, 0, 0, &neg) == -1 && errno == EINVAL, "");
    FD_ZERO(&rs);
    FD_SET(fd[0], &rs);
    check("select: a huge timeout, the byte there: readable at once", select(fd[0] + 1, &rs, 0, 0, &huge) == 1, "");
    close(fd[1]);
    char c;
    read(fd[0], &c, 1);
    p.revents = 0;
    r = poll(&p, 1, 0);
    check("poll: the writer gone (POLLHUP or POLLIN at the end)", r == 1 && (p.revents & (POLLHUP | POLLIN)), "");
    close(fd[0]);
    int pf[2];
    check("pipe2 O_NONBLOCK", pipe2(pf, O_NONBLOCK) == 0 && read(pf[0], &c, 1) < 0 && errno == EAGAIN, strerror(errno));
    close(pf[0]); close(pf[1]);
}

static void t_sockets(void)
{
    struct in_addr in;
    char b[INET_ADDRSTRLEN];
    check("inet_pton and inet_ntop", inet_pton(AF_INET, "10.0.2.15", &in) == 1 &&
          !strcmp(inet_ntop(AF_INET, &in, b, sizeof b), "10.0.2.15"), b);
    check("inet_addr (BSD's short form)", inet_addr("127.1") == htonl(0x7f000001), "");
    check("inet_pton refuses a bad address", inet_pton(AF_INET, "1.2.3", &in) == 0, "");
    struct addrinfo h, *res = 0;
    memset(&h, 0, sizeof h);
    h.ai_family = AF_INET;
    h.ai_socktype = SOCK_STREAM;
    int r = getaddrinfo("127.0.0.1", "http", &h, &res);
    struct sockaddr_in *sa = res ? (struct sockaddr_in *)res->ai_addr : 0;
    check("getaddrinfo of a number and a service", !r && sa && ntohs(sa->sin_port) == 80 &&
          sa->sin_addr.s_addr == htonl(INADDR_LOOPBACK) && res->ai_protocol == IPPROTO_TCP, gai_strerror(r));
    freeaddrinfo(res);
    checks("gai_strerror", gai_strerror(EAI_NONAME), "name or service not known");
    /* a UDP datagram to itself on the loopback: netd answers (started at the first socket) */
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in me;
    memset(&me, 0, sizeof me);
    me.sin_family = AF_INET;
    me.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t ml = sizeof me;
    check("socket, bind, getsockname", s >= 0 && bind(s, (struct sockaddr *)&me, sizeof me) == 0 &&
          getsockname(s, (struct sockaddr *)&me, &ml) == 0 && me.sin_port != 0, strerror(errno));
    ssize_t n = sendto(s, "dgram", 5, 0, (struct sockaddr *)&me, sizeof me);
    struct pollfd p = { s, POLLIN, 0 };
    char rb[16] = "";
    int ready = poll(&p, 1, 3000);
    ssize_t m = ready == 1 ? recvfrom(s, rb, sizeof rb - 1, 0, 0, 0) : -1;
    check("sendto itself, poll, recvfrom", n == 5 && m == 5 && !memcmp(rb, "dgram", 5), strerror(errno));
    int nb = 1;
    check("ioctl FIONBIO and fcntl F_GETFL see O_NONBLOCK", ioctl(s, FIONBIO, &nb) == 0 &&
          (fcntl(s, F_GETFL) & O_NONBLOCK), "");
    check("a recv with nothing there: EAGAIN", recv(s, rb, sizeof rb, 0) < 0 && errno == EAGAIN, strerror(errno));
    close(s);
}

static const char *lp300(void)                  /* a path too long for any system call */
{
    static char b[300];
    memset(b, 'x', sizeof b - 1);
    b[0] = '/';
    return b;
}

static void t_helpers(void)
{
    char *j = path_join("/tmp", "x");
    checks("path_join", j ? j : "", "/tmp/x");
    free(j);
    check("path_join too long: ENAMETOOLONG", !path_join(lp300(), "a-name") &&
          errno == ENAMETOOLONG, "");
    char **names;
    int n = dir_list("/tests", &names), found = 0;
    for (int i = 0; i < n; i++) if (!strcmp(names[i], "libctest")) found = 1;
    check("dir_list", n > 1 && found, "");
    dir_free(names, n);
    char p[64];
    check("path_find through $PATH", path_find("sh", p, sizeof p) && !strcmp(p, "/bin/sh"), p);
    check("zrealpath", zrealpath("/tmp/../etc/./", p, sizeof p) == 0 && !strcmp(p, "/etc"), p);
    {   /* realpath (libc/zenix/sys.c over zrealpath) and utimensat (over SYS_utimes) */
        char rp[ZPATH_MAX], *hp;
        check("realpath: . and .. resolved", realpath("/tmp/../etc/./passwd", rp) == rp && !strcmp(rp, "/etc/passwd"), rp);
        check("realpath of a missing name: ENOENT", !realpath("/tmp/no/such/thing", rp) && errno == ENOENT, "");
        hp = realpath("/etc/../etc", 0);
        check("realpath into a malloc'd buffer", hp && !strcmp(hp, "/etc"), hp ? hp : "null");
        free(hp);
        char ut[48];
        snprintf(ut, sizeof ut, "/tmp/lcut%d", getpid());
        int ufd = open(ut, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        struct stat us;
        close(ufd);
        struct timespec ts[2] = { { 1000000000, 0 }, { 1000000000, 0 } };
        check("utimensat: a given time", utimensat(AT_FDCWD, ut, ts, 0) == 0 && stat(ut, &us) == 0 &&
              us.st_mtim.tv_sec == 1000000000, "");
        struct timespec om[2] = { { 0, UTIME_OMIT }, { 0, UTIME_OMIT } };
        check("utimensat: UTIME_OMIT leaves the time", utimensat(AT_FDCWD, ut, om, 0) == 0 && stat(ut, &us) == 0 &&
              us.st_mtim.tv_sec == 1000000000, "");
        struct timespec nw[2] = { { 0, UTIME_NOW }, { 0, UTIME_NOW } };
        time_t t0 = time(0);
        check("utimensat: UTIME_NOW is the clock", utimensat(AT_FDCWD, ut, nw, 0) == 0 && stat(ut, &us) == 0 &&
              us.st_mtim.tv_sec >= t0 - 1 && us.st_mtim.tv_sec <= t0 + 2, "");
        utimensat(AT_FDCWD, ut, ts, 0);
        check("utimensat: a null array is now", utimensat(AT_FDCWD, ut, 0, 0) == 0 && stat(ut, &us) == 0 &&
              us.st_mtim.tv_sec >= t0 - 1 && us.st_mtim.tv_sec <= t0 + 2, "");
        struct timespec bad[2] = { { 0, 1000000000L }, { 0, 0 } };
        check("utimensat: a bad nanosecond count is EINVAL", utimensat(AT_FDCWD, ut, bad, 0) == -1 && errno == EINVAL, "");
        check("utimensat: another directory descriptor with a relative path is ENOSYS", utimensat(3, "rel", ts, 0) == -1 &&
              errno == ENOSYS, "");
        check("utimensat: an absolute path ignores the descriptor", utimensat(3, ut, ts, 0) == 0, "");
        check("utimensat of a missing file: ENOENT", utimensat(AT_FDCWD, "/tmp/no/such/thing", nw, 0) == -1 &&
              errno == ENOENT, "");
        unlink(ut);
    }
    check("color_arg", color_arg("--color=never", 2) == 0 && color_arg("--colour", 2) == 2 && color_arg("-x", 2) == -1, "");
    check("color_auto on a file: no", (n = open("/etc/passwd", O_RDONLY)) >= 0 && !color_auto(n), "");
    close(n);
}

static void t_users(void)
{
    struct passwd *pw = getpwnam("root");
    check("getpwnam root", pw && pw->pw_uid == 0 && pw->pw_gid == 0 && !strcmp(pw->pw_dir, "/") &&
          !strcmp(pw->pw_shell, "/bin/sh"), "");
    pw = getpwuid(1000);
    checks("getpwuid 1000", pw ? pw->pw_name : "", "user");
    check("getpwnam of nobody: 0", getpwnam("no-such-user") == 0, "");
    struct group *g = getgrnam("users");
    check("getgrnam users, its members", g && g->gr_gid == 100 && g->gr_mem[0] && !strcmp(g->gr_mem[0], "user") &&
          g->gr_mem[1] && !strcmp(g->gr_mem[1], "guest") && !g->gr_mem[2], "");
    g = getgrgid(10);
    checks("getgrgid 10", g ? g->gr_name : "", "wheel");
}

static void t_glob(void)
{
    char d[48], p[64];
    tmpname(d, ".g");
    mkdir(d, 0755);
    const char *files[] = { "b.c", "a.c", "c.h", ".hid.c", 0 };
    for (int i = 0; files[i]; i++) { snprintf(p, sizeof p, "%s/%s", d, files[i]); close(open(p, O_WRONLY | O_CREAT, 0644)); }
    snprintf(p, sizeof p, "%s/sub", d); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/sub/d.c", d); close(open(p, O_WRONLY | O_CREAT, 0644));
    glob_t g;
    char want[160];
    snprintf(p, sizeof p, "%s/*.c", d);
    int r = glob(p, 0, 0, &g);
    snprintf(want, sizeof want, "%s/a.c %s/b.c", d, d);
    char got[200] = "";
    for (int i = 0; !r && i < g.gl_pathc; i++) { if (i) strcat(got, " "); strncat(got, g.gl_pathv[i], 60); }
    checks("glob *.c: sorted, no dot file", got, want);
    globfree(&g);
    snprintf(p, sizeof p, "%s/*/*.c", d);
    r = glob(p, 0, 0, &g);
    snprintf(want, sizeof want, "%s/sub/d.c", d);
    checks("glob */*.c", !r && g.gl_pathc == 1 ? g.gl_pathv[0] : "", want);
    globfree(&g);
    snprintf(p, sizeof p, "%s/s*", d);
    r = glob(p, GLOB_MARK, 0, &g);
    snprintf(want, sizeof want, "%s/sub/", d);
    checks("glob GLOB_MARK", !r && g.gl_pathc == 1 ? g.gl_pathv[0] : "", want);
    snprintf(p, sizeof p, "%s/.h*", d);
    r = glob(p, GLOB_APPEND, 0, &g);
    check("glob GLOB_APPEND, a dot file by a dot pattern", !r && g.gl_pathc == 2 && strstr(g.gl_pathv[1], ".hid.c"), "");
    globfree(&g);
    snprintf(p, sizeof p, "%s/*.x", d);
    checki("glob with no match: GLOB_NOMATCH", glob(p, 0, 0, &g), GLOB_NOMATCH);
    r = glob(p, GLOB_NOCHECK, 0, &g);
    check("GLOB_NOCHECK gives the pattern", !r && g.gl_pathc == 1 && !strcmp(g.gl_pathv[0], p), "");
    globfree(&g);
    check("an unsupported flag: GLOB_ABEND", glob(p, GLOB_BRACE, 0, &g) == GLOB_ABEND && errno == EINVAL, "");
    for (int i = 0; files[i]; i++) { snprintf(p, sizeof p, "%s/%s", d, files[i]); unlink(p); }
    snprintf(p, sizeof p, "%s/sub/d.c", d); unlink(p);
    snprintf(p, sizeof p, "%s/sub", d); rmdir(p);
    rmdir(d);
}

static void t_regex(void)
{
    regex_t re;
    regmatch_t m[3];
    char b[64];
    int r = regcomp(&re, "(ab|cd)+e", REG_EXTENDED);
    checki("regcomp ERE", r, 0);
    checki("re_nsub", (long long)re.re_nsub, 1);
    r = regexec(&re, "xxabcde!", 3, m, 0);
    snprintf(b, sizeof b, "%d %d-%d %d-%d %d", r, (int)m[0].rm_so, (int)m[0].rm_eo, (int)m[1].rm_so, (int)m[1].rm_eo, (int)m[2].rm_so);
    checks("regexec: the match and its group", b, "0 2-7 4-6 -1");
    checki("REG_NOMATCH", regexec(&re, "abab", 0, 0, 0), REG_NOMATCH);
    regfree(&re);
    r = regcomp(&re, "a\\(b*\\)c", 0);
    r = r ? r : regexec(&re, "zabbbc", 2, m, 0);
    snprintf(b, sizeof b, "%d %d-%d %d-%d", r, (int)m[0].rm_so, (int)m[0].rm_eo, (int)m[1].rm_so, (int)m[1].rm_eo);
    checks("a basic expression (no REG_EXTENDED)", b, "0 1-6 2-5");
    regfree(&re);
    regcomp(&re, "^HeLLo", REG_EXTENDED | REG_ICASE | REG_NOSUB);
    check("REG_ICASE and REG_NOSUB", regexec(&re, "hello world", 0, 0, 0) == 0, "");
    check("REG_NOTBOL: ^ does not match at the start", regexec(&re, "hello", 0, 0, REG_NOTBOL) == REG_NOMATCH, "");
    regfree(&re);
    regcomp(&re, "^b", REG_EXTENDED | REG_NEWLINE);
    check("REG_NEWLINE: ^ after a newline", regexec(&re, "a\nb", 0, 0, 0) == 0, "");
    regfree(&re);
    regcomp(&re, "o+", REG_EXTENDED);
    m[0].rm_so = 5; m[0].rm_eo = 12;
    r = regexec(&re, "foo, goo, zoo", 1, m, REG_STARTEND);
    snprintf(b, sizeof b, "%d %d-%d", r, (int)m[0].rm_so, (int)m[0].rm_eo);
    checks("REG_STARTEND", b, "0 6-8");
    regfree(&re);
    r = regcomp(&re, "a(", REG_EXTENDED);
    check("a bad pattern is refused", r != 0, "");
    regerror(REG_NOMATCH, 0, b, sizeof b);
    checks("regerror", b, "no match");
}

/* the bytes a descriptor gives until its end */
static void slurp(int fd, char *b, int max)
{
    int n = 0, r;
    while (n < max - 1 && (r = (int)read(fd, b + n, (size_t)(max - 1 - n))) > 0) n += r;
    b[n] = 0;
}
static void t_spawn(void)
{
    int fd[2], st = -1;
    pid_t pid = 0;
    char b[96];
    pipe(fd);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fd[1], 1);
    char *echo[] = { "echo", "spawned", "here", 0 };
    int r = posix_spawn(&pid, "/bin/echo", &fa, 0, echo, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fd[1]);
    slurp(fd[0], b, sizeof b);
    close(fd[0]);
    check("posix_spawn with stdout to a pipe", !r && pid > 0 && !strcmp(b, "spawned here\n"), b);
    check("waitpid: exit status 0", waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0, "");
    char tf[48];
    tmpname(tf, ".sp");
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, tf, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    char *penv[] = { "printenv", "ZX_SPAWN", 0 }, *envp[] = { "ZX_SPAWN=forty-two", "PATH=/bin", 0 };
    r = posix_spawnp(&pid, "printenv", &fa, 0, penv, envp);
    posix_spawn_file_actions_destroy(&fa);
    waitpid(pid, &st, 0);
    int f = open(tf, O_RDONLY);
    slurp(f, b, sizeof b);
    close(f);
    unlink(tf);
    check("posix_spawnp, envp and an opened stdout", !r && !strcmp(b, "forty-two\n"), b);
    check("... our environment is as it was", getenv("ZX_SPAWN") == 0 && getenv("PATH") != 0, "");
    posix_spawn_file_actions_init(&fa);
    checki("a file action on descriptor 5: EINVAL", posix_spawn_file_actions_addclose(&fa, 5), EINVAL);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&at, 0);
    char *sl[] = { "sleep", "1", 0 };
    r = posix_spawn(&pid, "/bin/sleep", 0, &at, sl, 0);
    int pg = getpgid(pid);
    check("POSIX_SPAWN_SETPGROUP: a group of its own", !r && pg == pid && pg != getpgrp(), "");
    checki("another attribute: EINVAL", posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK), EINVAL);
    posix_spawnattr_destroy(&at);
    waitpid(pid, &st, 0);
    char *t1[] = { "true", 0 }, *t2[] = { "false", 0 };
    pid_t a, c;
    posix_spawn(&a, "/bin/true", 0, 0, t1, 0);
    posix_spawn(&c, "/bin/false", 0, 0, t2, 0);
    int s1 = -1, s2 = -1;
    pid_t w1 = wait(&s1), w2 = wait(&s2);
    check("wait: which child and its status", ((w1 == a && w2 == c && WEXITSTATUS(s1) == 0 && WEXITSTATUS(s2) == 1) ||
          (w1 == c && w2 == a && WEXITSTATUS(s1) == 1 && WEXITSTATUS(s2) == 0)), "");
    check("wait with no child: ECHILD", wait(&st) < 0 && errno == ECHILD, "");
    checki("system: the exit status", WEXITSTATUS(system("exit 3")), 3);
    check("system(NULL): there is a shell", system(0) != 0, "");
    FILE *pf = popen("echo through a pipe", "r");
    b[0] = 0;
    if (pf) fgets(b, sizeof b, pf);
    checks("popen r", b, "through a pipe\n");
    checki("pclose: the status", pf ? pclose(pf) : -1, 0);
    tmpname(tf, ".po");
    char cmd[80];
    snprintf(cmd, sizeof cmd, "cat > %s", tf);
    pf = popen(cmd, "w");
    if (pf) { fputs("into the command\n", pf); pclose(pf); }
    f = open(tf, O_RDONLY);
    slurp(f, b, sizeof b);
    close(f);
    unlink(tf);
    checks("popen w", b, "into the command\n");
    /* fflush(NULL): every stream with output waiting; a stream being read is left alone (DESIGN D52 9: picolibc's
     * took NULL as a stream; Lua's io.popen calls it). The read stream is a pipe: one that was flushed would seek
     * back over its buffered input, which a pipe cannot, and lose the rest of the line. */
    tmpname(tf, ".ff");
    FILE *fw = fopen(tf, "w"), *fr = popen("echo abc", "r");
    if (fw) fputs("flushed by NULL\n", fw);
    int fr0 = fr ? getc(fr) : EOF;
    checki("fflush(NULL): 0", fflush(0), 0);
    b[0] = 0;
    f = open(tf, O_RDONLY);
    slurp(f, b, sizeof b);
    close(f);
    checks("fflush(NULL): the written stream's bytes are in the file", b, "flushed by NULL\n");
    int fr1 = fr ? getc(fr) : EOF, fr2 = fr ? getc(fr) : EOF;
    check("fflush(NULL): a pipe being read keeps its buffered input", fr0 == 'a' && fr1 == 'b' && fr2 == 'c', "");
    if (fw) fclose(fw);
    if (fr) pclose(fr);
    unlink(tf);
    check("getppid", getppid() > 0 && getppid() != getpid(), "");
    /* system ignores SIGINT and SIGQUIT while it waits (POSIX: ^C is the command's), puts the caller's handlers
     * back after, and the command itself gets the defaults */
    signal(SIGINT, on_usr1);
    signal(SIGQUIT, on_usr1);
    got_sig = 0;
    snprintf(cmd, sizeof cmd, "sleep 1; kill -INT %d; kill -QUIT %d; exit 5", getpid(), getpid());
    checki("system: SIGINT and SIGQUIT ignored while it waits", WEXITSTATUS(system(cmd)) * 100 + got_sig, 500);
    check("system: the caller's handlers back after", signal(SIGINT, SIG_DFL) == on_usr1 &&
          signal(SIGQUIT, SIG_DFL) == on_usr1, "");
    snprintf(cmd, sizeof cmd, "%s sigdfl", myname);
    checki("system: the command's SIGINT and SIGQUIT are the defaults", WEXITSTATUS(system(cmd)), 0);
}

/* the console's grid (CON_READ, root) holds text on one of its rows: 1, 0, or -1 when it cannot be read */
static int grid_has(const char *text)
{
    static unsigned char *cells;
    unsigned cap = 132 * 64 * 3;
    if (!cells && !(cells = malloc(cap))) return -1;
    struct uconread r = { sizeof r, (u32)cells, cap, 0, 0, 0, 0, 0, 0, 0, 0 };
    if (sys_conread(0, &r) < 0 || !r.cols || r.cols > 132) return -1;
    char row[133];
    for (unsigned y = 0; y < r.rows && (y + 1) * r.cols * 3 <= cap; y++) {
        for (unsigned x = 0; x < r.cols; x++) row[x] = (char)cells[(y * r.cols + x) * 3];
        row[r.cols] = 0;
        if (strstr(row, text)) return 1;
    }
    return 0;
}
static void prompt(const char *p) { fputs(p, stdout); fflush(stdout); }
/* libctest tty: the terminal with keys the test types (tools/libc-test.sh) */
static int t_tty(void)
{
    struct winsize ws;
    int ti = sys_ttyinfo(), fd[2];
    check("TIOCGWINSZ: the console's size", ioctl(0, TIOCGWINSZ, &ws) == 0 && ws.ws_row == (ti >> 16) &&
          ws.ws_col == (ti & 0xFFFF), "");
    printf("winsize %ux%u\n", ws.ws_col, ws.ws_row);
    pipe(fd);
    check("TIOCGWINSZ on a pipe: ENOTTY", ioctl(fd[0], TIOCGWINSZ, &ws) < 0 && errno == ENOTTY, "");
    struct termios orig, t;
    check("tcgetattr on a pipe: ENOTTY", tcgetattr(fd[0], &t) < 0 && errno == ENOTTY, "");
    close(fd[0]); close(fd[1]);
    check("tcgetattr: cooked at the start", tcgetattr(0, &orig) == 0 && (orig.c_lflag & (ICANON | ECHO | ISIG)) ==
          (ICANON | ECHO | ISIG) && orig.c_cc[VMIN] == 1 && orig.c_cc[VTIME] == 0, "");
    t = orig;
    t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 5;
    check("VMIN 0 / VTIME 5: EINVAL", tcsetattr(0, TCSANOW, &t) < 0 && errno == EINVAL, "");
    t = orig;
    cfmakeraw(&t);
    check("tcsetattr raw", tcsetattr(0, TCSANOW, &t) == 0, "");
    prompt("raw> ");
    char c1 = 0, c2 = 0;
    read(0, &c1, 1);
    read(0, &c2, 1);
    tcsetattr(0, TCSANOW, &orig);
    printf("\n");
    check("raw: a key without Return; ISIG off: ^C is a byte", c1 == 'x' && c2 == 3, "");
    t = orig;
    t.c_lflag &= ~(tcflag_t)ECHO;
    tcsetattr(0, TCSANOW, &t);
    prompt("secret> ");
    char b[64];
    ssize_t n = read(0, b, sizeof b - 1);
    tcsetattr(0, TCSANOW, &orig);
    b[n > 0 ? n : 0] = 0;
    printf("\n");
    check("ICANON on, ECHO off: a line read", !strcmp(b, "hidden words\n"), "");
    check("... nothing of it echoed (the console's grid)", grid_has("hidden") == 0 && grid_has("secret>") == 1, "");
    t = orig;
    t.c_lflag &= ~(tcflag_t)ISIG;
    tcsetattr(0, TCSANOW, &t);
    prompt("edit> ");
    n = read(0, b, sizeof b - 1);
    b[n > 0 ? n : 0] = 0;
    check("the C library's line editing: erase", !strcmp(b, "abc\n"), b);
    check("... echoed", grid_has("edit> abc") == 1, "");
    prompt("pieces> ");
    char p1[4] = "", p2[4] = "", p3[4] = "";
    read(0, p1, 2); read(0, p2, 2); n = read(0, p3, 3);
    tcsetattr(0, TCSANOW, &orig);
    check("a line handed out in pieces", !memcmp(p1, "12", 2) && !memcmp(p2, "34", 2) && n == 2 && !memcmp(p3, "5\n", 2), "");
    char *pw = getpass("Password: ");
    check("getpass", pw && !strcmp(pw, "pw123"), pw);
    check("... not echoed", grid_has("pw123") == 0, "");
    prompt("cooked> ");
    b[0] = 0;
    fgets(b, sizeof b, stdin);
    checks("cooked again: a line", b, "plain\n");
    printf("libctest tty: %d checks, %d failed\n", nchk, nfail);
    return nfail ? 1 : 0;
}

static int atexit_ran;
/* stdout is flushed first: into a file it is fully buffered, and its last part would come after this line, splitting
 * whichever line the buffer ended in */
static void at_end(void) { if (!atexit_ran++) { fflush(stdout); dprintf(1, "atexit ran\n"); } }

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
    if (argc > 1 && !strcmp(argv[1], "tty")) return t_tty();
    if (argc > 1 && !strcmp(argv[1], "sigdfl")) {     /* run by system(): SIGINT and SIGQUIT not ignored here */
        struct sigaction i, q;
        return sigaction(SIGINT, 0, &i) == 0 && sigaction(SIGQUIT, 0, &q) == 0 && i.sa_handler == SIG_DFL &&
               q.sa_handler == SIG_DFL ? 0 : 7;
    }
    myname = argv[0];
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
    t_signals();
    t_poll();
    t_sockets();
    t_helpers();
    t_users();
    t_glob();
    t_regex();
    t_spawn();
    printf("libctest: %d checks, %d failed\n", nchk, nfail);
    if (argc > 2 && !strcmp(argv[1], "hold")) sleep((unsigned)atoi(argv[2]));   /* still running: free shows it */
    return nfail ? 1 : 0;
}
