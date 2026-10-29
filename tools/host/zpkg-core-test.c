/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * zpkg's core on the PC (DESIGN D22 8.1; tools/host/zpkg-test.sh): user/zpkg/ and net/http/ with the sanitizers.
 *   zpkg-core-test units                       the built-in checks (list, latest, trust, resolver, HTTP), PASS/FAIL
 *   zpkg-core-test index FILE                  parse a list: "ok CHANNEL SERIAL NPKG" or "refused: WHY"
 *   zpkg-core-test latest FILE                 "ok SERIAL" or "refused"
 *   zpkg-core-test slot IMG DIR NAME DOSTYPE [noflush] [blocksize reserved]
 *                                              the slot walk: "ok SIZE NBLK HEADER B0 B1 ..." or "refused: WHY"
 *   zpkg-core-test slotwrite IMG DIR NAME DOSTYPE OFF FILE    the walk, then FILE written at OFF (in place)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../user/zpkg/zpkg.h"
#include "../../net/http/http.h"

static int fails, passes;
#define CHECK(c, ...) do { if (c) passes++; else { fails++; printf("FAIL zpkg-core-test: "); printf(__VA_ARGS__); \
                           printf(" (line %d)\n", __LINE__); } } while (0)

static char *readfile(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    char *b;
    long k;
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    k = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc((size_t)k + 1);
    if (!b || fread(b, 1, (size_t)k, f) != (size_t)k) {
        fclose(f);
        free(b);
        return 0;
    }
    fclose(f);
    *n = (size_t)k;
    return b;
}

/* ---- lists ---- */
#define SHA "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define HEAD "zpkgindex 1\nchannel testing\nserial 2026110301\ndate 20261103\nexpires 20270102\n" \
             "system 2026110301 0.3b7 zenix-2026110301.zup 412160 " SHA " min 0\n"

static int parse(const char *text, struct zp_index *ix, const char **why)
{
    static char buf[ZP_INDEX_MAX + 16];
    size_t n = strlen(text);
    memcpy(buf, text, n + 1);
    return zp_index_parse(ix, buf, n, why);
}

static void t_index(void)
{
    struct zp_index ix;
    const char *why;
    const struct zp_pkg *p;
    int rc;
    rc = parse(HEAD "pkg awk\nversion 2026103001 0.3b6\nfile awk-2026103001.zup 31744 " SHA "\nsize 86016\n"
               "depends base 2026102601\ndesc pattern scanning and text processing (awk)\nnewword whatever it is\n"
               "pkg base\nversion 2026110301 0.3b7\ngroup base\nsize 300000\n"
               "pkg telnet\nversion 2026110301 0.3b7\nfile telnet-2026110301.zup 20000 " SHA "\nsize 40000\n"
               "depends base 2026110301\ndepends awk 2026103001\ntakes utils\n", &ix, &why);
    CHECK(rc == 0, "the note's example list parses (%s)", rc ? why : "");
    if (rc)
        return;
    CHECK(!strcmp(ix.channel, "testing") && ix.serial == 2026110301u && ix.date == 20261103 && ix.expires == 20270102,
          "the header");
    CHECK(ix.sys.vnum == 2026110301u && ix.sys.fsize == 412160 && !strcmp(ix.sys.file, "zenix-2026110301.zup") &&
          ix.sys.min == 0 && !strcmp(ix.sys.vstr, "0.3b7"), "the system line");
    CHECK(ix.npkg == 3 && !strcmp(ix.pkg[0].name, "awk") && !strcmp(ix.pkg[2].name, "telnet"), "three packages, sorted");
    p = zp_index_find(&ix, "awk");
    CHECK(p && p->vnum == 2026103001u && p->size == 86016 && p->fsize == 31744 && p->ndep == 1 &&
          !strcmp(zp_pkg_dep(&ix, p, 0)->name, "base") && zp_pkg_dep(&ix, p, 0)->vnum == 2026102601u &&
          p->desc && !strcmp(p->desc, "pattern scanning and text processing (awk)") && !p->base, "awk's block");
    p = zp_index_find(&ix, "telnet");
    CHECK(p && p->ndep == 2 && p->ntakes == 1 && !strcmp(ix.takes[p->takes0], "utils") &&
          !strcmp(zp_pkg_dep(&ix, p, 1)->name, "awk"), "telnet's dependencies and takes");
    p = zp_index_find(&ix, "base");
    CHECK(p && p->base && !p->file, "a base-group block without a file");
    CHECK(!zp_index_find(&ix, "zz") && !zp_index_find(&ix, "a"), "find: missing names");
    {
        unsigned char d[32];
        CHECK(zp_hex32(d, SHA) == 0 && d[0] == 0x01 && d[31] == 0xef, "hex");
    }
    zp_index_free(&ix);

    /* refusals: each one alone */
    static const struct { const char *text, *what; } bad[] = {
        { "zpkgindex 2\n", "another format" },
        { "zpkg index\n", "not a list" },
        { "zpkgindex 1\nchannel testing\nserial 1\ndate 20261103\nexpires 20270102\n", "no system line" },
        { HEAD "channel stable\n", "channel twice" },
        { HEAD "serial 2026110302\n", "serial twice" },
        { "zpkgindex 1\nchannel testing\nserial 2026110301\ndate 20261303\nexpires 20270102\n"
          "system 2026110301 0.3b7 zenix-2026110301.zup 412160 " SHA " min 0\n", "month 13" },
        { "zpkgindex 1\nchannel testing\nserial 2026110301\ndate 20261103\nexpires 20261102\n"
          "system 2026110301 0.3b7 zenix-2026110301.zup 412160 " SHA " min 0\n", "expires before its date" },
        { "zpkgindex 1\nchannel testing\nserial 2026110301\ndate 20261103\nexpires 20270102\n"
          "system 2026110301 0.3b7 zenix-2026110302.zup 412160 " SHA " min 0\n", "system file of another version" },
        { "zpkgindex 1\nchannel testing\nserial 99999999999\ndate 20261103\nexpires 20270102\n"
          "system 2026110301 0.3b7 zenix-2026110301.zup 412160 " SHA " min 0\n", "a serial over 32 bits" },
        { HEAD "pkg Awk\nversion 1 a\nfile Awk-1.zup 100 " SHA "\nsize 1\n", "an upper-case name" },
        { HEAD "pkg ../x\nversion 1 a\nfile ../x-1.zup 100 " SHA "\nsize 1\n", "a path as a name" },
        { HEAD "pkg awk\nversion 1 a\nfile ../awk-1.zup 100 " SHA "\nsize 1\n", "a path as a file" },
        { HEAD "pkg awk\nversion 1 a\nfile awk-2.zup 100 " SHA "\nsize 1\n", "a file of another version" },
        { HEAD "pkg awk\nversion 1 a\nfile awk-01.zup 100 " SHA "\nsize 1\n", "a file with a leading zero" },
        { HEAD "pkg awk\nversion 1 a\nsize 1\n", "no file line" },
        { HEAD "pkg awk\nversion 1 a\nfile awk-1.zup 100 " SHA "\n", "no size line" },
        { HEAD "pkg awk\nfile awk-1.zup 100 " SHA "\nsize 1\n", "no version line" },
        { HEAD "pkg awk\nversion 1 a\nversion 2 b\nfile awk-1.zup 100 " SHA "\nsize 1\n", "version twice" },
        { HEAD "pkg awk\nversion 1 a\nfile awk-1.zup 100 " SHA "\nsize 1\ngroup utils\n", "a group other than base" },
        { HEAD "pkg awk\nversion 1 a\nfile awk-1.zup 99999999 " SHA "\nsize 1\n", "a file over 16 MB" },
        { HEAD "pkg awk\nversion 1 a\nfile awk-1.zup 50 " SHA "\nsize 1\n", "a file smaller than a header" },
        { HEAD "pkg awk\nversion 1 a\nfile awk-1.zup 100 " "0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef"
          "\nsize 1\n", "an upper-case SHA-256" },
        { HEAD "pkg awk\nversion 1 a\nfile awk-1.zup 100 " SHA "\nsize 1\npkg awk\nversion 1 a\nfile awk-1.zup 100 " SHA
          "\nsize 1\n", "a package twice" },
        { HEAD "pkg awk\nversion 1 a\nfile awk-1.zup 100 " SHA "\nsize 1\ndesc \n", "an empty desc" },
        { HEAD "pkg awk\nversion 1 a\nfile awk-1.zup 100 " SHA "\nsize 1\ndesc 0123456789012345678901234567890123456789"
          "012345678901234567890123456789012\n", "a desc of 73 characters" },
        { HEAD "pkg awk\nversion 1 a\nfile awk-1.zup 100 " SHA "\nsize  1\n", "a double space" },
        { HEAD "pkg awk\nversion 1 a\nfile awk-1.zup 100 " SHA "\nsize 1 \n", "a trailing space" },
        { HEAD "pkg awk\nversion 1 a\nfile awk-1.zup 100 " SHA "\nsize 1\r\n", "a CR" },
        { HEAD "pkg awk\nversion 1 0123456789abcdef\nfile awk-1.zup 100 " SHA "\nsize 1\n", "a 16-character vstr" },
        { HEAD "pkg awk\nversion 1 a\nfile awk-1.zup 100 " SHA "\nsize 1\ndepends a 1\ndepends b 1\ndepends c 1\n"
          "depends d 1\ndepends e 1\ndepends f 1\ndepends g 1\ndepends h 1\ndepends i 1\n", "nine dependencies" },
        { HEAD "pkg awk\nversion 1 a\nfile awk-1.zup 100 " SHA "\nsize 1\ndepends base\n", "depends without a version" },
        { HEAD "pkg awk\nversion 1 a\nfile awk-1.zup 100 " SHA "\nsize -1\n", "a negative size" },
        { HEAD "pkg awk\nversion 1 a\nfile awk-1.zup 100 " SHA "\nsize 1\ndesc", "desc alone" },
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        rc = parse(bad[i].text, &ix, &why);
        CHECK(rc < 0, "refused: %s", bad[i].what);
        if (!rc)
            zp_index_free(&ix);
    }
    {
        static char big[ZP_INDEX_MAX + 2];
        memset(big, '#', sizeof big);
        memcpy(big, "zpkgindex 1\n", 12);
        CHECK(zp_index_parse(&ix, big, ZP_INDEX_MAX + 1, &why) == ZP_E2BIG, "a list over 256 KB");
        char nul[] = "zpkgindex 1\nchannel te\0st\n";
        CHECK(zp_index_parse(&ix, nul, sizeof nul - 1, &why) < 0, "a NUL byte");
    }
    /* growth: unknown header words and unknown block words are ignored */
    rc = parse(HEAD "mirror-hint x y z a b c d e f g h i j\npkg awk\nversion 1 a\nfile awk-1.zup 100 " SHA "\nsize 1\n"
               "signedby someone\n", &ix, &why);
    CHECK(rc == 0, "unknown words are ignored, even long lines (%s)", rc ? why : "");
    if (!rc)
        zp_index_free(&ix);
}

