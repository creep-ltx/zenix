#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Seeds for tools/host/lwip-fuzz (its input format: a mode byte, then chunks of u32 delay, u16 length, bytes):
lwIP's own sample frames (net/lwip/test/fuzz/inputs) as Ethernet chunks, and packets for Zenix's addresses
(10.0.2.15, the gateway 10.0.2.2, DNS 10.0.2.3) both as SLIP bytes and as Ethernet frames.
    seeds.py LWIP-INPUTS-DIR OUT-DIR"""
import os, struct, sys

src, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
ME, GW, DNS = bytes([10, 0, 2, 15]), bytes([10, 0, 2, 2]), bytes([10, 0, 2, 3])
MAC, GWMAC = bytes([2, 0x5a, 0x45, 0x4e, 0x49, 0x58]), bytes([0x52, 0x55, 10, 0, 2, 2])

def csum(b):
    if len(b) % 2:
        b += b"\0"
    s = sum(struct.unpack("!%dH" % (len(b) // 2), b))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return ~s & 0xFFFF

def ip4(proto, src, dst, payload, ident=1, frag=0):
    h = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(payload), ident, frag, 64, proto, 0, src, dst)
    return h[:10] + struct.pack("!H", csum(h)) + h[12:] + payload

def l4(proto, src, dst, seg):
    ph = src + dst + struct.pack("!BBH", 0, proto, len(seg))
    c = csum(ph + seg)
    off = 6 if proto == 17 else 16
    return seg[:off] + struct.pack("!H", c or 0xFFFF) + seg[off + 2:]

def udp(src, sport, dst, dport, data):
    return ip4(17, src, dst, l4(17, src, dst, struct.pack("!HHHH", sport, dport, 8 + len(data), 0) + data))

def tcp(src, sport, dst, dport, seq, ack, flags, data=b"", opts=b""):
    hl = (20 + len(opts)) // 4
    seg = struct.pack("!HHIIBBHHH", sport, dport, seq, ack, hl << 4, flags, 8192, 0, 0) + opts + data
    return ip4(6, src, dst, l4(6, src, dst, seg))

def icmp_echo(src, dst, ident, data):
    m = struct.pack("!BBHHH", 8, 0, 0, ident, 1) + data
    return ip4(1, src, dst, m[:2] + struct.pack("!H", csum(m)) + m[4:])

def dns_answer():
    q = b"\x04fuzz\x05zenix\x04test\x00" + struct.pack("!HH", 1, 1)
    a = b"\xc0\x0c" + struct.pack("!HHIH", 1, 1, 60, 4) + bytes([10, 1, 2, 3])
    return struct.pack("!HHHHHH", 0x1234, 0x8180, 1, 1, 0, 0) + q + a

def slip(p):
    return b"\xc0" + p.replace(b"\xdb", b"\xdb\xdd").replace(b"\xc0", b"\xdb\xdc") + b"\xc0"

def eth(p, ethertype=0x0800):
    return MAC + GWMAC + struct.pack("!H", ethertype) + p

def arp_request():
    return (b"\xff" * 6 + GWMAC + b"\x08\x06" + struct.pack("!HHBBH", 1, 0x800, 6, 4, 1) + GWMAC + GW + b"\0" * 6 + ME)

def chunks(mode, parts):
    b = bytes([mode])
    for delay, data in parts:
        b += struct.pack("!IH", delay, len(data)) + data
    return b

packets = {
    "syn23": tcp(GW, 40000, ME, 23, 1000, 0, 0x02, opts=b"\x02\x04\x05\xb4"),
    "syn-data23": tcp(GW, 40001, ME, 23, 5000, 0, 0x02) + b"",
    "udp5000": udp(GW, 40002, ME, 5000, b"hello fuzz"),
    "ping": icmp_echo(GW, ME, 0x4242, b"abcdefgh"),
    "dns": udp(DNS, 53, ME, 49200, dns_answer()),
    "frag": ip4(17, GW, ME, struct.pack("!HHHH", 40003, 5000, 24, 0) + b"first-fr", ident=77, frag=0x2000),
    "frag2": ip4(17, GW, ME, b"second-part!", ident=77, frag=2),
}
n = 0
def put(name, b):
    global n
    with open(os.path.join(out, name), "wb") as f:
        f.write(b)
    n += 1

for name, p in packets.items():
    put("slip-" + name, chunks(0, [(0, slip(p))]))
    put("eth-" + name, chunks(1, [(0, eth(p))]))
put("slip-frags", chunks(0, [(0, slip(packets["frag"])), (10, slip(packets["frag2"]))]))
put("eth-frags", chunks(1, [(0, eth(packets["frag"])), (10, eth(packets["frag2"]))]))
put("slip-noise", chunks(0, [(0, b"zenix: kernel text\r\n\xc0\xdb\x01\xc0" + slip(packets["ping"]))]))
put("slip-syn-then-time", chunks(0, [(0, slip(packets["syn23"])), (3000, b""), (3000, b"")]))
put("eth-arp", chunks(1, [(0, arp_request())]))
put("dhcp-start", chunks(2, [(0, b""), (4000, b"")]))
for root, _, files in os.walk(src):
    for f in files:
        if "ipv6" in root:
            continue
        put("lwip-" + f, chunks(1, [(0, open(os.path.join(root, f), "rb").read())]))
print("seeds: %d inputs in %s" % (n, out))
