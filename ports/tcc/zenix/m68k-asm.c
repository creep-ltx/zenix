/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 *  m68k inline assembler for TCC (Zenix): GNU as's 68k syntax, a subset
 *
 *  Copyright (c) 2026 the Zenix authors
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  Included by m68k-gen.c (it shares its effective-address helpers).
 *  What GNU C's asm statements need: operands bound to registers by
 *  their constraints (r, d, a, m, i, n, g, digits, +, =, &), register
 *  variables (register int x asm("d0")), clobbers, and the instructions
 *  inline asm uses, in gas's syntax with % registers ("move.l %d0,
 *  -(%sp)", "(4,%a0,%d1.l*4)", "-4(%a6)"): move, movea, moveq, lea, pea,
 *  add/sub/and/or/eor/cmp and their a/i/q forms, tst, clr, neg, not,
 *  ext, extb, swap, the shifts and rotates, mul/div (.w and 32-bit .l),
 *  the branches (Bcc, DBcc, Scc, bra, bsr, jmp, jsr) to labels, rts,
 *  rte, rtr, nop, illegal, trap, link, unlk, movem, btst/bset/bclr/
 *  bchg, exg, move from and to %sr and %ccr, %usp. tcc's assembler
 *  directives (.byte, .long, .section ...) come from tccasm.c. No
 *  floating point, no MIT syntax, no relaxation (a forward branch is
 *  a word branch). For .s files Zenix has zas.
 */

/* asm register numbers: 0-7 d0-d7, 8-15 a0-a7 (tccgen's are d0-d4) */
#define AREG(n) (8 + (n))

enum { O_DREG = 1, O_AREG, O_IND, O_POSTINC, O_PREDEC, O_DISP, O_INDEX, O_ABS, O_IMM, O_SR, O_CCR, O_USP, O_LIST,
       O_PCDISP, O_PCINDEX };

typedef struct AOp {
    int kind;
    int reg;            /* the register (0-7) or the list's mask (d0 = bit 0) */
    int xreg, xlong, scale;     /* the index (asm number) */
    ExprValue e;        /* displacement, address or immediate */
    int dreg2;          /* Dr:Dq's second register, or -1 */
} AOp;

ST_FUNC void g(int c)
{
    ob(c);
}

ST_FUNC void gen_le16(int c)
{
    o16(c);             /* the target's order (big-endian) */
}

ST_FUNC void gen_le32(int c)
{
    o32(c);
}

ST_FUNC void gen_expr32(ExprValue *pe)
{
    if (pe->pcrel && !pe->sym)
        tcc_error("a PC-relative value with no symbol");
    /* tccasm gives sym - label as pcrel with v = addend - label's offset:
       S + A - P wants A = v + this place's offset */
    if (pe->sym && !nocode_wanted)
        greloca(cur_text_section, pe->sym, ind, pe->pcrel ? R_68K_PC32 : R_68K_32,
                (int)pe->v + (pe->pcrel ? ind : 0));
    o32(pe->sym ? 0 : pe->v);
}

/* a register's name (with or without %): its asm number, or -1 */
static int asm_reg_by_name(const char *s)
{
    if (*s == '%')
        s++;
    if ((s[0] == 'd' || s[0] == 'a') && s[1] >= '0' && s[1] <= '7' && !s[2])
        return (s[0] == 'a' ? 8 : 0) + s[1] - '0';
    if (!strcmp(s, "sp"))
        return AREG(7);
    if (!strcmp(s, "fp"))
        return AREG(6);
    return -1;
}

ST_FUNC int asm_parse_regvar(int t)
{
    if (t < TOK_IDENT || (t & SYM_FIELD))
        return -1;
    return asm_reg_by_name(get_tok_str(t, NULL));
}

/* after '%': a register; its name may carry an index size (d1.l) */
static int parse_reg(int *xlong)
{
    char name[16];
    const char *s, *dot;
    int r;
    if (tok < TOK_IDENT)
        expect("register");
    s = get_tok_str(tok, NULL);
    dot = strchr(s, '.');
    snprintf(name, sizeof name, "%.*s", dot ? (int)(dot - s) : (int)strlen(s), s);
    if (xlong)
        *xlong = !dot || dot[1] == 'l' || dot[1] == 'L';
    next();
    if (!strcmp(name, "pc"))
        return 16;
    if (!strcmp(name, "sr"))
        return 17;
    if (!strcmp(name, "ccr"))
        return 18;
    if (!strcmp(name, "usp"))
        return 19;
    r = asm_reg_by_name(name);
    if (r < 0)
        tcc_error("unknown register '%%%s'", name);
    return r;
}

/* the inside of ( ... ): [disp,] base [, index[.s][*scale]] */
static void parse_paren(TCCState *s1, AOp *op)
{
    int r, have_disp = 0;
    next();             /* '(' */
    if (tok != '%') {
        asm_expr(s1, &op->e);
        have_disp = 1;
        skip(',');
    }
    skip('%');
    r = parse_reg(NULL);
    if (r == 16)
        op->kind = O_PCDISP;
    else if (r >= 8 && r < 16)
        op->kind = have_disp ? O_DISP : O_IND, op->reg = r - 8;
    else
        tcc_error("bad base register");
    if (tok == ',') {
        next();
        skip('%');
        op->xreg = parse_reg(&op->xlong);
        if (op->xreg > 15)
            tcc_error("bad index register");
        op->scale = 1;
        if (tok == '*') {
            next();
            op->scale = asm_int_expr(s1);
            if (op->scale != 1 && op->scale != 2 && op->scale != 4 && op->scale != 8)
                tcc_error("bad scale");
        }
        op->kind = op->kind == O_PCDISP ? O_PCINDEX : O_INDEX;
    }
    skip(')');
    if (op->kind == O_IND && tok == '+')
        next(), op->kind = O_POSTINC;
}

