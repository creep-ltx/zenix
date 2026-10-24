# SPDX-License-Identifier: GPL-2.0-only
# Audit batch E: the shell and command fixes, run by tools/cmds2-test.sh (output compared with tools/tests/batch-e.expected)
# The loops run in flat memory: each round gives its arena space back (a 1000-character case subject used to leak ~3 KB a round)
T=/tmp/batch-e.t; rm -rf $T; mkdir -p $T     # this script's own scratch folder, emptied first: repeatable
big=xxxxxxxxxx; big=$big$big$big$big$big$big$big$big$big$big; big=$big$big$big$big$big$big$big$big$big$big
i=0; while [ $i -lt 400 ]; do case "$big" in x*) ;; esac; i=$((i+1)); done; echo "while-flat i=$i"
n=0; for w in $(seq 1 400); do case "$big" in y*) ;; *) n=$((n+1));; esac; done; echo "for-flat n=$n"
# no silent caps: arguments, glob matches, words, pipeline stages, for items, xargs
echo $(seq 1 200) | wc -w
/bin/echo $(seq 1 250) | wc -w
seq 1 600 | xargs | wc -w
echo 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 32 33 34 35 36 37 38 39 40 41 42 43 44 45 46 47 48 49 50 51 52 53 54 55 56 57 58 59 60 61 62 63 64 65 66 67 68 69 70 | wc -w
mkdir $T/be; cd $T/be; for i in $(seq 1 150); do : > f$i; done; echo * | wc -w; rm *; ls | wc -l; cd /
echo a | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | wc -c
# expansions and builtins
v=abcdef; echo "sub=${v:1:-2}"
echo "${v:4:-5}"; echo "neg-st=$?"
set -- a b c d e f g h i j k l; echo "ten=${10} eleven=${11}"
shift -1; echo "shift-st=$?"
shift 2; echo "after-shift=$1 $#"
f() { local x=inner; echo "in=$x"; }; x=outer; f; echo "out=$x"
g() { local y; y=set; }; unset y; g; echo "y=[${y-unset}]"
local z=1; echo "local-st=$?"
setter() { set -- p q r; }; setter; echo "pos=$1"
set -- $(echo one two three); i=0; while [ $i -lt 5 ]; do case x in x) ;; esac; i=$((i+1)); done; echo "kept=$1 $3"
echo swap 3>&1 1>&2 2>&3; echo y >&3; echo "fd3-st=$?"
alias a1=b1; alias b1=a1; a1; echo "alias-st=$?"
o=; c=; i=0; while [ $i -lt 700 ]; do o="($o"; c="$c)"; i=$((i+1)); done
eval "echo \$(( $o 1 $c ))"; echo "arith-st=$?"; echo "arith=$(( (2+3)*4 ))"
sh -c 'cat <<E1 <<E2 <<E3 <<E4 <<E5 <<E6 <<E7 <<E8 <<E9
h1
E1
h2
E2
h3
E3
h4
E4
h5
E5
h6
E6
h7
E7
h8
E8
h9
E9
'; echo "hd-st=$?"
# commands
printf 'a b\n' | xargs -I '' echo x; echo "xargs-I-st=$?"
mkdir -p $T/d1/s $T/d2/s; echo same > $T/d1/s/f; echo same > $T/d2/s/f; echo one > $T/d1/g; echo two > $T/d2/g
diff -r $T/d1 $T/d2; echo "diff-st=$?"
timeout 3 watch -n 1 echo $(seq 1 150) > /dev/null; echo "watch-st=$?"
cd $T; mkdir tt; echo data > tt/a; tar cf $T/t.tar tt; tar tf $T/t.tar; echo "tar-t=$?"
echo X > $T/x1; dd if=$T/x1 of=$T/t.tar bs=1 seek=100 count=1 > /dev/null 2>&1; tar tf $T/t.tar; echo "tar-bad=$?"
tar cf $T/t2.tar tt; head -c 700 $T/t2.tar > $T/short.tar; mkdir $T/tx; tar xf $T/short.tar -C $T/tx; echo "tar-short=$?"; cd /
expr file.txt : '.*\.\(.*\)'; expr abc : 'a.'; expr abc : 'x'; echo "expr-st=$?"
mkdir $T/pb; cp /bin/echo $T/pb/myecho; chmod 755 $T/pb/myecho
PATH=$T/pb:/bin find $T/pb -name myecho -exec myecho found {} ';'
PATH=$T/pb:/bin timeout 5 myecho timeout-found
echo x | PATH=$T/pb:/bin xargs myecho xargs-found
printf 'b\na\n' | sort -; printf 'd\nc\n' | sort -- -
echo hello | grep -H hell -
grep x /nonexistent; echo "grep-st=$?"
mkdir -p $T/m1/d $T/m2; echo moved > $T/m1/d/f; mv $T/m1/d $T/m2/; cat $T/m2/d/f; ls $T/m1 | wc -l
ln $T/m2 $T/hard; echo "ln-dir=$?"       # a hard link to a directory: refused, with the reason
# audit G3: a process killed while it reads closes its file once (a nested exit closed it twice and leaked one of the
# 64 file slots per kill: about 60 kills ran the system out of files)
dd if=/dev/zero of=$T/big bs=1024 count=3000 > /dev/null 2>&1
for i in $(seq 1 70); do cat $T/big > /dev/null & sleep 1; kill $!; wait; done
echo after-kills > $T/g3; cat $T/g3
echo batch-e-done
