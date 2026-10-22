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