static void parse_aop(TCCState *s1, AOp *op)
{
    memset(op, 0, sizeof *op);
    op->dreg2 = -1;
    if (tok == '#') {
        next();
        asm_expr(s1, &op->e);
        op->kind = O_IMM;
        return;
    }
    if (tok == '%') {
        int r;
        next();
        r = parse_reg(NULL);
        if (r == 17 || r == 18 || r == 19) {
            op->kind = r == 17 ? O_SR : r == 18 ? O_CCR : O_USP;
            return;
        }
        if (r > 15)
            tcc_error("bad register");
        if (tok == '-' || tok == '/') {
            /* a register list: %d0-%d3/%a0 */
            int mask = 1 << r, last = r, t;
            while (tok == '-' || tok == '/') {
                int range = tok == '-';
                next();
                skip('%');
                t = parse_reg(NULL);
                if (t > 15)
                    tcc_error("bad register list");
                if (range) {
                    int i;
                    for (i = last; i <= t; i++)
                        mask |= 1 << i;
                } else {
                    mask |= 1 << t;
                }
                last = t;
            }
            op->kind = O_LIST, op->reg = mask;
            return;
        }
        if (tok == ':' && r < 8) {
            /* Dr:Dq */
            next();
            skip('%');
            op->dreg2 = parse_reg(NULL);
            if (op->dreg2 > 7)
                tcc_error("bad register pair");
        }
        op->kind = r < 8 ? O_DREG : O_AREG, op->reg = r & 7;
        return;
    }
    if (tok == '-') {
        next();
        if (tok == '(') {
            next();
            skip('%');
            op->reg = parse_reg(NULL);
            if (op->reg < 8 || op->reg > 15)
                tcc_error("bad predecrement register");
            op->reg -= 8;
            skip(')');
            op->kind = O_PREDEC;
            return;
        }
        unget_tok('-');
    }
    if (tok == '(') {
        parse_paren(s1, op);
        return;
    }
    asm_expr(s1, &op->e);
    if (tok == '(') {
        ExprValue d = op->e;
        parse_paren(s1, op);
        if (op->kind == O_IND)
            op->kind = O_DISP;
        if (op->kind == O_POSTINC)
            tcc_error("a displacement with (%%an)+");
        op->e = d;
        return;
    }
    op->kind = O_ABS;
}

/* an operand as an effective address (size: an immediate's) */
static void aop_ea(AOp *op, EA *e, int size, unsigned pc_ok)
{
    int d = (int)op->e.v;
    switch (op->kind) {
    case O_DREG: ea_dreg(e, op->reg); break;
    case O_AREG: ea_areg(e, op->reg); break;
    case O_IND: ea_ind(e, op->reg); break;
    case O_POSTINC: ea_postinc(e, op->reg); break;
    case O_PREDEC: ea_predec(e, op->reg); break;
    case O_DISP:
        if (op->e.sym)
            tcc_error("a symbol as a displacement");
        ea_disp(e, op->reg, d);
        break;
    case O_INDEX:
    case O_PCINDEX:
        if (op->e.sym)
            tcc_error("a symbol as a displacement");
        if (op->kind == O_PCINDEX)
            ea_clear(e, 7, 3);
        else
            ea_clear(e, 6, op->reg);
        {
            int ext = (op->xreg >= 8) << 15 | (op->xreg & 7) << 12 | op->xlong << 11
                      | (op->scale == 2 ? 1 : op->scale == 4 ? 2 : op->scale == 8 ? 3 : 0) << 9;
            if (fits8(d)) {
                e->w[e->n++] = ext | (d & 0xff);
            } else {
                e->w[e->n++] = ext | 0x0130;    /* full format, 32-bit bd */
                ea_long(e, d);
            }
        }
        break;
    case O_PCDISP:
        if (op->e.sym)
            tcc_error("a symbol as a displacement");
        if (!fits16(d))
            tcc_error("PC displacement out of range");
        ea_clear(e, 7, 2);
        e->w[e->n++] = d;
        break;
    case O_ABS:
        if (op->e.pcrel)
            tcc_error("a PC-relative expression as an address");
        ea_abs(e, op->e.sym, d);
        break;
    case O_IMM:
        if (op->e.pcrel)
            tcc_error("a PC-relative immediate (write the label's own address)");
        if (op->e.sym) {
            if (size != 4)
                tcc_error("a symbol in a short immediate");
            ea_imm_sym(e, op->e.sym, d);
        } else {
            ea_imm(e, (uint32_t)op->e.v, size);
        }
        break;
    default:
        tcc_error("operand not allowed here");
    }
    (void)pc_ok;
}

static int is_mem(AOp *op)
{
    return (op->kind >= O_IND && op->kind <= O_ABS) || op->kind == O_PCDISP || op->kind == O_PCINDEX;
}

