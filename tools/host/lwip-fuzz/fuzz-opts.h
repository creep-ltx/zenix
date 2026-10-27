/* SPDX-License-Identifier: GPL-2.0-only */
/* The fuzzing build (tools/host/lwip-fuzz.c) on top of Zenix's lwipopts.h: incoming checksums are not checked (as
 * lwIP's own test/fuzz/lwipopts.h: almost every mutated packet would fail them and go no further). Nothing else
 * changes. */
#define CHECKSUM_CHECK_IP               0
#define CHECKSUM_CHECK_UDP              0
#define CHECKSUM_CHECK_TCP              0
#define CHECKSUM_CHECK_ICMP             0