static void t_latest(void)
{
    zp_u32 s = 0;
    CHECK(zp_latest_parse("zpkglatest 1 2026110301\n", 24, &s) == 0 && s == 2026110301u, "latest: the note's 24 bytes");
    CHECK(zp_latest_parse("zpkglatest 1 2026110301", 23, &s) == 0, "latest without a newline");
    CHECK(zp_latest_parse("zpkglatest 2 2026110301\n", 24, &s) < 0, "latest: another format");
    CHECK(zp_latest_parse("zpkglatest 1 2026110301 x\n", 26, &s) < 0, "latest: an extra word");
    CHECK(zp_latest_parse("zpkglatest 1 \n", 14, &s) < 0, "latest: no serial");
    CHECK(zp_latest_parse("zpkglatest 1 99999999999\n", 25, &s) < 0, "latest: too big");
    CHECK(zp_latest_parse("zpkglatest 1 1\n\n", 16, &s) < 0, "latest: two lines");
    {
        char b[80];
        memset(b, '0', sizeof b);
        memcpy(b, "zpkglatest 1 ", 13);
        CHECK(zp_latest_parse(b, 65, &s) == ZP_E2BIG, "latest over 64 bytes");
    }
    CHECK(zp_latest_decide(5, 5) == ZP_LATEST_SAME && zp_latest_decide(6, 5) == ZP_LATEST_NEWER &&
          zp_latest_decide(4, 5) == ZP_LATEST_BEHIND && zp_latest_decide(1, 0) == ZP_LATEST_NEWER, "latest decisions");
}