static const char *const cc_names[16] = {
    "t", "f", "hi", "ls", "cc", "cs", "ne", "eq", "vc", "vs", "pl", "mi", "ge", "lt", "gt", "le"
};

static int cc_by_name(const char *s)
{
    int i;
    if (!strcmp(s, "hs"))
        return CC_CC;
    if (!strcmp(s, "lo"))
        return CC_CS;
    for (i = 0; i < 16; i++)
        if (!strcmp(s, cc_names[i]))
            return i;
    return -1;
}

/* a branch's target: its displacement from the extension word (pc),
   or a relocation of the given type */
static void branch_disp(ExprValue *e, int pc, int bytes)
{
    ElfSym *esym = e->sym ? elfsym(e->sym) : NULL;
    if (!e->sym) {
        tcc_error("a branch to a number");
    } else if (esym && esym->st_shndx == cur_text_section->sh_num) {
        int d = esym->st_value + (int)e->v - pc;
        if (bytes == 2 && !fits16(d))
            tcc_error("branch out of range");
        if (bytes == 2)
            o16(d);
        else
            o32(d);
    } else {
        if (!nocode_wanted)
            greloca(cur_text_section, e->sym, ind, bytes == 2 ? R_68K_PC16 : R_68K_PC32, (int)e->v);
        if (bytes == 2)
            o16(0);
        else
            o32(0);
    }
}

/* operand checks (gas refuses these; tcc must not emit another instruction) */
static void no_areg(AOp *op, const char *what)
{
    if (op->kind == O_AREG)
        tcc_error("%s: an address register is not allowed here", what);
}

static void alterable_data(AOp *op, const char *what)
{
    no_areg(op, what);
    if (op->kind == O_IMM || op->kind == O_PCDISP || op->kind == O_PCINDEX || op->kind > O_PCINDEX
        || op->kind == O_SR || op->kind == O_CCR || op->kind == O_USP || op->kind == O_LIST)
        tcc_error("%s: the operand cannot be written", what);
}

static int size_bits(int size)
{
    return size == 1 ? 0 : size == 2 ? 1 : 2;
}

/* an operand's register number for the opcode's register field */
static int need_dreg(AOp *op)
{
    if (op->kind != O_DREG)
        tcc_error("data register expected");
    return op->reg;
}

static int need_areg(AOp *op)
{
    if (op->kind != O_AREG)
        tcc_error("address register expected");
    return op->reg;
}

static int imm_value(AOp *op)
{
    if (op->kind != O_IMM || op->e.sym)
        tcc_error("constant immediate expected");
    return (int)op->e.v;
}

