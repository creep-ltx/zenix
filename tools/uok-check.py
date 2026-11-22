#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""uok-check: DESIGN D60 5.7 and 13 (docs/d60-full-protection.md): every user pointer a system call follows is checked
first, and the check names its kind. Run at every build of kernel.bin (Makefile).

Read from kernel/syscall.c's syscall_c: each `case SYS_...:` block that turns one of the call's arguments a1..a3 into a
pointer (a cast such as `(struct ustat *)a2`, `(void *)a3`, `((u32 *)a1)[0]`) must check that argument in the same
block first, with one of
  UOKW(aN, ...)  UOKR(aN, ...)  UOKS(n, aN, ...)  user_ok_call_wr/rd(current, aN, ...)   kind "call": the caller's own
                                                  memory or a window of a request it holds, for the time of the call
  user_ok_wr/rd(current, aN, ...)                 kind "own": the caller's own memory only (the kernel keeps the pointer,
                                                  or must have the caller's own memory: SYS_call's message)
  CSTR(name, aN)                                  copied in (copy_str checks it)
or be one of EXEMPT below, each with its reason (reviewed). An argument handed on as a number (do_spawn(a1, a2, a3),
sys_path1, ipc_peer, dbg_sys ...) is checked by the function it goes to; this check does not follow it there.
usage: tools/uok-check.py kernel/syscall.c        (prints nothing and exits 0 when every use is checked)
"""
import re
import sys

# case label -> why its unchecked pointer is safe (reviewed 2026-11-22, D60 stage 2)
EXEMPT = {
    "SYS_reply": "the message is looked up among the requests in flight (ipc_rec) before anything in it is read",
    "SYS_replyrecv": "the same (ipc_rec)",
    "SYS_blk_read": "the disk server only (priv): the buffer is its client's; checked once diskd asks for it (D60 stage 3)",
    "SYS_blk_write": "the same",
    "SYS_fd_rw": "the disk server only (priv), as blk_read",
    "SYS_memfree": "the address must be one of the caller's own regions (compared with them, followed only then)",
}

CHECKS = [(r"UOK[WR]\(\s*a{n}\b", "call"), (r"UOKS\(\s*\w+\s*,\s*a{n}\b", "call"),
          (r"user_ok_call_(?:wr|rd)\(\s*current\s*,\s*a{n}\b", "call"),
          (r"user_ok_(?:wr|rd)\(\s*current\s*,\s*a{n}\b", "own"), (r"CSTR\(\s*\w+\s*,\s*a{n}\b", "copied")]
CAST = re.compile(r"\(\s*(?:const\s+)?(?:struct\s+)?\w+\s*\*\s*\)\s*a([123])\b")


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    src = open(sys.argv[1]).read()
    m = re.search(r"\nvoid syscall_c\(u32 \*rb\)\n\{(.*?)\n\}\n", src, re.S)
    if not m:
        sys.exit("uok-check: no syscall_c in " + sys.argv[1])
    body = re.sub(r"/\*.*?\*/", lambda c: " " * len(c.group(0)), m.group(1), flags=re.S)
    # the blocks: from a run of case labels to the next one
    starts = [(c.start(), c.group(1)) for c in re.finditer(r"\n    case (SYS_\w+):", body)]
    errs, n = [], 0
    for i, (at, label) in enumerate(starts):
        end = starts[i + 1][0] if i + 1 < len(starts) else len(body)
        blk = body[at:end]
        labels = re.findall(r"case (SYS_\w+):", blk)
        for c in CAST.finditer(blk):
            arg = c.group(1)
            n += 1
            if any(lb in EXEMPT for lb in labels):
                continue
            before = blk[:c.start()]
            if not any(re.search(p.format(n=arg), before) for p, _ in CHECKS):
                line = src[:m.start(1) + at + c.start()].count("\n") + 1
                errs.append(f"{sys.argv[1]}:{line}: {'/'.join(labels)} follows a{arg} as a pointer without checking "
                            f"it first (UOKW/UOKR/UOKS: for the call; user_ok_wr/rd: own; tools/uok-check.py)")
    for e in errs:
        print("uok-check:", e)
    if errs:
        sys.exit(1)


if __name__ == "__main__":
    main()
