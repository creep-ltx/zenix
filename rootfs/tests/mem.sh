# SPDX-License-Identifier: GPL-2.0-only
# Memory tests (audit batch B), run by tools/memtest.sh under Zenix only. Every check prints one line ending in
# "ok" or "FAILED"; the test tool counts them.

# B1-B3: memory calls give 0 on failure, sizes near 4 GB do not wrap, malloc/free/realloc keep their promises
badptr

# B2: a long-running shell does not lose memory to variables, aliases and functions that come and go. The shell's
# free memory is read after a warm-up round and again after many more rounds: it must not have shrunk.
fastfree() { n=0; for x in $(free | cut -d" " -f4); do n=$(( n + x )); done; echo $n; }
churn() {
    i=0
    while [ $i -lt $1 ]; do
        v="x$i"; w="$v$v$v$v$v$v$v$v$v$v$v$v$v$v$v$v"; long="$w$w$w$w"
        eval "var$i=\$long"; eval "unset var$i"
        alias "al$i=echo $long"; unalias "al$i"
        f() { echo "$long"; }
        i=$(( i + 1 ))
    done
}
churn 20
before=$(fastfree)
churn 100
after=$(fastfree)
if [ "$after" -ge "$before" ]; then echo "mem: shell churn kept memory flat ($before KB -> $after KB) ok"
else echo "mem: shell churn lost $(( before - after )) KB ($before KB -> $after KB) FAILED"; fi

# B6: tools that walk directories work 25 levels deep (no stack arrays) and stop cleanly at the path limit
T=/tmp/mem.t; rm -rf $T; mkdir -p $T; cd $T     # this script's own scratch folder, emptied first: repeatable
p=deep; mkdir deep; k=0
while [ $k -lt 25 ]; do p=$p/d; mkdir $p; k=$(( k + 1 )); done
echo bottom > $p/f
n=$(find deep | wc -l); [ $n -eq 27 ] && echo "mem: find 25 deep ok" || echo "mem: find 25 deep ($n) FAILED"
du deep > $T/du.out && [ $(wc -l < $T/du.out) -eq 26 ] && echo "mem: du 25 deep ok" || echo "mem: du 25 deep FAILED"
grep -r bottom deep > $T/gr.out && [ $(wc -l < $T/gr.out) -eq 1 ] && echo "mem: grep -r 25 deep ok" || echo "mem: grep -r 25 deep FAILED"
cp -r deep deep2 && diff -r deep deep2 && echo "mem: cp -r, diff -r 25 deep ok" || echo "mem: cp -r, diff -r 25 deep FAILED"
chown -R 0 deep2 && echo "mem: chown -R 25 deep ok" || echo "mem: chown -R 25 deep FAILED"
tar cf deep.tar deep && [ $(tar tf deep.tar | wc -l) -eq 27 ] && echo "mem: tar c 25 deep ok" || echo "mem: tar c 25 deep FAILED"
rm -r deep2 && [ ! -d deep2 ] && echo "mem: rm -r 25 deep ok" || echo "mem: rm -r 25 deep FAILED"
mkdir mvd && mv deep mvd/ && [ -f mvd/$p/f ] && echo "mem: mv 25 deep ok" || echo "mem: mv 25 deep FAILED"
rm -r mvd deep.tar
cd /

# B7: program files with a corrupt header or relocation table are refused (the kernel says why on its log),
# nothing is loaded, and a good program still runs afterwards
k=1
while [ $k -le 11 ]; do
    /tests/badzexe$k 2>$T/bz.err; rc=$?
    if [ $rc -eq 127 ]; then echo "mem: corrupt program $k not found (/tests/badzexe$k is missing) FAILED"
    elif [ $rc -ne 0 ]; then echo "mem: corrupt program $k refused (rc $rc) ok"
    else echo "mem: corrupt program $k ran FAILED"; fi
    k=$(( k + 1 ))
done
n=$(dmesg | grep -c 'exec: /tests/badzexe.* refused')
[ $n -eq 11 ] && echo "mem: the kernel named all 11 refusals ok" || echo "mem: the kernel named $n of 11 refusals FAILED"
hello 1 | grep -q 'hello from pid' && echo "mem: a good program still runs ok" || echo "mem: a good program still runs FAILED"
# awk: its fixed 64 KB stack (no MMU, nothing grows it). Endless recursion and a deeply nested expression stop with
# awk's own message and status 2, not with the kernel's stack guard killing it; a long loop does not lose memory
# (20000 x 1 KB strings would need 20 MB if each one leaked).
awk 'function f(n) { return f(n + 1) } BEGIN { f(1) }' > $T/awk1.out 2>&1; rc=$?
[ $rc -eq 2 ] && grep -q 'nested too deeply' $T/awk1.out && echo "mem: awk endless recursion stopped cleanly ok" || echo "mem: awk endless recursion (rc $rc) FAILED"
e=1; k=0; while [ $k -lt 600 ]; do e="($e)"; k=$(( k + 1 )); done
awk "BEGIN { print $e }" > $T/awk2.out 2>&1; rc=$?
{ [ $rc -eq 0 ] || grep -q 'nested too deeply' $T/awk2.out; } && echo "mem: awk deep expression (rc $rc) ok" || echo "mem: awk deep expression (rc $rc) FAILED"
dmesg | grep -q 'awk.*overflowed' && echo "mem: awk overflowed its stack FAILED" || echo "mem: the stack guard never had to kill awk ok"
awk 'BEGIN { s = sprintf("%1000s", "x"); for (i = 0; i < 20000; i++) { t = s i; a[i % 10] = t } print "done" }' > $T/awk3.out 2>&1
grep -q '^done$' $T/awk3.out && echo "mem: awk 20000 string assignments ok" || echo "mem: awk 20000 string assignments FAILED: $(head -c 80 $T/awk3.out)"
echo "mem: done"