ST_FUNC void asm_opcode(TCCState *s1, int opcode)
{
    char base[32];
    const char *name = get_tok_str(opcode, NULL), *dot;
    int size = 0, n = 0, cc, sz;
    AOp ops[3];
    EA e, f;

    dot = strchr(name, '.');
    snprintf(base, sizeof base, "%.*s", dot ? (int)(dot - name) : (int)strlen(name), name);
    if (dot) {
        switch (dot[1]) {
        case 'b': case 's': size = 1; break;
        case 'w': size = 2; break;
        case 'l': size = 4; break;
        default: tcc_error("unknown size '%s'", dot);
        }
        if (dot[2])
            tcc_error("unknown size '%s'", dot);
    }
    if (tok != ';' && tok != TOK_LINEFEED) {
        for (;;) {
            if (n == 3)
                tcc_error("too many operands");
            parse_aop(s1, &ops[n++]);
            if (tok != ',')
                break;
            next();
        }
    }
#define NOPS(k) do { if (n != (k)) tcc_error("'%s' takes %d operand%s", name, k, (k) == 1 ? "" : "s"); } while (0)
    sz = size ? size : 2;

    /* no operands */
    if (!strcmp(base, "rts")) { NOPS(0); o16(0x4E75); return; }
    if (!strcmp(base, "rte")) { NOPS(0); o16(0x4E73); return; }
    if (!strcmp(base, "rtr")) { NOPS(0); o16(0x4E77); return; }
    if (!strcmp(base, "nop")) { NOPS(0); o16(0x4E71); return; }
    if (!strcmp(base, "illegal")) { NOPS(0); o16(0x4AFC); return; }
    if (!strcmp(base, "trap")) {
        NOPS(1);
        n = imm_value(&ops[0]);
        if (n < 0 || n > 15)
            tcc_error("trap number out of range");
        o16(0x4E40 | n);
        return;
    }
    if (!strcmp(base, "swap")) { NOPS(1); o16(0x4840 | need_dreg(&ops[0])); return; }
    if (!strcmp(base, "ext")) {
        NOPS(1);
        if (size == 1)
            tcc_error("ext.b does not exist");
        o16((size == 4 ? 0x48C0 : 0x4880) | need_dreg(&ops[0]));
        return;
    }
    if (!strcmp(base, "extb")) { NOPS(1); o16(0x49C0 | need_dreg(&ops[0])); return; }
    if (!strcmp(base, "unlk")) { NOPS(1); o16(0x4E58 | need_areg(&ops[0])); return; }
    if (!strcmp(base, "link")) {
        NOPS(2);
        n = need_areg(&ops[0]);
        cc = imm_value(&ops[1]);
        if (size == 4 || !fits16(cc))
            o16(0x4808 | n), o32(cc);
        else
            o16(0x4E50 | n), o16(cc);
        return;
    }
    if (!strcmp(base, "moveq")) {
        NOPS(2);
        cc = imm_value(&ops[0]);
        if (!fits8(cc))
            tcc_error("moveq: value out of range");
        o16(0x7000 | need_dreg(&ops[1]) << 9 | (cc & 0xff));
        return;
    }
    if (!strcmp(base, "lea") || !strcmp(base, "pea") || !strcmp(base, "jmp") || !strcmp(base, "jsr")) {
        int lea = base[0] == 'l';
        NOPS(lea ? 2 : 1);
        if (!is_mem(&ops[0]) || ops[0].kind == O_POSTINC || ops[0].kind == O_PREDEC)
            tcc_error("'%s' needs a control address", name);
        aop_ea(&ops[0], &e, 4, 1);
        op_ea(lea ? 0x41C0 | need_areg(&ops[1]) << 9 : base[0] == 'p' ? 0x4840 : base[1] == 'm' ? 0x4EC0 : 0x4E80, &e);
        return;
    }
    if (!strcmp(base, "exg")) {
        NOPS(2);
        if (ops[0].kind == O_DREG && ops[1].kind == O_DREG)
            o16(0xC140 | ops[0].reg << 9 | ops[1].reg);
        else if (ops[0].kind == O_AREG && ops[1].kind == O_AREG)
            o16(0xC148 | ops[0].reg << 9 | ops[1].reg);
        else if (ops[0].kind == O_DREG && ops[1].kind == O_AREG)
            o16(0xC188 | ops[0].reg << 9 | ops[1].reg);
        else if (ops[0].kind == O_AREG && ops[1].kind == O_DREG)
            o16(0xC188 | ops[1].reg << 9 | ops[0].reg);
        else
            tcc_error("exg takes two registers");
        return;
    }
    if (!strcmp(base, "move") || !strcmp(base, "movea")) {
        NOPS(2);
        if (ops[0].kind == O_SR || ops[0].kind == O_CCR) {
            aop_ea(&ops[1], &e, 2, 0);
            op_ea(ops[0].kind == O_SR ? 0x40C0 : 0x42C0, &e);
            return;
        }
        if (ops[1].kind == O_SR || ops[1].kind == O_CCR) {
            aop_ea(&ops[0], &e, 2, 1);
            op_ea(ops[1].kind == O_SR ? 0x46C0 : 0x44C0, &e);
            return;
        }
        if (ops[0].kind == O_USP) { o16(0x4E68 | need_areg(&ops[1])); return; }
        if (ops[1].kind == O_USP) { o16(0x4E60 | need_areg(&ops[0])); return; }
        if (!size)
            size = 2;
        if (ops[1].kind == O_AREG) {
            if (size == 1)
                tcc_error("movea.b does not exist");
            aop_ea(&ops[0], &e, size, 1);
            op_ea((size == 4 ? 0x2040 : 0x3040) | ops[1].reg << 9, &e);
            return;
        }
        if (ops[1].kind == O_IMM || ops[1].kind == O_PCDISP || ops[1].kind == O_PCINDEX)
            tcc_error("move to an immediate or PC-relative operand");
        if (size == 1 && ops[0].kind == O_AREG)
            tcc_error("move.b from an address register");
        aop_ea(&ops[0], &e, size, 1);
        aop_ea(&ops[1], &f, size, 0);
        emit_move(size, &e, &f);
        return;
    }
    if (!strcmp(base, "movem")) {
        int list, mask, i, ea_i, to_mem;
        NOPS(2);
        to_mem = ops[0].kind == O_LIST || ops[0].kind == O_DREG || ops[0].kind == O_AREG;
        list = to_mem ? 0 : 1;
        ea_i = 1 - list;
        mask = ops[list].kind == O_LIST ? ops[list].reg
             : ops[list].kind == O_DREG ? 1 << ops[list].reg
             : ops[list].kind == O_AREG ? 1 << (8 + ops[list].reg) : -1;
        if (mask < 0 || !is_mem(&ops[ea_i]))
            tcc_error("movem takes a register list and an address");
        if ((to_mem && (ops[ea_i].kind == O_POSTINC || ops[ea_i].kind == O_PCDISP || ops[ea_i].kind == O_PCINDEX))
            || (!to_mem && ops[ea_i].kind == O_PREDEC))
            tcc_error("movem: this addressing mode is not allowed in that direction");
        if (ops[ea_i].kind == O_PREDEC) {
            int r = 0;
            for (i = 0; i < 16; i++)
                if (mask & (1 << i))
                    r |= 0x8000 >> i;
            mask = r;
        }
        aop_ea(&ops[ea_i], &e, 4, 1);
        o16(0x4880 | (!to_mem) << 10 | (size == 4) << 6 | EA_FIELD(&e));
        o16(mask);
        emit_ea(&e);
        return;
    }
    if (!strcmp(base, "addq") || !strcmp(base, "subq")) {
        NOPS(2);
        cc = imm_value(&ops[0]);
        if (cc < 1 || cc > 8)
            tcc_error("%s: value out of range", base);
        if (ops[1].kind == O_AREG && size == 1)
            tcc_error("%s.b to an address register", base);
        if (ops[1].kind != O_AREG)
            alterable_data(&ops[1], name);
        aop_ea(&ops[1], &e, sz, 0);
        op_ea((base[0] == 'a' ? 0x5000 : 0x5100) | (cc & 7) << 9 | (ops[1].kind == O_AREG ? (size == 2 ? 1 : 2) : size_bits(sz)) << 6, &e);
        return;
    }
    if (!strcmp(base, "add") || !strcmp(base, "sub") || !strcmp(base, "and") || !strcmp(base, "or")
        || !strcmp(base, "eor") || !strcmp(base, "cmp") || !strcmp(base, "adda") || !strcmp(base, "suba")
        || !strcmp(base, "cmpa") || !strcmp(base, "addi") || !strcmp(base, "subi") || !strcmp(base, "andi")
        || !strcmp(base, "ori") || !strcmp(base, "eori") || !strcmp(base, "cmpi")) {
        static const struct { const char *n; unsigned dn, imm; } t[] = {
            { "add", 0xD000, 0x0600 }, { "sub", 0x9000, 0x0400 }, { "and", 0xC000, 0x0200 },
            { "or", 0x8000, 0x0000 }, { "eor", 0xB000, 0x0A00 }, { "cmp", 0xB000, 0x0C00 },
        };
        int k, len = strlen(base);
        char b2[32];
        snprintf(b2, sizeof b2, "%s", base);
        if (len > 2 && (b2[len - 1] == 'a' || b2[len - 1] == 'i') && strcmp(b2, "cmp"))
            b2[len - 1] = 0;
        for (k = 0; k < 6 && strcmp(t[k].n, b2); k++)
            ;
        NOPS(2);
        if (!size)
            size = 2;
        if (ops[1].kind == O_AREG && (k == 0 || k == 1 || k == 5)) {
            /* adda/suba/cmpa */
            if (size == 1)
                tcc_error("%s.b with an address register", b2);
            aop_ea(&ops[0], &e, size, 1);
            op_ea(t[k].dn | ops[1].reg << 9 | (size == 4 ? 0x1C0 : 0x0C0), &e);
            return;
        }
        if ((ops[1].kind == O_SR || ops[1].kind == O_CCR) && (k == 2 || k == 3 || k == 4)) {
            o16(t[k].imm | (ops[1].kind == O_SR ? 0x7C : 0x3C));
            o16(imm_value(&ops[0]) & (ops[1].kind == O_SR ? 0xffff : 0xff));
            return;
        }
        if (k == 2 || k == 3 || k == 4)
            no_areg(&ops[1], name), no_areg(&ops[0], name);
        if (ops[0].kind == O_IMM) {     /* (as gas: addi, not add #,dn) */
            /* the immediate forms */
            if (k != 5)
                alterable_data(&ops[1], name);
            aop_ea(&ops[1], &f, size, 0);
            o16(t[k].imm | size_bits(size) << 6 | EA_FIELD(&f));
            aop_ea(&ops[0], &e, size, 0);
            emit_ea(&e);
            emit_ea(&f);
            return;
        }
        if (ops[1].kind == O_DREG && k != 4) {
            aop_ea(&ops[0], &e, size, 1);
            op_ea(t[k].dn | ops[1].reg << 9 | size_bits(size) << 6, &e);
            return;
        }
        if (ops[0].kind == O_DREG && k != 5) {
            alterable_data(&ops[1], name);
            aop_ea(&ops[1], &e, size, 0);
            op_ea(t[k].dn | ops[0].reg << 9 | (4 + size_bits(size)) << 6, &e);
            return;
        }
        tcc_error("'%s': operands not supported", name);
    }
    if (!strcmp(base, "tst") || !strcmp(base, "clr") || !strcmp(base, "neg") || !strcmp(base, "not")) {
        NOPS(1);
        if (base[0] == 't')
            no_areg(&ops[0], name);
        else
            alterable_data(&ops[0], name);
        aop_ea(&ops[0], &e, sz, base[0] == 't');
        op_ea((base[0] == 't' ? 0x4A00 : base[0] == 'c' ? 0x4200 : base[1] == 'e' ? 0x4400 : 0x4600)
              | size_bits(sz) << 6, &e);
        return;
    }
    if (!strcmp(base, "muls") || !strcmp(base, "mulu") || !strcmp(base, "divs") || !strcmp(base, "divu")
        || !strcmp(base, "divsl") || !strcmp(base, "divul")) {
        int sgn = base[3] == 's';       /* muls, divs, divsl */
        int mul = base[0] == 'm';
        NOPS(2);
        aop_ea(&ops[0], &e, size == 4 || base[4] ? 4 : 2, 1);
        if (size == 4 || base[4]) {
            int q = need_dreg(&ops[1]);
            int r = ops[1].dreg2 >= 0 ? ops[1].dreg2 : q;
            if (ops[1].dreg2 >= 0) {
                q = ops[1].dreg2, r = ops[1].reg;      /* Dr:Dq */
                if (mul || !base[4])
                    tcc_error("%s with a register pair: the 64-bit form (no 68060 has it)", name);
            }
            if (mul)
                op_ext_ea(0x4C00, q << 12 | (sgn ? 0x0800 : 0), &e);
            else
                op_ext_ea(0x4C40, q << 12 | (sgn ? 0x0800 : 0) | r, &e);
        } else {
            op_ea((mul ? 0xC0C0 : 0x80C0) | (sgn ? 0x100 : 0) | need_dreg(&ops[1]) << 9, &e);
        }
        return;
    }
    {
        /* shifts and rotates on a data register */
        static const char *const sh[] = { "asr", "asl", "lsr", "lsl", "roxr", "roxl", "ror", "rol" };
        int k;
        for (k = 0; k < 8 && strcmp(base, sh[k]); k++)
            ;
        if (k < 8) {
            unsigned opc = 0xE000 | (k & 1) << 8 | size_bits(sz) << 6 | (k >> 1) << 3;
            NOPS(2);
            n = need_dreg(&ops[1]);
            if (ops[0].kind == O_IMM) {
                cc = imm_value(&ops[0]);
                if (cc < 1 || cc > 8)
                    tcc_error("shift count out of range");
                o16(opc | (cc & 7) << 9 | n);
            } else {
                o16(opc | 0x20 | need_dreg(&ops[0]) << 9 | n);
            }
            return;
        }
    }
    {
        static const char *const bits[] = { "btst", "bchg", "bclr", "bset" };
        int k;
        for (k = 0; k < 4 && strcmp(base, bits[k]); k++)
            ;
        if (k < 4) {
            NOPS(2);
            no_areg(&ops[1], name);
            if (k != 0)
                alterable_data(&ops[1], name);
            aop_ea(&ops[1], &e, 1, k == 0);
            if (ops[0].kind == O_IMM) {
                o16(0x0800 | k << 6 | EA_FIELD(&e));
                o16(imm_value(&ops[0]) & 0xff);
                emit_ea(&e);
            } else {
                op_ea(0x0100 | k << 6 | need_dreg(&ops[0]) << 9, &e);
            }
            return;
        }
    }
    /* branches: bra, bsr, bCC, dbCC (dbra), sCC */
    if (base[0] == 'b' && (!strcmp(base, "bra") || !strcmp(base, "bsr") || (cc = cc_by_name(base + 1)) >= 2)) {
        int pc, opc;
        NOPS(1);
        if (ops[0].kind != O_ABS)
            tcc_error("a branch takes a label");
        opc = !strcmp(base, "bra") ? 0x6000 : !strcmp(base, "bsr") ? 0x6100 : 0x6000 | cc_by_name(base + 1) << 8;
        pc = ind + 2;
        if (size == 1) {
            ElfSym *esym = ops[0].e.sym ? elfsym(ops[0].e.sym) : NULL;
            int d;
            if (!esym || esym->st_shndx != cur_text_section->sh_num)
                tcc_error("a short branch needs a label before it");
            d = esym->st_value + (int)ops[0].e.v - pc;
            if (!fits8(d) || d == 0 || d == -1)
                tcc_error("short branch out of range");
            o16(opc | (d & 0xff));
        } else if (size == 4) {
            o16(opc | 0xff);
            branch_disp(&ops[0].e, pc, 4);
        } else {
            o16(opc);
            branch_disp(&ops[0].e, pc, 2);
        }
        return;
    }
    if (base[0] == 'd' && base[1] == 'b' && ((cc = cc_by_name(base + 2)) >= 0 || !strcmp(base, "dbra"))) {
        NOPS(2);
        if (!strcmp(base, "dbra"))
            cc = CC_F;
        o16(0x50C8 | cc << 8 | need_dreg(&ops[0]));
        if (ops[1].kind != O_ABS)
            tcc_error("dbcc takes a label");
        branch_disp(&ops[1].e, ind, 2);
        return;
    }
    if (base[0] == 's' && (cc = cc_by_name(base + 1)) >= 0) {
        NOPS(1);
        alterable_data(&ops[0], name);
        aop_ea(&ops[0], &e, 1, 0);
        op_ea(0x50C0 | cc << 8, &e);
        return;
    }
    tcc_error("unknown or unsupported instruction '%s' (tcc's inline assembler knows a subset; zas assembles .s files)", name);
}

