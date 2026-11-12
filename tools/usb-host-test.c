/* SPDX-License-Identifier: GPL-2.0-only */
/* Host test of usbd's controller and core code (usb/xhci-core.c, usb/core.c) against the model of the controller and
 * a USB stick backed by an image file (tools/xhci-model.h): DESIGN D46, docs/d46-usb.md 6.1. Every result is compared,
 * never only printed. Built and run by tools/usb-host-test.sh (ASan and UBSan): usb-host-test [-v] IMAGE. -v prints
 * usbd's log. */
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t u32;
#include "xhci-model.h"
#include "../usb/xhci.h"

static int verbose, nocache, noinval;
static char logbuf[1 << 18];
static size_t loglen;

/* ---- the includer's side of xhci-core.c: the model's registers and memory, the model's clock ---- */
static u32 rg_r32(struct xhci *x, u32 off) { (void)x; if (off >= M_WIN) mfault("a register outside the window", off); return model_rd(off); }
static void rg_w32(struct xhci *x, u32 off, u32 v) { (void)x; if (off >= M_WIN) mfault("a register outside the window", off); model_wr(off, v); }
static u32 dm_r32(u32 a) { return cpu_r32(a); }
static void dm_w32(u32 a, u32 v) { cpu_w32(a, v); }
static void dm_rd(u32 a, void *p, u32 n) { cpu_rd(a, p, n); }
static void dm_wr(u32 a, const void *p, u32 n) { cpu_wr(a, p, n); }
static void dm_cache(struct xhci *x, unsigned how, u32 a, u32 n)
{
    (void)x;
    if (nocache) return;
    cpu_cache(noinval ? 1 : how, a, n);
}
static u32 xc_ticks(void) { return M.time; }
static void xc_wait(void)
{
    M.time++;
    if (M.cnr_ticks > 0) M.cnr_ticks--;
    model_tick();
}
static void xc_log(struct xhci *x, const char *fmt, ...)
{
    (void)x;
    char t[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    if (verbose) printf("    usbd: %s\n", t);
    size_t n = strlen(t);
    if (loglen + n + 2 < sizeof logbuf) { memcpy(logbuf + loglen, t, n); loglen += n; logbuf[loglen++] = '\n'; logbuf[loglen] = 0; }
}
#include "../usb/xhci-core.c"
#include "../usb/core.c"

/* what usbd would post to diskd */
static int news_add, news_del;
static u32 news_sectors;
static char news_label[32];
static void usb_news(struct usb_dev *d, int add)
{
    if (add) { news_add++; news_sectors = d->sectors; memcpy(news_label, d->label, sizeof news_label); }
    else news_del++;
}

static int pass, failed;
static void check(const char *what, int ok)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (ok) pass++; else failed++;
}

static struct xhci X;
static struct usb U;
#define DMA_AT (M_BASE + 0x40)          /* 64-byte aligned, not page aligned, as DRV_DMAMEM gives */

static int boot(unsigned nscratch, unsigned csz)
{
    model_reset(nscratch, csz);
    memset(&X, 0, sizeof X);
    memset(&U, 0, sizeof U);
    loglen = 0;
    logbuf[0] = 0;
    X.size = M_WIN;
    U.x = &X;
    news_add = news_del = 0;
    if (xc_probe(&X)) return -1;
    if (xc_dma_size(&X) > M_SIZE - 0x40) return -2;
    if (xc_start(&X, DMA_AT, xc_dma_size(&X))) return -3;
    return xc_selftest(&X);
}

/* usbd's loop for n ticks */
static void run(int n)
{
    for (int i = 0; i < n; i++) {
        usb_poll(&U);
        xc_wait();
    }
}
static struct usb_dev *stick(void)
{
    for (int i = 0; i < USB_MAXDEV; i++) if (U.dev[i].used && U.dev[i].kind == UK_STORAGE && U.dev[i].ready) return &U.dev[i];
    return 0;
}
static int ndev(void) { int n = 0; for (int i = 0; i < USB_MAXDEV; i++) n += U.dev[i].used; return n; }
static int nslots(void) { int n = 0; for (int i = 1; i <= M_SLOTS; i++) n += M.slot[i].on; return n; }
static int logged(const char *s) { return strstr(logbuf, s) != 0; }
static int count(const char *s) { int n = 0; for (const char *p = logbuf; (p = strstr(p, s)); p++) n++; return n; }

