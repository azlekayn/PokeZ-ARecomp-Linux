# Makes mods/PropDistance (runtime/services/fs.c layers it over the game's files): Pokémon Z-A's small props are drawn
# further out. The scene part every map loads, world/ik_scene/parts/common_resident_/common_resident_0.trscn
# (flatbuffers), gives ~60 prop models a trinity_ModelCullingSetting: a path regex (^field/model/t1/t1_ar/t1_ar010/.*,
# field_item013, the t1_pl plants...) and the distance they stop being drawn at - mostly 65 m, some 90-130, two 250.
# That cut, not their LOD meshes (mods/MaxLOD keeps those full and the pop stayed), is where benches, boxes and plants
# pop in. Here each distance is multiplied (--scale, default 3) in place: the file keeps its size and layout. Costs
# GPU time: more props drawn far away. Drop the folder (or rename it _PropDistance) to go back.
#
# The same file (and world/ik_scene/parts/t4_lod_setting_/t4_lod_setting_0.trscn, Lumiose's) also has
# trinity_ModelLodSetting per category (Field 50/100/150 m, chara 8/15/30, pokemon 50/100/150, flowers / ladder
# 25/100/150): --lod <names> (e.g. chara,pokemon,Field) multiplies those categories' distances by --lod-scale.
#
# --fade N: the model dither fade (world/ik_data/gfx/model_dither_fade/model_dither_fade_default_param/default.bin, a
# ModelDitherFadeDefaultParam: Far begin/end 75/90 m, Near 5/2 m, FadeTime 0.2 s) has its far distances multiplied:
# 75-90 m.
# Not solved by either (Steam Deck, 2026-11-03): the unmodded game's "mid ring", where people ~25-30 m+ away fade out and
# further ones are drawn again - --lod chara,pokemon and --fade 3 left it as it was.
# usage (from the checkout): python tools/za/prop_dist_mod.py --ooz <ooz.dll/.so> [--romfs extracted/romfs]
#   [--out mods/PropDistance] [--scale 3] [--lod chara,pokemon] [--lod-scale 3] [--fade 1]
import argparse, os, re, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import trinity

ap = argparse.ArgumentParser()
ap.add_argument('--ooz', required=True)
ap.add_argument('--romfs', default='extracted/romfs')
ap.add_argument('--out', default='mods/PropDistance')
ap.add_argument('--scale', type=float, default=3.0)
ap.add_argument('--fade', type=float, default=1.0, help='scale the model dither fade\'s far distances (1: unchanged)')
ap.add_argument('--lod', default='', help='ModelLodSetting categories to scale, comma-separated (chara,pokemon,Field...)')
ap.add_argument('--lod-scale', type=float, default=3.0)
args = ap.parse_args()
M64 = (1 << 64) - 1
FILES = ['world/ik_scene/parts/common_resident_/common_resident_0.trscn',
         'world/ik_scene/parts/t4_lod_setting_/t4_lod_setting_0.trscn']
FADE = 'world/ik_data/gfx/model_dither_fade/model_dither_fade_default_param/default.bin'


def patch_fade(d):
    """the dither fade param with its far distances scaled: the two floats over 10 m (the near ones are 2-5 m, the time
    0.2 s; checked so a changed layout is left alone)"""
    fb = trinity.FB(d)
    t = fb.table(fb.u32(0))
    if len(t) != 5 or None in t: return d, ['SKIPPED dither fade: unexpected layout']
    vals = [struct.unpack_from('<f', d, o)[0] for o in t]
    far = [o for o, v in zip(t, vals) if v > 10]
    if len(far) != 2: return d, [f'SKIPPED dither fade: {vals}']
    out = bytearray(d)
    for o in far: struct.pack_into('<f', out, o, struct.unpack_from('<f', d, o)[0] * args.fade)
    return bytes(out), [f'dither fade {vals} -> {[struct.unpack_from("<f", out, o)[0] for o in t]}']


def fnv(s):
    h = 0xCBF29CE484222645
    for c in s.encode(): h = ((h ^ c) * 0x100000001B3) & M64
    return h


def nice(d, o):  # a distance as the files write them: a whole or quarter number of metres, 1..100000
    x = struct.unpack_from('<f', d, o)[0]
    return 1 <= x <= 100000 and x * 4 == int(x * 4)


def patch(d):
    """the file with its culling (and --lod categories' LOD) distances scaled; and what changed, for the log"""
    out, log = bytearray(d), []
    lod = set(filter(None, args.lod.split(',')))
    # each setting is a table written before its strings: [... distance(s) ...][regex][name]["trinity_<kind>"];
    # the distances are the nice floats between the previous setting's kind string and this one's regex
    prev = 0
    for m in re.finditer(rb'trinity_(ModelCullingSetting|ModelLodSetting|JointPrioritySetting)\0', d):
        kind = m.group(1).decode()
        rx = re.search(rb'\^[ -~]+', d[prev:m.start()])
        regex = rx.group().decode() if rx else '?'
        seg_end = prev + rx.start() - 4 if rx else m.start()
        fl = [o for o in range(prev + (-prev % 4), seg_end - 3, 4) if nice(d, o)]
        strs = [x for x in re.finditer(rb'[ -~]+', d[prev:m.start()])
                if x.start() >= 4 and struct.unpack_from('<I', d, prev + x.start() - 4)[0] == len(x.group())]
        name = strs[-1].group().decode() if strs else '?'  # (the setting's name: chara, pokemon, Field...)
        prev = m.end()
        k = args.scale if kind == 'ModelCullingSetting' and len(fl) == 1 else \
            args.lod_scale if kind == 'ModelLodSetting' and name in lod and len(fl) == 3 else None
        if k is not None and k != 1:
            old = [struct.unpack_from('<f', d, o)[0] for o in fl]
            for o, v in zip(fl, old): struct.pack_into('<f', out, o, v * k)
            log.append(f'{kind} {name} {regex}: {old} -> {[v * k for v in old]}')
        elif kind == 'ModelCullingSetting' and len(fl) != 1:
            log.append(f'SKIPPED {kind} {regex}: {len(fl)} candidate distances')
    return bytes(out), log


want = {fnv(p): p for p in FILES + ([FADE] if args.fade != 1 else [])}
oodle = trinity.Oodle(args.ooz)
for _, fh, d in trinity.pack_files(os.path.join(args.romfs, 'arc', 'data.trpfs'), oodle, 1 << 20):
    if fh not in want: continue
    path = want.pop(fh)
    data, log = patch_fade(d) if path == FADE else patch(d)
    print(path, f'{len(log)} settings changed' if log else 'nothing to change')
    for line in log: print('  ', line)
    if data == d: continue
    dst = os.path.join(args.out, 'romfs', path)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    open(dst, 'wb').write(data)
    if not want: break
for p in want.values(): print('not found:', p)
