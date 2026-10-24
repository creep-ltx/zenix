/* SPDX-License-Identifier: GPL-2.0-only */
/* Paula's serial registers for the host test of kernel/serial.c (tools/serial-test.c). The wire takes one byte
 * every fake_wire_period reads of SERDATR (0 = it never does: a port that cannot send); TBE is set while the
 * transmit buffer is empty. Every byte written to SERDAT is recorded in fake_sent. One received byte can be
 * pending (fake_rx). */
#include <stdlib.h>
#include <string.h>
#define INTF_RBF 0x0800
static u16 fake_serdat = 0xFFFF;                /* 0xFFFF: nothing written since the last look */
static int fake_tbe = 1, fake_wire_period = 1, fake_reads;
static unsigned char *fake_sent;
static unsigned long fake_nsent, fake_cap;
static int fake_rx = -1;
static u16 fake_intreq_w, fake_serper;
static void fake_look(void)                     /* take what the driver wrote to SERDAT */
{
    if (fake_serdat != 0xFFFF) {
        if (fake_nsent == fake_cap) { fake_cap = fake_cap ? fake_cap * 2 : 65536; fake_sent = realloc(fake_sent, fake_cap); }
        fake_sent[fake_nsent++] = (unsigned char)fake_serdat;
        fake_serdat = 0xFFFF;
        fake_tbe = 0;
    }
}
static u16 fake_serdatr(void)
{
    fake_look();
    if (!fake_tbe && fake_wire_period && ++fake_reads % fake_wire_period == 0) fake_tbe = 1;
    u16 v = fake_tbe ? 0x2000 : 0;
    if (fake_rx >= 0) { v |= (u16)fake_rx; fake_rx = -1; }
    return v;
}
static u16 fake_intreqr(void) { return fake_rx >= 0 ? INTF_RBF : 0; }
#define SERDATR fake_serdatr()
#define SERDAT  fake_serdat
#define INTREQR fake_intreqr()
#define INTREQ  fake_intreq_w
#define SERPER  fake_serper
