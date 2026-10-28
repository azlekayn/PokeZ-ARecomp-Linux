"""Callee-saved registers as C locals (gen.py emit_function).

The guest's registers live in the Cpu struct (c->x[n]); every generated function reads and writes them there, and the
C compiler can't keep one in a host register across a call (the callee gets c). Compiled loops keep their variables
in the callee-saved registers x19..x28 (Pokémon Z-A's resource lookups while a battle loads: x19, x23, x25, x26), and
AAPCS64 says every callee leaves those as it found them. So a function may hold them in locals for its whole body:
loaded at its entry, written back (the ones it writes) where control leaves it to code that may read c->x - a return,
a tail call, the dispatcher - and around an inline-asm batch (it reads and writes the struct). A plain call needs
nothing: the callee saves and restores c->x[19..28] as the caller left them in memory (possibly stale), and the
caller's locals stay right.

Not for functions with entries or exits this can't see: a guest setjmp site (a longjmp restores those registers
through the struct), hook / patch / randomizer sites (runtime code reads the struct there), jump tables to any
instruction. Fibers are fine: each guest fiber has its own host stack and its own Cpu (runtime/hle/fiber.c).
Checked by tools/test_regpromote.py.
"""
import re

_REG = re.compile(r'c->x\[(19|2[0-8])\]')
_WRITE = re.compile(r'c->x\[(19|2[0-8])\]\s*=(?!=)')
_RET = re.compile(r'(__attribute__\(\(musttail\)\) )?return\b')


def promote(lines):
    """lines: a generated function, its first two lines `void f(Cpu *c) {` and FN_ENTER, its last `}`. Returns the
    function with x19..x28 as locals (unchanged when it uses none)."""
    body = lines[2:-1]
    text = '\n'.join(body)
    used = sorted({int(m) for m in _REG.findall(text)})
    if not used:
        return lines
    written = sorted({int(m) for m in _WRITE.findall(text)})
    wb = ' '.join(f'c->x[{r}] = xr_{r};' for r in written)
    reload = ' '.join(f'xr_{r} = c->x[{r}];' for r in used)
    out = lines[:2] + ['    uint64_t ' + ', '.join(f'xr_{r} = c->x[{r}]' for r in used) + ';']
    for ln in body:
        ln = _REG.sub(lambda m: f'xr_{m.group(1)}', ln)
        if wb and 'return' in ln:
            ln = _RET.sub(lambda m: wb + ' ' + m.group(0), ln)
        if '__asm__' in ln:
            ln = '    { ' + (wb + ' ' if wb else '') + ln.strip() + ' ' + reload + ' }'
        out.append(ln)
    out.append(lines[-1])
    return out
