# SPDX-License-Identifier: GPL-2.0-only
# command tests: run under Zenix and under bash with GNU tools; the output must be identical
D=${1:-/home/t}
T=/tmp/cmds.t                   # this script's own scratch folder, emptied first: repeatable, and no clash with other tests
export T; rm -rf $T; mkdir -p $T      # exported: the unlinkopen line runs a second sh
cd $D
t() { echo "== $1"; }
t head;      head -n 2 fruit.txt; head -2 text.txt; head -c 5 fruit.txt; echo
t tail;      tail -n 2 fruit.txt; tail -n +4 fruit.txt; tail -c 7 fruit.txt
t sort;      sort fruit.txt
t sort-r;    sort -r fruit.txt
t sort-u;    sort -u fruit.txt
t sort-nk;   sort -k2 -n fruit.txt
t sort-f;    sort -f fruit.txt
t uniq;      sort fruit.txt | uniq; sort fruit.txt | uniq -c; sort fruit.txt | uniq -d
t cut;       cut -d' ' -f1 fruit.txt; cut -c1-3 fruit.txt; cut -d: -f2,3 colon.txt; cut -d: -f2- colon.txt
t tr;        echo hello world | tr a-z A-Z; echo hello world | tr -d aeiou; echo "a   b    c" | tr -s ' '; echo abc | tr abc xyz
t seq;       seq 3; seq 2 2 8; seq 5 -2 1
t basename;  basename /a/b/c.txt; basename /a/b/c.txt .txt; dirname /a/b/c; dirname /a; dirname foo
t expr;      expr 2 + 3; expr 6 \* 7; expr 10 / 3; expr 10 % 3; expr length hello; expr 3 \< 5
t grep;      grep -c apple fruit.txt; grep -n cherry fruit.txt; grep -i banana fruit.txt; grep -v apple fruit.txt
t grep-re;   grep -E 'a.p|ch' fruit.txt; grep -o 'a[a-z]*' fruit.txt; grep -w the text.txt; grep -x 'apple 10' fruit.txt
t grep-ctx;  grep -A1 quick text.txt; grep -B1 jumps text.txt
t grep-r;    grep -r needle .; grep -rl needle .; grep -L needle fruit.txt
t grep-F;    grep -F 'a.p' fruit.txt; echo "rc=$?"; grep -q apple fruit.txt; echo "rc=$?"; grep -q zzz fruit.txt; echo "rc=$?"
t find;      find . -name '*.txt' | sort; find . -type d | sort; find . -maxdepth 1 -name 'f*'
t xargs;     echo a b c d e | xargs -n 2 echo; echo x y | xargs -I {} echo "[{}]"
t cmp;       cmp fruit.txt fruit.txt; echo "rc=$?"; cmp fruit.txt text.txt >$T/nullout 2>&1; echo "rc=$?"
t hexdump;   echo ABC | hexdump -C
t tee;       echo hi | tee $T/tee1.out; cat $T/tee1.out
t yes;       yes | head -n 3; yes ok | head -n 2
t status;    true; echo $?; false; echo $?
t env;       FOO=bar env | grep '^FOO='; printenv HOME | grep -c .
t which;     which ls | grep -c 'ls$'
t sed;       sed 's/a/A/' fruit.txt; sed 's/a/A/g' fruit.txt; sed -n '2,3p' fruit.txt; sed '2d' fruit.txt; sed -n '/cherry/p' fruit.txt
t sed2;      sed 's/\(a\)\(p\)/\2\1/' fruit.txt; sed -E 's/(b)(a)/\2\1/' fruit.txt; sed -n '$p' fruit.txt; sed '1!d' fruit.txt; sed 's/an/&&/' fruit.txt
t sed3;      sed 'y/abc/xyz/' fruit.txt; sed -n '=' text.txt; sed 2q fruit.txt; sed 's/[0-9]/#/g' colon.txt; sed -e 's/x/X/' -e 's/y/Y/' colon.txt
t sed4;      sed '2i\inserted' colon.txt; sed '2a appended' colon.txt; sed '/^1/c\changed' colon.txt; sed 's/a/Q/2' fruit.txt; sed '/apple/,/Banana/d' fruit.txt
t diff;      cp fruit.txt $T/f2.txt; echo extra >> $T/f2.txt; diff fruit.txt $T/f2.txt; echo "rc=$?"; diff fruit.txt fruit.txt; echo "rc=$?"
t diff2;     sed 's/apple/APPLE/' fruit.txt > $T/f3.txt; diff fruit.txt $T/f3.txt; diff -u fruit.txt $T/f3.txt | tail -n +3; diff -q fruit.txt $T/f3.txt | sed "s#$T/##"
t diff3;     diff colon.txt text.txt | head -n 8
t chmod;     rm -f $T/m1 $T/hard; cp fruit.txt $T/m1; chmod 755 $T/m1; [ -x $T/m1 ] && echo x-set; chmod u-x,g-x,o-x $T/m1; [ -x $T/m1 ] || echo x-clear; chmod a+x $T/m1; [ -x $T/m1 ] && echo x-again; chmod 444 $T/m1; [ -w $T/m1 ] || echo not-writable
t ln;        rm -f $T/lnk $T/hard; ln -s $D/fruit.txt $T/lnk; readlink $T/lnk | grep -c fruit.txt; head -n 1 $T/lnk; [ -L $T/lnk ] && echo islink; ln $T/m1 $T/hard; cmp $T/m1 $T/hard; echo "rc=$?"
t ls;        ls; ls -a . | head -n 4; ls -1 sub; ls -F; ls -d sub; ls -r fruit.txt text.txt; ls -S fruit.txt text.txt colon.txt
t ls-R;      ls -R sub | head -n 6; ls sub fruit.txt
t cat;       cat -n fruit.txt; cat -b colon.txt; cat -E colon.txt
t cpmv;      rm -rf $T/w; mkdir -p $T/w/a/b; cp -r sub $T/w/s; cat $T/w/s/deep.txt; mv $T/w/s $T/w/a/b/s2; ls $T/w/a/b/s2; mv $T/w/a/b/s2/deep.txt $T/w/d.txt; cat $T/w/d.txt | head -n 1; rm -rf $T/w; ls $T | grep -c '^w$'
t touch;     rm -f $T/tt1; touch $T/tt1; ls $T | grep -c '^tt1$'; touch -c $T/nothere-zz; ls $T | grep -c nothere-zz
t cpself;    rm -f $T/s1 $T/s2 $T/s3; cp fruit.txt $T/s1; cp $T/s1 $T/s1; echo "rc=$?"; ln $T/s1 $T/s2; cp $T/s1 $T/s2; echo "rc=$?"; cp $T/s1 $T/../cmds.t/s1; echo "rc=$?"; ln -s s1 $T/s3; cp $T/s3 $T/s1; echo "rc=$?"; cmp fruit.txt $T/s1; echo "rc=$?"
t gzip;      rm -f $T/gz1 $T/gz1.gz; cp text.txt $T/gz1; gzip $T/gz1; [ -f $T/gz1 ] || echo orig-gone; gunzip $T/gz1.gz; cmp $T/gz1 text.txt; echo "rc=$?"
t gzip2;     gzip -c fruit.txt | gzip -dc | cmp - fruit.txt; echo "rc=$?"; gzip -c fruit.txt | zcat | wc -l; gzip -9 -c text.txt | gunzip | cmp - text.txt; echo "rc=$?"
t gzip-hdr;  printf '\037\213\010\004\0\0\0\0\0\003\377\377\0\0\0\0\0\0' | gzip -dc 2>/dev/null | wc -c; printf '\037\213\010\004\0\0\0\0\0\003\377\377\0\0\0\0\0\0' | gzip -dc >/dev/null 2>&1; echo "rc=$?"
t tar;       rm -rf $T/tx $T/ty $T/t1.tar; mkdir $T/tx; cp -r sub fruit.txt text.txt $T/tx/; ln -s fruit.txt $T/tx/lnk; (cd $T && tar cf $T/t1.tar tx); tar tf $T/t1.tar | sort; mkdir $T/ty; tar xf $T/t1.tar -C $T/ty; cmp $T/tx/fruit.txt $T/ty/tx/fruit.txt; echo "rc=$?"; cat $T/ty/tx/sub/deep.txt; readlink $T/ty/tx/lnk
t tar2;      tar cf - fruit.txt colon.txt | gzip -c | gunzip | tar tf -; tar cf - fruit.txt | tar xf - -C $T/ty; cmp fruit.txt $T/ty/fruit.txt; echo "rc=$?"
t printf;     printf '%d|%5d|%-5d|%05d|%+d|% d\n' 42 42 42 42 42 42; printf '%x %X %o %#x %#o\n' 255 255 8 255 8; printf '%s|%10s|%-10s|%.3s|%10.3s|\n' hello hello hello hello hello
t printf2;    printf '%c%c\n' abc xyz; printf '[%*d] [%-*d] [%.*s]\n' 6 7 6 7 2 abcdef; printf 'a\tb\\n\x41\101|\n'; printf '%d %d\n' 1 2 3 4 5; printf '%b\n' 'x\ty'; printf '%d %d %d %d\n' -5 0x1f 010 "'A"; printf '%5s|%-5s|\n' abcdefgh ab; printf '%%|%s|\n'
t printf3;    printf '%s\n' 012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789; printf '%s|\n' 012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789 | wc -c; printf 'no newline'; echo
t case;       for w in foo bar baz.txt "a b" ""; do case "$w" in foo) echo foo-exact;; b*r|b*z) echo b-star;; *.txt) echo txt;; "a b") echo quoted;; "") echo empty;; *) echo other;; esac; done; case x in (x) echo paren;; esac; case yy in y|yy) echo alt ;; esac
t case2;      n=5; case $n in [0-9]) echo digit;; *) echo no;; esac; case abc in a?c) echo q;; esac; case "*" in "*") echo star-quoted;; esac; case b in a) echo a;; b) echo b ;; esac
t trim;       v=archive.tar.gz; echo ${v%.gz} ${v%%.*} ${v#*.} ${v##*.} ${v#arch} ${v/tar/TAR} ${v//a/A} ${v:3} ${v:3:4} ${#v}; p=/usr/local/bin/tool; echo ${p##*/} ${p%/*} ${p%%/*} ${p#/}
t brace;      echo {a,b,c}x; echo pre{1,2}post; echo {1..5}; echo {a..e}; echo {01..03}; echo {1..10..3}; echo x{a,b}{1,2}; echo {5..1}; echo {,a}b; echo '{a,b}'; echo "{a,b}"; echo ${HOME:+{1,2}} | wc -w
t trap;       (trap 'echo trapped' EXIT; echo body); echo after
# signals (DESIGN D16): trap's commands run after the command that let the signal in; '' ignores, - resets
t trap-sig;   trap 'echo got USR1' USR1; kill -USR1 $$; echo after-usr1; kill -s USR1 $$; trap -p USR1; trap - USR1; trap -p USR1; echo reset
t trap-ign;   trap '' USR2; kill -USR2 $$; echo usr2-ignored; trap -p USR2; trap - USR2
t trap-term;  trap 'echo got TERM' TERM; kill $$; echo after-term; trap TERM; trap -p TERM; echo end
t trap-loop;  n=0; trap 'n=$((n + 1))' USR1; for i in 1 2 3; do kill -USR1 $$; done; echo n=$n; trap - USR1
t kill-names; kill -l 15; kill -l 143; kill -l 9; kill -0 $$; echo probe=$?; kill -9999 $$ 2>/dev/null; echo bad=$?
t tac;        tac fruit.txt; printf 'a\nb\nc\n' | tac; printf 'one\ntwo' | tac | od -c 2>/dev/null | head -n 0
t rev;        rev fruit.txt | head -n 3; echo abc | rev
t nl;         nl fruit.txt | head -n 4; nl -ba -w3 -s: fruit.txt | head -n 3; nl -n rz -w4 text.txt | head -n 2; nl -n ln -w3 -i5 -v10 fruit.txt | head -n 3
t fold;       echo 'the quick brown fox jumps over the lazy dog' | fold -w 10; echo 'the quick brown fox jumps over the lazy dog' | fold -s -w 12
t paste;      paste fruit.txt text.txt | head -n 3; paste -d, fruit.txt text.txt | head -n 2; paste -s -d: colon.txt; paste -s fruit.txt | wc -l
t comm;       printf 'a\nb\nc\n' > $T/c1; printf 'b\nc\nd\n' > $T/c2; comm $T/c1 $T/c2; comm -12 $T/c1 $T/c2; comm -3 $T/c1 $T/c2; comm -23 $T/c1 $T/c2
t expand;     printf 'a\tb\tc\n' | expand; printf 'abc\tx\n' | expand -t 4
t base64;     echo hello | base64; echo aGVsbG8K | base64 -d; printf 'a' | base64; printf 'ab' | base64; printf 'abc' | base64; head -c 100 fruit.txt | base64 | base64 -d | cmp - fruit.txt 2>/dev/null; echo rc=$?
t md5sum;     md5sum fruit.txt text.txt; printf '' | md5sum; echo -n abc | md5sum
t realpath;   basename $(realpath ./sub/..); basename $(realpath -e fruit.txt); realpath /usr/../tmp; realpath -e /nonexistent-zz 2>/dev/null; echo rc=$?
t mktemp;     f=$(mktemp); [ -f $f ] && echo made; rm $f; d=$(mktemp -d); [ -d $d ] && echo dir; rmdir $d; f=$(mktemp /tmp/zzXXXXXX); echo $f | grep -c '^/tmp/zz'; rm $f
t timeout;    timeout 1 sleep 5; echo rc=$?; timeout 5 true; echo rc=$?; timeout 5 sh -c 'exit 3'; echo rc=$?
t subshell;   x=local; echo $x | cat; f() { echo in-f-$1; }; f a | cat; v=$(f b); echo $v; (f c); set -- p1 "p 2"; echo "$1|$2" | cat; ( echo "$#" ); y='it'"'"'s'; echo "$y" | cat; echo $(echo $x)
t unlinkopen; printf 'old-data\n' > $T/uo1; printf 'rm $T/uo1; echo NEW > $T/uo2; cat; cat $T/uo2\n' > $T/uo.sh; sh $T/uo.sh < $T/uo1; rm -f $T/uo2 $T/uo.sh
t substr2;    v=abcdef; echo ${v:1:-1} ${v:2:-3} ${v:0:-6}x
t awk-fields; awk '{ print $2, $1 }' fruit.txt; awk -F: '{ print $3 "-" $1, NF }' colon.txt; awk '{ $2 = $2 * 2; print; print NF }' fruit.txt | head -n 4
t awk-patterns; awk '/an/' fruit.txt; awk '$2 > 5 { n++ } END { print n " big" }' fruit.txt; awk 'NR == 2, NR == 3 { print NR ": " $0 }' fruit.txt; awk '!/apple/ && $2 < 5' fruit.txt
t awk-begin;  awk 'BEGIN { x = 3; y = 4; print x * y, x / y, 2 ^ 10, 7 % 3, -x, x "" y; print 0.1 + 0.2, 1e6, 1/3, int(-3.7), 100000 * 100000 }'
t awk-vars;   awk -v n=5 'BEGIN { for (i = 1; i <= n; i++) s = s i; print s }'; echo 'a b c' | awk '{ print NF, $NF, $(NF-1) }'; awk 'END { print NR, $0 }' fruit.txt
t awk-arrays; awk '{ sum[$1] += $2 } END { for (k in sum) print k, sum[k] }' fruit.txt | sort; awk 'BEGIN { a["x"] = 1; delete a["x"]; print ("x" in a), length("hello") }'
t awk-split;  awk 'BEGIN { n = split("a,b,,c", p, ","); print n, p[1], p[4]; m = split("  one two  ", q); print m, q[1] q[2] }'
t awk-strings; awk 'BEGIN { s = "Hello, World"; print substr(s, 1, 5), substr(s, 8), index(s, "World"), toupper(s), tolower(s), length(s) }'
t awk-subs;   echo 'aaa bbb aaa' | awk '{ n = gsub(/a/, "x"); print n, $0 }'; echo 'foo bar' | awk '{ sub(/o+/, "[&]"); print }'; echo 'xyz' | awk '{ if (match($0, /y+/)) print RSTART, RLENGTH }'
t awk-printf; awk 'BEGIN { printf "%5d|%-5s|%05.2f|%x|%o|%c|%e|%%\n", 42, "ab", 3.14159, 255, 8, 65, 12345.678 }'; awk '{ printf "%-8s%3d\n", $1, $2 }' fruit.txt
t awk-func;   awk 'function fact(n) { return n <= 1 ? 1 : n * fact(n - 1) } BEGIN { print fact(10); print fact(15); print 2 ^ 53, -2 ^ 52 + 1 }'; awk 'function f(a, i) { a["k"] = 9; i = 5 } BEGIN { f(arr); print arr["k"], i "" }'
t awk-wide;   awk 'BEGIN { printf "%300d|\n", 7 }' | wc -c; awk 'BEGIN { printf "%300d|\n", 7 }' | tr -d ' '; awk 'BEGIN { x = sprintf("%200c", "a"); print length(x); gsub(/ /, "", x); print x }'; awk 'BEGIN { x = sprintf("%-500.3f|", 3.14159); print length(x); gsub(/ /, "", x); print x }'; awk 'BEGIN { x = sprintf("%*x|%-*o|%0300i", 300, 255, 150, 8, -5); print length(x); gsub(/ /, "", x); sub(/^ff\|10\|-0+/, "", x); print x }'; awk 'BEGIN { printf "%4096s|\n", "e" }' | wc -c; awk 'BEGIN { printf "%4096u|\n", 3 }' | tr -d ' '
t awk-control; awk 'BEGIN { for (i = 0; i < 10; i++) { if (i == 2) continue; if (i == 5) break; printf "%d ", i }; print ""; while (j < 3) j++; do k++; while (k < 2); print j, k }'
t awk-ofs;    awk 'BEGIN { OFS = "-"; ORS = "|\n" } { $1 = $1; print }' colon.txt; awk -F: -v OFS=: '{ print $1, $3 }' colon.txt
t awk-io;     awk '{ print $1 > "'$T'/awk1.out" }' fruit.txt; sort $T/awk1.out | uniq -c; awk 'BEGIN { while ((getline line < "colon.txt") > 0) n++; print n }'; awk '{ print $2 | "sort -n" }' fruit.txt
t awk-getline; awk 'BEGIN { "echo hi there" | getline x; print x; close("echo hi there") }'; awk 'NR == 1 { getline; print "after getline:", $0, NR }' fruit.txt
t awk-cmp;    awk 'BEGIN { print ("10" < "9"), (10 < 9), ("abc" < "abd"); x = "3.0"; print (x == 3) }'; echo '10 9' | awk '{ print ($1 < $2) }'
t awk-rc;     awk 'BEGIN { exit 3 }'; echo rc=$?; awk 'BEGIN { print "x" } END { print "end" }' < /dev/null; awk '{ exit } END { print "end ran", NR }' fruit.txt
t awk-file;   printf '{ print NR ": " $1 }\n' > $T/prog.awk; awk -f $T/prog.awk colon.txt; awk '{ print FILENAME, FNR }' colon.txt fruit.txt | tail -n 2
# a long script, made here: read and run a command at a time (the shell once parsed a whole script at once and ran out of
# memory near 90 lines); a function, a here-document, if/case over several lines, a continued line, a pipe at a line's
# end, a sourced file, and a 2 KB line among 600 short ones
L=$T/long.sh; printf 'add() {\n    x=$((x + $1))\n}\nx=0\n' > $L; seq 0 599 | sed 's/^/add /' >> $L
printf 'cat <<EOF\nhere x=$x\nEOF\nif [ $x -gt 0 ]\nthen\n    echo big\nelse\n    echo small\nfi\n' >> $L
printf 'case $x in\n179700) echo case-ok ;;\n*) echo case-bad ;;\nesac\necho one \\\n  two\necho piped |\n  tr a-z A-Z\n' >> $L
printf "echo 'two\nlines'\ntrue &&\n  echo and-ok\ni=0\nwhile [ \$i -lt 3 ]\ndo\n  i=\$((i + 1))\ndone\necho i=\$i\n" >> $L
printf 'cat <<-E\n\tindented\n\tE\ncat <<"Q"\nfi done esac } ) \x27 " $x\nQ\n' >> $L
printf 'y=from-long2\n' > $T/long2.sh; echo ". $T/long2.sh; echo \$y" >> $L; echo set -- $(seq 1000 1399) >> $L
printf 'echo "n=$# last=${400}"\nf() { echo "f sees $x"; }\nf\necho "sum=$x"\n' >> $L
t sh-long;    sh $L; echo "rc=$? lines=$(wc -l < $L) longest=$(awk '{ if (length($0) > m) m = length($0) } END { print m }' $L)"
# redirections after a compound command (audit 2026-10-28 O10): a subshell, a group, a loop, if and case, in the
# foreground, in the background and in a pipeline
t sh-compound-redir; ( echo sub1; echo sub2 ) > $T/c1.txt & wait; cat $T/c1.txt; ( echo out; echo err >&2 ) 2>&1 | tr a-z A-Z & wait; { echo g1; echo g2; } > $T/c2.txt & wait; cat $T/c2.txt
t sh-compound-redir2; ( echo fg ) > $T/c3.txt; { echo app; } >> $T/c3.txt; cat $T/c3.txt; while read a b; do echo "<$a>"; done < fruit.txt; for i in 1 2; do echo i$i; done > $T/c4.txt; cat $T/c4.txt; if true; then echo iff; fi > $T/c5.txt 2>&1; cat $T/c5.txt; case x in x) echo cs >&2 ;; esac 2>&1 | tr a-z A-Z
t sh-compound-redir3; { echo e1 >&2; echo o1; } 2>/dev/null | wc -l; ( cd /; pwd ) > $T/c6.txt; cat $T/c6.txt; pwd | sed "s|$D|D|"; f() { echo inf; } > $T/c7.txt; f; cat $T/c7.txt; ( echo hd; cat ) <<E
here
E
# a here-document after a compound command in a pipeline stage and in the background (audit 2026-10-29 B2-6): the
# child shell gets the body too
t sh-compound-redir4; while read l; do echo "<$l>"; done <<E | tr a-z A-Z
one
two
E
{ cat; echo g; } <<'E' > $T/c8.txt & wait; cat $T/c8.txt
bg $x
E
# commands from a pipe or a file (audit 2026-10-26 F4): not interactive (no prompts, no line editor), read a command
# at a time, so a command run gets the rest of the input (from a pipe the shell reads one byte at a time)
t sh-stdin;   printf 'read x\nhello\necho "got $x"\n' | sh; printf 'echo a\ncat\nrest 1\nrest 2\n' | sh; printf 'echo one\necho two\nexit 3\necho never\n' | sh; echo "rc=$?"
t sh-stdin2;  printf 'if true\nthen echo multi\nfi\nfor i in 1 2; do echo i$i; done\ncat <<E\nhere doc\nE\nread a b\nx y z\necho "$b|$a"\n' | sh 2>&1; printf 'echo syntax (\necho never\n' | sh 2>/dev/null; echo "rc=$?"
t sh-stdin-file; printf 'read x\nhello\necho "got $x"\ncat\nlast line\n' > $T/in.sh; sh < $T/in.sh; printf 'echo first\nread y\nsecond\necho "y=$y"\n' > $T/in2.sh; sh < $T/in2.sh
H=$T/hd.sh; echo 'cat <<EOF | wc -c' > $H; i=0; while [ $i -lt 40 ]; do echo '$y$y$y$y$y$y$y$y$y$y$y$y$y$y$y$y$y$y$y$y$y$y$y$y$y' >> $H; i=$((i + 1)); done; echo EOF >> $H
t hd-long;    y=abcdefgh; . $H
# colours (DESIGN D14): GNU's exact bytes with --color=always (cmds-test runs the host's ls and grep with LS_COLORS and
# GREP_COLORS unset); auto into a file or a pipe gives none
C=$T/col; mkdir -p $C/dir $C/sticky; touch $C/file $C/exe $C/suid; chmod +x $C/exe; chmod 4755 $C/suid; chmod 1777 $C/sticky; ln -s file $C/link
t color-ls;   (cd $C && ls --color=always; ls --color=always -F; ls --color=always -d dir file exe; ls --color=always file; ls --color=auto; ls --color=never -F; ls --color | head -2)
t color-grep; grep --color=always apple fruit.txt; grep --color=always -n -i A fruit.txt text.txt; grep --color=always -c apple fruit.txt text.txt; grep --color=always -o 'a[a-z]*' fruit.txt; grep --color=always -v apple fruit.txt
t color-grep2; grep --color=always -l apple fruit.txt text.txt; grep --color=always -w -F the text.txt; grep --color=always 'x*' colon.txt | head -2; grep --color=auto apple fruit.txt; grep --color apple fruit.txt | wc -c
t grep-groups; grep -E -n -A1 'apple|cherry' fruit.txt; grep --colour=always -n -B1 cherry fruit.txt; grep --color=always -A0 an fruit.txt text.txt; grep -C1 -n o text.txt
t color-less; grep --color=always -n apple fruit.txt | less; ls --color=always $C | less
