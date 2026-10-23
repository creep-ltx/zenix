#include "libz.h"
/* Test program: hands the kernel pointers that are not ours (must get -1, not a crash), then checks that memory
 * calls fail with 0 (NULL) and that malloc, free and realloc keep their promises (audit B1-B3). One line per
 * check; exits 0 when every check passed. */
static int bad;
static void check(const char *what, int ok)
{
    printf("badptr: %s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) bad = 1;
}

int main(void)
{
    int a = sys_write(1, (const void *)0x1000, 16);
    int b = sys_write(1, (const void *)0xFFFFFFF0, 64);
    printf("badptr: write(0x1000)=%d write(0xfffffff0)=%d\n", a, b);
    check("bad pointers refused", a == -1 && b == -1);

    /* the kernel: 0 on failure, never -1; sizes near 4 GB do not wrap */
    check("memalloc(0xfffffff0) is 0", sys_memalloc(0xFFFFFFF0u) == 0);
    check("memalloc(0xffffffff) is 0", sys_memalloc(0xFFFFFFFFu) == 0);
    check("memalloc(0) is 0", sys_memalloc(0) == 0);
    check("malloc(-16) is NULL", malloc((u32)-16) == 0);
    check("malloc(0x7fffffff) is NULL", malloc(0x7FFFFFFFu) == 0);

    /* small blocks are reused after free */
    char *p = malloc(100);
    free(p);
    char *q = malloc(100);
    check("a freed small block is reused", p && p == q);

    /* the largest small block (32 KB: it needs a chunk bigger than the first one) */
    char *l = malloc(32768);
    if (l) { memset(l, 1, 32768); free(l); }
    check("malloc(32768) works", l != 0);

    /* realloc keeps the contents, copies only the old block */
    char *r = malloc(20);
    for (int i = 0; i < 20; i++) r[i] = (char)i;
    char *s = realloc(r, 50000);
    int same = s != 0;
    for (int i = 0; s && i < 20; i++) if (s[i] != (char)i) same = 0;
    check("realloc keeps the contents", same);
    free(s);

    /* big blocks each take a region (at most 8 per process): malloc must say NULL when they run out, and free
     * must give them back so that the next one works */
    void *big[16]; int nb = 0;
    while (nb < 16 && (big[nb] = malloc(65536))) nb++;
    printf("badptr: %d big blocks before NULL\n", nb);
    check("malloc says NULL when the regions run out", nb > 0 && nb < 16);
    for (int i = 0; i < nb; i++) free(big[i]);
    void *again = malloc(65536);
    check("free gives big blocks back", again != 0);
    free(again);

    /* a pointer malloc did not give out is reported, not followed */
    static u32 fake[4] = { 16, 0x12345678, 0, 0 };
    free(&fake[2]);
    check("a bad free is survived", 1);

    printf("badptr: %s\n", bad ? "FAILED" : "all ok");
    return bad;
}