/* constraints: r (a data or address register), d, a, m, i, n, g, a
   digit (the same as that output), + = & modifiers */
static int constraint_priority(const char *str)
{
    int priority = 0, c, pr;
    for (;;) {
        c = *str++;
        if (!c)
            break;
        switch (c) {
        case 'd': case 'a': pr = 2; break;
        case 'r': pr = 3; break;
        case 'i': case 'n': case 'm': case 'g': case 'I': case 'J': case 'K': case 'L': case 'M': case 'N': case 'O':
        case 'P': case 'Q': case 'U': case 'V': case 'o': case 's': case 'X': case 'p': pr = 4; break;
        case '=': case '+': case '&': case '%': continue;
        default:
            tcc_error("unknown constraint '%c'", c);
            pr = 0;
        }
        if (pr > priority)
            priority = pr;
    }
    return priority;
}

static const char *skip_constraint_modifiers(const char *p)
{
    while (*p == '=' || *p == '&' || *p == '+' || *p == '%')
        p++;
    return p;
}

#define REG_OUT_MASK 0x01
#define REG_IN_MASK  0x02
#define is_reg_allocated(reg) (regs_allocated[reg] & reg_mask)

/* the registers the constraints may take, in this order: the caller's
   first, d2-d7 and a2-a5 (saved by the function's prolog) next; a0 and
   a1 (this file's scratch) only for register variables */