static void t_trust(void)
{
    struct zp_index ix;
    const char *why;
    unsigned warn;
    struct zp_trust t = { "testing", ZP_KEY_RELEASE, 0, 2026102700u, 20261104, 20261027, 0, 0 };
    if (parse(HEAD "pkg awk\nversion 2026103001 0.3b6\nfile awk-2026103001.zup 31744 " SHA "\nsize 1\n", &ix, &why))
        { CHECK(0, "trust: the list"); return; }
    CHECK(zp_days(19700101) == 0 && zp_days(20000301) == 11017 && zp_days(20240229) > 0 && zp_days(20230229) < 0 &&
          zp_days(19691231) < 0, "dates");
    CHECK(zp_index_check(&ix, &t, &warn, &why) == 0 && warn == 0, "a fresh machine takes a good list");
    t.stored = 2026110300u;
    CHECK(zp_index_check(&ix, &t, &warn, &why) == 0, "a newer serial than the stored one");
    t.stored = 2026110301u;
    CHECK(zp_index_check(&ix, &t, &warn, &why) == ZP_EREPLAY, "an equal serial (fetched again) is refused");
    t.stored = 2026110302u;
    CHECK(zp_index_check(&ix, &t, &warn, &why) == ZP_EREPLAY, "an older serial (a replay) is refused");
    t.stored = 0;
    t.floor = 2026110400u;
    CHECK(zp_index_check(&ix, &t, &warn, &why) == ZP_EREPLAY, "a list older than zpkg's own build date");
    t.floor = 2026102700u;
    t.channel = "stable";
    CHECK(zp_index_check(&ix, &t, &warn, &why) == ZP_ECHANNEL, "another channel");
    t.channel = "testing";
    t.keyclass = ZP_KEY_TEST;
    CHECK(zp_index_check(&ix, &t, &warn, &why) == ZP_ECLASS, "a release list signed by the test key");
    t.keyclass = ZP_KEY_RELEASE;
    t.clock = 20270103;
    CHECK(zp_index_check(&ix, &t, &warn, &why) == ZP_EEXPIRED && zp_index_usable(&ix, &t, &warn, &why) == ZP_EEXPIRED,
          "expired, also at every use");
    t.clock = 20270102;
    CHECK(zp_index_usable(&ix, &t, &warn, &why) == 0, "valid on its expiry day");
    t.clock = 20990101;
    CHECK(zp_index_usable(&ix, &t, &warn, &why) == ZP_EEXPIRED, "the clock ahead: expired");
    t.flags = ZP_T_IGNORE_EXPIRY;
    CHECK(zp_index_usable(&ix, &t, &warn, &why) == 0 && (warn & ZP_W_EXPIRY_IGNORED), "--ignore-expiry, said");
    t.flags = 0;
    t.clock = 20261101;
    CHECK(zp_index_check(&ix, &t, &warn, &why) == 0 && (warn & ZP_W_CLOCK_BEHIND), "the clock behind: said");
    t.clock = 20261102;
    CHECK(zp_index_check(&ix, &t, &warn, &why) == 0 && !(warn & ZP_W_CLOCK_BEHIND), "a day behind: not said");
    t.clock = 0;
    t.clockfloor = 20270201;
    CHECK(zp_today(&t) == 20270201 && zp_index_usable(&ix, &t, &warn, &why) == ZP_EEXPIRED,
          "no clock: the floor is today; a floor past expiry refuses");
    t.clockfloor = 20261027;
    t.clock = 20261020;
    CHECK(zp_today(&t) == 20261027, "a clock behind the floor: the floor");
    /* D22 amendment 1 (audit 2026-10-28 O1): a list without a snapshot line is never a snapshot */
    t.clock = 20990101;
    t.channel = 0;
    t.snapshot = "2026.11";
    t.stored = 0;
    CHECK(zp_index_check(&ix, &t, &warn, &why) == ZP_ESNAPSHOT && zp_index_usable(&ix, &t, &warn, &why) == ZP_ESNAPSHOT,
          "a channel's list (an old one swapped in) is refused for --snapshot");
    zp_index_free(&ix);
    if (parse("zpkgindex 1\nchannel stable\nsnapshot 2026.11\nserial 2026110301\ndate 20261103\nexpires 20270102\n"
              "system 2026110301 0.3b7 zenix-2026110301.zup 412160 " SHA " min 0\n", &ix, &why))
        { CHECK(0, "trust: the snapshot list"); return; }
    t.stored = 2026120000u;
    CHECK(zp_index_check(&ix, &t, &warn, &why) == ZP_EREPLAY, "a snapshot whose serial is not the stored one's");
    t.stored = 2026110301u;
    CHECK(zp_index_check(&ix, &t, &warn, &why) == 0 && (warn & ZP_W_UNCHECKED),
          "the stored snapshot again: taken, its age and expiry not checked, said");
    t.stored = 0;
    t.floor = 2026120000u;
    CHECK(zp_index_check(&ix, &t, &warn, &why) == 0 && (warn & ZP_W_UNCHECKED),
          "a fresh machine: a snapshot older than zpkg's floor is taken (old on purpose)");
    t.floor = 2026102700u;
    t.snapshot = "2026.10";
    CHECK(zp_index_check(&ix, &t, &warn, &why) == ZP_ESNAPSHOT && zp_index_usable(&ix, &t, &warn, &why) == ZP_ESNAPSHOT,
          "another snapshot's list is refused (the name is signed)");
    t.snapshot = 0;
    t.channel = "stable";
    t.clock = 20261104;
    CHECK(zp_index_check(&ix, &t, &warn, &why) == ZP_ESNAPSHOT && zp_index_usable(&ix, &t, &warn, &why) == ZP_ESNAPSHOT,
          "a snapshot's list is refused as the channel's list");
    t.channel = 0;
    t.snapshot = "2026.11";
    t.keyclass = ZP_KEY_TEST;
    CHECK(zp_index_check(&ix, &t, &warn, &why) == ZP_ECLASS, "a snapshot: the class is still checked");
    zp_index_free(&ix);
    {
        static const char *badsnap[] = { "snapshot\n", "snapshot .x\n", "snapshot a b\n", "snapshot a/b\n",
            "snapshot aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n", "snapshot a\nsnapshot a\n" };
        char buf[512];
        for (size_t k = 0; k < sizeof badsnap / sizeof *badsnap; k++) {
            snprintf(buf, sizeof buf, "zpkgindex 1\nchannel stable\n%sserial 2026110301\ndate 20261103\n"
                     "expires 20270102\nsystem 2026110301 0.3b7 zenix-2026110301.zup 412160 " SHA " min 0\n", badsnap[k]);
            CHECK(parse(buf, &ix, &why) != 0, "a bad snapshot line is refused");
        }
        snprintf(buf, sizeof buf, "zpkgindex 1\nchannel stable\nsnapshot A0._-zzzzzzzzzzzzzzzzzzzzzzzzzzz\n"
                 "serial 2026110301\ndate 20261103\nexpires 20270102\n"
                 "system 2026110301 0.3b7 zenix-2026110301.zup 412160 " SHA " min 0\n");
        CHECK(!zp_snapshot_name_ok("") && !zp_snapshot_name_ok("../a") && !zp_snapshot_name_ok("a/b") &&
              !zp_snapshot_name_ok("-a") && !zp_snapshot_name_ok("2026.part") && zp_snapshot_name_ok("2026.11") &&
              zp_snapshot_name_ok("Q") && zp_snapshot_name_ok("a.partx") && zp_snapshot_name_ok("part"),
              "snapshot names: no path, no leading mark, no .part");
        CHECK(parse(buf, &ix, &why) == 0 && !strcmp(ix.snapshot, "A0._-zzzzzzzzzzzzzzzzzzzzzzzzzzz"),
              "a 32-character snapshot name");
        zp_index_free(&ix);
    }
    /* a serial above its date, a serial below a version it names */
    t = (struct zp_trust){ "testing", ZP_KEY_RELEASE, 0, 0, 20261104, 0, 0, 0 };
    if (!parse("zpkgindex 1\nchannel testing\nserial 2026110401\ndate 20261103\nexpires 20270102\n"
               "system 2026110301 0.3b7 zenix-2026110301.zup 412160 " SHA " min 0\n", &ix, &why)) {
        CHECK(zp_index_check(&ix, &t, &warn, &why) == ZP_ESERIAL, "a serial above its date");
        zp_index_free(&ix);
    }
    if (!parse("zpkgindex 1\nchannel testing\nserial 2026110300\ndate 20261103\nexpires 20270102\n"
               "system 2026110301 0.3b7 zenix-2026110301.zup 412160 " SHA " min 0\n", &ix, &why)) {
        CHECK(zp_index_check(&ix, &t, &warn, &why) == ZP_ESERIAL, "a serial below the system's version");
        zp_index_free(&ix);
    }
    if (!parse("zpkgindex 1\nchannel nightly\nserial 2026110301\ndate 20261103\nexpires 20270102\n"
               "system 2026110301 0.3b7 zenix-2026110301.zup 412160 " SHA " min 0\n", &ix, &why)) {
        t.channel = "nightly";
        CHECK(zp_index_check(&ix, &t, &warn, &why) == ZP_ECHANNEL, "an unknown channel");
        zp_index_free(&ix);
    }
    /* a downloaded bundle against the list */
    if (!parse(HEAD "pkg awk\nversion 2026103001 0.3b6\nfile awk-2026103001.zup 31744 " SHA "\nsize 1\n", &ix, &why)) {
        const struct zp_pkg *p = zp_index_find(&ix, "awk");
        CHECK(zp_bundle_fits(&ix, p, "awk", 2026103001u, "testing", ZP_KEY_RELEASE, &why) == 0, "bundle fits");
        CHECK(zp_bundle_fits(&ix, p, "awk", 2026103001u, "stable", ZP_KEY_RELEASE, &why) == 0,
              "a promoted file (header testing, list stable) fits by class");
        CHECK(zp_bundle_fits(&ix, p, "awk", 2026103001u, "test", ZP_KEY_RELEASE, &why) == ZP_ECLASS, "a test file");
        CHECK(zp_bundle_fits(&ix, p, "gawk", 2026103001u, "testing", ZP_KEY_RELEASE, &why) < 0, "another name");
        CHECK(zp_bundle_fits(&ix, p, "awk", 2026103002u, "testing", ZP_KEY_RELEASE, &why) < 0, "another version");
        CHECK(zp_bundle_fits(&ix, 0, 0, 2026110301u, "testing", ZP_KEY_RELEASE, &why) == 0 &&
              zp_bundle_fits(&ix, 0, 0, 2026110300u, "testing", ZP_KEY_RELEASE, &why) < 0, "the system bundle");
        zp_index_free(&ix);
    }
}

