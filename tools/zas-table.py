#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""The instruction table test's input for zas (tools/zas-host-test.sh, DESIGN D52 section 6): every mnemonic of
user/zas.c with every size suffix and every operand combination from a pool that covers the addressing modes (both
syntaxes: (d,%an,%xn.l*4) and %an@(d,%xn:l:4)), immediates, registers, lists, pairs, bitfields and symbols. Most
combinations are invalid; the test assembles the whole file with GNU as and with zas for each CPU and needs the same
lines refused and the same object for the lines accepted.

    tools/zas-table.py [all|int|fpu|mmu|branch]     the lines on standard output
"""
import sys

EA = ["%d3", "%a2", "(%a1)", "(%a4)+", "-(%a5)", "0x1234(%a2)", "(-8,%a6)", "0(%a3)", "8(%a0,%d1.w)",
      "(%a3,%a2.l*4)", "(0x12345,%a0,%d1.l*8)", "([4,%a1],%d2.w*2,8)", "([%a1,%d2.l],0x100)", "(0x1234.w,%a0)",
      "(%d3.l*2)", "(%za1,%d0)", "([0x10,%a2])", "0x1234.w", "0x123456", "0x7000", "-4", "4(%pc)",
      "(-8,%pc)", "6(%pc,%d0.w)", "(%pc,%a1.l*2)", "([2,%pc],%d1)", "#5", "#-100", "#0x12345678", "ext", "loc",
      "ext+4(%a0)", "loc(%pc)", "ext(%pc)", "(loc,%pc,%d0.w)", "%a0@", "%a0@+", "%a0@-", "%a0@(8)",
      "%a0@(8,%d0:l:4)", "%pc@(2,%d0:w)", "%a1@(4)@(8)"]
SMALL = ["%d3", "%a2", "(%a1)", "(%a4)+", "-(%a5)", "0x1234(%a2)", "8(%a0,%d1.w)", "(0x12345,%a0,%d1.l*8)",
         "0x1234.w", "0x123456", "4(%pc)", "6(%pc,%d0.w)", "#5", "#1", "#8", "#0", "#-1", "#300", "ext", "loc",
         "loc(%pc)"]
SPECIAL = ["%ccr", "%sr", "%usp", "%vbr", "%cacr", "%sfc", "%dfc", "%msp", "%isp", "%tc", "%itt0", "%dtt1",
           "%mmusr", "%urp", "%srp", "%pcr", "%buscr", "%caar", "%crp", "%tt0", "%tt1", "%psr"]
LISTS = ["%d0-%d2/%a0", "%d1", "%a6", "%d0-%d7/%a0-%a6", "#0x8001"]
FPR = ["%fp0", "%fp7", "%fpcr", "%fpsr", "%fpiar", "%fpcr/%fpsr", "%fpcr/%fpsr/%fpiar", "%fp0-%fp3", "%fp1/%fp3",
       "#0r1.5", "#0r-2.5e-3", "#0x3f800000", "%fp2:%fp3"]

SIZES = ["", ".b", ".w", ".l"]
FSIZES = ["", ".b", ".w", ".l", ".s", ".d", ".x", ".p"]
INT2 = ["add", "adda", "addi", "addq", "addx", "and", "andi", "cmp", "cmpa", "cmpi", "cmpm", "eor", "eori", "move",
        "movea", "or", "ori", "sub", "suba", "subi", "subq", "subx", "abcd", "sbcd", "chk", "chk2", "cmp2", "divs",
        "divu", "divsl", "divul", "muls", "mulu", "exg", "lea", "movem", "movep", "moveq", "moves", "movec", "link",
        "btst", "bchg", "bclr", "bset", "asl", "asr", "lsl", "lsr", "rol", "ror", "roxl", "roxr", "callm", "move16",
        "cinvl", "cinvp", "cpushl", "cpushp"]
INT1 = ["clr", "neg", "negx", "not", "tst", "nbcd", "tas", "jmp", "jsr", "pea", "ext", "extb", "swap", "unlk",
        "trap", "bkpt", "stop", "rtd", "rtm", "lpstop", "asl", "lsr", "roxl", "rol", "cinva", "cpusha", "pflush",
        "pflushn", "plpar", "plpaw", "ptestr", "ptestw", "frestore", "fsave", "seq", "st", "sf", "shi", "trapeq",
        "trapv"]
INT0 = ["nop", "rts", "rte", "rtr", "reset", "trapv", "illegal", "pflusha", "pflushan", "fnop", "trapt", "trapf",
        "trapcc", "ftrapeq", "ftrapt"]
BF = ["bftst", "bfextu", "bfexts", "bfffo", "bfchg", "bfclr", "bfset", "bfins"]
BFOP = ["%d1{3:5}", "(%a0){%d1:%d2}", "8(%a0){#0:32}", "0x1234{0:8}", "(%a0){32:1}", "loc{1:2}"]
FOPS = ["fabs", "facos", "fadd", "fasin", "fatan", "fatanh", "fcmp", "fcos", "fcosh", "fdiv", "fetox", "fetoxm1",
        "fgetexp", "fgetman", "fint", "fintrz", "flog10", "flog2", "flogn", "flognp1", "fmod", "fmul", "fneg", "frem",
        "fscale", "fsgldiv", "fsglmul", "fsin", "fsinh", "fsqrt", "fsub", "ftan", "ftanh", "ftentox", "ftwotox",
        "fsabs", "fdabs", "fsadd", "fdadd", "fsdiv", "fddiv", "fsmove", "fdmove", "fsmul", "fdmul", "fsneg", "fdneg",
        "fssqrt", "fdsqrt", "fssub", "fdsub"]
CC = ["t", "f", "hi", "ls", "cc", "cs", "ne", "eq", "vc", "vs", "pl", "mi", "ge", "lt", "gt", "le", "hs", "lo"]
FCC = ["f", "eq", "ogt", "oge", "olt", "ole", "ogl", "or", "un", "ueq", "ugt", "uge", "ult", "ule", "ne", "t", "sf",
       "seq", "gt", "ge", "lt", "le", "gl", "gle", "ngle", "ngl", "nle", "nlt", "nge", "ngt", "sne", "st"]


# GNU as's mistakes, which zas does not copy, and a form that stops as 2.45 with an internal error (docs/man/zas.txt, "Differences from GNU as"): a PC-relative destination
# for movem and fsave (no such mode), a movep displacement's symbol dropped, an extended-precision immediate's bits
# shifted (fmove.x #0r1.5), bfins with its operands the other way round taken as an absolute address
def gas_wrong(l):
    m, _, ops = l.partition(" ")
    if m.startswith("movem") and ops.split(",")[-1] in ("loc", "ext") and not ops.split(",")[0] in ("loc", "ext"):
        return True
    if m in ("fsave",) and ops in ("loc", "ext"):
        return True
    if m.startswith("movep") and "ext" in ops:
        return True
    if "#0r" in ops and m.endswith((".x", ".p")):
        return True
    if m.startswith("fmovem") and ops.startswith("#") and "%fp" in ops:     # (as 2.45 stops with an internal error)
        return True
    # forms the FPU does not have, which gas takes: %dn with .d/.x/.p, %fpcr/%fpsr to or from %an, a packed
    # immediate, a floating-point number as a register list
    o = ops.split(",")
    if m.startswith("f") and m.endswith((".d", ".x", ".p")) and o[0].startswith("%d") and not m.startswith("fmovem"):
        return True
    if m.startswith("fmove") and any(x in ("%fpcr", "%fpsr") for x in o) and any(x.startswith("%a") for x in o):
        return True
    if m.endswith(".p") and o[0].startswith("#"):
        return True
    if m.startswith("fmovem") and any(x.startswith("#0r") or x == "#0x3f800000" for x in o):
        return True
    if m.startswith("b") and m.endswith((".s", ".b")) and ops == "ext":    # (gas: an 8-bit relocation; see the man page)
        return True
    if m == "fmovem" and any(x == "%d2" for x in ops.split(",")):     # (gas writes it and reports an error)
        return True
    return m == "bfins" and ops.endswith(",%d4")


def out(lines):
    for l in lines:
        if not gas_wrong(l):
            print("\t" + l)


def gen_int():
    out(f"{m}{s} {a},{b}" for m in INT2 for s in SIZES for a in EA for b in SMALL)
    out(f"{m}{s} {a},{b}" for m in ["move", "add", "cmp", "and", "sub"] for s in SIZES for a in SMALL for b in EA)
    out(f"{m}{s} {a}" for m in INT1 for s in SIZES for a in EA + ["#3"])
    out(f"{m}" for m in INT0)
    # registers: move to and from the special ones, movec, the lists
    out(f"move{s} {a},{b}" for s in SIZES for a in SPECIAL + ["%d1", "%a1", "(%a0)", "#3"]
        for b in SPECIAL + ["%d1", "%a1", "(%a0)"])
    out(f"movec {a},{b}" for a in SPECIAL + ["%d1", "%a1"] for b in SPECIAL + ["%d1", "%a1"])
    out(f"{m}{s} {a},{b}" for m in ["movem"] for s in SIZES for a in LISTS for b in EA)
    out(f"{m}{s} {a},{b}" for m in ["movem"] for s in SIZES for a in EA for b in LISTS)
    out(f"{m}{s} {a},{b}" for m in ["andi", "ori", "eori", "and", "or", "eor"] for s in SIZES for a in ["#3", "#0x700"]
        for b in ["%ccr", "%sr"])
    # the 68020 forms with register pairs
    out(f"{m}{s} {a},%d0:%d1" for m in ["divs", "divu", "divsl", "divul", "muls", "mulu"] for s in SIZES
        for a in SMALL)
    out(f"cas{s} %d0,%d1,{a}" for s in SIZES for a in EA)
    out(f"cas2{s} %d0:%d1,%d2:%d3,{a}" for s in SIZES for a in ["(%a0):(%a1)", "(%d0):(%a7)", "(%a0)"])
    out(f"{m} {a},{b},#{c}" for m in ["pack", "unpk"] for a in ["%d0", "-(%a0)"] for b in ["%d1", "-(%a1)"]
        for c in [0, 0x1234])
    out(f"{m} {a},{b}" for m in BF for a in BFOP for b in ["%d4"])
    out(f"{m} {a},{b}" for m in ["bfins"] for a in ["%d4"] for b in BFOP)
    out(f"{m} {a}" for m in BF for a in BFOP)
    out(f"trap{c}{s} {a}" for c in CC for s in ["", ".w", ".l"] for a in ["#5", "#0x12345"])
    out(f"trap{c}{s}" for c in CC for s in ["", ".w", ".l"])
    out(f"s{c}{s} {a}" for c in CC for s in ["", ".b"] for a in ["%d2", "(%a0)", "0x1234", "%a2", "#1"])
    out(f"move16 {a},{b}" for a in ["(%a0)+", "(%a1)", "0x12345", "%d0"] for b in ["(%a2)+", "(%a3)", "0x123456"])
    out(f"{m} {a},{b}" for m in ["cinvl", "cinvp", "cpushl", "cpushp"] for a in ["%dc", "%ic", "%bc", "%nc"]
        for b in ["(%a0)", "%a0", "(%a0)+"])
    out(f"{m} {a}" for m in ["cinva", "cpusha"] for a in ["%dc", "%ic", "%bc", "%nc", "%d0"])
    out(f"{m}{s} {a},{b}" for m in ["exg"] for s in SIZES for a in ["%d1", "%a1"] for b in ["%d2", "%a2"])


def gen_mmu():
    regs = ["%tc", "%srp", "%crp", "%tt0", "%tt1", "%mmusr", "%psr"]
    out(f"{m}{s} {a},{b}" for m in ["pmove", "pmovefd"] for s in ["", ".w", ".l", ".d", ".q"]
        for a in regs + ["(%a0)", "%d0", "0x1234"] for b in regs + ["(%a1)", "%d1", "0x123456"])
    out(f"pflush {a},#{b}" for a in ["%sfc", "%dfc", "%d3", "#5", "#0x1f"] for b in [0, 7, 8])
    out(f"pflush {a},#3,{b}" for a in ["%sfc", "%d3", "#5"] for b in ["(%a0)", "0x1234", "%d0"])
    out(f"{m} {a},{b}" for m in ["ploadr", "ploadw"] for a in ["%sfc", "%d3", "#5"] for b in ["(%a0)", "0x1234", "%d0"])
    out(f"{m} {a},(%a0),#{b}{c}" for m in ["ptestr", "ptestw"] for a in ["%dfc", "%d3", "#5"] for b in [0, 7]
        for c in ["", ",%a2"])
    out(f"{m} {a}" for m in ["pflush", "pflushn", "ptestr", "ptestw", "plpar", "plpaw"] for a in ["(%a0)", "%a0", "%d0"])


def gen_fpu():
    srcs = ["%fp1", "%d2", "(%a0)", "0x1234(%a2)", "8(%a0,%d1.w)", "0x123456", "4(%pc)", "#3", "#0r1.5", "loc",
            "-(%a1)", "%a1"]
    out(f"{m}{s} {a},%fp3" for m in FOPS + ["fmove"] for s in FSIZES for a in srcs)
    out(f"{m}{s} %fp4" for m in FOPS + ["ftst"] for s in FSIZES)
    out(f"ftst{s} {a}" for s in FSIZES for a in srcs)
    out(f"fsincos{s} {a},%fp1:%fp2" for s in FSIZES for a in srcs)
    out(f"fmove{s} %fp3,{a}" for s in FSIZES for a in srcs + ["(%a0){#3}", "(%a0){%d2}", "%d0"])
    out(f"fmove{s} {a},{b}" for s in FSIZES for a in FPR + ["%d0", "%a0", "(%a0)", "#5"]
        for b in FPR + ["%d1", "%a1", "(%a1)"])
    out(f"fmovem{s} {a},{b}" for s in ["", ".l", ".x"] for a in FPR + ["%d2", "#0xc0"]
        for b in ["(%a0)", "-(%a7)", "(%a1)+", "0x1234(%a2)", "4(%pc)", "%d0", "loc"])
    out(f"fmovem{s} {a},{b}" for s in ["", ".l", ".x"] for a in ["(%a0)", "-(%a7)", "(%a1)+", "0x1234(%a2)", "4(%pc)",
                                                               "%d0", "loc", "#0x12"] for b in FPR + ["%d2", "#0xc0"])
    out(f"fmovecr{s} #{a},%fp{b}" for s in ["", ".x", ".d"] for a in [0, 0x32, 0x3f, 0x80] for b in [0, 7])
    out(f"fs{c}{s} {a}" for c in FCC for s in ["", ".b"] for a in ["%d2", "(%a0)", "%a1"])
    out(f"ftrap{c}{s} {a}" for c in FCC for s in [".w", ".l"] for a in ["#5"])
    out(f"ftrap{c}" for c in FCC)
    out(f"{m} {a}" for m in ["fsave", "frestore"] for a in ["(%a0)", "-(%a7)", "(%a7)+", "0x1234", "%d0", "4(%pc)"])


def gen_branch():
    print("loc0:")
    out(f"b{c}{s} {t}" for c in CC + ["ra", "sr"] for s in ["", ".s", ".b", ".w", ".l"] for t in ["loc", "ext", "loc0"])
    out(f"j{c} {t}" for c in CC + ["ra", "bsr", "bra"] for t in ["loc", "ext", "loc0", "(%a0)", "4(%a1)"])
    out(f"db{c}{s} %d{i},{t}" for c in CC + ["ra"] for s in ["", ".w"] for i in [0, 7] for t in ["loc", "ext", "loc0"])
    out(f"fb{c}{s} {t}" for c in FCC for s in ["", ".w", ".l"] for t in ["loc", "ext", "loc0"])
    out(f"fdb{c} %d1,{t}" for c in FCC for t in ["loc", "ext", "loc0"])


what = sys.argv[1] if len(sys.argv) > 1 else "all"
print("\t.text")
print("loc:\tnop")
if what in ("all", "int"):
    gen_int()
if what in ("all", "mmu"):
    gen_mmu()
if what in ("all", "fpu"):
    gen_fpu()
if what in ("all", "branch"):
    gen_branch()
print("\tnop")