/* sectors [lba, lba + n) through usbd compared with the image file */
static int same_as_image(int fd, u32 lba, u32 n)
{
    struct usb_dev *d = stick();
    static unsigned char got[65536], want[65536];
    if (!d || msc_read(&U, d, lba, n)) return 0;
    dm_rd(X.bounce, got, n * 512);
    if (pread(fd, want, n * 512, (off_t)lba * 512) != (ssize_t)(n * 512)) return 0;
    return !memcmp(got, want, n * 512);
}

static void new_stick(struct mdev *st, int speed, int fd, off_t isz)
{
    mdev_init(st, "stick", MD_MSC, speed, 0x1b1c, 0x1a0e);
    st->imgfd = fd;
    st->sectors = (uint32_t)(isz / 512);
}

int main(int argc, char **argv)
{
    int a = 1;
    if (a < argc && !strcmp(argv[a], "-v")) { verbose = 1; a++; }
    const char *img = a < argc ? argv[a] : 0;
    static struct mdev st, other;

    /* ---- U1: the controller ---- */
    check("start: probe, reset, rings, run, No-Op self-test (2 scratchpad pages, 32-byte contexts)", boot(2, 32) == 0);
    check("start: no model faults", M.faults == 0);
    check("the bounce buffer: 64 KB on a 64 KB boundary, inside the DMA memory",
          !(X.bounce & 0xFFFF) && X.bounce >= DMA_AT && X.bounce + 65536 <= DMA_AT + xc_dma_size(&X));
    u32 qo[4], qv[4], qw[4];
    int nq = xc_quiesce(&X, qo, qv, qw);
    check("quiesce: USBCMD = 0, a wait for HCHalted, IMAN = IP, inside the window",
          nq == 3 && qo[0] == M_CAPLEN && qv[0] == 0 && qw[0] == 0 && qo[1] == M_CAPLEN + 4 && qw[1] == 1 && qv[1] == 1 &&
          qo[2] == M_RTS + 0x20 && qv[2] == 1 && qo[2] < X.size);
    check("start with 64-byte contexts and no scratchpad", boot(0, 64) == 0 && M.faults == 0);
    xc_stop(&X);
    check("stop: halted", (M.usbsts & 1) == 1);
    boot(2, 32);
    check("a running controller is halted before anything is written, then reset",
          xc_start(&X, DMA_AT, xc_dma_size(&X)) == 0 && xc_selftest(&X) == 0 && M.faults == 0 && logged("was running"));
    nocache = 1; M.expect_faults = 1;
    check("self-test without cache calls: no answer (-ETIMEDOUT)", boot(2, 32) == -ETIMEDOUT);
    check("... and the model saw the uncleaned lines", M.faults > 0);
    nocache = 0; noinval = 1;
    check("self-test without invalidates: no answer (the CPU sees its stale copy)", boot(2, 32) == -ETIMEDOUT);
    noinval = 0; M.expect_faults = 0;
    model_reset(0, 32);
    M.allones = 1;
    memset(&X, 0, sizeof X); X.size = M_WIN; loglen = 0;
    check("registers reading all ones (an unpowered USB block): refused, said", xc_probe(&X) == -ENODEV && logged("not powered"));
    M.allones = 0;
    memset(&X, 0, sizeof X); X.size = 0x100;
    check("a window too small for its registers: refused", xc_probe(&X) == -ENODEV);

    /* ---- U3: a stick on the root port, backed by the image file ---- */
    int fd = img ? open(img, O_RDONLY) : -1;
    off_t isz = fd >= 0 ? lseek(fd, 0, SEEK_END) : 0;
    check("the stick's image file (tools/usb-host-test.sh makes it)", fd >= 0 && isz >= 512 * 4096);
    if (fd < 0) { printf("FAILED: no image\n"); return 1; }
    boot(2, 32);
    new_stick(&st, 3, fd, isz);
    st.not_ready = 2;
    model_attach(1, &st);
    run(5);
    check("plugged in: nothing before 100 ms of settling (USB 2.0 7.1.7.3)", !stick() && st.addressed == 0);
    run(30);
    struct usb_dev *d = stick();
    check("a high-speed stick: enumerated, its bulk endpoints opened, ready after two NOT READY answers",
          d && d->ep_in == 0x81 && d->ep_out == 0x02 && d->mps_in == 512 && st.not_ready == 0);
    check("its label from INQUIRY and its size from READ CAPACITY (10)",
          d && !strcmp(d->label, "Corsair Voyager GTX") && d->sectors == st.sectors);
    check("diskd told once: usb0 with that size and label",
          news_add == 1 && news_sectors == st.sectors && !strcmp(news_label, "Corsair Voyager GTX"));
    check("no model faults", M.faults == 0);
    int ok = 1;
    for (u32 lba = 0; lba + 128 <= st.sectors && lba < 2048; lba += 128) ok &= same_as_image(fd, lba, 128);
    check("1 MB in requests of 128 sectors: the image's bytes", ok);
    check("odd sizes: 1 sector, 37 sectors at the end", same_as_image(fd, 5, 1) && same_as_image(fd, st.sectors - 37, 37));
    check("past the end and more than 128 sectors refused (-EINVAL)",
          d && msc_read(&U, d, st.sectors - 1, 2) == -EINVAL && msc_read(&U, d, 0, 129) == -EINVAL && msc_read(&U, d, 0, 0) == -EINVAL);
    st.read_fail = 1;
    check("a read that fails once (MEDIUM ERROR): asked again, the right bytes", same_as_image(fd, 100, 8) && st.read_fail == 0);
    st.read_fail = 5;
    check("a read that keeps failing: -EIO after three tries, said", d && msc_read(&U, d, 100, 8) == -EIO && logged("could not be read"));
    st.read_fail = 0;
    check("... and the stick reads again after", same_as_image(fd, 200, 16));
    st.stall_data = 1;
    check("reads whose data stage stalls: -EIO, the endpoint recovered each time", d && msc_read(&U, d, 300, 4) == -EIO && M.faults == 0);
    st.stall_data = 0;
    check("... the halt cleared on both sides: it reads again", same_as_image(fd, 300, 4) && st.halt_in == 0);
    st.bad_sig = 1;
    int r = d ? msc_read(&U, d, 0, 1) : 0;
    st.bad_sig = 0;
    check("a status with a wrong signature: reset recovery (Bulk-Only Reset, halts cleared), -EIO", r == -EIO && st.resets >= 1);
    check("... it reads again after", same_as_image(fd, 0, 1));
    st.bad_tag = 1;
    r = d ? msc_read(&U, d, 0, 1) : 0;
    st.bad_tag = 0;
    check("a status for another command (wrong tag): refused, -EIO", r == -EIO && logged("not the command's"));
    st.phase_err = 1;
    r = d ? msc_read(&U, d, 0, 1) : 0;
    st.phase_err = 0;
    check("a phase error: reset recovery, -EIO", r == -EIO && logged("phase error"));
    st.babble = 1;
    r = d ? msc_read(&U, d, 0, 2) : 0;
    st.babble = 0;
    check("a stick sending more than asked (babble): -EIO, recovered", r == -EIO && same_as_image(fd, 2, 2) && M.faults == 0);
    st.never_csw = 1;
    u32 t0 = M.time;
    r = d ? msc_read(&U, d, 0, 1) : 0;
    st.never_csw = 0;
    check("a status that never comes: -EIO after the 1.5 s waits, inside diskd's 5 s, nothing hangs",
          r == -EIO && M.time - t0 >= 150 && M.time - t0 < 500 && logged("not done"));
    check("... and it reads again after the reset recovery", same_as_image(fd, 7, 3));
    ok = 1;
    for (int k = 0; k < 9; k++) ok &= same_as_image(fd, (u32)(400 + 3 * k), (u32)(1 + k));
    check("after every recovery the data toggles agree: nine reads of odd sizes in a row, no packet thrown away",
          ok && st.dropped == 0);
    model_detach(1);
    run(3);
    check("pulled: diskd told, its slot disabled", news_del == 1 && ndev() == 0 && nslots() == 0 && M.faults == 0);
    st.gone = 0; st.not_ready = 0; st.addressed = 0;
    model_attach(1, &st);
    run(30);
    check("plugged in again: served again", stick() && news_add == 2);
    d = stick();
    st.gone = 1;                        /* pulled in the middle of a read: the transfer fails, no hang */
    M.portsc[1] &= ~1u;
    t0 = M.time;
    r = d ? msc_read(&U, d, 0, 8) : 0;
    check("pulled in the middle of a read: -ENODEV at once (the port looked at between the stages)",
          r == -ENODEV && M.time - t0 < 20);
    model_detach(1);
    run(3);
    check("... then gone", ndev() == 0 && nslots() == 0);

    /* a full-speed stick whose READ CAPACITY (10) says "more" */
    boot(2, 32);
    new_stick(&st, 1, fd, isz);
    st.cap16 = 1;
    model_attach(1, &st);
    run(30);
    d = stick();
    check("a full-speed stick (64-byte packets) whose READ CAPACITY (10) says \"more\": READ CAPACITY (16) used",
          d && d->sectors == st.sectors && d->mps_in == 64 && same_as_image(fd, 64, 128) && M.faults == 0);
    boot(2, 32);
    new_stick(&st, 3, fd, isz);
    st.cap16 = 2;                       /* READ CAPACITY (16): 2^32 + n sectors */
    model_attach(1, &st);
    run(30);
    d = stick();
    check("a stick past 2 TB: the first 2 TB served, said", d && d->sectors == 0xFFFFFFFFu && logged("past 2 TB"));

    /* the variants that are left alone; the port reset once, never in a loop */
    static const struct { const char *what; int how; const char *said; } refused[] = {
        { "4096-byte blocks", 1, "only 512-byte ones" }, { "no medium (an empty card reader)", 2, "no medium" },
        { "never ready", 3, "not ready after 5 s" }, { "a CD-ROM (device type 5)", 4, "not a disk" },
        { "a stick that stalls GET_DESCRIPTOR", 5, "no device descriptor" },
        { "a device that never answers", 6, "not answered" }, { "an impossible EP0 packet size (7)", 7, "impossible packet size" },
        { "a configuration with a descriptor of length 0", 8, "not a USB stick" },
        { "a configuration running past its end", 9, "not a USB stick" }, { "a vendor-specific device", 10, "not a USB stick" },
        { "a stick whose size wraps to 0 sectors", 11, "no sectors" },
    };
    for (unsigned t = 0; t < sizeof refused / sizeof refused[0]; t++) {
        boot(2, 32);
        mdev_init(&st, "stick", refused[t].how == 10 ? MD_OTHER : MD_MSC, 3, 0xbad0, 0x10 + t);
        st.imgfd = fd; st.sectors = (uint32_t)(isz / 512);
        switch (refused[t].how) {
        case 1: st.blocksize = 4096; break;
        case 2: st.no_medium = 1; break;
        case 3: st.not_ready = 100000; break;
        case 4: st.devtype = 5; break;
        case 5: st.stall_dev_desc = 1; break;
        case 6: st.never_answer = 1; break;
        case 7: st.dd[7] = 7; break;
        case 8: st.cfg[9] = 0; break;
        case 9: st.cfg[9 + 9] = 200; break;
        case 11: st.cap16 = 3; break;
        }
        model_attach(1, &st);
        run(refused[t].how == 3 || refused[t].how == 6 ? 900 : 40);
        int listed = refused[t].how >= 8 && refused[t].how <= 10;
        int resets = count("high-speed device");
        char what[200];
        snprintf(what, sizeof what, "%s: said (\"%s\"), %s, its port reset once only, nothing told to diskd",
                 refused[t].what, refused[t].said, listed ? "listed only" : "left alone");
        int good = logged(refused[t].said) && news_add == 0 && !stick() && resets == 1 && ndev() == listed && !X.dead && M.faults == 0;
        if (!good) printf("    said: %d, news %d, resets %d, devices %d, dead %d, faults %d\n", logged(refused[t].said), news_add,
                          resets, ndev(), X.dead, M.faults);
        check(what, good);
        model_detach(1);
        run(3);
    }
    /* a refused device pulled and plugged in again is tried again */
    boot(2, 32);
    new_stick(&st, 3, fd, isz);
    st.no_medium = 1;
    model_attach(1, &st);
    run(30);
    model_detach(1);
    run(3);
    st.no_medium = 0; st.gone = 0; st.addressed = 0;
    model_attach(1, &st);
    run(30);
    check("a refused device pulled and plugged in again: tried again, served", stick() != 0 && count("high-speed device") == 2);

    /* a fuzz of configuration descriptors: nothing read past what the device sent (ASan), nothing crashes */
    srand(46);
    int fuzz_ok = 1;
    for (int it = 0; it < 300 && fuzz_ok; it++) {
        boot(0, 32);
        mdev_init(&other, "fuzz", MD_MSC, 3, 0xf022, (unsigned)it);
        other.imgfd = fd; other.sectors = (uint32_t)(isz / 512);
        unsigned n = 9 + (unsigned)(rand() % 60);
        for (unsigned i = 9; i < n; i++) other.cfg[i] = (unsigned char)rand();
        if (rand() & 1) other.cfg[9] = 9, other.cfg[10] = 4, other.cfg[14] = 8, other.cfg[15] = 6, other.cfg[16] = 0x50;
        other.cfg[2] = (unsigned char)n; other.cfg[3] = 0;
        other.cfglen = n;
        model_attach(1, &other);
        run(20);
        if (X.dead) fuzz_ok = 0;
        model_detach(1);
        run(3);
        if (ndev() || M.faults) fuzz_ok = 0;
    }
    check("300 random configuration descriptors: no crash, no stray access, every one let go", fuzz_ok);

    printf("%s: %d passed, %d failed\n", failed ? "FAILED" : "all ok", pass, failed);
    return failed != 0;
}
