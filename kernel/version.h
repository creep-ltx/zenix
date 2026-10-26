/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
/* The version of Zenix (DESIGN D15, docs/d15-self-updates.md): the one place it is written. The number orders versions
 * (YYYYMMDDNN: the date and a count within the day; a bundle, a package and the kernel all carry it, and an update
 * never installs a lower one); the string is what people read (uname). The kernel reports both (SYS_update INFO),
 * tools/mkzup.py reads them from here for the packages and checks that the kernel in a bundle carries the bundle's
 * number (the "ZVER" record in the kernel image, kernel/syscall.c). A test build may set its own:
 * make BUILD=/tmp/x ZVNUM=2026102701 ZVSTR=0.2-test (a build directory of its own: not tracked as a dependency). */
#ifdef ZENIX_VNUM_SET
#define ZENIX_VNUM ZENIX_VNUM_SET
#else
#define ZENIX_VNUM 2026102600u
#endif
#ifdef ZENIX_VSTR_SET
#define ZENIX_VSTR ZENIX_VSTR_SET
#else
#define ZENIX_VSTR "0.2"
#endif