static const uint8_t data_order[] = { 0, 1, 2, 3, 4, 5, 6, 7 };
static const uint8_t addr_order[] = { 10, 11, 12, 13 };

ST_FUNC void asm_compute_constraints(ASMOperand *operands, int nb_operands, int nb_outputs,
                                     const uint8_t *clobber_regs, int *pout_reg)
{
    ASMOperand *op;
    int sorted_op[MAX_ASM_OPERANDS];
    int i, j, k, p1, p2, tmp, reg, c, reg_mask;
    const char *str;
    uint8_t regs_allocated[NB_ASM_REGS];

    for (i = 0; i < nb_operands; i++) {
        op = &operands[i];
        op->input_index = -1;
        op->ref_index = -1;
        op->reg = -1;
        op->is_memory = 0;
        op->is_rw = 0;
        op->is_llong = 0;
    }
    for (i = 0; i < nb_operands; i++) {
        op = &operands[i];
        str = skip_constraint_modifiers(op->constraint);
        if (isnum(*str) || *str == '[') {
            k = find_constraint(operands, nb_operands, str, NULL);
            if ((unsigned)k >= i || i < nb_outputs)
                tcc_error("invalid reference in constraint %d ('%s')", i, str);
            op->ref_index = k;
            if (operands[k].input_index >= 0)
                tcc_error("cannot reference twice the same operand");
            operands[k].input_index = i;
            op->priority = 5;
        } else if ((op->vt->r & VT_VALMASK) == VT_LOCAL && op->vt->sym
                   && (reg = op->vt->sym->r & VT_VALMASK) < VT_CONST) {
            op->priority = 1;
            op->reg = reg;
        } else {
            op->priority = constraint_priority(str);
        }
    }
    for (i = 0; i < nb_operands; i++)
        sorted_op[i] = i;
    for (i = 0; i < nb_operands - 1; i++) {
        for (j = i + 1; j < nb_operands; j++) {
            p1 = operands[sorted_op[i]].priority;
            p2 = operands[sorted_op[j]].priority;
            if (p2 < p1) {
                tmp = sorted_op[i];
                sorted_op[i] = sorted_op[j];
                sorted_op[j] = tmp;
            }
        }
    }
    for (i = 0; i < NB_ASM_REGS; i++)
        regs_allocated[i] = clobber_regs[i] ? REG_IN_MASK | REG_OUT_MASK : 0;
    regs_allocated[AREG(6)] = regs_allocated[AREG(7)] = REG_IN_MASK | REG_OUT_MASK;    /* the frame, the stack */

    for (i = 0; i < nb_operands; i++) {
        j = sorted_op[i];
        op = &operands[j];
        str = op->constraint;
        if (op->ref_index >= 0)
            continue;
        if (op->input_index >= 0)
            reg_mask = REG_IN_MASK | REG_OUT_MASK;
        else if (j < nb_outputs)
            reg_mask = REG_OUT_MASK;
        else
            reg_mask = REG_IN_MASK;
        if (op->reg >= 0) {
            if (is_reg_allocated(op->reg))
                tcc_error("asm regvar requests register that's taken already");
            reg = op->reg;
        }
    try_next:
        c = *str++;
        switch (c) {
        case '=':
        case '%':
            goto try_next;
        case '+':
            op->is_rw = 1;
            /* fall through */
        case '&':
            if (j >= nb_outputs)
                tcc_error("'%c' modifier can only be applied to outputs", c);
            reg_mask = REG_IN_MASK | REG_OUT_MASK;
            goto try_next;
        case 'd':
        case 'r':
        case 'a':
            if (op->reg >= 0) {
                reg = op->reg;
                if ((c == 'd' && reg >= 8) || (c == 'a' && reg < 8))
                    goto try_next;
                goto reg_found;
            }
            if (c != 'a')
                for (k = 0; k < (int)sizeof data_order; k++)
                    if (!is_reg_allocated(reg = data_order[k]))
                        goto reg_found;
            if (c != 'd')
                for (k = 0; k < (int)sizeof addr_order; k++)
                    if (!is_reg_allocated(reg = addr_order[k]))
                        goto reg_found;
            goto try_next;
        reg_found:
            op->reg = reg;
            regs_allocated[reg] |= reg_mask;
            break;
        case 'i':
        case 'n':
        case 'I': case 'J': case 'K': case 'L': case 'M': case 'N': case 'O': case 'P':
            if ((op->vt->r & (VT_VALMASK | VT_LVAL | (c == 'i' ? 0 : VT_SYM))) != VT_CONST)
                goto try_next;
            break;
        case 'm':
        case 'g':
        case 'o':
        case 'Q':
        case 'U':
        case 'V':
            if (j < nb_outputs || c != 'g') {
                if ((op->vt->r & VT_VALMASK) == VT_LLOCAL || ((op->vt->r & VT_LVAL)
                    && (op->vt->r & VT_VALMASK) < VT_CONST)) {
                    /* the address in an address register */
                    for (k = 0; k < (int)sizeof addr_order; k++)
                        if (!(regs_allocated[reg = addr_order[k]] & REG_IN_MASK))
                            goto reg_found1;
                    goto try_next;
                reg_found1:
                    regs_allocated[reg] |= REG_IN_MASK;
                    op->reg = reg;
                    op->is_memory = 1;
                }
            } else if (!(op->vt->r & VT_LVAL) && (op->vt->r & VT_VALMASK) != VT_CONST) {
                goto try_next;
            }
            break;
        default:
            tcc_error("asm constraint %d ('%s') could not be satisfied", j, op->constraint);
            break;
        }
        if (op->input_index >= 0) {
            operands[op->input_index].reg = op->reg;
            operands[op->input_index].is_memory = op->is_memory;
        }
    }
    /* out_reg: an address register to store an output through a saved
       pointer (VT_LLOCAL) */
    *pout_reg = -1;
    for (i = 0; i < nb_operands; i++) {
        op = &operands[i];
        if (op->reg >= 0 && (op->vt->r & VT_VALMASK) == VT_LLOCAL && !op->is_memory) {
            for (k = 0; k < (int)sizeof addr_order; k++)
                if (!(regs_allocated[reg = addr_order[k]] & REG_OUT_MASK))
                    goto reg_found2;
            tcc_error("could not find free output register for reloading");
        reg_found2:
            *pout_reg = reg;
            break;
        }
    }
}

