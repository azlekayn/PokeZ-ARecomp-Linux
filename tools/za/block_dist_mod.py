# Makes mods/BlockDistance (runtime/services/fs.c layers it over the game's files): Lumiose's buildings keep their full
# detail further out. Each city block is a scene part, world/ik_scene/field/area/t1/sub_scene/t1_bk01_/t1_bk01_0.trscn and
# so on (flatbuffers, 75 of them), holding a merged low-detail model (trinity_CombineLODEntity, field/model/t1/t1_clod/...)
# and sub-scenes with the real buildings, streamed in by a trinity_ConditionalStreamingArea. That swap is the big pop:
# a whole block goes from flat, painted-on windows to the real thing at once. mods/PropDistance's cull table and
# mods/MaxLOD don't touch it. Here the distances are multiplied (--scale, default 1.5) in place, so the file keeps its
# size and layout: the entity's 180 m, and the area's 200 m box with its 205 m way out. All of them together: 180 is under
# 200 so that the real block has loaded before the merged one goes, and scaling only one could leave holes.
# What each number does was worked out from the data and one test, not from a format description.
# Costs GPU time and memory: more real buildings loaded and drawn. Drop the folder (or rename it _BlockDistance) to go back.
# usage (from the checkout): python tools/za/block_dist_mod.py --ooz <ooz.dll/.so> [--romfs extracted/romfs]
#   [--out mods/BlockDistance] [--scale 1.5]
import argparse, mmap, os, re, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import trinity

ap = argparse.ArgumentParser()
ap.add_argument('--ooz', required=True)
ap.add_argument('--romfs', default='extracted/romfs')
ap.add_argument('--out', default='mods/BlockDistance')
ap.add_argument('--scale', type=float, default=1.5)
args = ap.parse_args()
ROOT = 'world/ik_scene/field/area/t1/sub_scene/'
M64 = (1 << 64) - 1


def fnv(s):
    h = 0xCBF29CE484222645
    for c in s.encode(): h = ((h ^ c) * 0x100000001B3) & M64
    return h


oodle = trinity.Oodle(args.ooz)
f = open(os.path.join(args.romfs, 'arc', 'data.trpfs'), 'rb')
m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
fb = trinity.FB(m)
root = fb.u64(8)
n, op = fb.vec(fb.table(root + fb.u32(root))[1])
first = fnv(ROOT + 't1_bk01_/t1_bk01_0.trscn')
files = None
for i in range(n):  # (the pack holding the first block: they share one)
    off = fb.u64(op + 8 * i)
    t = fb.table(off + fb.u32(off))
    nh, hp = fb.vec(t[0])
    hashes = [fb.u64(hp + 8 * j) for j in range(nh)]
    if first in hashes:
        files = {}
        nf, fp = fb.vec(t[1])
        for j in range(nf):
            e = fp + 4 * j
            ft = fb.table(e + fb.u32(e))
            comp = m[ft[1]] if len(ft) > 1 and ft[1] else 0xFF
            size = fb.u64(ft[3]) if len(ft) > 3 and ft[3] else 0
            dn, dp = fb.vec(ft[4])
            try: files[hashes[j]] = oodle.unpack(comp, size, m[dp:dp + dn])
            except ValueError: pass
        break
if files is None: sys.exit('error: no city block file (another game version?)')

names = set()
for d in files.values():
    if b'trinity_CombineLODEntity' in d:
        names |= {x.group().decode() for x in re.finditer(rb't1_[a-z0-9_]+', d)}
path = {}
for b in names:
    h = fnv(f'{ROOT}{b}_/{b}_0.trscn')
    if h in files: path[h] = f'{ROOT}{b}_/{b}_0.trscn'

floats = {180.0: 1, 200.0: 3, 205.0: 3}
done = skipped = 0
for h, d in files.items():
    if b'trinity_CombineLODEntity' not in d: continue
    at = {v: [o for o in range(0, len(d) - 3, 4) if struct.unpack_from('<f', d, o)[0] == v] for v in floats}
    if any(len(at[v]) != c for v, c in floats.items()) or h not in path:  # (not a block with its own streaming area)
        skipped += 1
        print('left alone:', path.get(h, hex(h)), {v: len(a) for v, a in at.items()})
        continue
    out = bytearray(d)
    for v, offs in at.items():
        for o in offs: struct.pack_into('<f', out, o, v * args.scale)
    p = os.path.join(args.out, 'romfs', path[h])
    os.makedirs(os.path.dirname(p), exist_ok=True)
    open(p, 'wb').write(out)
    done += 1
print(f'{done} city blocks at {args.scale}x (merged model to {180 * args.scale:g} m, streaming box {200 * args.scale:g} m), '
      f'{skipped} left alone -> {args.out}')
