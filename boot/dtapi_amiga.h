/* SPDX-License-Identifier: GPL-2.0-only */
/* devicetree.resource calls (base in a6, arguments in a0/a1, result in d0), as seen from
 * the 68k while AmigaOS is alive. Vector numbers from the resource's published function
 * list: OpenKey -6, CloseKey -12, GetChild -18, FindProperty -24, GetProperty -30,
 * GetPropLen -36, GetPropName -42, GetPropValue -48, GetParent -54, GetKeyName -60. */
struct ctx { void *db; };

#define DT_CALL1(lvo, c, a) ({                                                   \
    register void *r_a0 __asm__("a0") = (void *)(a);                             \
    register void *r_a6 __asm__("a6") = (c)->db;                                 \
    register void *r_d0 __asm__("d0");                                           \
    __asm__ volatile("jsr " #lvo "(%%a6)"                                        \
        : "=r"(r_d0), "+r"(r_a0), "+r"(r_a6) : : "d1", "a1", "cc", "memory");    \
    r_d0; })
#define DT_CALL2(lvo, c, a, b) ({                                                \
    register void *r_a0 __asm__("a0") = (void *)(a);                             \
    register void *r_a1 __asm__("a1") = (void *)(b);                             \
    register void *r_a6 __asm__("a6") = (c)->db;                                 \
    register void *r_d0 __asm__("d0");                                           \
    __asm__ volatile("jsr " #lvo "(%%a6)"                                        \
        : "=r"(r_d0), "+r"(r_a0), "+r"(r_a1), "+r"(r_a6) : : "d1", "cc", "memory"); \
    r_d0; })

static inline void *dt_open(struct ctx *c, const char *path) { return DT_CALL1(-6, c, path); }
static inline void *dt_child(struct ctx *c, void *key, void *prev) { return DT_CALL2(-18, c, key, prev); }
static inline void *dt_find(struct ctx *c, void *key, const char *name) { return DT_CALL2(-24, c, key, name); }
static inline unsigned dt_len(struct ctx *c, void *prop) { return (unsigned)(unsigned long)DT_CALL1(-36, c, prop); }
static inline const void *dt_val(struct ctx *c, void *prop) { return DT_CALL1(-48, c, prop); }
static inline void *dt_parent(struct ctx *c, void *key) { return DT_CALL1(-54, c, key); }
static inline const char *dt_name(struct ctx *c, void *key) { return DT_CALL1(-60, c, key); }
