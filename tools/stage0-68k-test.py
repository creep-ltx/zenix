#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the real 68020 code of boot/stage0.c (build/stage0.bin) in a CPU emulator against a real
Raspberry Pi device tree, with a fake exec.library and devicetree.resource around it.

Why: stage0 calls the OS through register-based vectors and keeps a6 (the compiler's frame pointer)
intact across them, and it must work wherever the kernel image happens to be loaded. The host test
(tools/stage0-test.sh) runs the same logic on x86 and cannot see those things.

Needs the "unicorn" package (pip install unicorn, e.g. in a venv) and a CM4 device tree:
    tools/stage0-68k-test.py tools/testdata/bcm2711-rpi-cm4.dtb [build/stage0.bin]
The tree is given Emu68's treatment first (the ranges of /soc and /scb rewritten to start at
0xF2000000, see raspi/start_rpi64.c in Emu68) and the /emu68 properties are added.
"""
import struct
import sys

from unicorn import (Uc, UC_ARCH_M68K, UC_MODE_BIG_ENDIAN, UC_HOOK_CODE, UcError)
from unicorn.m68k_const import (UC_M68K_REG_A0, UC_M68K_REG_A1, UC_M68K_REG_A6, UC_M68K_REG_A7,
                                UC_M68K_REG_D0, UC_M68K_REG_D1, UC_M68K_REG_D2, UC_M68K_REG_D3,
                                UC_M68K_REG_PC, UC_M68K_REG_A2, UC_M68K_REG_A3, UC_M68K_REG_A4,
                                UC_M68K_REG_A5, UC_M68K_REG_D4, UC_M68K_REG_D5, UC_M68K_REG_D6,
                                UC_M68K_REG_D7)
try:
    from unicorn.m68k_const import UC_CPU_M68K_M68020
except ImportError:                                     # older bindings
    UC_CPU_M68K_M68020 = None

# ---------------------------------------------------------------- device tree (FDT) -> python tree
class Node:
    def __init__(self, name, parent):
        self.name, self.parent, self.kids, self.props = name, parent, [], {}
        self.addr = 0


def parse_fdt(blob):
    magic, total, off_struct, off_strings = struct.unpack(">IIII", blob[:16])
    assert magic == 0xD00DFEED
    pos, root, cur = off_struct, None, None
    while True:
        tok = struct.unpack(">I", blob[pos:pos + 4])[0]
        pos += 4
        if tok == 1:
            end = blob.index(b"\0", pos)
            name = blob[pos:end].decode()
            pos = (end + 1 + 3) & ~3
            n = Node(name, cur)
            if cur:
                cur.kids.append(n)
            else:
                root = n
            cur = n
        elif tok == 2:
            cur = cur.parent
        elif tok == 3:
            ln, noff = struct.unpack(">II", blob[pos:pos + 8])
            pos += 8
            s = off_strings + noff
            pname = blob[s:blob.index(b"\0", s)].decode()
            cur.props[pname] = bytearray(blob[pos:pos + ln])
            pos = (pos + ln + 3) & ~3
        elif tok == 4:
            continue
        elif tok == 9:
            break
    return root


def find(root, path):
    n = root
    for part in [p for p in path.split("/") if p]:
        n = next((k for k in n.kids if k.name == part), None)
        if n is None:
            return None
    return n


def cells(prop):
    return list(struct.unpack(">%dI" % (len(prop) // 4), bytes(prop)))


def emu68_rewrite(root):
    """port of map_peripheral_ranges(): consecutive 2 MB aligned windows from 0xF2000000"""
    state = {"map": 0xF20 // 2}
    for path in ("/soc", "/scb"):
        e = find(root, path)
        if not e or "ranges" not in e.props:
            continue
        r = cells(e.props["ranges"])
        acpu = cells(e.parent.props["#address-cells"])[0]
        abus = cells(e.props["#address-cells"])[0] if "#address-cells" in e.props else 2
        sbus = cells(e.props["#size-cells"])[0] if "#size-cells" in e.props else 1
        step = abus + acpu + sbus
        for i in range(0, len(r), step):
            b, cpu, size = r[i + abus - 1], r[i + abus + acpu - 1], r[i + step - 1]
            if path == "/scb" and b == cpu:
                continue
            r[i + abus + acpu - 1] = (state["map"] << 21) & 0xFFFFFFFF
            state["map"] += (size + (1 << 21) - 1) >> 21
        e.props["ranges"] = bytearray(struct.pack(">%dI" % len(r), *r))
    # what Emu68 adds
    em = Node("emu68", root)
    root.kids.append(em)
    em.props["dcache-range-ops"] = bytearray(struct.pack(">I", 1))
    em.props["vc4-mem"] = bytearray(struct.pack(">II", 0x30000000, 16 << 20))
    eth = find(root, "/scb/ethernet@7d580000")
    eth.props["local-mac-address"] = bytearray([0xDC, 0xA6, 0x32, 1, 2, 3])


# ---------------------------------------------------------------- memory image of the tree (Emu68's layout)
TREE_BASE = 0x00800000


class Image:
    def __init__(self, base):
        self.base, self.buf = base, bytearray()

    def alloc(self, data, align=4):
        while len(self.buf) % align:
            self.buf.append(0)
        a = self.base + len(self.buf)
        self.buf += data
        return a

    def put32(self, addr, v):
        struct.pack_into(">I", self.buf, addr - self.base, v)


def lay_out(img, node):
    """of_node_t {next, parent, name, children, properties}; of_property_t {next, name, length, value}"""
    node.addr = img.alloc(b"\0" * 20)
    name = img.alloc(node.name.encode() + b"\0", 1)
    img.put32(node.addr + 8, name)
    prev = 0
    for pname, val in node.props.items():
        p = img.alloc(b"\0" * 16)
        pn = img.alloc(pname.encode() + b"\0", 1)
        pv = img.alloc(bytes(val) + b"\0", 4)
        img.put32(p + 4, pn)
        img.put32(p + 8, len(val))
        img.put32(p + 12, pv)
        if prev:
            img.put32(prev, p)
        else:
            img.put32(node.addr + 16, p)
        prev = p
    prevk = 0
    for k in node.kids:
        lay_out(img, k)
        img.put32(k.addr + 4, node.addr)
        if prevk:
            img.put32(prevk.addr, k.addr)
        else:
            img.put32(node.addr + 12, k.addr)
        prevk = k


# ---------------------------------------------------------------- the emulated machine
EXEC_BASE = 0x00300000
DT_BASE = 0x00400000                 # resource base: vectors at DT_BASE-6, -12, ...
HOOK_BASE = 0x00500000               # jump targets of the vectors, 8 bytes apart
EXP_BASE = 0x00380000                # fake expansion.library (FindConfigDev at -72)
CD_BASE = 0x00390000                 # fake ConfigDev images, 64 bytes each
HALT = 0x00700000
STACK_TOP = 0x00680000
HWINFO = 0x1100
FIELDS = ["magic", "flags", "soc_base", "mailbox", "emmc", "gpio", "genet", "genet_irq0", "genet_irq1",
          "phy_addr"]


BOARDS = [  # type, product, er_flags, cd_flags, manufacturer, serial, addr, size
    (0xE0, 0x0A, 0x40, 2, 0x082C, 0x12345678, 0x00200000, 0x00400000),
    (0x90, 0x21, 0x00, 0, 0x6D73, 0, 0x40000000, 0x00010000),
]


def run(blob_bytes, base, root, with_dt=True, with_boards=True):
    uc = Uc(UC_ARCH_M68K, UC_MODE_BIG_ENDIAN)
    if UC_CPU_M68K_M68020 is not None:
        uc.ctl_set_cpu_model(UC_CPU_M68K_M68020) if hasattr(uc, "ctl_set_cpu_model") else None
    uc.mem_map(0, 0x2000)                                        # low RAM with the hwinfo table
    uc.mem_map(0x00200000, 0x00600000)                          # code, exec, resource, stack
    img = Image(TREE_BASE)
    lay_out(img, root)
    uc.mem_map(TREE_BASE, (len(img.buf) + 0xFFFF) & ~0xFFFF)
    uc.mem_write(TREE_BASE, bytes(img.buf))
    uc.mem_write(base, blob_bytes)
    # fake exec: OpenResource (-498) and a way back; fake resource: jmp.l hook for each of 11 vectors
    uc.mem_write(EXEC_BASE - 498, b"\x4e\xf9" + struct.pack(">I", HOOK_BASE + 0x100))
    uc.mem_write(EXEC_BASE - 552, b"\x4e\xf9" + struct.pack(">I", HOOK_BASE + 0x140))      # OpenLibrary
    uc.mem_write(EXP_BASE - 72, b"\x4e\xf9" + struct.pack(">I", HOOK_BASE + 0x180))        # FindConfigDev
    cds = []
    for i, (typ, prod, erfl, cdfl, mfr, ser, addr, size) in enumerate(BOARDS):
        img = bytearray(64)
        img[14] = cdfl; img[16] = typ; img[17] = prod; img[18] = erfl
        struct.pack_into(">H", img, 20, mfr); struct.pack_into(">I", img, 22, ser)
        struct.pack_into(">I", img, 32, addr); struct.pack_into(">I", img, 36, size)
        uc.mem_write(CD_BASE + 64 * i, bytes(img))
        cds.append(CD_BASE + 64 * i)
    for i in range(1, 12):
        uc.mem_write(DT_BASE - 6 * i, b"\x4e\xf9" + struct.pack(">I", HOOK_BASE + 8 * i))
    uc.mem_write(HALT, b"\x60\xfe")                              # bra.s *
    names = {1: "OpenKey", 2: "CloseKey", 3: "GetChild", 4: "FindProperty", 5: "GetProperty", 6: "GetPropLen",
             7: "GetPropName", 8: "GetPropValue", 9: "GetParent", 10: "GetKeyName", 11: "FindPropertyRecursive"}
    by_addr = {}
    nodes = {}

    def index(n):
        nodes[n.addr] = n
        for k in n.kids:
            index(k)
    index(root)

    def rd_str(a):
        s = b""
        while True:
            c = bytes(uc.mem_read(a, 1))
            if c == b"\0":
                return s.decode()
            s += c
            a += 1

    def prop_addr(n, name):
        p = struct.unpack(">I", bytes(uc.mem_read(n.addr + 16, 4)))[0]
        while p:
            nm = struct.unpack(">I", bytes(uc.mem_read(p + 4, 4)))[0]
            if rd_str(nm) == name:
                return p
            p = struct.unpack(">I", bytes(uc.mem_read(p, 4)))[0]
        return 0

    state = {"pending": None, "calls": {}, "a6_ok": True}

    def hook(uc, addr, size, _):
        if addr == HOOK_BASE + 0x100:                            # exec OpenResource, name in a1
            name = rd_str(uc.reg_read(UC_M68K_REG_A1))
            assert name == "devicetree.resource", name
            assert uc.reg_read(UC_M68K_REG_A6) == EXEC_BASE, "a6 is not ExecBase at OpenResource"
            uc.reg_write(UC_M68K_REG_D0, DT_BASE if with_dt else 0)
        elif addr == HOOK_BASE + 0x140:                          # exec OpenLibrary(name a1, version d0)
            name = rd_str(uc.reg_read(UC_M68K_REG_A1))
            assert name == "expansion.library", name
            assert uc.reg_read(UC_M68K_REG_A6) == EXEC_BASE, "a6 is not ExecBase at OpenLibrary"
            uc.reg_write(UC_M68K_REG_D0, EXP_BASE if with_boards else 0)
        elif addr == HOOK_BASE + 0x180:                          # FindConfigDev(prev a0, mfr d0, product d1)
            assert uc.reg_read(UC_M68K_REG_A6) == EXP_BASE, "a6 is not the expansion base"
            assert uc.reg_read(UC_M68K_REG_D0) == 0xFFFFFFFF and uc.reg_read(UC_M68K_REG_D1) == 0xFFFFFFFF, "not a wildcard search"
            prev = uc.reg_read(UC_M68K_REG_A0)
            nxt = cds[0] if prev == 0 else (cds[cds.index(prev) + 1] if cds.index(prev) + 1 < len(cds) else 0)
            state["boardcalls"] = state.get("boardcalls", 0) + 1
            uc.reg_write(UC_M68K_REG_D0, nxt)
            uc.reg_write(UC_M68K_REG_D1, 0xDEADBEEF)
            uc.reg_write(UC_M68K_REG_A1, 0xDEADBEEF)
        elif HOOK_BASE + 8 <= addr < HOOK_BASE + 8 * 12 and (addr - HOOK_BASE) % 8 == 0:
            idx = (addr - HOOK_BASE) // 8
            if uc.reg_read(UC_M68K_REG_A6) != DT_BASE:
                state["a6_ok"] = False
            state["calls"][names[idx]] = state["calls"].get(names[idx], 0) + 1
            a0, a1 = uc.reg_read(UC_M68K_REG_A0), uc.reg_read(UC_M68K_REG_A1)
            d0 = 0
            if idx == 1:                                         # OpenKey(path)
                p = rd_str(a0)
                n = root if p == "/" else find(root, p)
                d0 = n.addr if n else 0
            elif idx == 3:                                       # GetChild(key, prev)
                n = nodes[a0]
                if a1 == 0:
                    d0 = n.kids[0].addr if n.kids else 0
                else:
                    sib = nodes[a1].parent.kids
                    i = sib.index(nodes[a1])
                    d0 = sib[i + 1].addr if i + 1 < len(sib) else 0
            elif idx == 4:                                       # FindProperty(key, name)
                d0 = prop_addr(nodes[a0], rd_str(a1))
            elif idx == 6:                                       # GetPropLen(prop)
                d0 = struct.unpack(">I", bytes(uc.mem_read(a0 + 8, 4)))[0]
            elif idx == 8:                                       # GetPropValue(prop)
                d0 = struct.unpack(">I", bytes(uc.mem_read(a0 + 12, 4)))[0]
            elif idx == 9:                                       # GetParent(key)
                n = nodes[a0].parent
                d0 = n.addr if n else 0
            elif idx == 10:                                      # GetKeyName(key)
                d0 = struct.unpack(">I", bytes(uc.mem_read(a0 + 8, 4)))[0]
            else:
                raise AssertionError("unexpected call " + names[idx])
            uc.reg_write(UC_M68K_REG_D0, d0)
            # clobber what AmigaOS clobbers, to prove the code does not rely on it
            uc.reg_write(UC_M68K_REG_D1, 0xDEADBEEF)
            uc.reg_write(UC_M68K_REG_A1, 0xDEADBEEF)
        else:
            return
        # emulate the RTS of the "function"
        sp = uc.reg_read(UC_M68K_REG_A7)
        ret = struct.unpack(">I", bytes(uc.mem_read(sp, 4)))[0]
        uc.reg_write(UC_M68K_REG_A7, sp + 4)
        state["pending"] = ret
        uc.emu_stop()

    uc.hook_add(UC_HOOK_CODE, hook, HOOK_BASE, HOOK_BASE + 0x200)
    # registers the caller (the bootblock) expects back
    marks = {UC_M68K_REG_D2: 0x22222222, UC_M68K_REG_D3: 0x33333333, UC_M68K_REG_D4: 0x44444444,
             UC_M68K_REG_D5: 0x55555555, UC_M68K_REG_D6: 0x66666666, UC_M68K_REG_D7: 0x77777777,
             UC_M68K_REG_A2: 0xA2A2A2A2, UC_M68K_REG_A3: 0xA3A3A3A3, UC_M68K_REG_A4: 0xA4A4A4A4,
             UC_M68K_REG_A5: 0xA5A5A5A5}
    for r, v in marks.items():
        uc.reg_write(r, v)
    uc.reg_write(UC_M68K_REG_A6, EXEC_BASE)
    uc.reg_write(UC_M68K_REG_A1, HWINFO)                         # the bootblock passes the table in a1
    sp = STACK_TOP - 4
    uc.mem_write(sp, struct.pack(">I", HALT))
    uc.reg_write(UC_M68K_REG_A7, sp)
    pc = base                                                    # stage0_entry is at offset 0
    for _ in range(100000):
        try:
            uc.emu_start(pc, HALT, count=2000000)
        except UcError as e:
            raise SystemExit(f"CPU fault in stage0: {e} at pc={uc.reg_read(UC_M68K_REG_PC):#x}")
        if state["pending"] is None:
            break
        pc, state["pending"] = state["pending"], None
    else:
        raise SystemExit("stage0 did not finish")
    bad = [hex(r) for r, v in marks.items() if uc.reg_read(r) != v]
    tbl = bytes(uc.mem_read(HWINFO, 408))
    return tbl, state, uc.reg_read(UC_M68K_REG_A6), uc.reg_read(UC_M68K_REG_A7), bad


def main():
    dtb = open(sys.argv[1], "rb").read()
    blob = open(sys.argv[2] if len(sys.argv) > 2 else "build/stage0.bin", "rb").read()
    root = parse_fdt(dtb)
    emu68_rewrite(root)
    fails = 0

    def check(name, got, want):
        nonlocal fails
        ok = got == want
        print(f"{name:<14} {got:#010x} {'ok' if ok else 'WRONG (expected %#x)' % want}")
        fails += not ok

    for base in (0x00200000, 0x00240000 + 0x1C):                 # two different load addresses
        print(f"--- code loaded at {base:#x}")
        tbl, st, a6, sp, bad = run(blob, base, root)
        f = struct.unpack(">%dI" % (len(FIELDS)), tbl[:4 * len(FIELDS)])
        d = dict(zip(FIELDS, f))
        check("magic", d["magic"], 0x5A485731)
        check("flags", d["flags"], 7)
        check("soc_base", d["soc_base"], 0xF2000000)
        check("mailbox", d["mailbox"], 0xF200B880)
        check("gpio", d["gpio"], 0xF2200000)
        check("emmc", d["emmc"], 0xF2340000)
        check("genet", d["genet"], 0xF7580000)
        check("genet_irq0", d["genet_irq0"], 0x9D)
        check("genet_irq1", d["genet_irq1"], 0x9E)
        check("phy_addr", d["phy_addr"], 0)
        gic_dist, gic_cpu = struct.unpack(">II", tbl[48:56])
        check("gic_dist", gic_dist, 0xF5841000)
        check("gic_cpu", gic_cpu, 0xF5842000)
        check("mac[0:2]", tbl[40] << 8 | tbl[41], 0xDCA6)
        vc_base, vc_size = struct.unpack(">II", tbl[56:64])
        check("vc4_base", vc_base, 0x30000000)
        check("vc4_size", vc_size, 16 << 20)
        nb = struct.unpack(">I", tbl[64:68])[0]
        check("nboards", nb, 2)
        b0 = struct.unpack(">HBBIIIBBH", tbl[88:108])
        check("b0 mfr", b0[0], 0x082C); check("b0 product", b0[1], 0x0A); check("b0 type", b0[2], 0xE0)
        check("b0 serial", b0[3], 0x12345678); check("b0 addr", b0[4], 0x00200000); check("b0 size", b0[5], 0x00400000)
        check("b0 erflags", b0[6], 0x40); check("b0 cdflags", b0[7], 2)
        b1 = struct.unpack(">HBBIIIBBH", tbl[108:128])
        check("b1 mfr", b1[0], 0x6D73); check("b1 addr", b1[4], 0x40000000)
        check("a6 at calls", int(st["a6_ok"]), 1)
        check("a6 on return", a6, EXEC_BASE)
        check("saved regs", len(bad), 0)
        print("   calls:", st["calls"])
    print("--- no devicetree.resource (plain Amiga), the boards are still listed")
    tbl, st, a6, sp, bad = run(blob, 0x00200000, root, with_dt=False)
    magic, flags = struct.unpack(">II", tbl[:8])
    check("magic", magic, 0x5A485731)
    check("flags", flags, 0)
    check("emmc", struct.unpack(">I", tbl[16:20])[0], 0)
    check("nboards", struct.unpack(">I", tbl[64:68])[0], 2)
    print("--- no expansion.library")
    tbl, st, a6, sp, bad = run(blob, 0x00200000, root, with_dt=False, with_boards=False)
    check("nboards", struct.unpack(">I", tbl[64:68])[0], 0)
    check("a6 on return", a6, EXEC_BASE)
    print("FAILED" if fails else "all ok")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
