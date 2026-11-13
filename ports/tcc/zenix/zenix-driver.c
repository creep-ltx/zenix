/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 *  tcc on Zenix: building a program (no -c, -E or -run)
 *
 *  Copyright (c) 2026 the Zenix authors
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  Zenix programs are linked by Zenix's own linker, zld (it writes the
 *  ZEXE format the loader reads, with the C library gcc built). So
 *  "tcc -o prog a.c b.s c.o -lm" compiles each C file to an object in
 *  TMPDIR (default /tmp), assembles each .s (and .S, preprocessed by tcc)
 *  with zas, then runs
 *
 *    zld -o prog [-L dir]... [the -Wl, options] --defsym=vfprintf=...
 *        LIB/crt0.o OBJECTS... [-lname]... -lc -lgcc
 *
 *  LIB is tcc's directory's parent ({B}/.., /usr/lib on Zenix). zld and
 *  zas are /bin/zld and /bin/zas, or the programs the environment
 *  variables TCC_ZLD and TCC_ZAS name (the PC's cross compiler). The
 *  printf and scanf of the C library are the floating-point ones
 *  (__d_vfprintf, __d_vfscanf) unless a -Wl,--defsym=vfprintf=... (or
 *  vfscanf) names another; -Wl,-s,BYTES sets the stack.
 */

#include <spawn.h>
#include <sys/wait.h>

extern char **environ;

#ifndef CONFIG_ZENIX_ZLD
# define CONFIG_ZENIX_ZLD "/bin/zld"
#endif
#ifndef CONFIG_ZENIX_ZAS
# define CONFIG_ZENIX_ZAS "/bin/zas"
#endif

typedef struct ZxArgs {
    char **v;
    int n, max;
} ZxArgs;

static void zx_add(ZxArgs *a, const char *s)
{
    if (a->n + 2 > a->max) {
        a->max = a->max ? a->max * 2 : 32;
        a->v = tcc_realloc(a->v, a->max * sizeof *a->v);
    }
    a->v[a->n++] = tcc_strdup(s);
    a->v[a->n] = NULL;
}

static void zx_free(ZxArgs *a)
{
    int i;
    for (i = 0; i < a->n; i++)
        tcc_free(a->v[i]);
    tcc_free(a->v);
    a->v = NULL, a->n = a->max = 0;
}

/* run a program and wait for it: its exit status, or -1 */
static int zx_run(char **argv, int verbose)
{
    pid_t pid;
    int st, i;
    if (verbose) {
        for (i = 0; argv[i]; i++)
            printf("%s%s", i ? " " : "", argv[i]);
        printf("\n");
        fflush(stdout);
    }
    if (posix_spawn(&pid, argv[0], NULL, NULL, argv, environ) != 0) {
        fprintf(stderr, "tcc: cannot run %s: %s\n", argv[0], strerror(errno));
        return -1;
    }
    if (waitpid(pid, &st, 0) < 0)
        return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* a new temporary file's name in TMPDIR (it exists, empty) */
static char *zx_temp(ZxArgs *temps)
{
    char buf[1024];
    const char *dir = getenv("TMPDIR");
    int fd;
    if (!dir || !*dir)
        dir = "/tmp";
    snprintf(buf, sizeof buf, "%s/tccXXXXXX", dir);
    fd = mkstemp(buf);
    if (fd < 0) {
        fprintf(stderr, "tcc: cannot make a temporary file in %s: %s\n", dir, strerror(errno));
        return NULL;
    }
    close(fd);
    zx_add(temps, buf);
    return temps->v[temps->n - 1];
}

static const char *zx_tool(const char *env, const char *def)
{
    const char *p = getenv(env);
    return p && *p ? p : def;
}

/* compile (or preprocess, with 'pp') one file with the command line's
   options into 'out' */
static int zx_compile(int argc, char **argv, const char *file, int type, const char *out, int pp)
{
    TCCState *s;
    int ret;
    FILE *f = NULL;

    s = tcc_new();
    if (tcc_parse_args(s, &argc, &argv) < 0) {
        tcc_delete(s);
        return 1;
    }
    set_environment(s);
    if (pp) {
        f = fopen(out, "w");
        if (!f) {
            fprintf(stderr, "tcc: cannot write %s\n", out);
            tcc_delete(s);
            return 1;
        }
        tcc_set_output_type(s, TCC_OUTPUT_PREPROCESS);
        s->ppfp = f;
    } else {
        tcc_set_output_type(s, TCC_OUTPUT_OBJ);
    }
    s->filetype = type;
    ret = tcc_add_file(s, file);
    if (!ret && !pp)
        ret = tcc_output_file(s, out);
    if (f)
        fclose(f);
    tcc_delete(s);
    return ret ? 1 : 0;
}

/* does this command line build a program? Its -Wl, options go to 'wl'
   and are left out of 'targs' (tcc's own linker would refuse zld's) */
static int zx_wants_link(int argc, char **argv, ZxArgs *targs, ZxArgs *wl, TCCState **ps)
{
    TCCState *s;
    int i, opt, ac;
    char **av;

    for (i = 0; i < argc; i++) {
        if (i > 0 && !strncmp(argv[i], "-Wl,", 4)) {
            char *p = argv[i] + 4, *q;
            char buf[1024];
            while (*p) {
                q = strchr(p, ',');
                if (!q)
                    q = p + strlen(p);
                snprintf(buf, sizeof buf, "%.*s", (int)(q - p), p);
                zx_add(wl, buf);
                p = *q ? q + 1 : q;
            }
        } else {
            zx_add(targs, argv[i]);
        }
    }
    s = tcc_new();
    ac = targs->n, av = targs->v;
    opt = tcc_parse_args(s, &ac, &av);
    if (opt != 0 || s->nb_files == 0
        || (s->output_type != 0 && s->output_type != TCC_OUTPUT_EXE)) {
        tcc_delete(s);
        return 0;
    }
    *ps = s;
    return 1;
}

static int zx_main(TCCState *s, ZxArgs *targs, ZxArgs *wl)
{
    ZxArgs za = { 0 }, temps = { 0 };
    char buf[1024];
    const char *lib;
    int i, ret = 0, printf_set = 0, scanf_set = 0;

    tcc_set_output_type(s, TCC_OUTPUT_OBJ);     /* sets the library directory */
    lib = s->tcc_lib_path;

    zx_add(&za, zx_tool("TCC_ZLD", CONFIG_ZENIX_ZLD));
    zx_add(&za, "-o");
    zx_add(&za, s->outfile ? s->outfile : "a.out");
    for (i = 0; i < s->nb_library_paths; i++) {
        zx_add(&za, "-L");
        zx_add(&za, s->library_paths[i]);
    }
    for (i = 0; i < wl->n; i++) {
        if (!strncmp(wl->v[i], "--defsym=vfprintf=", 18))
            printf_set = 1;
        if (!strncmp(wl->v[i], "--defsym=vfscanf=", 17))
            scanf_set = 1;
        zx_add(&za, wl->v[i]);
    }
    if (!printf_set)
        zx_add(&za, "--defsym=vfprintf=__d_vfprintf");
    if (!scanf_set)
        zx_add(&za, "--defsym=vfscanf=__d_vfscanf");
    snprintf(buf, sizeof buf, "-L%s/..", lib);
    zx_add(&za, buf);
    if (!s->nostdlib) {         /* -nostdlib: no crt0, no -lc -lgcc */
        snprintf(buf, sizeof buf, "%s/../crt0.o", lib);
        zx_add(&za, buf);
    }

    for (i = 0; i < s->nb_files && !ret; i++) {
        struct filespec *f = s->files[i];
        int type = f->type & AFF_TYPE_MASK;
        const char *ext = tcc_fileextension(f->name);
        char *obj;

        if (f->type & AFF_TYPE_LIB) {
            snprintf(buf, sizeof buf, "-l%s", f->name);
            zx_add(&za, buf);
            continue;
        }
        if (type == AFF_TYPE_NONE) {
            if (!strcmp(ext, ".c") || !strcmp(f->name, "-"))
                type = AFF_TYPE_C;
            else if (!strcmp(ext, ".S"))
                type = AFF_TYPE_ASMPP;
            else if (!strcmp(ext, ".s"))
                type = AFF_TYPE_ASM;
        }
        if (type == AFF_TYPE_C) {
            if (!(obj = zx_temp(&temps))) {
                ret = 1;
                break;
            }
            ret = zx_compile(targs->n, targs->v, f->name, AFF_TYPE_C, obj, 0);
            zx_add(&za, obj);
        } else if (type == AFF_TYPE_ASM || type == AFF_TYPE_ASMPP) {
            ZxArgs as = { 0 };
            const char *src = f->name;
            if (type == AFF_TYPE_ASMPP) {
                char *pp = zx_temp(&temps);
                if (!pp || zx_compile(targs->n, targs->v, f->name, AFF_TYPE_ASMPP, pp, 1)) {
                    ret = 1;
                    break;
                }
                src = pp;
            }
            if (!(obj = zx_temp(&temps))) {
                ret = 1;
                break;
            }
            zx_add(&as, zx_tool("TCC_ZAS", CONFIG_ZENIX_ZAS));
            zx_add(&as, "-o");
            zx_add(&as, obj);
            zx_add(&as, src);
            if (zx_run(as.v, s->verbose) != 0)
                ret = 1;
            zx_free(&as);
            zx_add(&za, obj);
        } else {
            zx_add(&za, f->name);       /* an object or an archive */
        }
    }
    if (!ret) {
        if (!s->nostdlib) {
            zx_add(&za, "-lc");
            zx_add(&za, "-lgcc");
        }
        if (zx_run(za.v, s->verbose) != 0)
            ret = 1;
    }
    for (i = 0; i < temps.n; i++)
        unlink(temps.v[i]);
    zx_free(&temps);
    zx_free(&za);
    return ret;
}

/* tcc.c's main calls this first: a command line that builds a program
   is done here (the exit status in *ret, and 1 returned) */
static int zenix_driver(int argc, char **argv, int *ret)
{
    ZxArgs targs = { 0 }, wl = { 0 };
    TCCState *s = NULL;
    int done = 0;

    if (zx_wants_link(argc, argv, &targs, &wl, &s)) {
        *ret = zx_main(s, &targs, &wl);
        tcc_delete(s);
        done = 1;
    }
    zx_free(&targs);
    zx_free(&wl);
    return done;
}
