/* SPDX-License-Identifier: GPL-2.0-only */
/* The USB core (DESIGN D46): what enumeration and mass storage share. From the USB 2.0 specification (chapter 9:
 * descriptors and standard requests) and the USB Mass Storage Class Bulk-Only Transport 1.0. */
#pragma once

#define USB_SPEED_FULL  1               /* as xHCI's PORTSC speed IDs (7.2.2.1.1, the default ones) */
#define USB_SPEED_LOW   2
#define USB_SPEED_HIGH  3

/* the setup packet's bmRequestType */
#define RT_IN           0x80
#define RT_CLASS        0x20
#define RT_IFACE        0x01
#define RT_EP           0x02
/* standard requests */
#define REQ_CLEAR_FEAT  1
#define REQ_GET_DESC    6
#define REQ_SET_CONFIG  9
/* descriptor types */
#define DT_DEVICE       1
#define DT_CONFIG       2
#define DT_IFACE        4
#define DT_ENDPOINT     5
/* mass storage, bulk-only (BOT 1.0 3.1, 3.2, 5.1, 5.2) */
#define MSC_RESET       0xFF
#define MSC_GET_MAX_LUN 0xFE
#define CBW_SIG         0x43425355u     /* "USBC" */
#define CSW_SIG         0x53425355u     /* "USBS" */

#define USB_MAXDEV      2               /* one socket, one stick (no hubs yet: the partner 2026-11-12); the second for a
                                         * device plugged in while the first is still being let go */
#define USB_CFGMAX      512             /* configuration bytes read and kept */
#define MSC_MAXSECT     128             /* sectors a request: the 64 KB bounce buffer */

/* what usbd knows of a device (also what USB_LIST answers, usb/usbproto.h) */
struct usb_dev {
    unsigned char used, slot, speed, port;      /* port: the root port's number */
    unsigned char kind;                         /* UK_* */
    unsigned char cfgval;
    unsigned char dd[18];                       /* the device descriptor */
    unsigned cfglen;
    unsigned char *cfg;                         /* its configuration descriptor, cfglen bytes */
    /* mass storage */
    unsigned char iface, ep_in, ep_out;         /* the bulk endpoints' addresses */
    unsigned short mps_in, mps_out;
    unsigned char ready;                        /* the stick answered READ CAPACITY: it is served */
    unsigned tag;                               /* the next CBW's tag */
    u32 sectors;                                /* its size in 512-byte sectors (at most 2^32 - 1) */
    char label[32];                             /* INQUIRY's vendor and product */
    unsigned errors;
};
#define UK_OTHER        0
#define UK_STORAGE      4