static const char *asm_reg_name(int reg)
{
    static const char *const n[16] = { "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7",
                                       "a0", "a1", "a2", "a3", "a4", "a5", "a6", "sp" };
    return n[reg & 15];
}

ST_FUNC void subst_asm_operand(CString *add_str, SValue *sv, int modifier)
{
    int r = sv->r, reg;

    if ((r & VT_VALMASK) == VT_CONST) {
        if (!(r & VT_LVAL) && modifier != 'c' && modifier != 'n' && modifier != 'P')
            cstr_ccat(add_str, '#');
        if (r & VT_SYM) {
            const char *name = get_tok_str(sv->sym->v, NULL);
            if (sv->sym->v >= SYM_FIRST_ANOM)
                get_asm_sym(tok_alloc_const(name), sv->sym);
            cstr_cat(add_str, name, -1);
            if ((uint32_t)sv->c.i == 0)
                return;
            cstr_ccat(add_str, '+');
        }
        cstr_printf(add_str, "%d", modifier == 'n' ? -(int)sv->c.i : (int)sv->c.i);
    } else if ((r & VT_VALMASK) == VT_LOCAL) {
        cstr_printf(add_str, "%d(%%a6)", (int)sv->c.i);
    } else if (r & VT_LVAL) {
        reg = r & VT_VALMASK;
        if (reg < 8 || reg >= 16)
            tcc_error("a memory operand needs an address register");
        cstr_printf(add_str, "(%%%s)", asm_reg_name(reg));
    } else {
        reg = r & VT_VALMASK;
        if (reg >= 16)
            tcc_internal_error("asm operand");
        cstr_printf(add_str, "%%%s", asm_reg_name(reg));
    }
}

