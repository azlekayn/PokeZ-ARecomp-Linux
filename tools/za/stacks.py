# build/stacks.txt (LARECOMP_SAMPLE=<delay>,<s> LARECOMP_SAMPLE_EXEC=1, runtime/platform/windows.c: the thread running
# the game's command lists, its call stack ~1000 times a second) -> functions ranked by self time (the leaf) and by
# inclusive time (anywhere on the stack), as a share of the samples. lasrecomp.exe's functions from its DWARF line
# info (addr2line -i: static and inlined functions by their own name), other modules as <module> (the driver:
# nvoglv64 / amdvlk64, ntdll...).
# usage: python tools/za/stacks.py <stacks.txt> <lasrecomp.exe> [rows, default 40] [function: what it calls]
import bisect, collections, os, subprocess, sys

path, exe = sys.argv[1], sys.argv[2]
rows = int(sys.argv[3]) if len(sys.argv) > 3 else 40
focus = sys.argv[4] if len(sys.argv) > 4 else None
mods, stacks = [], []
for line in open(path):
    p = line.split()
    if p[0] == 'M': mods.append((int(p[1], 16), int(p[2], 16), ' '.join(p[3:])))
    elif p[0] == 'S': stacks.append([int(x, 16) for x in p[1:]])
mods.sort()
mbase = [m[0] for m in mods]
exe_mod = next((m for m in mods if m[2].lower().endswith('.exe')), None)  # (the process's own image: the one .exe)
IMAGE_BASE = 0x140000000  # (the exe's preferred base: what its debug info is in)


def module_of(a):
    i = bisect.bisect_right(mbase, a) - 1
    return mods[i] if i >= 0 and a < mods[i][0] + mods[i][1] else None


# every exe address at once through addr2line: address -> [innermost function, ..., the physical one]
exe_addrs = sorted({a for s in stacks for a in s if exe_mod and module_of(a) is exe_mod})
chains = {}
if exe_addrs:
    q = '\n'.join('%x' % (a - exe_mod[0] + IMAGE_BASE) for a in exe_addrs)
    out = subprocess.run(['addr2line', '-a', '-f', '-i', '-e', exe], input=q, capture_output=True, text=True).stdout.splitlines()
    cur, names, k = None, [], 0
    while k < len(out):
        l = out[k]
        if l.startswith('0x'):
            if cur is not None: chains[cur] = names
            cur, names = int(l, 16) - IMAGE_BASE + exe_mod[0], []
            k += 1
            continue
        names.append(l.strip()); k += 2  # (function, then file:line)
    if cur is not None: chains[cur] = names


def frames(a):
    if a in chains and chains[a] and chains[a][0] != '??': return chains[a]
    m = module_of(a)
    return [os.path.basename(m[2]) if m else '?%x' % a]


n = len(stacks)
self_t, incl, callees = collections.Counter(), collections.Counter(), collections.Counter()
for s in stacks:
    names = [nm for a in s for nm in frames(a)]  # (innermost first)
    self_t[names[0]] += 1
    for nm in set(names): incl[nm] += 1
    if focus and focus in names:
        k = names.index(focus)
        callees[names[k - 1] if k else '(itself)'] += 1
print(f'{n} samples')
print('\n-- self (the function itself) --')
for nm, c in self_t.most_common(rows): print(f'{100 * c / n:6.2f}%  {nm}')
print('\n-- inclusive (it and what it calls) --')
for nm, c in incl.most_common(rows): print(f'{100 * c / n:6.2f}%  {nm}')
if focus:
    print(f'\n-- what {focus} calls (its share of all samples) --')
    for nm, c in callees.most_common(rows): print(f'{100 * c / n:6.2f}%  {nm}')
