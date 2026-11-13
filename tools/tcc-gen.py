#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Generated C programs for tcc's tests (DESIGN D52 section 7, tools/tcc-host-test.sh, tools/tcc-test.sh).

  tcc-gen.py abi DIR       DIR/abi.h, def.c, call.c: 150 functions of random signatures (every scalar type, 29 struct
                           shapes chosen for gcc's register-or-memory rules, variadic calls); def.c defines them and
                           checks each argument, call.c calls them and checks each result. Built once by tcc and once
                           by gcc, the four pairings must print "abi: 0 bad": tcc's calls and gcc's agree.
  tcc-gen.py fuzz SEED     a program of random integer expressions over globals, struct fields through a pointer and
                           a union (every integer type, casts, ternaries, shifts, division, compound assignments),
                           printing each result: built by tcc and by gcc -fwrapv, the outputs must be the same.
Deterministic: the same arguments give the same files."""
import os
import random
import sys


def abi(d):
    random.seed(7)
    scalars = ['char', 'signed char', 'unsigned char', 'short', 'unsigned short', 'int', 'unsigned', 'long',
               'long long', 'unsigned long long', 'float', 'double', 'void *']
    structs = {
        'S1': 'char a;', 'S2': 'char a,b;', 'S2a': 'short a;', 'S3': 'char a,b,c;', 'S4': 'char a[4];', 'S4i': 'int a;',
        'S4s': 'short a,b;', 'S5': 'char a[5];', 'S6': 'short a[3];', 'S8': 'int a,b;', 'S8c': 'char a[8];',
        'S8d': 'double a;', 'S8f': 'float a,b;', 'S8l': 'long long a;', 'S9': 'char a[9];', 'S12': 'int a[3];',
        'S16': 'double a,b;', 'S7': 'char a; short b; char c[3];', 'Sa': 'short s; char c[2];',
        'Sb': 'int a; char c[3]; char d;', 'Sc': 'char a[2]; short b;', 'Sd': 'short s; struct { char a, b; } in;',
        'Se': 'float f; short s;', 'Sg': 'struct { char a[3]; } x; char y;', 'Sh': 'char a; char b[3];',
        'Si': 'long long a; char b[0];', 'Sf': 'float a;', 'Sx': 'char c; double d; char e;',
    }
    h = ['#include <stdio.h>', '#include <stdarg.h>', '#include <string.h>']
    for n, body in structs.items():
        h.append(f'typedef struct {{ {body} }} {n};')
        h.append(f'static {n} mk_{n}(int k) {{ {n} s; unsigned char *p=(unsigned char*)&s; int i; '
                 f'for(i=0;i<(int)sizeof s;i++) p[i]=(unsigned char)(k*31+i*7+1); return s; }}')
        h.append(f'static int eq_{n}({n} a, {n} b) {{ return !memcmp(&a,&b,sizeof a); }}')

    def val(t, k):
        if t in ('float', 'double'):
            return f'(({t}){k}.25)'
        if t == 'void *':
            return f'((void*)0x{k:x}0)'
        if 'long long' in t:
            return f'(({t}){k}*0x100000001LL)'
        return f'(({t}){k})'
    types = scalars + list(structs)
    defs, calls, protos = [], [], []
    for i in range(150):
        nargs = random.randint(0, 7)
        args = [random.choice(types) for _ in range(nargs)]
        ret = random.choice(types + ['void'])
        var = random.random() < 0.2 and nargs > 0
        fn = f'f{i}'
        params = ', '.join(f'{t} a{j}' for j, t in enumerate(args)) or 'void'
        if var:
            params += ', ...'
        protos.append(f'{ret} {fn}({params});')
        body = []
        for j, t in enumerate(args):
            k = i * 10 + j + 1
            if t in structs:
                body.append(f'  if (!eq_{t}(a{j}, mk_{t}({k}))) {{ printf("{fn} arg {j} bad\\n"); bad++; }}')
            else:
                body.append(f'  if (a{j} != {val(t, k)}) {{ printf("{fn} arg {j} bad\\n"); bad++; }}')
        if var:
            body.append('  { va_list ap; va_start(ap, a%d);' % (nargs - 1))
            body.append(f'    if (va_arg(ap,int) != {i}) {{ printf("{fn} va int bad\\n"); bad++; }}')
            body.append(f'    if (va_arg(ap,double) != {i}.5) {{ printf("{fn} va double bad\\n"); bad++; }}')
            body.append(f'    if (va_arg(ap,long long) != {i}LL*0x100000003LL) {{ printf("{fn} va ll bad\\n"); bad++; }}')
            body.append(f'    {{ S3 s = va_arg(ap,S3); if (!eq_S3(s, mk_S3({i}))) {{ printf("{fn} va S3 bad\\n"); bad++; }} }}')
            body.append(f'    {{ S12 s = va_arg(ap,S12); if (!eq_S12(s, mk_S12({i}))) {{ printf("{fn} va S12 bad\\n"); bad++; }} }}')
            body.append(f'    if (va_arg(ap,int) != -{i}) {{ printf("{fn} va end bad\\n"); bad++; }}')
            body.append('    va_end(ap); }')
        kr = i * 10 + 9
        rexpr = None if ret == 'void' else (f'mk_{ret}({kr})' if ret in structs else val(ret, kr))
        defs.append(f'{ret} {fn}({params})\n{{\n' + '\n'.join(body) + (f'\n  return {rexpr};' if rexpr else '') + '\n}')
        cargs = [(f'mk_{t}({i*10+j+1})' if t in structs else val(t, i * 10 + j + 1)) for j, t in enumerate(args)]
        if var:
            cargs += [str(i), f'{i}.5', f'{i}LL*0x100000003LL', f'mk_S3({i})', f'mk_S12({i})', f'-{i}']
        call = f'{fn}({", ".join(cargs)})'
        if ret == 'void':
            calls.append(f'  {call};')
        elif ret in structs:
            calls.append(f'  {{ {ret} r = {call}; if (!eq_{ret}(r, mk_{ret}({kr}))) {{ printf("{fn} ret bad\\n"); bad++; }} }}')
        else:
            calls.append(f'  {{ {ret} r = {call}; if (r != {val(ret, kr)}) {{ printf("{fn} ret bad\\n"); bad++; }} }}')
    with open(os.path.join(d, 'abi.h'), 'w') as f:
        f.write('\n'.join(h) + '\nextern int bad;\n' + '\n'.join(protos) + '\n')
    with open(os.path.join(d, 'def.c'), 'w') as f:
        f.write('#include "abi.h"\n' + '\n'.join(defs) + '\n')
    with open(os.path.join(d, 'call.c'), 'w') as f:
        f.write('#include "abi.h"\nint bad;\nint main(void)\n{\n' + '\n'.join(calls) +
                '\n  printf("abi: %d bad\\n", bad);\n  return bad != 0;\n}\n')
def fuzz(seed):
    random.seed(seed)
    T = ['signed char','unsigned char','short','unsigned short','int','unsigned','long long','unsigned long long','_Bool']
    vars_ = []
    lines = ['#include <stdio.h>', 'typedef unsigned long long u64;']
    fields = []
    for i in range(12):
        t = random.choice(T)
        v = random.choice([0,1,-1,2,3,7,100,-100,255,256,32767,-32768,65535,0x7fffffff,-0x7fffffff-1,0x123456789abc,-5,13,0xdeadbeef])
        vars_.append((f'v{i}', t))
        lines.append(f'volatile {t} v{i} = ({t}){v}LL;')
    for i in range(8):
        t = random.choice([x for x in T if x != '_Bool'])
        fields.append((i, t))
    lines.append('struct S { ' + ' '.join(f'{t} f{i};' for i, t in fields) + ' } s0, *ps = &s0;')
    lines.append('union U { unsigned long long ll; unsigned u[2]; unsigned short h[4]; unsigned char b[8]; } u0, *pu = &u0;')
    for i, t in fields:
        vars_.append((f'ps->f{i}', t))
    vars_.append(('pu->ll', 'unsigned long long')); vars_.append(('pu->u[1]', 'unsigned')); vars_.append(('pu->h[2]', 'unsigned short'))
    def expr(d):
        if d <= 0 or random.random() < 0.25:
            if random.random() < 0.7:
                return random.choice(vars_)[0]
            return str(random.choice([0,1,2,3,5,8,15,16,31,63,100,-1,-7,1000,65536,0x7fffffff]))
        k = random.random()
        a = expr(d-1); b = expr(d-1)
        if k < 0.5:
            op = random.choice(['+','-','*','&','|','^','<','>','<=','>=','==','!=','&&','||'])
            return f'({a} {op} {b})'
        if k < 0.6:
            op = random.choice(['/','%'])
            return f'(({b}) ? ({a}) {op} ({b}) : 0)'
        if k < 0.7:
            op = random.choice(['<<','>>'])
            return f'({a} {op} (({b}) & 15))'
        if k < 0.8:
            return f'(({random.choice(T)})({a}))'
        if k < 0.9:
            return f'({a} ? {b} : {expr(d-1)})'
        return random.choice([f'(- {a})', f'(~{a})', f'(!{a})'])
    for i in range(40):
        lines.append(f'static u64 e{i}(void) {{ return (u64)({expr(4)}); }}')
    lines.append('int main(void) {')
    for i, t in fields:
        lines.append(f'  ps->f{i} = ({t}){random.choice([1,-1,300,-70000,0x12345678,0x7fff,255])}LL;')
    lines.append('  pu->ll = 0x0123456789abcdefULL;')
    for i in range(40):
        lines.append(f'  printf("{i} %llx\\n", e{i}());')
    # assignments with compound ops
    lines.append('  { int i; for (i = 0; i < 8; i++) printf("%02x", ((unsigned char *)&u0)[i]); printf("\\n"); }')
    for i in range(20):
        v, t = random.choice(vars_)
        op = random.choice(['+=','-=','*=','&=','|=','^=','<<=','>>='])
        rhs = expr(2)
        if op in ('<<=','>>='): rhs = f'(({rhs}) & 7)'
        lines.append(f'  {v} {op} {rhs}; printf("a{i} %llx\\n", (u64){v});')
        lines.append(f'  {v}++; printf("b{i} %llx\\n", (u64){v});')
    lines.append('  return 0;\n}')
    sys.stdout.write('\n'.join(lines) + '\n')


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == 'abi':
        abi(sys.argv[2])
    elif len(sys.argv) == 3 and sys.argv[1] == 'fuzz':
        fuzz(int(sys.argv[2]))
    else:
        sys.exit('usage: tcc-gen.py abi DIR | fuzz SEED')
