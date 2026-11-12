/* SPDX-License-Identifier: GPL-2.0-only */
/* The USB server's port (DESIGN D46, docs/d46-usb.md 3.2): requests to /sbin/usbd, the D11 service of port "usb".
 * Frozen once shipped: the requests and the structs only grow at their ends (each struct says its own size). */
#pragma once

#define USB_PORT        "usb"
#define USB_LIST        1       /* a0 the index (0, 1, ...), a1 struct usbdevinfo *, a2 its size -> 0, -ENOENT past the
                                 * last device */
#define USB_DESC        2       /* a0 the index, a1 a buffer, a2 its size -> the bytes of the device's configuration
                                 * descriptor copied (at most 512), -ENOENT */
#define USB_HOLD        3       /* root: answered only when usbd ends (lsusb --hold keeps it running, docs/man/usbd.txt);
                                 * -ENODEV at once when there is no controller to drive */
#define USB_STAT        4       /* a1 struct usbstat *, a2 its size -> 0 */

struct usbdevinfo {
    u32 size;                   /* the caller's sizeof */
    unsigned char slot, speed, port, parent;    /* speed 1 full, 2 low, 3 high; parent 0xFF: the root port */
    unsigned char kind, pad[3]; /* kind 0 no driver, 4 a USB stick */
    u32 sectors;                /* a stick's size in 512-byte sectors, 0 for the rest */
    unsigned char dd[18];       /* the device descriptor as the device sent it */
    unsigned short cfglen;
    char label[32];             /* a stick's vendor and product from INQUIRY */
};
#define USBDEVINFO_FIRST 68

struct usbstat {
    u32 size;
    u32 events, commands, transfers, errors;    /* the controller's counts since usbd started */
    u32 refused;                /* devices left alone (too many, impossible descriptors, no answer) */
    u32 devices;                /* in the table now */
    u32 cache_user;             /* 1: Emu68's range opcodes from usbd; 0: the kernel's DRV_CACHE */
};
#define USBSTAT_FIRST 32