/* load an operand's value (or the address of a memory operand) in its
   register before the asm, store the outputs after it. Callee-saved
   registers the asm uses are marked so the prolog saves them. */
ST_FUNC void asm_gen_code(ASMOperand *operands, int nb_operands, int nb_outputs, int is_output,
                          uint8_t *clobber_regs, int out_reg)
{
    ASMOperand *op;
    int i, pass;

    for (i = 0; i < NB_ASM_REGS; i++)
        if (clobber_regs[i])
            use_reg(i);
    for (i = 0; i < nb_operands; i++)
        if (operands[i].reg >= 0)
            use_reg(operands[i].reg);
    if (!is_output) {
        /* data registers first, then a2-a7, a1, a0 (the loads use a0) */
        for (pass = 0; pass < 4; pass++) {
            for (i = 0; i < nb_operands; i++) {
                int r;
                op = &operands[i];
                r = op->reg;
                if (r < 0)
                    continue;
                if ((pass == 0) != (r < 8) || (pass == 1 && r < 10) || (pass == 2 && r != 9) || (pass == 3 && r != 8))
                    continue;
                if (op->is_memory && ((op->vt->r & VT_VALMASK) == VT_LLOCAL || (op->vt->r & VT_VALMASK) < VT_CONST)) {
                    SValue sv = *op->vt;
                    if ((sv.r & VT_VALMASK) == VT_LLOCAL)
                        sv.r = (sv.r & ~VT_VALMASK) | VT_LOCAL | VT_LVAL;
                    else
                        sv.r &= ~VT_LVAL;
                    sv.type.t = VT_PTR;
                    load(r, &sv);
                } else if (i >= nb_outputs || op->is_rw) {
                    load(r, op->vt);
                }
            }
        }
    } else {
        for (i = 0; i < nb_outputs; i++) {
            op = &operands[i];
            if (op->reg < 0 || op->is_memory)
                continue;
            if ((op->vt->r & VT_VALMASK) == VT_LLOCAL) {
                SValue sv = *op->vt;
                sv.r = (sv.r & ~VT_VALMASK) | VT_LOCAL | VT_LVAL;
                sv.type.t = VT_PTR;
                load(out_reg, &sv);
                sv = *op->vt;
                sv.r = (sv.r & ~VT_VALMASK) | out_reg | VT_LVAL;
                store(op->reg, &sv);
            } else {
                store(op->reg, op->vt);
            }
        }
    }
}

ST_FUNC void asm_clobber(uint8_t *clobber_regs, const char *str)
{
    int reg;
    if (!strcmp(str, "memory") || !strcmp(str, "cc"))
        return;
    reg = asm_reg_by_name(str);
    if (reg < 0)
        tcc_error("invalid clobber register '%s'", str);
    clobber_regs[reg] = 1;
}
