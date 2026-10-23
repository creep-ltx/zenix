/* SPDX-License-Identifier: MIT */
/* Part of the miniz build (a stand-in for a C library header), under the same MIT licence as miniz: ../LICENSE. */
#pragma once
/* the freestanding C library of the Zenix user programs (libz) */
#include "../../libz.h"
#ifndef NULL
#define NULL ((void *)0)
#endif
void *realloc(void *p, unsigned long n);