/* ---- the resolver ---- */
static struct zp_index rix;
static struct zp_dep idep[16][4];
static struct zp_inst inst[16];
static int ninst;

static void install(const char *name, zp_u32 v, int base, int asdep, const char *deps)
{
    struct zp_inst *in = &inst[ninst];
    static char store[16][128];
    int nd = 0;
    char *s = deps ? strcpy(store[ninst], deps) : 0, *tok;
    memset(in, 0, sizeof *in);
    in->name = name;
    in->vnum = v;
    in->base = (unsigned char)base;
    in->asdep = (unsigned char)asdep;
    for (tok = s ? strtok(s, " ") : 0; tok; tok = strtok(0, " ")) {
        char *c = strchr(tok, ':');
        *c = 0;
        idep[ninst][nd].name = tok;
        idep[ninst][nd++].vnum = (zp_u32)strtoul(c + 1, 0, 10);
    }
    in->dep = idep[ninst];
    in->ndep = nd;
    ninst++;
}

static int plan(const char *names, unsigned flags, int remove, struct zp_plan *pl, const char *const *hold, int nhold,
                zp_u32 effbase, const char **why)
{
    static char store[128];
    const char *v[8];
    int n = 0;
    char *s = strcpy(store, names), *tok;
    struct zp_world w = { &rix, inst, ninst, effbase, hold, nhold };
    for (tok = strtok(s, " "); tok; tok = strtok(0, " "))
        v[n++] = tok;
    return remove ? zp_plan_remove(&w, v, n, flags, pl, why) : zp_plan_sync(&w, v, n, flags, pl, why);
}

/* the plan as "kind:name ..." */
static const char *show(const struct zp_plan *pl)
{
    static char b[512];
    static const char k[] = "?IUDWHR";
    b[0] = 0;
    for (int i = 0; i < pl->n; i++) {
        const struct zp_step *s = &pl->step[i];
        sprintf(b + strlen(b), "%s%c:%s%s", i ? " " : "", k[s->kind], s->pkg ? s->pkg->name : s->from->name,
                s->asdep ? "+" : "");
    }
    return b;
}

#define LIST(body) HEAD body
/* audit 2026-10-28 S21: a chain of dependencies deeper than 64 is refused (the resolver recursed once a link, and a
 * 256 KB list holds a chain of 32,768: the 16 KB stack has no guard); 60 deep still plans */
static void deep_chain(void)
{
    static char text[16384];
    struct zp_index save = rix;
    struct zp_plan pl;
    const char *why;
    int rc, i;
    for (int depth = 60; depth <= 70; depth += 10) {
        strcpy(text, HEAD);
        for (i = 0; i < depth; i++) {
            char l[200];
            snprintf(l, sizeof l, "pkg d%d\nversion 5 v\nfile d%d-5.zup 200 " SHA "\nsize 1\n", i, i);
            strcat(text, l);
            if (i + 1 < depth) { snprintf(l, sizeof l, "depends d%d 1\n", i + 1); strcat(text, l); }
        }
        if ((rc = zp_index_parse(&rix, text, strlen(text), &why)) != 0) { CHECK(0, "deep list: %s", why); break; }
        rc = plan("d0", 0, 0, &pl, 0, 0, 2026102600, &why);
        if (depth == 60) CHECK(rc == 0 && pl.n == 60, "-S d0: a chain 60 deep plans (%d)", rc);
        else CHECK(rc == ZP_ELOOP, "-S d0: a chain 70 deep is refused, never recursed through (%d)", rc);
        zp_plan_free(&pl);
        zp_index_free(&rix);
    }
    rix = save;
}

