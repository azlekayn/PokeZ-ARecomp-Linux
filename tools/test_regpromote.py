#!/usr/bin/env python3
"""tools/regpromote.py check: generated-style functions run as generated and with x19..x28 promoted, compiled with
runtime/cpu.h, must leave the same registers and guest memory - a loop over guest memory, a call to a callee that
saves / restores x19 the way a compiled prologue / epilogue does (and clobbers it in between), a tail call into a
function reading a promoted register, and a return. Usage: python tools/test_regpromote.py [cc] (default clang)."""
import os, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import regpromote

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
cc = sys.argv[1] if len(sys.argv) > 1 else 'clang'

def fn(name, body):
    return [f'void {name}(Cpu *c) {{', '    FN_ENTER(0x1000ull);'] + ['    ' + l for l in body] + ['}']

# a callee as compiled code does it: saves x19 on the guest stack, uses x19 for itself, restores it
callee = fn('callee', ['c->sp -= 16; ST64(c->sp, c->x[19]);', 'c->x[19] = 0xdead; c->x[0] = c->x[19] + c->x[0];',
                       'c->x[19] = LD64(c->sp); c->sp += 16;', 'return;'])
tail = fn('tail', ['c->x[1] = c->x[19] * 3 + c->x[26];', 'return;'])
work = ['c->x[19] = c->x[0]; c->x[26] = 0;',
        'L_loop:;',
        '{ uint64_t ea = c->x[19]; uint64_t v = LD64(ea); c->x[26] = c->x[26] + v; ST64(ea, v + 1); }',
        'c->x[19] = c->x[19] + 8;',
        'if (c->x[19] != c->x[1]) { goto L_loop; }',
        'c->x[0] = 5; c->x[30] = 0x1234; callee(c);',
        'c->x[20] = c->x[0] + c->x[19];',
        'if (c->x[2] & 1) { __attribute__((musttail)) return tail(c); }',
        'c->x[21] = c->x[26] ^ c->x[20];',
        'return;']
plain = fn('work_plain', work)
promoted = regpromote.promote(fn('work_prom', work))
assert any('xr_19' in l for l in promoted) and not any('c->x[26] + v' in l for l in promoted), promoted
assert regpromote.promote(tail) != tail and regpromote.promote(fn('none', ['c->x[0] = 1;', 'return;'])) == fn('none', ['c->x[0] = 1;', 'return;'])
src = ['#include <stdio.h>', '#include <string.h>', '#include <stdint.h>', '#include "cpu.h"', 'volatile int g_fnprof;',
       'void fnprof_hit(uint64_t a, uint64_t b, uint64_t c) { (void)a; (void)b; (void)c; }']
src += callee + tail + plain + promoted
src += ['''
static uint64_t mem[64], stack[16], mem_a[64];
static void setup(Cpu *c, int odd) {  /* (both runs on the same buffers: pointers compare as they are) */
    memset(c, 0, sizeof *c);
    for (int i = 0; i < 31; i++) c->x[i] = 0x1000 + i * 7;
    for (int i = 0; i < 64; i++) mem[i] = i * 3 + 1;
    c->x[0] = (uint64_t)(uintptr_t)&mem[4];
    c->x[1] = (uint64_t)(uintptr_t)&mem[20];
    c->x[2] = odd;
    c->sp = (uint64_t)(uintptr_t)&stack[15];
}
int main(void) {
    int bad = 0;
    for (int odd = 0; odd < 2; odd++) {
        Cpu a, b;
        setup(&a, odd); work_plain(&a); memcpy(mem_a, mem, sizeof mem);
        setup(&b, odd); work_prom(&b);
        for (int i = 0; i < 31; i++)
            if (a.x[i] != b.x[i]) { printf("odd %d: x%d %llx != %llx\\n", odd, i, (unsigned long long)a.x[i], (unsigned long long)b.x[i]); bad++; }
        if (a.sp != b.sp) { printf("odd %d: sp differs\\n", odd); bad++; }
        if (memcmp(mem_a, mem, sizeof mem)) { printf("odd %d: guest memory differs\\n", odd); bad++; }
    }
    printf("%d mismatches\\n", bad);
    return bad != 0;
}''']
d = tempfile.mkdtemp()
c_path, exe = os.path.join(d, 't.c'), os.path.join(d, 't.exe')
open(c_path, 'w').write('\n'.join(src))
subprocess.run([cc, '-O1', '-DGUEST_VOLATILE', '-w', '-I', os.path.join(root, 'runtime'), c_path, '-o', exe], check=True)
sys.exit(subprocess.run([exe]).returncode)
