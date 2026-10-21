#pragma once
#include <stdint.h>
#include <stdarg.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t s32;

/* bootinfo v3, written by the bootblock at 0x1000 */
#define MEMF_CHIP 2u
#define MEMF_FAST 4u
struct bootregion { u32 attrs, lower, upper; };
struct bootinfo {
    u16 attn;                   /* exec AttnFlags: bit1 68020, bit2 68030, ... */
    u16 nregions;
    u32 kernel_pa;
    u32 kernel_size;
    u32 font;                   /* unused for now */
    struct bootregion r[8];
};

/* serial.c */
void serial_init(void);
void serial_putc(char c);
void serial_flush(void);
int serial_tx_pending(void);
void serial_tx_drain(void);
void serial_rx_isr(void);
void serial_puts(const char *s);
void kprintf(const char *fmt, ...);

/* main.c */
void panic(const char *why) __attribute__((noreturn));
void *memset(void *d, int c, unsigned long n);
void *memcpy(void *d, const void *s, unsigned long n);
extern volatile u32 ticks;      /* 100 Hz, CIA-A timer A */

/* start.S */
void set_vbr(void *table);
u16 irq_save(void);
void irq_restore(u16 sr);
void ints_on(void);
void ints_off(void);
void cpu_idle(void);
void cache_enable(void);
void exc_common(void);
void irq2_entry(void);
