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
t env;       FOO=bar env | grep '^FOO='; printenv HOME | grep -c home
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