static void t_resolve(void)
{
    static char text[8192];
    const char *why;
    struct zp_plan pl;
    int rc;
    /* the list: system 2026110301; base; libx <- tool <- app; loop a <-> b; c needs a missing package;
     * newbase needs a newer base (waits); e needs libx newer than the list has */
    strcpy(text, LIST("pkg base\nversion 2026110301 v\ngroup base\nsize 1\n"
                      "pkg zup\nversion 2026110301 v\ngroup base\nsize 1\n"
                      "pkg libx\nversion 2026103000 v\nfile libx-2026103000.zup 200 " SHA "\nsize 1\ndepends base 2026102600\n"
                      "pkg tool\nversion 2026103100 v\nfile tool-2026103100.zup 200 " SHA "\nsize 1\ndepends libx 2026103000\n"
                      "pkg app\nversion 2026103100 v\nfile app-2026103100.zup 200 " SHA "\nsize 1\ndepends tool 2026103100\n"
                      "depends base 2026102600\n"
                      "pkg a\nversion 5 v\nfile a-5.zup 200 " SHA "\nsize 1\ndepends b 1\n"
                      "pkg b\nversion 5 v\nfile b-5.zup 200 " SHA "\nsize 1\ndepends a 1\n"
                      "pkg c\nversion 5 v\nfile c-5.zup 200 " SHA "\nsize 1\ndepends nothere 1\n"
                      "pkg newbase\nversion 2026110301 v\nfile newbase-2026110301.zup 200 " SHA "\nsize 1\n"
                      "depends base 2026110301\n"
                      "pkg e\nversion 5 v\nfile e-5.zup 200 " SHA "\nsize 1\ndepends libx 2026120000\n"
                      "pkg self\nversion 5 v\nfile self-5.zup 200 " SHA "\nsize 1\ndepends self 1\n"
                      "pkg old\nversion 2026100100 v\nfile old-2026100100.zup 200 " SHA "\nsize 1\n"
                      "pkg xa\nversion 5 v\nfile xa-5.zup 200 " SHA "\nsize 1\ndepends lb 5\n"
                      "pkg lb\nversion 4 v\nfile lb-4.zup 200 " SHA "\nsize 1\n"));
    if ((rc = zp_index_parse(&rix, text, strlen(text), &why)) != 0) {
        CHECK(0, "resolver list: %s", why);
        return;
    }
    ninst = 0;
    install("base", 2026102600, 1, 0, "zup:2026102600");
    install("zup", 2026102600, 1, 0, 0);
    rc = plan("app", 0, 0, &pl, 0, 0, 2026102600, &why);
    CHECK(rc == 0 && !strcmp(show(&pl), "I:libx+ I:tool+ I:app") && !pl.system, "-S app: dependencies first, as deps (%s)",
          rc ? why : show(&pl));
    zp_plan_free(&pl);
    rc = plan("a", 0, 0, &pl, 0, 0, 2026102600, &why);
    CHECK(rc == ZP_ELOOP, "-S a: a loop is refused (%d)", rc);
    rc = plan("self", 0, 0, &pl, 0, 0, 2026102600, &why);
    CHECK(rc == ZP_ELOOP && !strcmp(pl.bad, "self"), "-S self: depends on itself");
    deep_chain();
    rc = plan("c", 0, 0, &pl, 0, 0, 2026102600, &why);
    CHECK(rc == ZP_ENOENT && !strcmp(pl.bad, "c") && !strcmp(pl.bad2, "nothere"), "-S c: a missing dependency, named");
    rc = plan("e", 0, 0, &pl, 0, 0, 2026102600, &why);
    CHECK(rc == ZP_ENOENT && !strcmp(pl.bad2, "libx"), "-S e: a dependency newer than the list has");
    rc = plan("nope", 0, 0, &pl, 0, 0, 2026102600, &why);
    CHECK(rc == ZP_ENOENT, "-S nope: not in the list");
    rc = plan("zup", 0, 0, &pl, 0, 0, 2026102600, &why);
    CHECK(rc == ZP_EPERM, "-S zup: the base group never installs live");
    rc = plan("newbase", 0, 0, &pl, 0, 0, 2026102600, &why);
    CHECK(rc == 0 && !strcmp(show(&pl), "W:newbase"), "-S newbase: waits for the system update (%s)", show(&pl));
    zp_plan_free(&pl);
    rc = plan("newbase", 0, 0, &pl, 0, 0, 2026110301, &why);
    CHECK(rc == 0 && !strcmp(show(&pl), "W:newbase"), "the effective base counts, not only the kernel");
    zp_plan_free(&pl);
    {
        static const char *hb[] = { "base" };
        rc = plan("newbase", 0, 0, &pl, hb, 1, 2026102600, &why);
        CHECK(rc < 0, "hold base: a package that needs the newer base is refused");
    }
    /* -Syu: installed libx old, tool old; app newer list version */
    ninst = 2;
    install("libx", 2026102900, 0, 1, "base:2026102600");
    install("tool", 2026103000, 0, 0, "libx:2026102900");
    install("old", 2026100500, 0, 0, 0);
    rc = plan("", ZP_SYNC_U, 0, &pl, 0, 0, 2026102600, &why);
    CHECK(rc == 0 && !strcmp(show(&pl), "U:libx U:tool") && pl.system, "-Syu: upgrades and the system (%s)",
          rc ? why : show(&pl));
    zp_plan_free(&pl);
    rc = plan("", ZP_SYNC_UU, 0, &pl, 0, 0, 2026102600, &why);
    CHECK(rc == 0 && !strcmp(show(&pl), "U:libx U:tool D:old"), "-Syuu: also down to the list's version (%s)",
          rc ? why : show(&pl));
    zp_plan_free(&pl);
    {
        static const char *hl[] = { "libx" };
        rc = plan("", ZP_SYNC_U, 0, &pl, hl, 1, 2026102600, &why);
        CHECK(rc == 0 && !strcmp(show(&pl), "H:libx H:tool"), "hold libx: libx and tool (needs the newer libx) held (%s)",
              rc ? why : show(&pl));
        zp_plan_free(&pl);
        static const char *hbase[] = { "base" };
        rc = plan("", ZP_SYNC_U, 0, &pl, hbase, 1, 2026102600, &why);
        CHECK(rc == 0 && !pl.system, "hold base holds the system");
        zp_plan_free(&pl);
    }
    /* a downgrade that an installed package's needs forbid */
    ninst = 2;
    install("libx", 2026110000, 0, 1, 0);
    install("tool", 2026103100, 0, 0, "libx:2026110000");
    rc = plan("", ZP_SYNC_UU, 0, &pl, 0, 0, 2026102600, &why);
    CHECK(rc < 0 && pl.bad && !strcmp(pl.bad, "tool") && !strcmp(pl.bad2, "libx"),
          "-Syuu: a downgrade an installed package's dependency forbids is refused, named (%d %s %s)", rc,
          pl.bad ? pl.bad : "-", pl.bad2 ? pl.bad2 : "-");
    rc = plan("", ZP_SYNC_U, 0, &pl, 0, 0, 2026102600, &why);
    CHECK(rc == 0 && pl.n == 0, "-Syu: newer installed stays");
    zp_plan_free(&pl);
    /* an upgrade planned first, then a downgrade of what it needs (found in review, 2026-10-27) */
    ninst = 2;
    install("xa", 4, 0, 0, "lb:1");
    install("lb", 6, 0, 1, 0);
    rc = plan("", ZP_SYNC_UU, 0, &pl, 0, 0, 2026102600, &why);
    CHECK(rc < 0 && pl.bad && !strcmp(pl.bad, "xa") && !strcmp(pl.bad2, "lb"),
          "-Syuu: an upgrade whose new version needs what a later downgrade takes away is refused (%d)", rc);
    rc = plan("", ZP_SYNC_U, 0, &pl, 0, 0, 2026102600, &why);
    CHECK(rc == 0 && !strcmp(show(&pl), "U:xa"), "-Syu: the same upgrade alone is fine (%s)", rc ? why : show(&pl));
    zp_plan_free(&pl);
    /* -R, -Rs */
    ninst = 2;
    install("libx", 2026103000, 0, 1, 0);
    install("tool", 2026103100, 0, 1, "libx:2026103000");
    install("app", 2026103100, 0, 0, "tool:2026103100 base:2026102600");
    install("other", 1, 0, 0, "libx:1");
    rc = plan("tool", 0, 1, &pl, 0, 0, 2026102600, &why);
    CHECK(rc == ZP_EPERM && !strcmp(pl.bad2, "app"), "-R tool: app needs it, named");
    rc = plan("app", 0, 1, &pl, 0, 0, 2026102600, &why);
    CHECK(rc == 0 && !strcmp(show(&pl), "R:app"), "-R app: only app");
    zp_plan_free(&pl);
    rc = plan("app", ZP_RM_S, 1, &pl, 0, 0, 2026102600, &why);
    CHECK(rc == 0 && !strcmp(show(&pl), "R:app R:tool"), "-Rs app: tool too; libx stays (other needs it) (%s)",
          rc ? why : show(&pl));
    zp_plan_free(&pl);
    rc = plan("app other", ZP_RM_S, 1, &pl, 0, 0, 2026102600, &why);
    CHECK(rc == 0 && !strcmp(show(&pl), "R:app R:other R:tool R:libx"), "-Rs app other: the whole chain, dependents first (%s)",
          rc ? why : show(&pl));
    zp_plan_free(&pl);
    rc = plan("zup", 0, 1, &pl, 0, 0, 2026102600, &why);
    CHECK(rc == ZP_EPERM, "-R zup: never the base group");
    ninst = 0;
    install("base", 2026102600, 1, 0, "zup:2026102600 ca-certificates:1");
    install("zup", 2026102600, 1, 0, 0);
    install("ca-certificates", 1, 0, 0, 0);
    rc = plan("ca-certificates", 0, 1, &pl, 0, 0, 2026102600, &why);
    CHECK(rc == ZP_EPERM, "-R of what a base-group package depends on is refused");
    rc = plan("nothing", 0, 1, &pl, 0, 0, 2026102600, &why);
    CHECK(rc == ZP_ENOENT, "-R of a package not installed");
    zp_index_free(&rix);
}

