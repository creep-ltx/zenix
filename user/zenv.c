/* SPDX-License-Identifier: GPL-2.0-only */
/* The environment: libz's, one source for libz and the C library (user/zshare.h). The kernel keeps a process's copy
 * (SYS_envget/SYS_envset, inherited by its children); setenv and unsetenv write it back, so a spawned program sees it. */
#include "zshare.h"

char **environ;
static char env_store[1024];
static char *env_ptrs[64];
static int env_ready;

void zenv_load(void)
{
    if (env_ready) return;
    env_ready = 1;
    int n = sys_envget(env_store, sizeof env_store), k = 0;
    if (n > (int)sizeof env_store) n = (int)sizeof env_store;
    for (int i = 0; i < n && env_store[i] && k < 63;) { env_ptrs[k++] = env_store + i; i += (int)strlen(env_store + i) + 1; }
    env_ptrs[k] = 0;
    environ = env_ptrs;
}
#ifdef ZENIX_LIBC
/* the C library loads the environment before main in a program that links it (getenv, setenv, environ), from the
 * init array (libc/zenix/start.c): a weak reference from the start would be address 0 in a program without it, and
 * a ZEXE's relocation makes that the program's own start (found 2026-10-29: such a program called itself until its
 * stack overflowed) */
static void zenv_init(void) { zenv_load(); }
static void (*const zenv_init_p)(void) __attribute__((section(".init_array"), used)) = zenv_init;   /* gcc's
                                         * constructor attribute would use .ctors here, which the link scripts drop */
#endif
static void env_save(void)
{
    static char blk[1024];
    u32 n = 0;
    for (int i = 0; env_ptrs[i]; i++) { u32 l = strlen(env_ptrs[i]); if (n + l + 2 > sizeof blk) break; memcpy(blk + n, env_ptrs[i], l); n += l; blk[n++] = 0; }
    blk[n++] = 0;
    sys_envset(blk, n);
}
char *getenv(const char *name)
{
    zenv_load();
    u32 l = strlen(name);
    for (int i = 0; env_ptrs[i]; i++) if (!strncmp(env_ptrs[i], name, l) && env_ptrs[i][l] == '=') return env_ptrs[i] + l + 1;
    return 0;
}
int unsetenv(const char *name)
{
    zenv_load();
    u32 l = strlen(name);
    for (int i = 0; env_ptrs[i]; i++)
        if (!strncmp(env_ptrs[i], name, l) && env_ptrs[i][l] == '=') { for (int k = i; env_ptrs[k]; k++) env_ptrs[k] = env_ptrs[k + 1]; env_save(); return 0; }
    return 0;
}
int setenv(const char *name, const char *value, int overwrite)
{
    zenv_load();
    if (!overwrite && getenv(name)) return 0;
    unsetenv(name);
    int k = 0; while (env_ptrs[k]) k++;
    if (k >= 63) return -1;
    char *e = malloc(strlen(name) + strlen(value) + 2);
    if (!e) return -1;
    strcpy(e, name); strcat(e, "="); strcat(e, value);
    env_ptrs[k] = e; env_ptrs[k + 1] = 0;
    env_save();
    return 0;
}

