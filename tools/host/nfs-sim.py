#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""nfs-sim: a small NFS server of our own for the tests of nfscd's core (DESIGN D23 11.1), as bbs-sim.py is for telnet.

Its own portmapper (v2 GETPORT), MOUNT v3 (MNT, UMNT) and NFSv3 (RFC 1813, every call nfscd uses and a few more) over
TCP with record marking, on an in-memory tree, with faults on command: answers dropped, delayed, out of order,
duplicated; the connection cut mid-record; records in several fragments; a changed write verifier (a "restart" that
loses what was not committed); JUKEBOX; BAD_COOKIE; stale handles; a reused fileid with a new handle; a different root
handle; oversize records and lengths; and a duplicate request cache keyed by the client's address, port and xid.

Used two ways:
  import (tools/host/nfs-sim-test.py): Sim(...).start(); sim.fault(...); sim.tree checks
  nfs-sim.py --serve [--pmap P --mount M --nfs N]         serve until killed (an export /export)
  nfs-sim.py --pmap-only MOUNTPORT NFSPORT [--pmap P]      only a portmapper that names these ports (for unfs3 -p)
  nfs-sim.py --self-test                                  its own checks over a socket
"""
import os
import random
import socket
import struct
import sys
import threading
import time

PMAP_PROG, MOUNT_PROG, NFS_PROG = 100000, 100005, 100003
NF3REG, NF3DIR, NF3BLK, NF3CHR, NF3LNK, NF3SOCK, NF3FIFO = 1, 2, 3, 4, 5, 6, 7
OK, PERM, NOENT, IO, ACCES, EXIST, NOTDIR, ISDIR, INVAL, NAMETOOLONG, NOTEMPTY, STALE = 0, 1, 2, 5, 13, 17, 20, 21, 22, 63, 66, 70
BADHANDLE, BAD_COOKIE, NOTSUPP, JUKEBOX = 10001, 10003, 10004, 10008
NONIDEMPOTENT = {2, 7, 8, 9, 10, 11, 12, 13, 14, 15, 21}     # SETATTR WRITE CREATE MKDIR SYMLINK MKNOD REMOVE RMDIR RENAME LINK COMMIT


class Short(Exception):
    pass


class R:
    """an XDR reader"""
    def __init__(self, b, p=0):
        self.b, self.p = b, p

    def u32(self):
        if self.p + 4 > len(self.b):
            raise Short()
        v = struct.unpack_from(">I", self.b, self.p)[0]
        self.p += 4
        return v

    def u64(self):
        return self.u32() << 32 | self.u32()

    def fixed(self, n):
        pad = (n + 3) & ~3
        if self.p + pad > len(self.b):
            raise Short()
        v = self.b[self.p:self.p + n]
        self.p += pad
        return bytes(v)

    def opaque(self, mx=1 << 20):
        n = self.u32()
        if n > mx:
            raise Short()
        return self.fixed(n)


class W:
    """an XDR writer"""
    def __init__(self):
        self.parts = []

    def u32(self, v):
        self.parts.append(struct.pack(">I", v & 0xFFFFFFFF))
        return self

    def u64(self, v):
        self.parts.append(struct.pack(">Q", v & 0xFFFFFFFFFFFFFFFF))
        return self

    def fixed(self, b):
        self.parts.append(bytes(b) + b"\0" * ((4 - len(b) % 4) % 4))
        return self

    def opaque(self, b):
        self.u32(len(b))
        return self.fixed(b)

    def bytes(self):
        return b"".join(self.parts)


class Inode:
    def __init__(self, fileid, gen, typ, mode, uid, gid):
        self.fileid, self.gen, self.type, self.mode, self.uid, self.gid = fileid, gen, typ, mode, uid, gid
        self.nlink = 2 if typ == NF3DIR else 1
        self.data = bytearray()
        self.committed = bytearray()
        self.entries = {}            # name -> fileid (directories)
        self.parent = None
        self.target = b""
        self.cverf = None
        self.dverf = 1               # the cookie verifier of a directory: changes with it
        t = now_ts()
        self.atime = self.mtime = self.ctime = t


_clock = [0]


def now_ts():
    """seconds and nanoseconds, every call later than the one before"""
    t = time.time_ns()
    if t <= _clock[0]:
        t = _clock[0] + 1000
    _clock[0] = t
    return (t // 10**9, t % 10**9)


class Sim:
    def __init__(self, pmap_port=0, mount_port=0, nfs_port=0, host="127.0.0.1", fh_len=28, export="/export",
                 pmap_only=None):
        self.host = host
        self.fh_len = fh_len
        self.export = export.encode()
        self.lock = threading.RLock()
        self.verf = os.urandom(8)
        self.exportid = 1
        self.inodes = {}
        self.next_fileid = 1000
        self.free_fileids = []
        self.reuse = False
        self.drc = {}
        self.drc_hits = 0
        self.log = []                # (prog, proc, xid, peer port, drc hit)
        self.faults = {}
        self.fault_proc = {}
        self.cur_proc = None
        self.conns = []
        self.squash = None           # (uid, gid): every user and root act as this one
        self.fsstat = (8 << 40, 7 << 40, 6 << 40, 1 << 20, 1 << 19, 1 << 18)   # 8 TB: above Zenix's 4 TB cap
        self.rtmax = 65536
        self.pmap_only = pmap_only   # (mount port, nfs port) of another server
        self.held = None
        self.running = True
        root = self._new(NF3DIR, 0o755, 0, 0)
        root.parent = root.fileid
        self.root = root.fileid
        self.listeners = {}
        self.ports = {}
        for name, port in (("pmap", pmap_port), ("mount", mount_port), ("nfs", nfs_port)):
            if pmap_only and name != "pmap":
                continue
            self._listen(name, port)

    # ---- the tree ----
    def _new(self, typ, mode, uid, gid):
        if self.reuse and self.free_fileids:
            fid, gen = self.free_fileids.pop()
            gen += 1
        else:
            fid, gen = self.next_fileid, 1
            self.next_fileid += 1
        ino = Inode(fid, gen, typ, mode, uid, gid)
        self.inodes[fid] = ino
        return ino

    def _free(self, ino):
        del self.inodes[ino.fileid]
        self.free_fileids.append((ino.fileid, ino.gen))

    def fh(self, ino):
        b = b"ZS" + struct.pack(">BQI", self.exportid, ino.fileid, ino.gen)
        return b + bytes((ino.fileid * 7 + k) & 0xFF for k in range(self.fh_len - len(b)))

    def _ino(self, fh):
        if len(fh) != self.fh_len or fh[:2] != b"ZS":
            return None, BADHANDLE
        eid, fid, gen = struct.unpack_from(">BQI", fh, 2)
        ino = self.inodes.get(fid)
        if eid != self.exportid or not ino or ino.gen != gen:
            return None, STALE
        return ino, OK

    def path(self, p):
        """the inode at an export path ("a/b"), or None"""
        ino = self.inodes[self.root]
        for part in [x for x in p.split("/") if x]:
            if ino.type != NF3DIR or part.encode() not in ino.entries:
                return None
            ino = self.inodes[ino.entries[part.encode()]]
        return ino

    def put_file(self, p, data, mode=0o644, uid=1000, gid=100):
        with self.lock:
            d = self.path(os.path.dirname(p))
            name = os.path.basename(p).encode()
            if name in d.entries:
                ino = self.inodes[d.entries[name]]
            else:
                ino = self._new(NF3REG, mode, uid, gid)
                d.entries[name] = ino.fileid
                self._touch(d)
            ino.data = bytearray(data)
            ino.committed = bytearray(data)
            self._touch(ino)
            return ino

    def mkdir(self, p, mode=0o755, uid=1000, gid=100):
        with self.lock:
            d = self.path(os.path.dirname(p))
            ino = self._new(NF3DIR, mode, uid, gid)
            ino.parent = d.fileid
            d.entries[os.path.basename(p).encode()] = ino.fileid
            d.nlink += 1
            self._touch(d)
            return ino

    def symlink(self, p, target):
        with self.lock:
            d = self.path(os.path.dirname(p))
            ino = self._new(NF3LNK, 0o777, 0, 0)
            ino.target = target.encode()
            d.entries[os.path.basename(p).encode()] = ino.fileid
            self._touch(d)
            return ino

    def mknode(self, p, typ, mode=0o644):
        with self.lock:
            d = self.path(os.path.dirname(p))
            ino = self._new(typ, mode, 0, 0)
            d.entries[os.path.basename(p).encode()] = ino.fileid
            self._touch(d)
            return ino

    def remove(self, p):
        """the PC deletes a file on the NAS"""
        with self.lock:
            d = self.path(os.path.dirname(p))
            ino = self.inodes[d.entries.pop(os.path.basename(p).encode())]
            ino.nlink -= 1
            if ino.nlink <= 0:
                self._free(ino)
            self._touch(d)

    def _touch(self, ino, data=True):
        t = now_ts()
        ino.ctime = t
        if data:
            ino.mtime = t
        if ino.type == NF3DIR:
            ino.dverf += 1

    # ---- faults (the tests' commands) ----
    def fault(self, name, value=1, proc=None):
        """a fault; proc: only for answers to that NFS procedure"""
        with self.lock:
            if proc is not None:
                self.fault_proc[name] = proc
            if name == "restart":         # the NAS restarts: a new verifier, what was not committed is lost
                self.verf = os.urandom(8)
                for ino in self.inodes.values():
                    if ino.type == NF3REG:
                        ino.data = bytearray(ino.committed)
                if value == "nodrc":
                    self.drc.clear()
                self.close_all()
            elif name == "stale":         # the file at a path gets a new handle: the old one is stale
                self.path(value).gen += 1
            elif name == "rootchange":    # another export at the same address
                self.exportid += 1
            elif name == "reuse":
                self.reuse = bool(value)
            elif name == "down":
                self.close_all()
                for s in self.listeners.values():
                    self._unlisten(s)
                self.listeners = {}
            elif name == "up":
                for n, port in self.ports.items():
                    self._listen(n, port)
            elif name == "squash":
                self.squash = value
            elif name == "clear":
                self.faults = {}
                self.fault_proc = {}
            else:
                self.faults[name] = value

    def take(self, name, proc=None):
        """use up one of a counted fault: True if it applies now"""
        with self.lock:
            v = self.faults.get(name)
            if not v:
                return False
            if name in self.fault_proc and self.fault_proc[name] != proc:
                return False
            if isinstance(v, int) and v is not True:
                if v <= 1:
                    del self.faults[name]
                else:
                    self.faults[name] = v - 1
            return True

    def close_all(self):
        for c in list(self.conns):
            try:
                c.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
                c.close()
            except OSError:
                pass
        self.conns = []

    # ---- the network ----
    def _listen(self, name, port):
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind((self.host, port))
        s.listen(16)
        self.listeners[name] = s
        self.ports[name] = s.getsockname()[1]
        threading.Thread(target=self._accept, args=(s, name), daemon=True).start()

    def _accept(self, s, name):
        while self.running:
            try:
                c, addr = s.accept()
            except OSError:
                return
            with self.lock:
                self.conns.append(c)
            threading.Thread(target=self._serve, args=(c, addr), daemon=True).start()

    @staticmethod
    def _unlisten(s):
        try:
            s.shutdown(socket.SHUT_RDWR)      # wakes the thread in accept(), so the port is free at once
        except OSError:
            pass
        s.close()

    def stop(self):
        self.running = False
        for s in self.listeners.values():
            self._unlisten(s)
        self.listeners = {}
        self.close_all()

    def _recv_record(self, c):
        rec = b""
        while True:
            h = self._recvn(c, 4)
            if h is None:
                return None
            n = struct.unpack(">I", h)[0]
            last, n = n >> 31, n & 0x7FFFFFFF
            if n > (4 << 20):
                return None
            b = self._recvn(c, n)
            if b is None:
                return None
            rec += b
            if last:
                return rec

    @staticmethod
    def _recvn(c, n):
        b = b""
        while len(b) < n:
            try:
                k = c.recv(n - len(b))
            except OSError:
                return None
            if not k:
                return None
            b += k
        return b

    def _serve(self, c, addr):
        while self.running:
            rec = self._recv_record(c)
            if rec is None:
                break
            if self.faults.get("blackhole"):
                continue                  # the NAS is gone without a word: nothing answers
            try:
                reply, proc = self.dispatch(rec, addr)
            except Short:
                reply = None
            if reply is None:
                continue
            self._send(c, reply, proc)
        try:
            c.close()
        except OSError:
            pass
        with self.lock:
            if c in self.conns:
                self.conns.remove(c)

    def _send(self, c, body, proc=None):
        try:
            if self.take("drop", proc):
                return
            d = self.faults.get("delay")
            if d:
                time.sleep(d / 1000)
            if self.take("garbage", proc):
                body = body[:4] + os.urandom(max(0, len(body) - 4))
            if self.take("oversize", proc):
                body = body + b"\0" * (self.faults.get("oversize_len", 70000))
            if self.take("badlen", proc):
                c.sendall(struct.pack(">I", 0xFFFFFFFF) + body)
                return
            if self.take("cut_nodrc", proc):    # cut, and the NAS's cache lost the answer
                with self.lock:
                    self.drc.clear()
                rec = struct.pack(">I", 0x80000000 | len(body)) + body
                c.sendall(rec[:max(1, len(rec) // 2)])
                c.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
                c.close()
                return
            if self.take("cut", proc):
                rec = struct.pack(">I", 0x80000000 | len(body)) + body
                c.sendall(rec[:max(1, len(rec) // 2)])
                c.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
                c.close()
                return
            frag = self.faults.get("frag")
            if frag:
                out = b""
                for i in range(0, len(body), frag):
                    part = body[i:i + frag]
                    out += struct.pack(">I", (0x80000000 if i + frag >= len(body) else 0) | len(part)) + part
                if not body:
                    out = struct.pack(">I", 0x80000000)
                for i in range(0, len(out), 3):   # and in tiny pieces on the wire
                    c.sendall(out[i:i + 3])
                return
            rec = struct.pack(">I", 0x80000000 | len(body)) + body
            if self.faults.get("reorder"):
                with self.lock:
                    if self.held is None:
                        self.held = (c, rec)
                        threading.Timer(0.05, self._release, args=(self.held,)).start()   # alone: sent a bit late
                        return
                    hc, hrec = self.held
                    self.held = None
                c.sendall(rec)
                hc.sendall(hrec)
                return
            c.sendall(rec)
            if self.take("dup", proc):
                c.sendall(rec)
        except OSError:
            pass

    def _release(self, h):
        with self.lock:
            if self.held is not h:
                return
            self.held = None
        try:
            h[0].sendall(h[1])
        except OSError:
            pass

    # ---- RPC ----
    def dispatch(self, rec, addr):
        rep, proc = self._dispatch(rec, addr)
        return rep, proc

    def _dispatch(self, rec, addr):
        r = R(rec)
        xid, mtype = r.u32(), r.u32()
        if mtype != 0:
            return None, None
        rpcvers, prog, vers, proc = r.u32(), r.u32(), r.u32(), r.u32()
        flavor = r.u32()
        cred = R(r.opaque(400))
        r.u32(); r.opaque(400)          # the verifier
        uid, gid, gids = 65534, 65534, []
        if flavor == 1:
            cred.u32(); cred.opaque(255)
            uid, gid = cred.u32(), cred.u32()
            gids = [cred.u32() for _ in range(min(cred.u32(), 16))]
        if self.squash:
            uid, gid = self.squash
        head = W().u32(xid).u32(1).u32(0).u32(0).u32(0)   # REPLY, MSG_ACCEPTED, AUTH_NONE verifier
        key = (addr[0], addr[1], xid)
        with self.lock:
            self.log.append((prog, proc, xid, addr[1], key in self.drc))
            if prog == NFS_PROG and proc in NONIDEMPOTENT and key in self.drc:
                self.drc_hits += 1
                return self.drc[key], proc
        if rpcvers != 2:
            return W().u32(xid).u32(1).u32(1).u32(0).u32(2).u32(2).bytes(), proc   # MSG_DENIED RPC_MISMATCH
        if self.faults.get("autherr") and prog == NFS_PROG:
            return W().u32(xid).u32(1).u32(1).u32(1).u32(5).bytes(), proc          # AUTH_ERROR AUTH_TOOWEAK
        with self.lock:
            if prog == PMAP_PROG and vers == 2:
                body = self.pmap(proc, r)
            elif prog == MOUNT_PROG and vers == 3 and not self.pmap_only:
                body = self.mount(proc, r)
            elif prog == NFS_PROG and vers == 3 and not self.pmap_only:
                body = self.nfs(proc, r, uid, gid, gids)
            else:
                return head.u32(1).bytes(), proc                              # PROG_UNAVAIL
            if body is None:
                return head.u32(3).bytes(), proc                              # PROC_UNAVAIL
            reply = head.u32(0).bytes() + body
            if prog == NFS_PROG and proc in NONIDEMPOTENT:
                self.drc[key] = reply
            return reply, proc

    def pmap(self, proc, r):
        if proc == 0:
            return b""
        if proc != 3:
            return None
        prog, vers, prot, _ = r.u32(), r.u32(), r.u32(), r.u32()
        port = 0
        if prot == 6 and vers == 3:
            if prog == MOUNT_PROG:
                port = self.pmap_only[0] if self.pmap_only else self.ports.get("mount", 0)
            elif prog == NFS_PROG:
                port = self.pmap_only[1] if self.pmap_only else self.ports.get("nfs", 0)
        if self.faults.get("nomount"):
            port = 0
        return W().u32(port).bytes()

    def mount(self, proc, r):
        if proc == 0:
            return b""
        if proc == 1:
            path = r.opaque(1024)
            if self.faults.get("mntdeny"):
                return W().u32(ACCES).bytes()
            if path.rstrip(b"/") != self.export.rstrip(b"/"):
                return W().u32(NOENT).bytes()
            return W().u32(0).opaque(self.fh(self.inodes[self.root])).u32(1).u32(1).bytes()
        if proc == 3:
            r.opaque(1024)
            return b""
        if proc == 5:                     # EXPORT: the one export, for everyone
            return W().u32(1).opaque(self.export).u32(1).opaque(b"*").u32(0).u32(0).bytes()
        return None

    # ---- NFSv3 ----
    def fattr(self, w, ino):
        size = len(ino.data) if ino.type == NF3REG else (len(ino.target) if ino.type == NF3LNK else 4096)
        w.u32(ino.type).u32(ino.mode & 0o7777).u32(ino.nlink).u32(ino.uid).u32(ino.gid)
        w.u64(size).u64(size).u32(0).u32(0).u64(1).u64(ino.fileid)
        for t in (ino.atime, ino.mtime, ino.ctime):
            w.u32(t[0]).u32(t[1])

    def post(self, w, ino):
        if ino is None:
            return w.u32(0)
        w.u32(1)
        self.fattr(w, ino)
        return w

    def pre(self, ino):
        if ino is None:
            return None
        size = len(ino.data) if ino.type == NF3REG else 4096
        return (size, ino.mtime, ino.ctime)

    def wcc(self, w, before, ino):
        if before is None:
            w.u32(0)
        else:
            w.u32(1).u64(before[0]).u32(before[1][0]).u32(before[1][1]).u32(before[2][0]).u32(before[2][1])
        return self.post(w, ino)

    def access(self, ino, uid, gid, gids):
        if uid == 0:
            return 0x3F
        m = ino.mode
        bits = (m >> 6) if uid == ino.uid else (m >> 3) if (gid == ino.gid or ino.gid in gids) else m
        a = 0
        if bits & 4:
            a |= 1
        if bits & 2:
            a |= 4 | 8 | 16
        if bits & 1:
            a |= 2 | 32
        return a

    @staticmethod
    def sattr(r):
        s = {}
        if r.u32():
            s["mode"] = r.u32()
        if r.u32():
            s["uid"] = r.u32()
        if r.u32():
            s["gid"] = r.u32()
        if r.u32():
            s["size"] = r.u64()
        h = r.u32()
        if h == 2:
            s["atime"] = (r.u32(), r.u32())
        elif h == 1:
            s["atime"] = "now"
        h = r.u32()
        if h == 2:
            s["mtime"] = (r.u32(), r.u32())
        elif h == 1:
            s["mtime"] = "now"
        return s

    def apply(self, ino, s):
        if "mode" in s:
            ino.mode = s["mode"] & 0o7777
        if "uid" in s:
            ino.uid = s["uid"]
        if "gid" in s:
            ino.gid = s["gid"]
        if "size" in s and ino.type == NF3REG:
            n = s["size"]
            ino.data = ino.data[:n] + bytearray(max(0, n - len(ino.data)))
            ino.committed = bytearray(ino.data)          # SETATTR is synchronous
            self._touch(ino)
        for k in ("atime", "mtime"):
            if k in s:
                setattr(ino, k, now_ts() if s[k] == "now" else s[k])
        ino.ctime = now_ts()

    def fail(self, proc, status, w, dir_before=None, d=None, ino=None):
        """the failure arm of each result"""
        w.u32(status)
        if proc in (1,):
            pass
        elif proc in (2, 7, 21):
            self.wcc(w, self.pre(ino), ino)
        elif proc == 3:
            self.post(w, d)
        elif proc in (4, 5, 6, 18, 19, 20):
            self.post(w, ino)
        elif proc in (8, 9, 10, 11, 12, 13):
            self.wcc(w, dir_before, d)
        elif proc == 14:
            self.wcc(w, None, None); self.wcc(w, None, None)
        elif proc == 15:
            self.post(w, ino); self.wcc(w, None, None)
        elif proc in (16, 17):
            self.post(w, ino)
        return w.bytes()

    def nfs(self, proc, r, uid, gid, gids):
        w = W()
        if proc == 0:
            return b""
        if proc > 21:
            return None
        if self.faults.get("jukebox") and self.take("jukebox"):
            return self.fail(proc, JUKEBOX, w)
        ino, st = self._ino(r.opaque(64))
        if st:
            return self.fail(proc, st, w)
        if proc == 1:                                             # GETATTR
            w.u32(OK)
            self.fattr(w, ino)
            return w.bytes()
        if proc == 2:                                             # SETATTR
            s = self.sattr(r)
            if r.u32():
                r.u32(); r.u32()
            before = self.pre(ino)
            if uid != 0 and uid != ino.uid and set(s) - {"atime", "mtime", "size"}:
                return self.fail(proc, PERM, w, ino=ino)
            self.apply(ino, s)
            w.u32(OK)
            self.wcc(w, before, ino)
            return w.bytes()
        if proc == 3:                                             # LOOKUP
            name = r.opaque(255)
            if ino.type != NF3DIR:
                return self.fail(proc, NOTDIR, w, d=ino)
            if not self.access(ino, uid, gid, gids) & 2:
                return self.fail(proc, ACCES, w, d=ino)
            if name == b".":
                c = ino
            elif name == b"..":
                c = self.inodes[ino.parent]
            elif name in ino.entries:
                c = self.inodes[ino.entries[name]]
            else:
                return self.fail(proc, NOENT, w, d=ino)
            w.u32(OK).opaque(self.fh(c))
            self.post(w, c)
            self.post(w, ino)
            return w.bytes()
        if proc == 4:                                             # ACCESS
            want = r.u32()
            w.u32(OK)
            self.post(w, ino)
            w.u32(self.access(ino, uid, gid, gids) & want)
            return w.bytes()
        if proc == 5:                                             # READLINK
            if ino.type != NF3LNK:
                return self.fail(proc, INVAL, w, ino=ino)
            w.u32(OK)
            self.post(w, ino)
            w.opaque(ino.target)
            return w.bytes()
        if proc == 6:                                             # READ
            off, cnt = r.u64(), r.u32()
            if ino.type != NF3REG:
                return self.fail(proc, ISDIR if ino.type == NF3DIR else INVAL, w, ino=ino)
            cnt = min(cnt, self.rtmax)
            data = bytes(ino.data[off:off + cnt])
            w.u32(OK)
            self.post(w, ino)
            w.u32(len(data)).u32(1 if off + len(data) >= len(ino.data) else 0).opaque(data)
            return w.bytes()
        if proc == 7:                                             # WRITE
            off, cnt, stable = r.u64(), r.u32(), r.u32()
            data = r.opaque(1 << 20)
            if cnt != len(data):
                return self.fail(proc, INVAL, w, ino=ino)
            if ino.type != NF3REG:
                return self.fail(proc, ISDIR, w, ino=ino)
            if not self.access(ino, uid, gid, gids) & 4:
                return self.fail(proc, ACCES, w, ino=ino)
            before = self.pre(ino)
            if len(ino.data) < off:
                ino.data += bytearray(off - len(ino.data))
            ino.data[off:off + len(data)] = data
            if stable:
                ino.committed = bytearray(ino.data)
            self._touch(ino)
            w.u32(OK)
            self.wcc(w, before, ino)
            w.u32(len(data)).u32(2 if stable else 0).fixed(self.verf)
            return w.bytes()
        if proc in (8, 9, 10):                                    # CREATE MKDIR SYMLINK
            name = r.opaque(255)
            if ino.type != NF3DIR:
                return self.fail(proc, NOTDIR, w, self.pre(ino), ino)
            before = self.pre(ino)
            how, verf, s = 0, None, {}
            if proc == 8:
                how = r.u32()
                if how == 2:
                    verf = r.fixed(8)
                else:
                    s = self.sattr(r)
            else:
                s = self.sattr(r)
            target = r.opaque(1024) if proc == 10 else None
            if not self.access(ino, uid, gid, gids) & 4:
                return self.fail(proc, ACCES, w, before, ino)
            if name in (b"", b".", b"..") or b"/" in name or b"\0" in name:
                return self.fail(proc, INVAL if name not in (b".", b"..") else EXIST, w, before, ino)
            if name in ino.entries:
                old = self.inodes[ino.entries[name]]
                if proc == 8 and how == 0 and old.type == NF3REG:
                    c = old
                    if "size" in s:
                        self.apply(c, {"size": s["size"]})
                elif proc == 8 and how == 2 and old.cverf == verf:
                    c = old                           # the verifier proves the earlier create was this one's
                else:
                    return self.fail(proc, EXIST, w, before, ino)
            else:
                typ = NF3REG if proc == 8 else NF3DIR if proc == 9 else NF3LNK
                c = self._new(typ, s.get("mode", 0o777 if proc == 10 else 0o644) & 0o7777, uid, gid)
                if "uid" in s:
                    c.uid = s["uid"]
                if "gid" in s:
                    c.gid = s["gid"]
                if proc == 9:
                    c.parent = ino.fileid
                    ino.nlink += 1
                if proc == 10:
                    c.target = target
                if verf is not None:
                    c.cverf = verf
                    c.atime = struct.unpack(">II", verf)
                    c.mtime = c.atime
                ino.entries[name] = c.fileid
                self._touch(ino)
            w.u32(OK).u32(1).opaque(self.fh(c))
            self.post(w, c)
            self.wcc(w, before, ino)
            return w.bytes()
        if proc == 11:                                            # MKNOD
            return self.fail(proc, NOTSUPP, w, None, ino)
        if proc in (12, 13):                                      # REMOVE RMDIR
            name = r.opaque(255)
            before = self.pre(ino)
            if ino.type != NF3DIR:
                return self.fail(proc, NOTDIR, w, before, ino)
            if name not in ino.entries:
                return self.fail(proc, NOENT, w, before, ino)
            if not self.access(ino, uid, gid, gids) & 4:
                return self.fail(proc, ACCES, w, before, ino)
            c = self.inodes[ino.entries[name]]
            if proc == 12 and c.type == NF3DIR:
                return self.fail(proc, ISDIR, w, before, ino)
            if proc == 13 and c.type != NF3DIR:
                return self.fail(proc, NOTDIR, w, before, ino)
            if proc == 13 and c.entries:
                return self.fail(proc, NOTEMPTY, w, before, ino)
            del ino.entries[name]
            c.nlink -= 1
            if proc == 13:
                ino.nlink -= 1
                c.nlink = 0
            if c.nlink <= 0:
                self._free(c)
            else:
                self._touch(c, False)
            self._touch(ino)
            w.u32(OK)
            self.wcc(w, before, ino)
            return w.bytes()
        if proc == 14:                                            # RENAME
            name = r.opaque(255)
            to, st2 = self._ino(r.opaque(64))
            toname = r.opaque(255)
            if st2:
                return self.fail(proc, st2, w)
            b1, b2 = self.pre(ino), self.pre(to)
            if name not in ino.entries:
                return self.fail(proc, NOENT, w)
            c = self.inodes[ino.entries[name]]
            if toname in to.entries:
                old = self.inodes[to.entries[toname]]
                if old is not c:
                    if old.type == NF3DIR and old.entries:
                        return self.fail(proc, NOTEMPTY, w)
                    old.nlink -= 1
                    if old.nlink <= 0 or old.type == NF3DIR:
                        self._free(old)
            del ino.entries[name]
            to.entries[toname] = c.fileid
            if c.type == NF3DIR:
                c.parent = to.fileid
            self._touch(ino)
            self._touch(to)
            self._touch(c, False)
            w.u32(OK)
            self.wcc(w, b1, ino)
            self.wcc(w, b2, to)
            return w.bytes()
        if proc == 15:                                            # LINK
            d, st2 = self._ino(r.opaque(64))
            name = r.opaque(255)
            if st2:
                return self.fail(proc, st2, w)
            before = self.pre(d)
            if name in d.entries:
                w.u32(EXIST); self.post(w, ino); self.wcc(w, before, d)
                return w.bytes()
            d.entries[name] = ino.fileid
            ino.nlink += 1
            self._touch(ino, False)
            self._touch(d)
            w.u32(OK)
            self.post(w, ino)
            self.wcc(w, before, d)
            return w.bytes()
        if proc in (16, 17):                                      # READDIR READDIRPLUS
            cookie, cverf = r.u64(), r.fixed(8)
            dircount = r.u32()
            maxcount = r.u32() if proc == 17 else dircount
            if ino.type != NF3DIR:
                return self.fail(proc, NOTDIR, w, ino=ino)
            if proc == 17 and self.faults.get("noplus"):
                return self.fail(proc, NOTSUPP, w, ino=ino)
            myverf = struct.pack(">Q", ino.dverf)
            if cookie and (cverf != myverf or self.take("badcookie")):
                return self.fail(proc, BAD_COOKIE, w, ino=ino)
            names = [b".", b".."] + sorted(ino.entries)
            w.u32(OK)
            self.post(w, ino)
            w.fixed(myverf)
            size = 16 + 88 + 8
            i = cookie
            ents = W()
            n = 0
            while i < len(names):
                name = names[i]
                c = ino if name == b"." else self.inodes[ino.parent] if name == b".." else self.inodes[ino.entries[name]]
                e = W().u32(1).u64(c.fileid).opaque(name).u64(i + 1)
                if proc == 17:
                    self.post(e, c)
                    e.u32(1).opaque(self.fh(c))
                eb = e.bytes()
                if size + len(eb) + 8 > maxcount and n:
                    break
                if size + len(eb) + 8 > maxcount:
                    return self.fail(proc, 10005, W(), ino=ino)   # TOOSMALL
                ents.parts.append(eb)
                size += len(eb)
                n += 1
                i += 1
            w.parts.append(ents.bytes())
            w.u32(0).u32(1 if i >= len(names) else 0)
            return w.bytes()
        if proc == 18:                                            # FSSTAT
            w.u32(OK)
            self.post(w, ino)
            for v in self.fsstat:
                w.u64(v)
            w.u32(0)
            return w.bytes()
        if proc == 19:                                            # FSINFO
            w.u32(OK)
            self.post(w, ino)
            w.u32(self.rtmax).u32(self.rtmax).u32(4096).u32(self.rtmax).u32(self.rtmax).u32(4096).u32(8192)
            w.u64((1 << 63) - 1).u32(0).u32(1).u32(0x1B)
            return w.bytes()
        if proc == 20:                                            # PATHCONF
            w.u32(OK)
            self.post(w, ino)
            w.u32(32000).u32(255).u32(1).u32(0).u32(0).u32(1)
            return w.bytes()
        if proc == 21:                                            # COMMIT
            r.u64(); r.u32()
            before = self.pre(ino)
            if ino.type == NF3REG:
                ino.committed = bytearray(ino.data)
            w.u32(OK)
            self.wcc(w, before, ino)
            w.fixed(self.verf)
            return w.bytes()
        return None


# ---- a tiny client, for the self-test ----
def rpc_call(s, xid, prog, vers, proc, args=b"", uid=0, gid=0):
    cred = W().u32(0).opaque(b"test").u32(uid).u32(gid).u32(0).bytes()
    body = W().u32(xid).u32(0).u32(2).u32(prog).u32(vers).u32(proc).u32(1).opaque(cred).u32(0).u32(0).bytes() + args
    s.sendall(struct.pack(">I", 0x80000000 | len(body)) + body)
    rec = b""
    while True:
        h = Sim._recvn(s, 4)
        n = struct.unpack(">I", h)[0]
        rec += Sim._recvn(s, n & 0x7FFFFFFF)
        if n >> 31:
            break
    r = R(rec)
    assert r.u32() == xid
    assert r.u32() == 1 and r.u32() == 0
    r.u32(); r.opaque()
    assert r.u32() == 0
    return r


def self_test():
    sim = Sim()
    sim.put_file("hello.txt", b"hello\n")
    s = socket.create_connection(("127.0.0.1", sim.ports["pmap"]))
    r = rpc_call(s, 1, PMAP_PROG, 2, 3, W().u32(MOUNT_PROG).u32(3).u32(6).u32(0).bytes())
    assert r.u32() == sim.ports["mount"]
    s.close()
    s = socket.create_connection(("127.0.0.1", sim.ports["mount"]))
    r = rpc_call(s, 2, MOUNT_PROG, 3, 1, W().opaque(b"/export").bytes())
    assert r.u32() == 0
    root = r.opaque(64)
    s.close()
    s = socket.create_connection(("127.0.0.1", sim.ports["nfs"]))
    r = rpc_call(s, 3, NFS_PROG, 3, 3, W().opaque(root).opaque(b"hello.txt").bytes())
    assert r.u32() == 0
    fh = r.opaque(64)
    r = rpc_call(s, 4, NFS_PROG, 3, 6, W().opaque(fh).u64(0).u32(100).bytes())
    assert r.u32() == 0
    assert r.u32() == 1
    r.fixed(84)                                   # the attributes
    assert r.u32() == 6 and r.u32() == 1 and r.opaque() == b"hello\n"
    # a WRITE, the DRC answering the same xid again without doing it twice
    args = W().opaque(fh).u64(6).u32(3).u32(0).opaque(b"abc").bytes()
    rpc_call(s, 5, NFS_PROG, 3, 7, args)
    rpc_call(s, 5, NFS_PROG, 3, 7, args)
    assert sim.drc_hits == 1 and bytes(sim.path("hello.txt").data) == b"hello\nabc"
    sim.fault("restart")
    assert bytes(sim.path("hello.txt").data) == b"hello\n"      # the UNSTABLE write was lost
    sim.stop()
    print("nfs-sim: self-test PASS")


def main():
    a = sys.argv[1:]
    if not a or a[0] == "--self-test":
        self_test()
        return
    opts = {"--pmap": 111, "--mount": 0, "--nfs": 2049}
    po = None
    i = 1
    if a[0] == "--pmap-only":
        po = (int(a[1]), int(a[2]))
        i = 3
    while i + 1 < len(a):
        opts[a[i]] = int(a[i + 1])
        i += 2
    sim = Sim(opts["--pmap"], opts["--mount"], opts["--nfs"], pmap_only=po)
    print("nfs-sim: serving", sim.ports, flush=True)
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
