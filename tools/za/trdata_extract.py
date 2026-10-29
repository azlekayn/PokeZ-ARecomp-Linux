#!/usr/bin/env python3
# Pull the trainer table out of Pokémon Z-A's own game files, for the Royale randomizer.
#
# The game keeps its data in the ONEPACK set arc/data.trpfs (Oodle-compressed files) + arc/data.trpfd
# (the index: FNV-1a 64 of a file's RomFS path -> its pack entry). This finds two known paths by hash
# and decompresses them with Powzix ooz, the same decoder the zaprompts helper uses:
#   world/ik_data/trainer/trdata/trdata_array.bin    (the FlatBuffer table)
#   world/ik_data/trainer/trdata/trdata_array.bfbs   (its reflection schema, for flatc)
#
# Needs ooz built as a shared library. On Linux (uses the shim tools/za/ooz_linux_stdafx.h):
#   mkdir -p build/ooz-linux && cp third_party/ooz/{kraken,bitknit,lzna,ooz_lib}.cpp build/ooz-linux/ \
#     && cp tools/za/ooz_linux_stdafx.h build/ooz-linux/stdafx.h \
#     && (cd build/ooz-linux && g++ -O2 -shared -fPIC -msse2 -Dmain=ooz_main \
#          kraken.cpp bitknit.cpp lzna.cpp ooz_lib.cpp -o ooz.so)
# (The Windows release builds it with tools/za/dist.sh into build/ooz/ooz.dll.)
#
# Usage: python tools/za/trdata_extract.py --ooz ooz.so [--romfs extracted/romfs.bin | --dir extracted/romfs]
#          --out work/
import argparse, ctypes, os, struct, sys

PATHS = [
    "world/ik_data/trainer/trdata/trdata_array.bin",
    "world/ik_data/trainer/trdata/trdata_array.bfbs",
]


def fnv64(s):
    x = 0xCBF29CE484222645
    for b in s.encode():
        x = ((x ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return x


class Oodle:
    def __init__(self, path):
        self.lib = ctypes.CDLL(os.path.abspath(path))
        self.lib.ooz_decompress.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]

    def unpack(self, comp, size, raw):
        if comp == 0xFF or size == len(raw):
            return bytes(raw)
        out = ctypes.create_string_buffer(size + 64)
        if self.lib.ooz_decompress(bytes(raw), len(raw), out, size) != size:
            raise ValueError("ooz")
        return out.raw[:size]


class FB:
    def __init__(s, m): s.m = m
    def u32(s, p): return struct.unpack_from("<I", s.m, p)[0]
    def u64(s, p): return struct.unpack_from("<Q", s.m, p)[0]
    def table(s, p):
        vt = p - struct.unpack_from("<i", s.m, p)[0]
        n = (struct.unpack_from("<H", s.m, vt)[0] - 4) // 2
        return [(p + o) if (o := struct.unpack_from("<H", s.m, vt + 4 + 2 * i)[0]) else None for i in range(n)]
    def vec(s, fp):
        v = fp + s.u32(fp)
        return s.u32(v), v + 4


def open_romfs_image(path):
    """arc/data.trpfs + arc/data.trpfd bytes out of a RomFS image (romfs.bin), or None."""
    f = open(path, "rb")
    h = struct.unpack("<10Q", f.read(0x50))
    doff, dsz, foff, fsz, data = h[3], h[4], h[7], h[8], h[9]
    f.seek(doff); dm = f.read(dsz)
    f.seek(foff); fm = f.read(fsz)

    def dir_path(d):
        parts = []
        while d:
            p, = struct.unpack_from("<I", dm, d)
            nl, = struct.unpack_from("<I", dm, d + 0x14)
            parts.append(dm[d + 0x18:d + 0x18 + nl].decode())
            d = p
        return "/".join(reversed(parts))

    o = 0
    want = {}
    while o + 0x20 <= fsz:
        parent, _, off, size, _, nl = struct.unpack_from("<IIQQII", fm, o)
        name = fm[o + 0x20:o + 0x20 + nl].decode()
        p = (dir_path(parent) + "/" if parent else "") + name
        o += 0x20 + ((nl + 3) & ~3)
        if p in ("arc/data.trpfs", "arc/data.trpfd"):
            f.seek(data + off); want[p] = f.read(size)
    return want.get("arc/data.trpfs"), want.get("arc/data.trpfd")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ooz", required=True)
    ap.add_argument("--romfs", help="extracted/romfs.bin")
    ap.add_argument("--dir", help="extracted/romfs (has arc/data.trpfs + data.trpfd)")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    if a.romfs:
        trpfs, trpfd = open_romfs_image(a.romfs)
    else:
        trpfs = open(os.path.join(a.dir, "arc", "data.trpfs"), "rb").read()
        trpfd = open(os.path.join(a.dir, "arc", "data.trpfd"), "rb").read()
    if not trpfs or not trpfd:
        sys.exit("arc/data.trpfs + data.trpfd not found")
    oodle = Oodle(a.ooz)
    m = trpfs
    fb = FB(m)
    assert m[:8] == b"ONEPACK\0", "not a ONEPACK file"
    root = fb.u64(8)
    t = fb.table(root + fb.u32(root))
    n, op = fb.vec(t[1])
    th = {fnv64(p): p for p in PATHS}
    os.makedirs(a.out, exist_ok=True)
    got = 0
    for i in range(n):
        off = fb.u64(op + 8 * i)
        tt = fb.table(off + fb.u32(off))
        nh, hp = fb.vec(tt[0])
        nf, fp = fb.vec(tt[1])
        for j in range(nf):
            hv = fb.u64(hp + 8 * j)
            if hv not in th:
                continue
            e = fp + 4 * j
            ft = fb.table(e + fb.u32(e))
            comp = m[ft[1]] if len(ft) > 1 and ft[1] else 0xFF
            size = fb.u64(ft[3]) if len(ft) > 3 and ft[3] else 0
            dn, dp = fb.vec(ft[4])
            data = oodle.unpack(comp, size, m[dp:dp + dn])
            name = os.path.basename(th[hv])
            open(os.path.join(a.out, name), "wb").write(data)
            print("wrote", os.path.join(a.out, name), len(data))
            got += 1
    if not got:
        sys.exit("trdata_array.bin/.bfbs not found (game version?)")


if __name__ == "__main__":
    main()