/* ---- HTTP ---- */
struct got {
    int heads, ends, status, err;
    char body[70000];
    size_t blen;
    size_t rest;                    /* bytes left after END */
};

/* feed a whole response in pieces of at most step bytes (0: random 1-17) */
static void feed_all(const char *resp, size_t n, unsigned long limit, size_t step, struct got *g)
{
    struct http_resp *r = malloc(sizeof *r);
    size_t off = 0;
    memset(g, 0, sizeof *g);
    http_resp_init(r, limit, 0);
    while (off < n) {
        size_t k = step ? step : (size_t)(rand() % 17 + 1), used;
        const unsigned char *b;
        size_t bl;
        if (k > n - off)
            k = n - off;
        size_t end = off + k;
        while (off < end) {
            int rc = http_resp_feed(r, (const unsigned char *)resp + off, end - off, &used, &b, &bl);
            off += used;
            if (rc < 0) {
                g->err = rc;
                free(r);
                return;
            }
            if (rc == HTTP_HEAD) {
                g->heads++;
                g->status = r->status;
            }
            if (rc == HTTP_DATA) {
                memcpy(g->body + g->blen, b, bl);
                g->blen += bl;
            }
            if (rc == HTTP_END) {
                g->ends++;
                g->rest = n - off;
                free(r);
                return;
            }
            if (rc == HTTP_NEED)
                break;
        }
    }
    {
        size_t used;
        const unsigned char *b;
        size_t bl;
        int rc = http_resp_feed(r, (const unsigned char *)"", 0, &used, &b, &bl);
        if (rc == HTTP_END)
            g->ends++;
        else {
            rc = http_resp_eof(r);
            if (rc == HTTP_END)
                g->ends++;
            else
                g->err = rc;
        }
    }
    free(r);
}

