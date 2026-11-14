/* SPDX-License-Identifier: GPL-2.0-only */
/* BSD's <sys/ttydefaults.h>, the one macro the BSD games take from it: the control character of a letter */
#ifndef ZGAMES_TTYDEFAULTS_H
#define ZGAMES_TTYDEFAULTS_H
#define CTRL(x) ((x) & 037)
#endif
