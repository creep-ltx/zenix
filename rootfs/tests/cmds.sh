# SPDX-License-Identifier: GPL-2.0-only
# command tests: run under Zenix and under bash with GNU tools; the output must be identical
D=${1:-/home/t}
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
t cmp;       cmp fruit.txt fruit.txt; echo "rc=$?"; cmp fruit.txt text.txt >/tmp/nullout 2>&1; echo "rc=$?"
t hexdump;   echo ABC | hexdump -C
t tee;       echo hi | tee /tmp/tee1.out; cat /tmp/tee1.out
t yes;       yes | head -n 3; yes ok | head -n 2
t status;    true; echo $?; false; echo $?
t env;       FOO=bar env | grep '^FOO='; printenv HOME | grep -c .
t which;     which ls | grep -c 'ls$'
t sed;       sed 's/a/A/' fruit.txt; sed 's/a/A/g' fruit.txt; sed -n '2,3p' fruit.txt; sed '2d' fruit.txt; sed -n '/cherry/p' fruit.txt
t sed2;      sed 's/\(a\)\(p\)/\2\1/' fruit.txt; sed -E 's/(b)(a)/\2\1/' fruit.txt; sed -n '$p' fruit.txt; sed '1!d' fruit.txt; sed 's/an/&&/' fruit.txt
t sed3;      sed 'y/abc/xyz/' fruit.txt; sed -n '=' text.txt; sed 2q fruit.txt; sed 's/[0-9]/#/g' colon.txt; sed -e 's/x/X/' -e 's/y/Y/' colon.txt
t sed4;      sed '2i\inserted' colon.txt; sed '2a appended' colon.txt; sed '/^1/c\changed' colon.txt; sed 's/a/Q/2' fruit.txt; sed '/apple/,/Banana/d' fruit.txt
t diff;      cp fruit.txt /tmp/f2.txt; echo extra >> /tmp/f2.txt; diff fruit.txt /tmp/f2.txt; echo "rc=$?"; diff fruit.txt fruit.txt; echo "rc=$?"
t diff2;     sed 's/apple/APPLE/' fruit.txt > /tmp/f3.txt; diff fruit.txt /tmp/f3.txt; diff -u fruit.txt /tmp/f3.txt | tail -n +3; diff -q fruit.txt /tmp/f3.txt | sed 's#/tmp/##'
t diff3;     diff colon.txt text.txt | head -n 8
t chmod;     rm -f /tmp/m1 /tmp/hard; cp fruit.txt /tmp/m1; chmod 755 /tmp/m1; [ -x /tmp/m1 ] && echo x-set; chmod u-x,g-x,o-x /tmp/m1; [ -x /tmp/m1 ] || echo x-clear; chmod a+x /tmp/m1; [ -x /tmp/m1 ] && echo x-again; chmod 444 /tmp/m1; [ -w /tmp/m1 ] || echo not-writable
t ln;        rm -f /tmp/lnk /tmp/hard; ln -s $D/fruit.txt /tmp/lnk; readlink /tmp/lnk | grep -c fruit.txt; head -n 1 /tmp/lnk; [ -L /tmp/lnk ] && echo islink; ln /tmp/m1 /tmp/hard; cmp /tmp/m1 /tmp/hard; echo "rc=$?"
t ls;        ls; ls -a . | head -n 4; ls -1 sub; ls -F; ls -d sub; ls -r fruit.txt text.txt; ls -S fruit.txt text.txt colon.txt
t ls-R;      ls -R sub | head -n 6; ls sub fruit.txt
t cat;       cat -n fruit.txt; cat -b colon.txt; cat -E colon.txt
t cpmv;      rm -rf /tmp/w; mkdir -p /tmp/w/a/b; cp -r sub /tmp/w/s; cat /tmp/w/s/deep.txt; mv /tmp/w/s /tmp/w/a/b/s2; ls /tmp/w/a/b/s2; mv /tmp/w/a/b/s2/deep.txt /tmp/w/d.txt; cat /tmp/w/d.txt | head -n 1; rm -rf /tmp/w; ls /tmp | grep -c '^w$'
t touch;     rm -f /tmp/tt1; touch /tmp/tt1; ls /tmp | grep -c '^tt1$'; touch -c /tmp/nothere-zz; ls /tmp | grep -c nothere-zz
t gzip;      rm -f /tmp/gz1 /tmp/gz1.gz; cp text.txt /tmp/gz1; gzip /tmp/gz1; [ -f /tmp/gz1 ] || echo orig-gone; gunzip /tmp/gz1.gz; cmp /tmp/gz1 text.txt; echo "rc=$?"
t gzip2;     gzip -c fruit.txt | gzip -dc | cmp - fruit.txt; echo "rc=$?"; gzip -c fruit.txt | zcat | wc -l; gzip -9 -c text.txt | gunzip | cmp - text.txt; echo "rc=$?"
t tar;       rm -rf /tmp/tx /tmp/ty /tmp/t1.tar; mkdir /tmp/tx; cp -r sub fruit.txt text.txt /tmp/tx/; ln -s fruit.txt /tmp/tx/lnk; (cd /tmp && tar cf /tmp/t1.tar tx); tar tf /tmp/t1.tar | sort; mkdir /tmp/ty; tar xf /tmp/t1.tar -C /tmp/ty; cmp /tmp/tx/fruit.txt /tmp/ty/tx/fruit.txt; echo "rc=$?"; cat /tmp/ty/tx/sub/deep.txt; readlink /tmp/ty/tx/lnk
t tar2;      tar cf - fruit.txt colon.txt | gzip -c | gunzip | tar tf -; tar cf - fruit.txt | tar xf - -C /tmp/ty; cmp fruit.txt /tmp/ty/fruit.txt; echo "rc=$?"
t printf;     printf '%d|%5d|%-5d|%05d|%+d|% d\n' 42 42 42 42 42 42; printf '%x %X %o %#x %#o\n' 255 255 8 255 8; printf '%s|%10s|%-10s|%.3s|%10.3s|\n' hello hello hello hello hello
t printf2;    printf '%c%c\n' abc xyz; printf '[%*d] [%-*d] [%.*s]\n' 6 7 6 7 2 abcdef; printf 'a\tb\\n\x41\101|\n'; printf '%d %d\n' 1 2 3 4 5; printf '%b\n' 'x\ty'; printf '%d %d %d %d\n' -5 0x1f 010 "'A"; printf '%5s|%-5s|\n' abcdefgh ab; printf '%%|%s|\n'
t printf3;    printf '%s\n' 012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789; printf '%s|\n' 012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789 | wc -c; printf 'no newline'; echo
t case;       for w in foo bar baz.txt "a b" ""; do case "$w" in foo) echo foo-exact;; b*r|b*z) echo b-star;; *.txt) echo txt;; "a b") echo quoted;; "") echo empty;; *) echo other;; esac; done; case x in (x) echo paren;; esac; case yy in y|yy) echo alt ;; esac
t case2;      n=5; case $n in [0-9]) echo digit;; *) echo no;; esac; case abc in a?c) echo q;; esac; case "*" in "*") echo star-quoted;; esac; case b in a) echo a;; b) echo b ;; esac
t trim;       v=archive.tar.gz; echo ${v%.gz} ${v%%.*} ${v#*.} ${v##*.} ${v#arch} ${v/tar/TAR} ${v//a/A} ${v:3} ${v:3:4} ${#v}; p=/usr/local/bin/tool; echo ${p##*/} ${p%/*} ${p%%/*} ${p#/}
t brace;      echo {a,b,c}x; echo pre{1,2}post; echo {1..5}; echo {a..e}; echo {01..03}; echo {1..10..3}; echo x{a,b}{1,2}; echo {5..1}; echo {,a}b; echo '{a,b}'; echo "{a,b}"; echo ${HOME:+{1,2}} | wc -w
t trap;       (trap 'echo trapped' EXIT; echo body); echo after
t tac;        tac fruit.txt; printf 'a\nb\nc\n' | tac; printf 'one\ntwo' | tac | od -c 2>/dev/null | head -n 0
t rev;        rev fruit.txt | head -n 3; echo abc | rev
t nl;         nl fruit.txt | head -n 4; nl -ba -w3 -s: fruit.txt | head -n 3; nl -n rz -w4 text.txt | head -n 2; nl -n ln -w3 -i5 -v10 fruit.txt | head -n 3
t fold;       echo 'the quick brown fox jumps over the lazy dog' | fold -w 10; echo 'the quick brown fox jumps over the lazy dog' | fold -s -w 12
t paste;      paste fruit.txt text.txt | head -n 3; paste -d, fruit.txt text.txt | head -n 2; paste -s -d: colon.txt; paste -s fruit.txt | wc -l
t comm;       printf 'a\nb\nc\n' > /tmp/c1; printf 'b\nc\nd\n' > /tmp/c2; comm /tmp/c1 /tmp/c2; comm -12 /tmp/c1 /tmp/c2; comm -3 /tmp/c1 /tmp/c2; comm -23 /tmp/c1 /tmp/c2
t expand;     printf 'a\tb\tc\n' | expand; printf 'abc\tx\n' | expand -t 4
t base64;     echo hello | base64; echo aGVsbG8K | base64 -d; printf 'a' | base64; printf 'ab' | base64; printf 'abc' | base64; head -c 100 fruit.txt | base64 | base64 -d | cmp - fruit.txt 2>/dev/null; echo rc=$?
t md5sum;     md5sum fruit.txt text.txt; printf '' | md5sum; echo -n abc | md5sum
t realpath;   basename $(realpath ./sub/..); basename $(realpath -e fruit.txt); realpath /usr/../tmp; realpath -e /nonexistent-zz 2>/dev/null; echo rc=$?
t mktemp;     f=$(mktemp); [ -f $f ] && echo made; rm $f; d=$(mktemp -d); [ -d $d ] && echo dir; rmdir $d; f=$(mktemp /tmp/zzXXXXXX); echo $f | grep -c '^/tmp/zz'; rm $f
t timeout;    timeout 1 sleep 5; echo rc=$?; timeout 5 true; echo rc=$?; timeout 5 sh -c 'exit 3'; echo rc=$?
t subshell;   x=local; echo $x | cat; f() { echo in-f-$1; }; f a | cat; v=$(f b); echo $v; (f c); set -- p1 "p 2"; echo "$1|$2" | cat; ( echo "$#" ); y='it'"'"'s'; echo "$y" | cat; echo $(echo $x)
t unlinkopen; printf 'old-data\n' > /tmp/uo1; printf 'rm /tmp/uo1; echo NEW > /tmp/uo2; cat; cat /tmp/uo2\n' > /tmp/uo.sh; sh /tmp/uo.sh < /tmp/uo1; rm -f /tmp/uo2 /tmp/uo.sh
t substr2;    v=abcdef; echo ${v:1:-1} ${v:2:-3} ${v:0:-6}x
