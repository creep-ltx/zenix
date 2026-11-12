/* SPDX-License-Identifier: GPL-2.0-only */
/* lsusb: the USB devices the USB server (/sbin/usbd, DESIGN D46) drives or sees, and `lsusb --hold`, which keeps the
 * server running (/etc/daemons). docs/man/lsusb.txt. A call to port "usb" starts usbd (it is a D11 service). */
#include "libz.h"
#include "../usb/usbproto.h"

static const char *speed(unsigned s) { return s == 1 ? "full" : s == 2 ? "low" : s == 3 ? "high" : "?"; }
static const char *kind(unsigned k) { return k == 4 ? "USB stick" : "no driver"; }

static int call(int port, u32 type, u32 a0, u32 a1, u32 a2)
{
    struct umsg m;
    memset(&m, 0, sizeof m);
    m.type = type;
    m.a[0] = a0; m.a[1] = a1; m.a[2] = a2;
    return sys_call(port, &m);
}

/* the configuration descriptor in words: interfaces and endpoints */
static void verbose(const unsigned char *c, int n)
{
    for (int i = 0; i + 2 <= n;) {
        int l = c[i];
        if (l < 2 || i + l > n) { dprintf(1, "    (a descriptor of length %d at %d: the rest not shown)\n", l, i); return; }
        if (c[i + 1] == 2 && l >= 9)
            dprintf(1, "    configuration %u: %u interface(s), %u mA\n", c[i + 5], c[i + 4], 2u * c[i + 8]);
        else if (c[i + 1] == 4 && l >= 9)
            dprintf(1, "    interface %u alt %u: class %02x subclass %02x protocol %02x, %u endpoint(s)\n", c[i + 2], c[i + 3],
                   c[i + 5], c[i + 6], c[i + 7], c[i + 4]);
        else if (c[i + 1] == 5 && l >= 7) {
            static const char *t[4] = { "control", "isochronous", "bulk", "interrupt" };
            dprintf(1, "    endpoint %02x %s %s, %u bytes, interval %u\n", c[i + 2], c[i + 2] & 0x80 ? "IN" : "OUT",
                   t[c[i + 3] & 3], (c[i + 4] | c[i + 5] << 8) & 0x7FF, c[i + 6]);
        }
        i += l;
    }
}

int main(int argc, char **argv)
{
    int v = 0, hold = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) v = 1;
        else if (!strcmp(argv[i], "--hold")) hold = 1;
        else { dprintf(2, "usage: lsusb [-v] | lsusb --hold\n"); return 2; }
    }
    int port = sys_port_find(USB_PORT);
    if (port < 0) { dprintf(2, "lsusb: no USB server (no service \"usb\" in /etc/svctab)\n"); return 1; }
    if (hold) {                         /* answered only when usbd ends; then the next call starts it again */
        for (;;) {
            int r = call(port, USB_HOLD, 0, 0, 0);
            if (r == -ENODEV) return 0; /* no controller: nothing to keep running */
            if (r == -EPERM) { dprintf(2, "lsusb: --hold is root's\n"); return 1; }
            sys_sleep(100);             /* usbd ended (or was given up): a second, then again */
        }
    }
    struct usbdevinfo d;
    int n = 0;
    for (u32 i = 0;; i++) {
        int r = call(port, USB_LIST, i, (u32)&d, sizeof d);
        if (r == -ENODEV) { dprintf(2, "lsusb: no USB controller on this machine\n"); return 1; }
        if (r == -ENOENT) break;
        if (r < 0) { dprintf(2, "lsusb: %s\n", zstrerror(r)); return 1; }
        n++;
        dprintf(1, "Device %u: ID %04x:%04x %s, %s speed, %s port %u", d.slot, d.dd[8] | d.dd[9] << 8,
               d.dd[10] | d.dd[11] << 8, kind(d.kind), speed(d.speed), d.parent == 0xFF ? "root" : "hub", d.port);
        if (d.kind == 4)
            dprintf(1, ": \"%.31s\", %u MB, usb0", d.label, d.sectors >> 11);
        dprintf(1, "\n");
        if (v) {
            dprintf(1, "    USB %x.%02x, class %02x subclass %02x protocol %02x, EP0 %u bytes, %u configuration(s)\n",
                   d.dd[3], d.dd[2], d.dd[4], d.dd[5], d.dd[6], d.dd[7], d.dd[17]);
            unsigned char *c = malloc(512);
            if (!c) { dprintf(2, "lsusb: out of memory\n"); return 1; }
            int k = call(port, USB_DESC, i, (u32)c, 512);
            if (k > 0) verbose(c, k);
            free(c);
        }
    }
    if (!n) dprintf(1, "no USB devices\n");
    return 0;
}
