/* SPDX-License-Identifier: GPL-2.0-only */
/* devicetree.resource API on top of libfdt, for testing boot/stage0.c on the host. */
#include <libfdt.h>
struct ctx { void *db; };
#define FDT(c) ((const void *)(c)->db)
static inline void *dt_open(struct ctx *c, const char *path)
{ int o = fdt_path_offset(FDT(c), path); return o < 0 ? 0 : (void *)(long)(o + 1); }
static inline void *dt_child(struct ctx *c, void *key, void *prev)
{ int o = prev ? fdt_next_subnode(FDT(c), (int)(long)prev - 1) : fdt_first_subnode(FDT(c), (int)(long)key - 1);
  return o < 0 ? 0 : (void *)(long)(o + 1); }
static inline void *dt_find(struct ctx *c, void *key, const char *name)
{ int l; return (void *)fdt_get_property(FDT(c), (int)(long)key - 1, name, &l); }
static inline unsigned dt_len(struct ctx *c, void *p) { (void)c; return fdt32_to_cpu(((struct fdt_property *)p)->len); }
static inline const void *dt_val(struct ctx *c, void *p) { (void)c; return ((struct fdt_property *)p)->data; }
static inline void *dt_parent(struct ctx *c, void *key)
{ int o = fdt_parent_offset(FDT(c), (int)(long)key - 1); return o < 0 ? 0 : (void *)(long)(o + 1); }
static inline const char *dt_name(struct ctx *c, void *key) { return fdt_get_name(FDT(c), (int)(long)key - 1, 0); }
