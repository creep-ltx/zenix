#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# mail in FS-UAE (DESIGN D59 section 8): the A1200 with 4 MB of fast RAM (the minimum Zenix is made for) and FS-UAE's
# A2065 (slirp) against tools/host/mail-fake.py on the PC, in a user, mount and network namespace (tools/mail-ns.py:
# IMAP over TLS on 10.0.2.2:993 and SMTP with STARTTLS on :587, a test CA made at this run whose anchors file becomes
# the image's /etc/ssl/anchors; rsa.test in /etc/hosts). mail runs on the console with a settings file (imap and smtp
# rsa.test, the defaults' ports and TLS, a sent folder, an editor that is a script: it fills in a new message's To and
# Subject and adds two lines); `termtest screen` reads the console's grid at each step and then types the next keys:
# the index (newest first, unread bold, attachments marked), a message with an alternative and attachments opened and
# its first attachment saved, the HTML-only message read, r (a reply, sent through STARTTLS, its copy appended to
# Sent), c (the folders, modified UTF-7 shown), m (a new message, sent), d and Q (the deleted message removed at the
# end). Then mail -s from the command line with its text on standard input. Checked: the screen's text at each step,
# the attachment saved byte for byte, and each message the fake SMTP server received byte for byte against the same
# draft built on the PC by mail's core (tools/host/mail-client build; only Date and Message-ID differ), the envelope,
# the commands on the wire (APPEND, EXPUNGE). The memory with mail open (and the TLS helper) is printed.
#   tools/mail-test.sh
# Needs a C compiler, Python's cryptography module, debugfs and unprivileged user namespaces with an /etc/subuid range.
cd "$(dirname "$0")/.." || exit 1
grep() { /usr/bin/grep "$@"; }
BD=${ZENIX_BUILD:-build}
make -s BUILD=$BD all >/dev/null 2>&1 || { echo "FAIL the build"; exit 1; }
OUT=${ZENIX_OUT:-/tmp/zenix-mail}; mkdir -p $OUT
OUT=$(cd "$OUT" && pwd); BD=$(cd "$BD" && pwd)
for t in debugfs e2fsck cc; do command -v $t >/dev/null || { echo "FAIL $t is not installed"; exit 1; }; done
python3 -c 'import cryptography' 2>/dev/null || { echo "FAIL Python's cryptography module is missing"; exit 1; }
unshare --map-root-user --map-auto -nm true 2>/dev/null || { echo "FAIL unshare --map-root-user --map-auto fails (newuidmap, /etc/subuid)"; exit 1; }
# mail's core on the PC, to build the same drafts (no sanitizers: only the bytes matter here)
BRDEF="-DBR_LE_UNALIGNED=0 -DBR_BE_UNALIGNED=0 -DBR_USE_URANDOM=0 -DBR_USE_GETENTROPY=0 -DBR_USE_UNIX_TIME=0 -DBR_RDRAND=0"
# shellcheck disable=SC2086
cc -std=gnu99 -O1 -DMAIL_HOST $BRDEF -Icrypto/bearssl/inc -Icrypto/bearssl/src -o $OUT/mail-client tools/host/mail-client.c \
    net/mail/*.c crypto/zc/*.c $(find crypto/bearssl/src -name '*.c') crypto/monocypher/monocypher.c \
    crypto/monocypher/monocypher-ed25519.c 2>$OUT/client.build.log || { echo "FAIL tools/host/mail-client does not build"; exit 1; }
T=$OUT/ns; rm -rf "$T"; mkdir -p "$T"
head -c 64 /dev/urandom > $OUT/seed
rm -f $BD/zenix.hdf; make -s BUILD=$BD $BD/zenix.hdf >/dev/null 2>&1 || { echo "FAIL the image"; exit 1; }
printf 'auto eth0\niface eth0 inet dhcp\n    driver a2065\n' > $OUT/interfaces
printf '127.0.0.1\tlocalhost\n10.0.2.2\trsa.test\n' > $OUT/hosts
printf 'imap rsa.test\nuser amiga@example.org\npassword fake-pass-1\nsmtp rsa.test\nfrom Amiga Tester <amiga@example.org>\nsent Sent\neditor sh /tests/ed.sh\n' > $OUT/config
cat > $OUT/ed.sh <<'EOS'
if grep -q '^To: *$' "$1"; then
    sed -e 's/^To: *$/To: Bob <bob@example.org>/' -e 's/^Subject: *$/Subject: Zenix grüßt/' "$1" > /tests/ed.tmp
    cat /tests/ed.tmp > "$1"
fi
printf 'Thanks from Zenix: räksmörgås.\n.a dot line\n' >> "$1"
cat "$1" >> /tests/drafts.txt
echo ==END== >> /tests/drafts.txt
EOS
cat > $OUT/setup.sh <<'EOS'
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30; do
    ifconfig eth0 2>/dev/null | grep -q 'lease [0-9]' && break
    sleep 1
done
cat /tests/seed > /dev/random
mkdir -p /tmp/h/.config/mail; cp /tests/config /tmp/h/.config/mail/config; chmod 600 /tmp/h/.config/mail/config
echo setup-done
EOS
cat > $OUT/k1.sh <<'EOS'
termtest screen /dev/tty1 'hello from alice' 900 'jjjj\r' > /tmp/s0.txt
free > /tmp/free.txt; ps > /tmp/ps.txt
termtest screen /dev/tty1 'mjukt' 300 's1\r\x15/tests/saved.pdf\r' > /tmp/s1.txt
termtest screen /dev/tty1 'saved 274 bytes' 120 'qk\r' > /tmp/s2.txt
termtest screen /dev/tty1 'News' 300 'r' > /tmp/s3.txt
termtest screen /dev/tty1 'Send to mallory@example.org' 300 'y' > /tmp/s4.txt
termtest screen /dev/tty1 'a copy kept' 900 'qc' > /tmp/s5.txt
termtest screen /dev/tty1 'Entwürfe' 120 'qm' > /tmp/s6.txt
termtest screen /dev/tty1 'Send to bob@example.org' 300 'y' > /tmp/s7.txt
termtest screen /dev/tty1 'a copy kept' 900 'd' > /tmp/s8.txt
termtest screen /dev/tty1 'deleted (sync' 120 'Q' > /tmp/s9.txt
termtest screen /dev/tty1 'Remove the 1 deleted' 120 'y' > /tmp/s10.txt
EOS
cat > $OUT/k.txt <<'EOS'
wait zenix:/# 120
sleep 1
send sh /tests/setup.sh\r
wait setup-done 120
send sh /tests/k1.sh & HOME=/tmp/h mail; echo st1=$?\r
wait st1= 3000
send printf 'Batch text: åäö\\n.dot first\\nend\\n' | HOME=/tmp/h mail -s 'Batch ünïcode' carol@example.org dave@example.org; echo st2=$?\r
wait st2= 900
send for i in 0 1 2 3 4 5 6 7 8 9 10; do echo ==s$i; cat /tmp/s$i.txt; done; echo ==mem; cat /tmp/free.txt /tmp/ps.txt; echo ==files; ls -l /tmp/h/.config/mail; grep -c reply_all /tmp/h/.config/mail/keys; sync; echo ==m""done\r
wait ==mdone 120
stop
EOS
cat > $OUT/inner.sh <<'EOS'
#!/usr/bin/env bash
# inside the namespace: the files onto the image, the run, then the files the run wrote taken off it
T=$1 OUT=$2 BD=$3
img=$BD/zenix.hdf
put() {     # put FILE PATH MODE: a file on the image, root's
    debugfs -w -R "rm $2" $img >/dev/null 2>&1
    debugfs -w -R "write $1 $2" $img >/dev/null 2>&1
    debugfs -w -R "sif $2 uid 0" $img >/dev/null 2>&1
    debugfs -w -R "sif $2 gid 0" $img >/dev/null 2>&1
    debugfs -w -R "sif $2 mode 0100$3" $img >/dev/null 2>&1
}
mkd() { debugfs -w -R "mkdir $1" $img >/dev/null 2>&1; debugfs -w -R "sif $1 uid 0" $img >/dev/null 2>&1
        debugfs -w -R "sif $1 mode 040755" $img >/dev/null 2>&1; }
mkd /etc/network; mkd /etc/ssl
put $OUT/interfaces /etc/network/interfaces 644
put $OUT/hosts /etc/hosts 644
put $T/tls/test.anchors /etc/ssl/anchors 644
put $OUT/seed /tests/seed 600
put $OUT/config /tests/config 600
for f in setup k1 ed; do put $OUT/$f.sh /tests/$f.sh 755; done
ZENIX_BUILD=$BD tools/run-test.sh "$OUT/a1200-a2065.fs-uae" 3600 "$OUT/k.txt"
rm -f $OUT/saved.pdf $OUT/drafts.txt
debugfs -R "dump /tests/saved.pdf $OUT/saved.pdf" $img >/dev/null 2>&1
debugfs -R "dump /tests/drafts.txt $OUT/drafts.txt" $img >/dev/null 2>&1
exit 0
EOS
chmod +x $OUT/inner.sh
sed '/^\[fs-uae\]/a network_card = a2065' fsuae/a1200-020-fast.fs-uae > "$OUT/a1200-a2065.fs-uae"
unshare --map-root-user --map-auto -nm python3 -I tools/mail-ns.py "$T" -- "$OUT/inner.sh" "$T" "$OUT" "$BD" 2>&1 | tr -d '\r' > $OUT/run.log
# the drafts and the batch text built on the PC by the same core, for the byte comparison
python3 - "$OUT" <<'PY'
import os, sys
o = sys.argv[1]
d = open(os.path.join(o, "drafts.txt"), "rb").read() if os.path.exists(os.path.join(o, "drafts.txt")) else b""
parts = [p.lstrip(b"\n") for p in d.split(b"==END==\n") if p.strip()]
parts.append("From: Amiga Tester <amiga@example.org>\nTo: carol@example.org, dave@example.org\nSubject: Batch ünïcode\n\n"
             "Batch text: åäö\n.dot first\nend\n".encode())
sc = open(os.path.join(o, "build.script"), "w")
for i, p in enumerate(parts):
    open(os.path.join(o, "draft%d.txt" % i), "wb").write(p)
    sc.write("build %s %s\n" % (os.path.join(o, "draft%d.txt" % i), os.path.join(o, "expect%d.eml" % i)))
PY
$OUT/mail-client $OUT/build.script > $OUT/build.out 2>&1
pass=1
python3 - "$OUT/run.log" "$T/mail.log" "$T/smtp" "$OUT" <<'PY' || pass=0
import os, re, sys
sys.path.insert(0, "tools")
from screenlog import Log, rows, attrs
L = Log(sys.argv[1])
check = L.check
check("==mdone" in L.text, "the run reached its end (the emulator ran the whole script)", L.text[-400:])
srv = open(sys.argv[2], encoding="utf-8", errors="replace").read() if os.path.exists(sys.argv[2]) else ""
smtp, out = sys.argv[3], sys.argv[4]
s = [rows(L.part("s%d" % i, "s%d" % (i + 1) if i < 10 else "mem")) for i in range(11)]
a = [attrs(L.part("s%d" % i, "s%d" % (i + 1) if i < 10 else "mem")) for i in range(11)]
txt = lambda rr: "\n".join(rr.get(i, "") for i in sorted(rr))
st = lambda k: (lambda m: m and int(m.group(1)))(re.search(r"%s=(\d+)" % k, L.text))   # (mail's last escape ends in a letter)
check(st("st1") == 0 and st("st2") == 0, "mail ended with status 0, and mail -s too", (st("st1"), st("st2")))
r = s[0]
n = max(r) if r else 0
check(n >= 24 and r.get(0, "").startswith(" mail 0.1b1  INBOX  6 messages, 4 unread"), "the title row: the folder, counts", r.get(0))
D = r"(?:Nov 16|  2026)"     # (the date column: "  2026" when the emulated clock is in another year)
check(re.match(r"^N  %s  Bob +plans" % D, r.get(1, "")) and re.match(r"^N\+ %s  Björn +räksmörgås" % D, r.get(5, "")) and
      re.match(r"^   %s  Alice Example +hello from alice" % D, r.get(6, "")) and "ok [2J]0;owned html" in r.get(4, ""),
      "the index: newest first, N unread, + attachments, names, the subjects decoded and clean", txt(r))
check(a[0].get(1, "").startswith("7777777777") and 6 not in a[0], "the cursor's row reversed, a read message plain", a[0])
check("Enter:open" in r.get(n - 1, "") or "Return:open" in r.get(n - 1, ""), "the keys row", r.get(n - 1))
t1 = txt(s[1])
check("message 5 of 6" in s[1].get(0, "") and "From: Björn <bjorn@example.org>" in t1 and "Subject: räksmörgås" in t1 and
      "Här är texten med ÅÄÖ, en lång rad som bryts mjukt." in t1 and "HTML version" not in t1,
      "a message read: headers, the alternative's plain part (quoted-printable ISO-8859-1)", t1)
check("[1] räkning_.._secret.pdf  (application/pdf, 282 bytes)" in t1 and "[2] notes.txt  (text/plain, 10 bytes)" in t1,
      "its attachments listed", t1)
pdf = open(os.path.join(out, "saved.pdf"), "rb").read() if os.path.exists(os.path.join(out, "saved.pdf")) else b""
check(pdf == b"%PDF-1.4 fake pdf\n" + bytes(range(256)) and "saved 274 bytes to /tests/saved.pdf" in txt(s[2]),
      "the attachment saved byte for byte", (len(pdf), txt(s[2])[-200:]))
t3 = txt(s[3])
check("(from HTML)" in s[3].get(0, "") and "News" in t3 and "Café & more[31m red?" in t3 and "* one" in t3 and
      "link <http://example.org/x>" in t3 and "evil" not in t3 and "Subject: ok [2J]0;owned html" in t3,
      "the HTML-only message: made text, the escapes dropped (on the screen as text, not obeyed)", t3)
check(not any(set(v) - set(".07f") for k, v in a[3].items() if 2 <= k < n - 1 and "Subject" not in s[3].get(k, "") and
              k not in (1, 2, 3, 4)), "no colour from the message's escape", a[3])
check("Send to mallory@example.org? y send, e edit, q drop it:" in txt(s[4]), "the reply: the editor ran, the question asked", txt(s[4]))
check("sent, a copy kept in the sent folder" in txt(s[5]), "the reply sent and a copy kept", txt(s[5]))
check("Entwürfe" in txt(s[6]) and "* INBOX" in txt(s[6]) and "Parent" not in txt(s[6]), "the folders: modified UTF-7, the current one marked, \\Noselect left out", txt(s[6]))
check("Send to bob@example.org? y send" in txt(s[7]) and "a copy kept" in txt(s[8]), "a new message: the editor filled it, sent", txt(s[8]))
check(re.search(r"^D", s[9].get(4, "")) is not None, "d: the message marked D", txt(s[9]))
check("Remove the 1 deleted message from the server? y/n" in txt(s[10]), "Q: the deleted message offered for removal", txt(s[10]))
check(re.search(r"IMAP 1 > A\d+ UID STORE 102 \+FLAGS\.SILENT \(\\Deleted\)\n(.*\n)*IMAP 1 > A\d+ EXPUNGE\n", srv) is not None and
      srv.count("APPEND \"Sent\" (\\Seen)") == 2 and "UID STORE 102 +FLAGS.SILENT (\\Answered)" in srv and
      "UID STORE 101 +FLAGS.SILENT (\\Seen)" in srv and "IMAP 1 > A" in srv and "LOGOUT" in srv,
      "on the wire: read, answered, deleted and expunged, two copies appended, the logout", srv[-1500:])
mask = lambda x: re.sub(rb"(Date|Message-ID): [^\r]*", rb"\1: X", x)
envs = []
for i in range(3):
    got = open(os.path.join(smtp, "smtp-%d.eml" % (i + 1)), "rb").read() if os.path.exists(os.path.join(smtp, "smtp-%d.eml" % (i + 1))) else b""
    exp = open(os.path.join(out, "expect%d.eml" % i), "rb").read() if os.path.exists(os.path.join(out, "expect%d.eml" % i)) else b"?"
    envs.append(open(os.path.join(smtp, "smtp-%d.env" % (i + 1))).read() if os.path.exists(os.path.join(smtp, "smtp-%d.env" % (i + 1))) else "")
    check(got and mask(got) == mask(exp) and re.search(rb"Date: \w\w\w, \d+ \w\w\w (19|20)\d\d \d\d:\d\d:\d\d \+0000\r\n", got) and
          re.search(rb"Message-ID: <[0-9a-f]{8}\.[0-9a-f]{8}@example\.org>\r\n", got),
          "message %d received byte for byte as the PC's core builds the same draft (Date, Message-ID aside)" % (i + 1), (got[:400], exp[:400]))
r1 = open(os.path.join(smtp, "smtp-1.eml"), "rb").read() if os.path.exists(os.path.join(smtp, "smtp-1.eml")) else b""
check(b"To: Mallory <mallory@example.org>\r\n" in r1 and b"Subject: Re: ok [2J]0;owned html\r\n" in r1 and
      b"> Caf=C3=A9 & more[31m red?" in r1 and b"\r\n.a dot line\r\n" in r1, "the reply: To, Re:, the text quoted, a dot line", r1[:900])
r3 = open(os.path.join(smtp, "smtp-3.eml"), "rb").read() if os.path.exists(os.path.join(smtp, "smtp-3.eml")) else b""
check(envs[0] == "amiga@example.org\nmallory@example.org\n" and envs[1] == "amiga@example.org\nbob@example.org\n" and
      envs[2] == "amiga@example.org\ncarol@example.org dave@example.org\n" and b"Subject: =?UTF-8?B?" in r3 and
      b"\r\n.dot first\r\n" in r3, "the envelopes; mail -s: the subject as words, the text from standard input", (envs, r3[:500]))
check("STARTTLS" in srv and "SMTP 3 > STARTTLS" in srv, "every message sent through STARTTLS", srv[-600:])
f = L.part("files", "mdone")
check(re.search(r"^-rw------- .* config$", f, re.M) is not None and re.search(r"^1$", f, re.M) is not None,
      "the settings kept at mode 600; the keys file written with the defaults", f)
m = L.part("mem", "files")
ps = re.search(r"^ +\d+ +\d+ +\d+ +-?\d+ +\w+ +(\d+) +(\d+) mail$", m, re.M)
tp = re.search(r"^ +\d+ +\d+ +\d+ +-?\d+ +\w+ +(\d+) +(\d+) tlspipe$", m, re.M)
print("memory with mail open over TLS (A1200 + 4 MB fast, FS-UAE): mail %s KB, tlspipe %s KB" %
      (ps and ps.group(1), tp and tp.group(1)))
L.waits()
L.done()
PY
e2fsck -fn $BD/zenix.hdf > $OUT/e2fsck.log 2>&1 && echo "PASS the host's e2fsck finds the root clean" || { echo "FAIL e2fsck"; pass=0; }
[ $pass = 1 ] && echo "mail-test: all checks passed" || { echo "mail-test: some checks failed (log $OUT/run.log)"; exit 1; }