static void t_http(void)
{
    struct http_url u;
    char req[2048];
    int hops = 0, n;
    struct got *g = malloc(sizeof *g);
    struct http_resp r;

    CHECK(http_url_parse(&u, "http://nas.local:8088/zenix/testing/latest") == 0 && !u.tls && u.port == 8088 &&
          !strcmp(u.host, "nas.local") && !strcmp(u.path, "/zenix/testing/latest"), "url");
    CHECK(http_url_parse(&u, "https://owner.github.io") == 0 && u.tls && u.port == 443 && !strcmp(u.path, "/"),
          "url: https, no path");
    CHECK(http_url_parse(&u, "ftp://x/") < 0 && http_url_parse(&u, "http://a b/") < 0 && http_url_parse(&u, "http:///x") < 0 &&
          http_url_parse(&u, "http://x:0/") < 0 && http_url_parse(&u, "http://x:65536/") < 0 &&
          http_url_parse(&u, "http://x/a b") < 0 && http_url_parse(&u, "http://x/a\r\nHost: evil") < 0 &&
          http_url_parse(&u, "http://x:8a/") < 0, "url refusals (scheme, spaces, ports, CRLF in a path)");
    http_url_parse(&u, "http://nas:8088/zenix/pool/awk-1.zup");
    n = http_request(req, sizeof req, &u, 0, 0);
    CHECK(n > 0 && !strcmp(req, "GET /zenix/pool/awk-1.zup HTTP/1.1\r\nHost: nas:8088\r\n\r\n"), "request: %s", req);
    n = http_request(req, sizeof req, &u, 12345, 1);
    CHECK(n > 0 && !strcmp(req, "GET /zenix/pool/awk-1.zup HTTP/1.1\r\nHost: nas:8088\r\nRange: bytes=12345-\r\n"
                           "Connection: close\r\n\r\n"), "request with Range and close: %s", req);
    CHECK(http_request(req, 20, &u, 0, 0) == HTTP_EINVAL, "a request that does not fit");
    http_url_parse(&u, "https://a.github.io/x");
    hops = 0;
    CHECK(http_redirect(&u, "http://a.github.io/x", &hops) == HTTP_EPERM, "never https to http");
    hops = 0;
    CHECK(http_redirect(&u, "/y", &hops) == 0 && u.tls && !strcmp(u.path, "/y"), "a redirect to a path");
    CHECK(http_redirect(&u, "https://b.github.io/z", &hops) == 0 && !strcmp(u.host, "b.github.io"), "a second one");
    CHECK(http_redirect(&u, "/w", &hops) == 0, "a third one");
    CHECK(http_redirect(&u, "/v", &hops) == HTTP_ELOOP, "a fourth one is refused");
    hops = 0;
    CHECK(http_redirect(&u, "relative", &hops) == HTTP_EPERM && http_redirect(&u, "//evil/x", &hops) == HTTP_EPERM,
          "relative and scheme-relative locations are not followed");

    static const char ok[] = "HTTP/1.1 200 OK\r\nContent-Length: 11\r\nContent-Type: text/plain\r\n\r\nhello worldHTTP/1.1";
    for (size_t step = 0; step <= 20; step++) {
        feed_all(ok, sizeof ok - 1, 100, step, g);
        CHECK(!g->err && g->heads == 1 && g->ends == 1 && g->status == 200 && g->blen == 11 &&
              !memcmp(g->body, "hello world", 11) && g->rest == 8, "Content-Length, in pieces of %zu (err %d)", step, g->err);
    }
    static const char ch[] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5;ext=1\r\nhello\r\n6\r\n world\r\n"
                             "0\r\nX-Trailer: 1\r\n\r\n";
    for (size_t step = 0; step <= 9; step++) {
        feed_all(ch, sizeof ch - 1, 100, step, g);
        CHECK(!g->err && g->ends == 1 && g->blen == 11 && !memcmp(g->body, "hello world", 11) && g->rest == 0,
              "chunked with an extension and a trailer, pieces of %zu (err %d)", step, g->err);
    }
    static const char lf[] = "HTTP/1.0 200 OK\nContent-Length: 2\n\nhi";
    feed_all(lf, sizeof lf - 1, 100, 0, g);
    CHECK(!g->err && g->blen == 2, "bare LF line ends, HTTP/1.0");
    static const char cl[] = "HTTP/1.1 200 OK\r\n\r\nuntil the end";
    feed_all(cl, sizeof cl - 1, 100, 3, g);
    CHECK(!g->err && g->ends == 1 && g->blen == 13, "a body until the connection closes");
    feed_all(cl, sizeof cl - 1, 5, 3, g);
    CHECK(g->err == HTTP_E2BIG, "... but not past the limit");
    static const char big[] = "HTTP/1.1 200 OK\r\nContent-Length: 101\r\n\r\n";
    feed_all(big, sizeof big - 1, 100, 0, g);
    CHECK(g->err == HTTP_E2BIG, "a Content-Length over the limit");
    static const char huge[] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nFFFFFFFFF\r\n";
    feed_all(huge, sizeof huge - 1, 100, 0, g);
    CHECK(g->err == HTTP_E2BIG, "a chunk size that overflows");
    static const char huge2[] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n65\r\n";
    feed_all(huge2, sizeof huge2 - 1, 100, 0, g);
    CHECK(g->err == HTTP_E2BIG, "a chunk past the limit");
    static const char chsum[] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n32\r\n"
                                "01234567890123456789012345678901234567890123456789\r\n33\r\n";
    feed_all(chsum, sizeof chsum - 1, 100, 0, g);
    CHECK(g->err == HTTP_E2BIG, "chunks that add up past the limit");
    /* the framing counted (audit 2026-10-29 B2-9, network audit O5): a 4 KB extension per data byte is refused */
    {
        static char ext[16384];
        size_t o = (size_t)snprintf(ext, sizeof ext, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1;");
        memset(ext + o, 'x', 300); o += 300;
        memcpy(ext + o, "\r\nX\r\n0\r\n\r\n", 10); o += 10;
        feed_all(ext, o, 100, 0, g);
        CHECK(g->err == HTTP_E2BIG, "a chunk-size line of 300 bytes refused (err %d)", g->err);
        o = (size_t)snprintf(ext, sizeof ext, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
        for (int k = 0; k < 40; k++) {
            memcpy(ext + o, "1;", 2); o += 2;
            memset(ext + o, 'x', 200); o += 200;
            memcpy(ext + o, "\r\nX\r\n", 5); o += 5;
        }
        memcpy(ext + o, "0\r\n\r\n", 5); o += 5;
        feed_all(ext, o, 100, 0, g);
        CHECK(g->err == HTTP_E2BIG, "200 bytes of extension per data byte: refused after 4 KB (err %d)", g->err);
        o = (size_t)snprintf(ext, sizeof ext, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
        for (int k = 0; k < 90; k++) { memcpy(ext + o, "1\r\nY\r\n", 6); o += 6; }
        memcpy(ext + o, "0\r\n\r\n", 5); o += 5;
        feed_all(ext, o, 100, 1, g);
        CHECK(!g->err && g->ends == 1 && g->blen == 90, "90 one-byte chunks are fine (err %d, %zu bytes)", g->err, g->blen);
    }
    static const char short_[] = "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc";
    feed_all(short_, sizeof short_ - 1, 100, 0, g);
    CHECK(g->err == HTTP_EIO, "a body shorter than announced (the connection closed)");
    static const char both[] = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\n";
    feed_all(both, sizeof both - 1, 100, 0, g);
    CHECK(g->err == HTTP_EPROTO, "Content-Length and chunked together");
    static const char two[] = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\nContent-Length: 4\r\n\r\n";
    feed_all(two, sizeof two - 1, 100, 0, g);
    CHECK(g->err == HTTP_EPROTO, "two different Content-Lengths");
    static const char gz[] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n";
    feed_all(gz, sizeof gz - 1, 100, 0, g);
    CHECK(g->err == HTTP_ENOTSUP, "a transfer coding other than chunked");
    static const char fold[] = "HTTP/1.1 200 OK\r\nX: a\r\n b\r\n\r\n";
    feed_all(fold, sizeof fold - 1, 100, 0, g);
    CHECK(g->err == HTTP_EPROTO, "obsolete line folding");
    static const char notkhttp[] = "SSH-2.0-OpenSSH\r\n";
    feed_all(notkhttp, sizeof notkhttp - 1, 100, 0, g);
    CHECK(g->err == HTTP_EPROTO, "not HTTP");
    static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 204 No Content\r\n\r\n";
    feed_all(cont, sizeof cont - 1, 100, 0, g);
    CHECK(!g->err && g->status == 204 && g->blen == 0 && g->ends == 1, "an interim 100, then 204 without a body");
    static const char badch[] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabcX\r\n";
    feed_all(badch, sizeof badch - 1, 100, 0, g);
    CHECK(g->err == HTTP_EPROTO, "a chunk without its CRLF");
    {
        static char hb[5000];
        strcpy(hb, "HTTP/1.1 200 OK\r\nX: ");
        memset(hb + strlen(hb), 'a', 4100);
        strcat(hb, "\r\n\r\n");
        feed_all(hb, strlen(hb), 100, 0, g);
        CHECK(g->err == HTTP_E2BIG, "a header over 4 KB");
    }
    {
        static const char junk[] = "XTTP/1.1 200 OK\r\n\r\n";
        http_resp_init(&r, 1000, 0);
        size_t u2 = 99999;
        const unsigned char *b2;
        size_t bl2;
        int rc2 = http_resp_feed(&r, (const unsigned char *)junk, sizeof junk - 1, &u2, &b2, &bl2);
        CHECK(rc2 == HTTP_EPROTO && u2 <= sizeof junk - 1, "an error still says how many bytes were taken (fuzz, 2026-10-27)");
    }
    /* resume */
    static const char r206[] = "HTTP/1.1 206 Partial\r\nContent-Range: bytes 100-199/200\r\nContent-Length: 100\r\n\r\n";
    size_t used;
    const unsigned char *b;
    size_t bl;
    http_resp_init(&r, 1000, 0);
    CHECK(http_resp_feed(&r, (const unsigned char *)r206, sizeof r206 - 1, &used, &b, &bl) == HTTP_HEAD &&
          http_resume(&r, 100, 200) == HTTP_RESUME && http_resume(&r, 99, 200) < 0 && http_resume(&r, 100, 300) < 0,
          "resume: 206 with the right Content-Range, refused when it is not");
    static const char r206b[] = "HTTP/1.1 206 Partial\r\nContent-Range: bytes 100-150/200\r\n\r\n";
    http_resp_init(&r, 1000, 0);
    http_resp_feed(&r, (const unsigned char *)r206b, sizeof r206b - 1, &used, &b, &bl);
    CHECK(http_resume(&r, 100, 200) < 0, "resume: a range that stops short");
    static const char r206c[] = "HTTP/1.1 206 Partial\r\nContent-Range: bytes 100-199/*\r\n\r\n";
    http_resp_init(&r, 1000, 0);
    CHECK(http_resp_feed(&r, (const unsigned char *)r206c, sizeof r206c - 1, &used, &b, &bl) == HTTP_EPROTO,
          "resume: an unknown total");
    static const char r200[] = "HTTP/1.1 200 OK\r\nContent-Length: 200\r\n\r\n";
    http_resp_init(&r, 1000, 0);
    http_resp_feed(&r, (const unsigned char *)r200, sizeof r200 - 1, &used, &b, &bl);
    CHECK(http_resume(&r, 100, 200) == HTTP_RESTART && http_resume(&r, 100, 201) < 0, "resume: 200 restarts");
    static const char r404[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
    http_resp_init(&r, 1000, 0);
    http_resp_feed(&r, (const unsigned char *)r404, sizeof r404 - 1, &used, &b, &bl);
    CHECK(http_resume(&r, 0, 200) < 0 && r.status == 404, "404");
    static const char loc[] = "HTTP/1.1 302 Found\r\nLocation: /other/place\r\nContent-Length: 0\r\n\r\n";
    http_resp_init(&r, 1000, 0);
    http_resp_feed(&r, (const unsigned char *)loc, sizeof loc - 1, &used, &b, &bl);
    CHECK(http_is_redirect(r.status) && !strcmp(r.location, "/other/place"), "a Location");
    static const char ka[] = "HTTP/1.0 200 OK\r\nConnection: keep-alive\r\nContent-Length: 0\r\n\r\n";
    http_resp_init(&r, 1000, 0);
    http_resp_feed(&r, (const unsigned char *)ka, sizeof ka - 1, &used, &b, &bl);
    CHECK(!r.close, "HTTP/1.0 keep-alive");
    static const char cc[] = "HTTP/1.1 200 OK\r\nConnection: Keep-Alive, Close\r\nContent-Length: 0\r\n\r\n";
    http_resp_init(&r, 1000, 0);
    http_resp_feed(&r, (const unsigned char *)cc, sizeof cc - 1, &used, &b, &bl);
    CHECK(r.close, "Connection: close among tokens");
    free(g);
}

/* ---- the slot ---- */
struct img {
    FILE *f;
    zp_u32 n;
};
static int img_read(void *u, zp_u32 blk, unsigned char *buf)
{
    struct img *m = u;
    if (blk >= m->n || fseek(m->f, (long)blk * 512, SEEK_SET) || fread(buf, 1, 512, m->f) != 512)
        return -5;
    return 0;
}
static int img_write(void *u, zp_u32 blk, unsigned char *buf)
{
    struct img *m = u;
    if (blk >= m->n || fseek(m->f, (long)blk * 512, SEEK_SET) || fwrite(buf, 1, 512, m->f) != 512)
        return -5;
    return 0;
}

static int slot_cmd(int argc, char **argv, int write)
{
    struct img m;
    struct zp_ffs f;
    struct zp_slot s;
    const char *why = "";
    int rc;
    long sz;
    m.f = fopen(argv[2], write ? "r+b" : "rb");
    if (!m.f) {
        printf("cannot open %s\n", argv[2]);
        return 2;
    }
    fseek(m.f, 0, SEEK_END);
    sz = ftell(m.f);
    m.n = (zp_u32)(sz / 512);
    memset(&f, 0, sizeof f);
    f.blocksize = 512;
    f.reserved = 2;
    f.dostype = strcmp(argv[5], "DOS3") ? (strcmp(argv[5], "DOS1") ? 0x444F5300u : ZP_DOS1) : ZP_DOS3;
    f.nblocks = m.n;
    f.canflush = 1;
    f.read = img_read;
    f.write = img_write;
    f.u = &m;
    if (!write) {
        for (int i = 6; i < argc; i++) {
            if (!strcmp(argv[i], "noflush"))
                f.canflush = 0;
            else if (i + 1 < argc) {
                f.blocksize = (zp_u32)atoi(argv[i]);
                f.reserved = (zp_u32)atoi(argv[i + 1]);
                i++;
            }
        }
    }
    rc = zp_slot_find(&f, argv[3], argv[4], &s, &why);
    if (rc) {
        printf("refused: %s\n", why);
        fclose(m.f);
        return 1;
    }
    if (write) {
        size_t n;
        char *data = readfile(argv[7], &n);
        if (!data) {
            printf("cannot read %s\n", argv[7]);
            return 2;
        }
        rc = zp_slot_pwrite(&f, &s, (zp_u32)atol(argv[6]), (unsigned char *)data, (zp_u32)n);
        free(data);
        printf(rc ? "write refused %d\n" : "written\n", rc);
    } else {
        printf("ok %u %u %u", s.size, s.nblk, s.header);
        for (zp_u32 i = 0; i < s.nblk; i++)
            printf(" %u", s.blk[i]);
        printf("\n");
    }
    zp_slot_free(&s);
    fclose(m.f);
    return rc ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "units")) {
        srand(1);
        t_index();
        t_latest();
        t_trust();
        t_resolve();
        t_http();
        printf("zpkg-core-test: %d checks passed, %d failed\n", passes, fails);
        fflush(stdout);
        return fails != 0;
    }
    if (argc == 3 && (!strcmp(argv[1], "index") || !strcmp(argv[1], "latest"))) {
        size_t n;
        char *t = readfile(argv[2], &n);
        struct zp_index ix;
        const char *why = "";
        zp_u32 s;
        if (!t) {
            printf("cannot read %s\n", argv[2]);
            return 2;
        }
        if (argv[1][0] == 'l') {
            int rc = zp_latest_parse(t, n, &s);
            rc ? printf("refused\n") : printf("ok %u\n", s);
            free(t);
            return rc != 0;
        }
        if (zp_index_parse(&ix, t, n, &why)) {
            printf("refused: %s\n", why);
            free(t);
            return 1;
        }
        printf("ok %s %u %d %s\n", ix.channel, ix.serial, ix.npkg, ix.snapshot ? ix.snapshot : "-");
        zp_index_free(&ix);
        free(t);
        return 0;
    }
    if (argc >= 6 && !strcmp(argv[1], "slot"))
        return slot_cmd(argc, argv, 0);
    if (argc == 8 && !strcmp(argv[1], "slotwrite"))
        return slot_cmd(argc, argv, 1);
    fprintf(stderr, "usage: zpkg-core-test units | index FILE | latest FILE | slot IMG DIR NAME DOSTYPE [noflush] [BS RES]"
                    " | slotwrite IMG DIR NAME DOSTYPE OFF FILE\n");
    return 2;
}
