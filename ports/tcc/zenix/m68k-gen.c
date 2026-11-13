/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 *  m68k code generator for TCC (Zenix: 68020+, gcc's m68k-elf rules)
 *
 *  Copyright (c) 2026 the Zenix authors
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  The calling rules are gcc's for m68k-elf, so that tcc's objects link
 *  with code gcc built (Zenix's C library, libgcc):
 *  - arguments on the stack, last pushed first, each in a multiple of 4
 *    bytes; a value of fewer than 4 bytes (char, short, a small struct)
 *    lies in the last bytes of its slot (big-endian: right-justified);
 *  - the result in d0; a 64-bit result (long long, double) in d0 (the
 *    high, first word) and d1; a struct of 1, 2, 4 or 8 bytes (2, 4 and
 *    8 only when aligned to 2) in d0 / d0:d1 like an integer; any other
 *    struct through a hidden pointer the caller passes in a0;
 *  - d0, d1, a0, a1 are the caller's; d2-d7, a2-a6 are kept (this code
 *    saves the d2-d4 it uses, a6 is the frame pointer);
 *  - int, long, pointers, long long, float and double are aligned to 2
 *    bytes (gcc's BIGGEST_ALIGNMENT on the 68k);
 *  - floating point in software through libgcc (__adddf3 and friends):
 *    a float lives in one data register, a double in two, like a long
 *    long, so no FPU is needed. long double is double (8 bytes; gcc's
 *    is the 12-byte extended format: do not pass long doubles between
 *    tcc's and gcc's code).
 *
 *  tccgen's registers are d0-d4. a0 and a1 are this file's scratch
 *  registers (an address taken from a data register, the struct return
 *  pointer, an indirect call), a6 the frame, a7 the stack.
 */

#ifdef TARGET_DEFS_ONLY

/* number of available registers */
#define NB_REGS            5
/* inline asm (m68k-asm.c): d0-d7 are 0-7, a0-a7 8-15 */
#define NB_ASM_REGS        16
#define CONFIG_TCC_ASM

/* a register can belong to several classes. The classes must be
   sorted from more general to more precise (see gv2() code which does
   assumptions on it). Floats live in the data registers (soft float):
   RC_FLOAT is RC_INT. */
#define RC_INT     0x0001
#define RC_FLOAT   0x0001
#define RC_D0      0x0002
#define RC_D1      0x0004
#define RC_D2      0x0008
#define RC_D3      0x0010
#define RC_D4      0x0020

/* a two-word result: the low word (tccgen's r) in d1, the high one (r2)
   in d0; a one-word result in d0 (R_RET in tccgen.c) */
#define RC_IRET    RC_D1
#define RC_IRE2    RC_D0
#define RC_FRET    RC_D0

/* pretty names for the registers */
enum {
    TREG_D0 = 0,
    TREG_D1,
    TREG_D2,
    TREG_D3,
    TREG_D4,
};

/* return registers for function */
#define REG_IRET  TREG_D1 /* low word of a two-word result */
#define REG_IRE2  TREG_D0 /* high word of a two-word result */
#define REG_FRET  TREG_D0 /* a float's result */
#define REG_IRET1 TREG_D0 /* a one-word result */

/* pointer size, in bytes */
#define PTR_SIZE 4

/* long double size and alignment, in bytes (long double is double) */
#define LDOUBLE_SIZE  8
#define LDOUBLE_ALIGN 2
/* maximum alignment (for aligned attribute support) */
#define MAX_ALIGN     8

/* the largest alignment of a scalar type: gcc's BIGGEST_ALIGNMENT */
#define TCC_SCALAR_ALIGN_MAX 2

/* return values must be extended at the caller (gcc does not) */
#define PROMOTE_RET

/* struct copies inline */
#define TCC_TARGET_NATIVE_STRUCT_COPY

/******************************************************/
#else /* ! TARGET_DEFS_ONLY */
/******************************************************/
#define USING_GLOBALS
#include "tcc.h"

ST_DATA const char * const target_machine_defs =
    "__m68k__\0"
    "__mc68000__\0"
    "__mc68000\0"
    "mc68000\0"
    "__mc68020__\0"
    "__mc68020\0"
    "mc68020\0"
    ;

ST_DATA const int reg_classes[NB_REGS] = {
    /* d0 */ RC_INT | RC_D0,
    /* d1 */ RC_INT | RC_D1,
    /* d2 */ RC_INT | RC_D2,
    /* d3 */ RC_INT | RC_D3,
    /* d4 */ RC_INT | RC_D4,
};

/* registers outside tccgen's set */
#define REG_A0 0
#define REG_A1 1
#define REG_FP 6        /* a6 */
#define REG_SP 7        /* a7 */

/* the 68k's condition codes */
enum {
    CC_T = 0, CC_F, CC_HI, CC_LS, CC_CC, CC_CS, CC_NE, CC_EQ,
    CC_VC, CC_VS, CC_PL, CC_MI, CC_GE, CC_LT, CC_GT, CC_LE
};

static int func_prolog_ind;     /* where the prolog goes */
/* A comparison's flags wait for their branch (VT_CMP): tccgen may spill
   or load registers in between, and on the 68k a move sets the flags (on
   x86 it does not). While this is set, load() and store() keep the
   condition codes (move.w ccr,-(sp) ... move.w (sp)+,ccr). */
static int cc_live;
static int func_regs_used;      /* d2-d4 written in this function: saved */
#define PROLOG_SIZE 10          /* link.l a6,#-n (6) + movem.l (4) */

/******************************************************/
/* emitting */

static void ob(int c)
{
    int ind1;
    if (nocode_wanted)
        return;
    ind1 = ind + 1;
    if (ind1 > cur_text_section->data_allocated)
        section_realloc(cur_text_section, ind1);
    cur_text_section->data[ind] = c;
    ind = ind1;
}

static void o16(unsigned v)
{
    ob(v >> 8);
    ob(v);
}

static void o32(unsigned v)
{
    o16(v >> 16);
    o16(v);
}

/* tccgen's 'o': one instruction word */
ST_FUNC void o(unsigned int c)
{
    o16(c);
}

static uint32_t rd32(int pos)
{
    unsigned char *p = cur_text_section->data + pos;
    return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

static void wr32(int pos, uint32_t v)
{
    unsigned char *p = cur_text_section->data + pos;
    p[0] = v >> 24, p[1] = v >> 16, p[2] = v >> 8, p[3] = v;
}

static void wr16(int pos, unsigned v)
{
    unsigned char *p = cur_text_section->data + pos;
    p[0] = v >> 8, p[1] = v;
}

/* a register written in this function: the callee-saved ones (d2-d7,
   a2-a5; inline asm may use the ones tccgen does not) are saved */
static void use_reg(int r)
{
    if ((r >= 2 && r <= 7) || (r >= 10 && r <= 13))
        func_regs_used |= 1 << r;
}

static int fits8(int v)
{
    return v == (int8_t)v;
}

static int fits16(int v)
{
    return v == (int16_t)v;
}

/* the bits of a constant scalar of 4 bytes or less (a float is kept in
   c.f, whose bytes are not c.i's low ones on a big-endian host) */
static uint32_t const_bits(SValue *sv)
{
    if ((sv->type.t & VT_BTYPE) == VT_FLOAT) {
        uint32_t u;
        memcpy(&u, &sv->c.f, 4);
        return u;
    }
    return (uint32_t)sv->c.i;
}

/******************************************************/
/* effective addresses */

typedef struct EA {
    int mode, reg;              /* the 6-bit field */
    int n;                      /* extension words */
    unsigned short w[5];
    Sym *sym;                   /* a relocation on the long at w[rel] */
    int rel, rtype, addend;
} EA;

#define EA_FIELD(e) ((e)->mode << 3 | (e)->reg)

static void ea_clear(EA *e, int mode, int reg)
{
    e->mode = mode, e->reg = reg, e->n = 0, e->sym = NULL;
}

static void ea_long(EA *e, uint32_t v)
{
    e->w[e->n++] = v >> 16;
    e->w[e->n++] = v;
}

static void ea_dreg(EA *e, int r)
{
    ea_clear(e, 0, r);
}

static void ea_areg(EA *e, int a)
{
    ea_clear(e, 1, a);
}

static void ea_ind(EA *e, int a)
{
    ea_clear(e, 2, a);
}

static void ea_postinc(EA *e, int a)
{
    ea_clear(e, 3, a);
}

static void ea_predec(EA *e, int a)
{
    ea_clear(e, 4, a);
}

/* (d,An): no displacement, 16 bits, or the 68020's 32-bit base
   displacement (full format, index suppressed) */
static void ea_disp(EA *e, int a, int d)
{
    if (d == 0) {
        ea_clear(e, 2, a);
    } else if (fits16(d)) {
        ea_clear(e, 5, a);
        e->w[e->n++] = d;
    } else {
        ea_clear(e, 6, a);
        e->w[e->n++] = 0x0170;   /* full format, IS, 32-bit bd */
        ea_long(e, d);
    }
}

/* an absolute address: sym+c, or the number c */
static void ea_abs(EA *e, Sym *sym, int c)
{
    if (!sym && fits16(c)) {
        ea_clear(e, 7, 0);
        e->w[e->n++] = c;
        return;
    }
    ea_clear(e, 7, 1);
    if (sym) {
        e->sym = sym, e->rel = e->n, e->rtype = R_68K_32, e->addend = c;
        ea_long(e, 0);
    } else {
        ea_long(e, c);
    }
}

/* an immediate operand of 'size' bytes */
static void ea_imm(EA *e, uint32_t v, int size)
{
    ea_clear(e, 7, 4);
    if (size == 4)
        ea_long(e, v);
    else
        e->w[e->n++] = size == 1 ? (v & 0xff) : (v & 0xffff);
}

static void ea_imm_sym(EA *e, Sym *sym, int c)
{
    ea_clear(e, 7, 4);
    e->sym = sym, e->rel = 0, e->rtype = R_68K_32, e->addend = c;
    ea_long(e, 0);
}

static void emit_ea(EA *e)
{
    int i;
    for (i = 0; i < e->n; i++) {
        if (e->sym && i == e->rel && !nocode_wanted)
            greloca(cur_text_section, e->sym, ind, e->rtype, e->addend);
        o16(e->w[i]);
    }
}

/* an opcode word carrying one ea in its low 6 bits, then the ea */
static void op_ea(unsigned opc, EA *e)
{
    o16(opc | EA_FIELD(e));
    emit_ea(e);
}

/* opcode, one extension word, then the ea's words (muls.l, divs.l) */
static void op_ext_ea(unsigned opc, unsigned ext, EA *e)
{
    o16(opc | EA_FIELD(e));
    o16(ext);
    emit_ea(e);
}

/* move.<size> src,dst */
static void emit_move(int size, EA *src, EA *dst)
{
    int sz = size == 1 ? 1 : size == 2 ? 3 : 2;
    o16(sz << 12 | dst->reg << 9 | dst->mode << 6 | EA_FIELD(src));
    emit_ea(src);
    emit_ea(dst);
}

/* movea.l ea,An */
static void emit_movea(EA *src, int a)
{
    op_ea(0x2040 | a << 9, src);
}

/* lea ea,An */
static void emit_lea(EA *src, int a)
{
    op_ea(0x41C0 | a << 9, src);
}

/* the memory a value names, its address set up through the scratch An
   when it is in a register or a saved pointer. sv->r: VT_CONST (absolute,
   with or without a symbol), VT_LOCAL (the frame), VT_LLOCAL (a pointer
   saved in the frame) or a data register holding the address. */
static void ea_mem(EA *e, SValue *sv, int an)
{
    int v = sv->r & VT_VALMASK;
    int c = sv->c.i;
    EA t;

    if (v == VT_CONST) {
        ea_abs(e, (sv->r & VT_SYM) ? sv->sym : NULL, c);
    } else if (v == VT_LOCAL) {
        ea_disp(e, REG_FP, c);
    } else if (v == VT_LLOCAL) {
        ea_disp(&t, REG_FP, c);
        emit_movea(&t, an);
        ea_ind(e, an);
    } else if (v >= 8 && v < 16) {
        ea_ind(e, v - 8);       /* inline asm's address registers */
    } else if (v < VT_CONST) {
        ea_dreg(&t, v);
        emit_movea(&t, an);
        ea_ind(e, an);
    } else {
        tcc_internal_error("m68k: no address for this value");
    }
}

/* the same memory, 'add' bytes further (a memory ea from ea_mem) */
static void ea_add(EA *e, int add)
{
    int d;
    switch (e->mode) {
    case 2:
        ea_disp(e, e->reg, add);
        break;
    case 5:
        ea_disp(e, e->reg, (int16_t)e->w[0] + add);
        break;
    case 6:
        d = (int)((uint32_t)e->w[1] << 16 | e->w[2]) + add;
        ea_disp(e, e->reg, d);
        break;
    case 7:
        if (e->reg == 0) {
            ea_abs(e, NULL, (int16_t)e->w[0] + add);
        } else if (e->sym) {
            e->addend += add;
        } else {
            d = (int)((uint32_t)e->w[0] << 16 | e->w[1]) + add;
            ea_abs(e, NULL, d);
        }
        break;
    default:
        tcc_internal_error("m68k: ea_add");
    }
}

/* the size in memory of a value of this type, for loads and stores (a
   two-word type is stored a word at a time) */
static int mem_size(int t)
{
    switch (t & VT_BTYPE) {
    case VT_BYTE:
    case VT_BOOL:
        return 1;
    case VT_SHORT:
        return 2;
    default:
        return 4;
    }
}

/******************************************************/
/* tccgen's interface */

static void load1(int r, SValue *sv);

static void keep_cc(void)
{
    o16(0x42E7);                /* move.w ccr,-(sp) (68010+) */
}

static void restore_cc(void)
{
    o16(0x44DF);                /* move.w (sp)+,ccr */
}

/* load 'r' from value 'sv' */
ST_FUNC void load(int r, SValue *sv)
{
    if (cc_live && (sv->r & (VT_VALMASK | VT_LVAL)) != VT_CMP) {
        keep_cc();
        load1(r, sv);
        restore_cc();
    } else {
        load1(r, sv);
    }
}

/* load an address register (inline asm's operands: r 8-15) */
static void load_areg(int a, SValue *sv)
{
    int fr = sv->r, v = fr & VT_VALMASK, size;
    EA e;

    if (fr & VT_LVAL) {
        size = mem_size(sv->type.t);
        if (size == 1)
            tcc_error("a char in an address register");
        ea_mem(&e, sv, a == REG_A0 ? REG_A1 : REG_A0);
        op_ea((size == 4 ? 0x2040 : 0x3040) | a << 9, &e);       /* movea */
    } else if (v == VT_CONST) {
        if (fr & VT_SYM)
            ea_imm_sym(&e, sv->sym, sv->c.i);
        else
            ea_imm(&e, const_bits(sv), 4);
        emit_movea(&e, a);
    } else if (v == VT_LOCAL) {
        ea_disp(&e, REG_FP, sv->c.i);
        emit_lea(&e, a);
    } else if (v < 16) {
        if (v != 8 + a) {
            if (v >= 8)
                ea_areg(&e, v - 8);
            else
                ea_dreg(&e, v);
            emit_movea(&e, a);
        }
    } else {
        tcc_error("this value cannot go in an address register");
    }
}

static void load1(int r, SValue *sv)
{
    int fr, ft, fc, v, t;
    EA e, d;

    fr = sv->r;
    ft = sv->type.t & ~VT_DEFSIGN;
    fc = sv->c.i;
    v = fr & VT_VALMASK;
    use_reg(r);
    if (r >= 8) {
        load_areg(r - 8, sv);
        return;
    }
    ea_dreg(&d, r);

    if (fr & VT_LVAL) {
        int size = mem_size(ft);
        int uns = (ft & VT_UNSIGNED) || (ft & VT_BTYPE) == VT_BOOL;
        ea_mem(&e, sv, REG_A0);
        if (size < 4 && uns)
            o16(0x7000 | r << 9);                  /* moveq #0,dr */
        emit_move(size, &e, &d);
        if (size < 4 && !uns)
            o16((size == 1 ? 0x49C0 : 0x48C0) | r); /* extb.l / ext.l */
        return;
    }

    if (v == VT_CONST) {
        if (fr & VT_SYM) {
            ea_imm_sym(&e, sv->sym, fc);
            emit_move(4, &e, &d);
        } else {
            uint32_t c = const_bits(sv);
            if (fits8(c)) {
                o16(0x7000 | r << 9 | (c & 0xff)); /* moveq */
            } else {
                ea_imm(&e, c, 4);
                emit_move(4, &e, &d);
            }
        }
    } else if (v == VT_LOCAL) {
        if (fc == 0) {
            ea_areg(&e, REG_FP);
            emit_move(4, &e, &d);
        } else {
            ea_disp(&e, REG_FP, fc);
            emit_lea(&e, REG_A0);
            ea_areg(&e, REG_A0);
            emit_move(4, &e, &d);
        }
    } else if (v == VT_CMP) {
        int cc;
        switch (fc) {
        case TOK_EQ: cc = CC_EQ; break;
        case TOK_NE: cc = CC_NE; break;
        case TOK_LT: cc = CC_LT; break;
        case TOK_GE: cc = CC_GE; break;
        case TOK_LE: cc = CC_LE; break;
        case TOK_GT: cc = CC_GT; break;
        case TOK_ULT: cc = CC_CS; break;
        case TOK_UGE: cc = CC_CC; break;
        case TOK_ULE: cc = CC_LS; break;
        case TOK_UGT: cc = CC_HI; break;
        case TOK_Nset: cc = CC_MI; break;
        case TOK_Nclear: cc = CC_PL; break;
        default: tcc_internal_error("m68k: condition"); cc = 0;
        }
        o16(0x50C0 | cc << 8 | r);      /* scc dr */
        cc_live = 0;
        o16(0x49C0 | r);                /* extb.l dr */
        o16(0x4480 | r);                /* neg.l dr */
    } else if (v == VT_JMP || v == VT_JMPI) {
        t = v & 1;
        o16(0x7000 | r << 9 | t);       /* moveq #t,dr */
        o16(0x6002);                    /* bra.s over the next */
        gsym(fc);
        o16(0x7000 | r << 9 | (t ^ 1)); /* moveq #!t,dr */
    } else if (v < VT_CONST) {
        if (v != r) {
            if (v >= 8)
                ea_areg(&e, v - 8);
            else
                ea_dreg(&e, v);
            emit_move(4, &e, &d);
        }
    } else {
        tcc_internal_error("m68k: load");
    }
}

/* store register 'r' in lvalue 'v' */
ST_FUNC void store(int r, SValue *v)
{
    int fr, keep = cc_live;
    EA e, s;

    if (keep)
        keep_cc();

    fr = v->r & VT_VALMASK;
    if (r >= 8)
        ea_areg(&s, r - 8);     /* inline asm's address registers */
    else
        ea_dreg(&s, r);
    if (fr == VT_CONST || fr == VT_LOCAL || (v->r & VT_LVAL)) {
        int size = mem_size(v->type.t);
        if (r >= 8 && size == 1)
            tcc_error("a char from an address register");
        ea_mem(&e, v, r == 8 ? REG_A1 : REG_A0);
        emit_move(size, &s, &e);
    } else if (fr != r) {
        use_reg(fr);
        if (fr >= 8)
            emit_movea(&s, fr - 8);
        else {
            ea_dreg(&e, fr);
            emit_move(4, &s, &e);
        }
    }
    if (keep)
        restore_cc();
}

/* output a symbol and patch all calls to it */
ST_FUNC void gsym_addr(int t, int a)
{
    while (t) {
        uint32_t n = rd32(t);   /* next in the chain */
        wr32(t, a - t);         /* a branch's PC is its extension word */
        t = n;
    }
}

ST_FUNC void gen_fill_nops(int bytes)
{
    if (bytes & 1)
        ob(0), bytes--;
    while (bytes > 0)
        o16(0x4E71), bytes -= 2;
}

/* generate a jump to a label: bra.l, its displacement the chain */
ST_FUNC int gjmp(int t)
{
    int p;
    if (nocode_wanted)
        return t;
    o16(0x60FF);
    p = ind;
    o32(t);
    return p;
}

/* generate a jump to a fixed address */
ST_FUNC void gjmp_addr(int a)
{
    int d = a - (ind + 2);
    if (fits8(d) && d != 0 && d != -1) {
        o16(0x6000 | (d & 0xff));
    } else if (fits16(d)) {
        o16(0x6000);
        o16(d);
    } else {
        o16(0x60FF);
        o32(d);
    }
}

ST_FUNC int gjmp_append(int n, int t)
{
    if (n) {
        uint32_t n1 = n, n2;
        while ((n2 = rd32(n1)))
            n1 = n2;
        wr32(n1, t);
        t = n;
    }
    return t;
}

static int cc_of(int op)
{
    switch (op) {
    case TOK_EQ: return CC_EQ;
    case TOK_NE: return CC_NE;
    case TOK_LT: return CC_LT;
    case TOK_GE: return CC_GE;
    case TOK_LE: return CC_LE;
    case TOK_GT: return CC_GT;
    case TOK_ULT: return CC_CS;
    case TOK_UGE: return CC_CC;
    case TOK_ULE: return CC_LS;
    case TOK_UGT: return CC_HI;
    case TOK_Nset: return CC_MI;
    case TOK_Nclear: return CC_PL;
    }
    tcc_internal_error("m68k: unknown condition");
    return 0;
}

/* a jump taken when the flags say 'op' */
ST_FUNC int gjmp_cond(int op, int t)
{
    int p;
    cc_live = 0;
    if (nocode_wanted)
        return t;
    o16(0x60FF | cc_of(op) << 8);
    p = ind;
    o32(t);
    cc_live = 0;
    return p;
}

static int is_plain_const(SValue *sv)
{
    return (sv->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;
}

/* shift dr by a constant 1..31: 'imm' is the opcode with count 8 */
static void shift_const(unsigned imm, int r, int c)
{
    while (c > 0) {
        int n = c > 8 ? 8 : c;
        o16(imm | (n & 7) << 9 | r);
        c -= n;
    }
}

ST_FUNC void gen_opi(int op)
{
    int r, fr, c, rr;
    unsigned opc;
    EA e;

    switch (op) {
    case '+':
    case TOK_ADDC1:
    case '-':
    case TOK_SUBC1:
        if (is_plain_const(vtop)) {
            vswap();
            r = gv(RC_INT);
            vswap();
            use_reg(r);
            c = vtop->c.i;
            {
                int isadd = op == '+' || op == TOK_ADDC1;
                if (op == '+' || op == '-') {
                    /* no carry wanted: the sign of c may flip */
                    if (c < 0 && c >= -8)
                        c = -c, isadd = !isadd;
                }
                if (c >= 1 && c <= 8) {
                    /* addq.l / subq.l */
                    o16((isadd ? 0x5080 : 0x5180) | (c & 7) << 9 | r);
                } else {
                    ea_imm(&e, c, 4);                    /* add.l / sub.l #c */
                    op_ea((isadd ? 0xD080 : 0x9080) | r << 9, &e);
                }
            }
        } else {
            gv2(RC_INT, RC_INT);
            r = vtop[-1].r;
            fr = vtop[0].r;
            use_reg(r);
            ea_dreg(&e, fr);
            opc = (op == '+' || op == TOK_ADDC1) ? 0xD080 : 0x9080;
            op_ea(opc | r << 9, &e);
        }
        vtop--;
        break;
    case TOK_ADDC2:
    case TOK_SUBC2:
        /* addx/subx: registers only; the loads before keep X (moves and
           moveq do not touch it) */
        gv2(RC_INT, RC_INT);
        r = vtop[-1].r;
        fr = vtop[0].r;
        use_reg(r);
        o16((op == TOK_ADDC2 ? 0xD180 : 0x9180) | r << 9 | fr);
        vtop--;
        break;
    case '&':
    case '|':
    case '^':
        if (is_plain_const(vtop)) {
            vswap();
            r = gv(RC_INT);
            vswap();
            use_reg(r);
            c = vtop->c.i;
            if (op == '^' && c == -1) {
                o16(0x4680 | r);                    /* not.l */
            } else {
                opc = op == '&' ? 0x0280 : op == '|' ? 0x0080 : 0x0A80;
                ea_dreg(&e, r);
                o16(opc | EA_FIELD(&e));            /* andi/ori/eori.l */
                o32(c);
            }
        } else {
            gv2(RC_INT, RC_INT);
            r = vtop[-1].r;
            fr = vtop[0].r;
            use_reg(r);
            if (op == '^') {
                o16(0xB180 | fr << 9 | r);          /* eor.l fr,r */
            } else {
                ea_dreg(&e, fr);
                op_ea((op == '&' ? 0xC080 : 0x8080) | r << 9, &e);
            }
        }
        vtop--;
        break;
    case '*':
        if (is_plain_const(vtop)) {
            vswap();
            r = gv(RC_INT);
            vswap();
            use_reg(r);
            c = vtop->c.i;
            ea_imm(&e, c, 4);
        } else {
            gv2(RC_INT, RC_INT);
            r = vtop[-1].r;
            use_reg(r);
            ea_dreg(&e, vtop[0].r);
        }
        op_ext_ea(0x4C00, r << 12 | 0x0800, &e);     /* muls.l e,dr */
        vtop--;
        break;
    case TOK_SHL:
    case TOK_SHR:
    case TOK_SAR:
        if (is_plain_const(vtop)) {
            vswap();
            r = gv(RC_INT);
            vswap();
            use_reg(r);
            c = vtop->c.i & 31;
            if (c == 16) {
                if (op == TOK_SHL)
                    o16(0x4840 | r), o16(0x4240 | r);   /* swap; clr.w */
                else if (op == TOK_SHR)
                    o16(0x4240 | r), o16(0x4840 | r);   /* clr.w; swap */
                else
                    o16(0x4840 | r), o16(0x48C0 | r);   /* swap; ext.l */
            } else if (c) {
                shift_const(op == TOK_SHL ? 0xE188 : op == TOK_SHR ? 0xE088 : 0xE080, r, c);
            }
        } else {
            gv2(RC_INT, RC_INT);
            r = vtop[-1].r;
            fr = vtop[0].r;
            use_reg(r);
            o16((op == TOK_SHL ? 0xE1A8 : op == TOK_SHR ? 0xE0A8 : 0xE0A0) | fr << 9 | r);
        }
        vtop--;
        break;
    case '/':
    case TOK_PDIV:
    case TOK_UDIV:
    case '%':
    case TOK_UMOD:
        {
            int sgn = (op == '/' || op == TOK_PDIV || op == '%') ? 0x0800 : 0;
            int mod = op == '%' || op == TOK_UMOD;
            if (mod)
                save_regs(2);   /* a free register for the remainder */
            if (is_plain_const(vtop)) {
                vswap();
                r = gv(RC_INT);
                vswap();
                ea_imm(&e, vtop->c.i, 4);
            } else {
                gv2(RC_INT, RC_INT);
                r = vtop[-1].r;
                ea_dreg(&e, vtop[0].r);
            }
            use_reg(r);
            if (mod) {
                rr = get_reg(RC_INT);   /* the remainder */
                use_reg(rr);
                op_ext_ea(0x4C40, r << 12 | sgn | rr, &e);  /* divsl.l e,rr:r */
                vtop--;
                vtop->r = rr;
            } else {
                op_ext_ea(0x4C40, r << 12 | sgn | r, &e);   /* divs.l e,r */
                vtop--;
            }
        }
        break;
    default:
        /* comparisons */
        if (is_plain_const(vtop)) {
            vswap();
            r = gv(RC_INT);
            vswap();
            c = vtop->c.i;
            if (c == 0) {
                o16(0x4A80 | r);                    /* tst.l */
            } else {
                o16(0x0C80 | r);                    /* cmpi.l #c */
                o32(c);
            }
        } else {
            gv2(RC_INT, RC_INT);
            r = vtop[-1].r;
            fr = vtop[0].r;
            ea_dreg(&e, fr);
            op_ea(0xB080 | r << 9, &e);             /* cmp.l fr,r */
        }
        vtop--;
        vset_VT_CMP(op);
        if (!nocode_wanted)
            cc_live = 1;
        break;
    }
}

/* the soft-float helper for an operation on 'bt' (VT_FLOAT or VT_DOUBLE) */
static int float_helper(int op, int dbl)
{
    switch (op) {
    case '+': return dbl ? TOK___adddf3 : TOK___addsf3;
    case '-': return dbl ? TOK___subdf3 : TOK___subsf3;
    case '*': return dbl ? TOK___muldf3 : TOK___mulsf3;
    case '/': return dbl ? TOK___divdf3 : TOK___divsf3;
    case TOK_EQ: return dbl ? TOK___eqdf2 : TOK___eqsf2;
    case TOK_NE: return dbl ? TOK___nedf2 : TOK___nesf2;
    case TOK_LT: case TOK_ULT: return dbl ? TOK___ltdf2 : TOK___ltsf2;
    case TOK_LE: case TOK_ULE: return dbl ? TOK___ledf2 : TOK___lesf2;
    case TOK_GT: case TOK_UGT: return dbl ? TOK___gtdf2 : TOK___gtsf2;
    case TOK_GE: case TOK_UGE: return dbl ? TOK___gedf2 : TOK___gesf2;
    }
    tcc_internal_error("m68k: float operation");
    return 0;
}

/* set vtop to the result registers of a call returning type t */
static void set_ret(int t)
{
    int bt = t & VT_BTYPE;
    vtop->type.t = t;
    if (bt == VT_LLONG || bt == VT_DOUBLE || bt == VT_LDOUBLE)
        vtop->r = REG_IRET, vtop->r2 = REG_IRE2;
    else
        vtop->r = REG_IRET1, vtop->r2 = VT_CONST;
}

/* call a helper with the top n values as its arguments, the result of
   type t replacing them */
static void call_helper(int tok, int n, int t)
{
    vpush_helper_func(tok);
    vrott(n + 1);
    gfunc_call(n);
    vpushi(0);
    set_ret(t);
}

/* a floating point operation 'v = t1 op t2' (the two of the same type),
   or TOK_NEG: in software, through libgcc */
ST_FUNC void gen_opf(int op)
{
    int t = vtop->type.t & VT_BTYPE;
    int dbl = t != VT_FLOAT;

    if (op == TOK_NEG) {
        int r;
        gv(RC_INT);
        r = dbl ? vtop->r2 : vtop->r;
        use_reg(r);
        o16(0x0840 | r);        /* bchg #31,r: the sign */
        o16(31);
        return;
    }
    if (op >= TOK_ULT && op <= TOK_GT) {
        /* the helper's int result compared with 0 as gcc does it:
           __eqdf2 == 0, __ltdf2 < 0, __gedf2 >= 0, ... */
        int cmp = op;
        if (cmp == TOK_ULT) cmp = TOK_LT;
        if (cmp == TOK_ULE) cmp = TOK_LE;
        if (cmp == TOK_UGT) cmp = TOK_GT;
        if (cmp == TOK_UGE) cmp = TOK_GE;
        call_helper(float_helper(op, dbl), 2, VT_INT);
        vpushi(0);
        gen_op(cmp);
        return;
    }
    call_helper(float_helper(op, dbl), 2, vtop->type.t);
}

/* convert integers to fp 't' type: int, unsigned int and long long (the
   unsigned long long case is tccgen's) */
ST_FUNC void gen_cvt_itof(int t)
{
    int st = vtop->type.t & (VT_BTYPE | VT_UNSIGNED);
    int dbl = (t & VT_BTYPE) != VT_FLOAT;
    int tok;

    if ((st & VT_BTYPE) == VT_LLONG)
        tok = dbl ? TOK___floatdidf : TOK___floatdisf;
    else if (st == (VT_INT | VT_UNSIGNED))
        tok = dbl ? TOK___floatunsidf : TOK___floatunsisf;
    else
        tok = dbl ? TOK___floatsidf : TOK___floatsisf;
    call_helper(tok, 1, t);
}

/* convert fp to int 't' type: int, unsigned int, long long */
ST_FUNC void gen_cvt_ftoi(int t)
{
    int dbl = (vtop->type.t & VT_BTYPE) != VT_FLOAT;
    int tok;

    if ((t & VT_BTYPE) == VT_LLONG)
        tok = (t & VT_UNSIGNED) ? (dbl ? TOK___fixunsdfdi : TOK___fixunssfdi)
                                : (dbl ? TOK___fixdfdi : TOK___fixsfdi);
    else if (t & VT_UNSIGNED)
        tok = dbl ? TOK___fixunsdfsi : TOK___fixunssfsi;
    else
        tok = dbl ? TOK___fixdfsi : TOK___fixsfsi;
    call_helper(tok, 1, t);
}

/* convert from one floating point type to another */
ST_FUNC void gen_cvt_ftof(int t)
{
    int st = vtop->type.t & VT_BTYPE;
    int dt = t & VT_BTYPE;
    if (st == VT_FLOAT && dt != VT_FLOAT)
        call_helper(TOK___extendsfdf2, 1, t);
    else if (st != VT_FLOAT && dt == VT_FLOAT)
        call_helper(TOK___truncdfsf2, 1, t);
    else
        vtop->type.t = t;       /* double and long double are one type */
}

/* char/short to int conversion in a register */
ST_FUNC void gen_cvt_csti(int t)
{
    int r = gv(RC_INT);
    use_reg(r);
    if ((t & VT_BTYPE) == VT_SHORT) {
        if (t & VT_UNSIGNED)
            o16(0x0280 | r), o32(0xffff);       /* andi.l #$ffff */
        else
            o16(0x48C0 | r);                    /* ext.l */
    } else {
        if (t & VT_UNSIGNED)
            o16(0x0280 | r), o32(0xff);         /* andi.l #$ff */
        else
            o16(0x49C0 | r);                    /* extb.l */
    }
}

/* gcc's "BLKmode that forces the containing struct to BLKmode": an
   aggregate whose size is not 1, 2, 4 or 8 bytes (and not 0), or that
   holds such a member (gcc's compute_record_mode) */
static int blk_forcing(CType *t)
{
    int size, align;
    Sym *f;

    if (!(t->t & VT_ARRAY) && (t->t & VT_BTYPE) != VT_STRUCT)
        return 0;
    size = type_size(t, &align);
    if (size < 0)
        return 1;               /* a flexible array: BLKmode */
    if (size == 0)
        return 0;
    if (size != 1 && size != 2 && size != 4 && size != 8)
        return 1;
    if (t->t & VT_ARRAY)
        return blk_forcing(&t->ref->type);
    for (f = t->ref->next; f; f = f->next)
        if (blk_forcing(&f->type))
            return 1;
    return 0;
}

/* Return the number of registers needed to return the struct, or 0 if
   returning via struct pointer. gcc's m68k_return_in_memory: in memory a
   struct of BLKmode (blk_forcing) or one aligned below its integer mode
   (2 bytes for 2, 4 and 8); else in d0 or d0:d1 like an integer of its
   size. */
ST_FUNC int gfunc_sret(CType *vt, int variadic, CType *ret, int *ret_align, int *regsize)
{
    int size, align;
    size = type_size(vt, &align);
    *ret_align = 1;
    *regsize = 4;
    ret->ref = NULL;
    if (blk_forcing(vt) || (size > 1 && align < 2))
        return 0;
    if (size == 1)
        ret->t = VT_BYTE;
    else if (size == 2)
        ret->t = VT_SHORT;
    else if (size == 4)
        ret->t = VT_INT;
    else
        ret->t = VT_LLONG;
    return 1;
}

/* does a call to a function of type 'fs' pass a hidden struct pointer? */
static int call_sret(Sym *fs)
{
    CType ret;
    int ra, rs;
    if ((fs->type.t & VT_BTYPE) != VT_STRUCT)
        return 0;
    return gfunc_sret(&fs->type, fs->f.func_type == FUNC_ELLIPSIS, &ret, &ra, &rs) == 0;
}

/* push the long at e on the stack */
static void push_ea(EA *e)
{
    EA d;
    ea_predec(&d, REG_SP);
    emit_move(4, e, &d);
}

static void push_dreg(int r)
{
    o16(0x2F00 | r);            /* move.l dr,-(sp) */
}

static void push_imm(uint32_t c)
{
    if (fits16(c)) {
        o16(0x4878);            /* pea c.w */
        o16(c);
    } else {
        o16(0x2F3C);            /* move.l #c,-(sp) */
        o32(c);
    }
}

/* add n to sp, flags untouched (lea) */
static void gadd_sp(int n)
{
    EA e;
    if (n == 0)
        return;
    if (n > 0 && n <= 8) {
        o16(0x508F | (n & 7) << 9);     /* addq.l #n,sp (no flags on An) */
    } else {
        ea_disp(&e, REG_SP, n);
        emit_lea(&e, REG_SP);
    }
}

/* push vtop, an argument: return its size on the stack */
static int push_arg(void)
{
    int bt = vtop->type.t & VT_BTYPE;
    int size, align, slot, r;
    EA e;

    if (bt == VT_STRUCT) {
        size = type_size(&vtop->type, &align);
        slot = (size + 3) & ~3;
        gadd_sp(-slot);
        r = get_reg(RC_INT);
        use_reg(r);
        /* the struct's place: a small one lies at the end of its slot */
        ea_disp(&e, REG_SP, size < 4 ? 4 - size : 0);
        emit_lea(&e, REG_A0);
        ea_areg(&e, REG_A0);
        {
            EA d;
            ea_dreg(&d, r);
            emit_move(4, &e, &d);
        }
        vset(&vtop->type, r | VT_LVAL, 0);
        vswap();
        vstore();
        return slot;
    }
    if (bt == VT_LLONG || bt == VT_DOUBLE || bt == VT_LDOUBLE) {
        if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST) {
            uint64_t ll = vtop->c.i;
            push_imm((uint32_t)ll);
            push_imm((uint32_t)(ll >> 32));
        } else if ((vtop->r & VT_LVAL) && !(vtop->type.t & VT_BITFIELD)) {
            ea_mem(&e, vtop, REG_A0);
            ea_add(&e, 4);
            push_ea(&e);        /* the low word, then the high one */
            ea_add(&e, -4);
            push_ea(&e);
        } else {
            gv(RC_INT);
            use_reg(vtop->r);
            use_reg(vtop->r2);
            push_dreg(vtop->r);
            push_dreg(vtop->r2);
        }
        return 8;
    }
    if ((vtop->r & (VT_VALMASK | VT_LVAL)) == VT_CONST) {
        if (vtop->r & VT_SYM) {
            ea_abs(&e, vtop->sym, vtop->c.i);
            op_ea(0x4840, &e);  /* pea sym */
        } else {
            push_imm(const_bits(vtop));
        }
    } else if ((vtop->r & VT_LVAL) && !(vtop->type.t & VT_BITFIELD)
               && mem_size(vtop->type.t) == 4 && bt != VT_FUNC) {
        ea_mem(&e, vtop, REG_A0);
        push_ea(&e);
    } else {
        r = gv(RC_INT);
        push_dreg(r);
    }
    return 4;
}

/* Generate function call. The function address is pushed first, then
   all the parameters in call order. This functions pops all the
   parameters and the function address. */
ST_FUNC void gfunc_call(int nb_args)
{
    int args_size = 0, sret, i, fr = -1;
    Sym *fs;
    EA e;

    fs = vtop[-nb_args].type.ref;
    sret = call_sret(fs);
    save_regs(nb_args + 1);
    for (i = nb_args - sret; i > 0; i--) {
        args_size += push_arg();
        vtop--;
    }
    if (sret) {
        /* the hidden struct pointer goes in a0, set last */
        if ((vtop[-1].r & (VT_VALMASK | VT_LVAL | VT_SYM)) != (VT_CONST | VT_SYM)) {
            vswap();
            fr = gv(RC_INT);
            vswap();
        }
        if ((vtop->r & (VT_VALMASK | VT_LVAL)) == VT_LOCAL) {
            ea_disp(&e, REG_FP, vtop->c.i);
            emit_lea(&e, REG_A0);
        } else {
            ea_dreg(&e, gv(RC_INT));
            emit_movea(&e, REG_A0);
        }
        vtop--;
    }
    if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == (VT_CONST | VT_SYM)) {
        /* bsr.l sym: PC-relative, no relocation in the program */
        o16(0x61FF);
        if (!nocode_wanted)
            greloca(cur_text_section, vtop->sym, ind, R_68K_PC32, vtop->c.i);
        o32(0);
    } else {
        if (fr < 0)
            fr = gv(RC_INT);
        ea_dreg(&e, fr);
        emit_movea(&e, REG_A1);
        o16(0x4E91);            /* jsr (a1) */
    }
    gadd_sp(args_size);
    vtop--;
}

/* generate function prolog of type 't' */
ST_FUNC void gfunc_prolog(Sym *func_sym)
{
    CType *func_type = &func_sym->type;
    Sym *sym;
    int addr, size, align;
    EA e, s;

    sym = func_type->ref;
    addr = 8;           /* the saved a6 and the return address */
    loc = 0;
    func_vc = 0;
    func_regs_used = 0;
    cc_live = 0;
    func_prolog_ind = ind;
    ind += PROLOG_SIZE;

    if (call_sret(sym)) {
        /* the hidden pointer, from a0 */
        loc -= 4;
        func_vc = loc;
        ea_areg(&s, REG_A0);
        ea_disp(&e, REG_FP, loc);
        emit_move(4, &s, &e);
    }
    while ((sym = sym->next) != NULL) {
        CType *type = &sym->type;
        size = type_size(type, &align);
        gfunc_set_param(sym, size < 4 ? addr + 4 - size : addr, 0);
        addr += (size + 3) & ~3;
    }
}

/* generate function epilog */
ST_FUNC void gfunc_epilog(void)
{
    int v, saved_ind, mask, rmask, n, i;
    EA e;

    v = (-loc + 3) & -4;
    mask = 0, rmask = 0, n = 0;
    for (i = 2; i < 16; i++) {
        if (func_regs_used & (1 << i)) {
            mask |= 1 << i;             /* movem's order: d0 = bit 0 */
            rmask |= 0x8000 >> i;       /* -(sp)'s order: d0 = bit 15 */
            n++;
        }
    }
    if (func_vc) {
        /* gcc's callee gives the struct's address back in d0 */
        EA d;
        ea_disp(&e, REG_FP, func_vc);
        ea_dreg(&d, TREG_D0);
        emit_move(4, &e, &d);
    }
    if (mask) {
        ea_disp(&e, REG_FP, -v - 4 * n);
        o16(0x4CC0 | EA_FIELD(&e));     /* movem.l e,regs */
        o16(mask);
        emit_ea(&e);
    }
    o16(0x4E5E);                        /* unlk a6 */
    o16(0x4E75);                        /* rts */

    saved_ind = ind;
    ind = func_prolog_ind;
    o16(0x480E);                        /* link.l a6,#-v */
    o32(-v);
    if (mask) {
        o16(0x48E7);                    /* movem.l regs,-(sp) */
        o16(rmask);
    } else {
        o16(0x4FEF);                    /* lea 0(sp),sp: nothing */
        o16(0);
    }
    ind = saved_ind;
}

/* computed goto support */
ST_FUNC void ggoto(void)
{
    EA e;
    ea_dreg(&e, gv(RC_INT));
    emit_movea(&e, REG_A0);
    o16(0x4ED0);                        /* jmp (a0) */
    vtop--;
}

/* Save the stack pointer onto the stack */
ST_FUNC void gen_vla_sp_save(int addr)
{
    EA s, d;
    ea_areg(&s, REG_SP);
    ea_disp(&d, REG_FP, addr);
    emit_move(4, &s, &d);
}

/* Restore the SP from a location on the stack */
ST_FUNC void gen_vla_sp_restore(int addr)
{
    EA s;
    ea_disp(&s, REG_FP, addr);
    emit_movea(&s, REG_SP);
}

/* Subtract from the stack pointer, and push the resulting value onto the stack */
ST_FUNC void gen_vla_alloc(CType *type, int align)
{
    int r;
    if (align < 4)
        align = 4;
    r = gv(RC_INT);
    use_reg(r);
    o16(0x9FC0 | r);                    /* suba.l dr,sp */
    o16(0x200F | r << 9);               /* move.l sp,dr */
    o16(0x0280 | r);                    /* andi.l #-align,dr */
    o32(-align);
    o16(0x2E40 | r);                    /* movea.l dr,sp */
    vpop();
}

/* struct copy: vtop the source address, vtop[-1] the destination's */
ST_FUNC void gen_struct_copy(int size)
{
    int n = size / 4, r, rs;
    EA e;

    gv2(RC_INT, RC_INT);
    r = vtop[-1].r;
    rs = vtop[0].r;
    ea_dreg(&e, r);
    emit_movea(&e, REG_A1);
    ea_dreg(&e, rs);
    emit_movea(&e, REG_A0);
    if (n <= 8) {
        while (n--)
            o16(0x22D8);                /* move.l (a0)+,(a1)+ */
    } else {
        int loop;
        use_reg(rs);
        o16(0x2000 | rs << 9 | 0x3C);   /* move.l #n,rs */
        o32(n);
        loop = ind;
        o16(0x22D8);
        o16(0x5380 | rs);               /* subq.l #1,rs */
        o16(0x6600 | ((loop - (ind + 2)) & 0xff));  /* bne.s loop */
    }
    if (size & 2)
        o16(0x32D8);                    /* move.w (a0)+,(a1)+ */
    if (size & 1)
        o16(0x12D8);                    /* move.b (a0)+,(a1)+ */
    vpop();
    vpop();
}

/* increment tcov counter (64 bits, big-endian) */
ST_FUNC void gen_increment_tcov(SValue *sv)
{
    EA e;
    ea_abs(&e, sv->sym, sv->c.i + 4);
    op_ea(0x5280, &e);                  /* addq.l #1,low */
    o16(0x6400 | (2 + e.n * 2));        /* bcc.s over the next */
    ea_abs(&e, sv->sym, sv->c.i);
    op_ea(0x5280, &e);                  /* addq.l #1,high */
}

#ifdef CONFIG_TCC_ASM
#include "m68k-asm.c"
#endif

/* end of m68k code generator */
/*************************************************************/
#endif
/*************************************************************/
