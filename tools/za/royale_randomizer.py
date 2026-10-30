#!/usr/bin/env python3
# Z-A Royale (night Battle Zone) trainer randomizer -> a ZA Recomp romfs mod.
#
# Randomizes the Pokemon of the night-time Z-A Royale trainers (the `za_rank_*` trdata entries:
# ranks Z..B plus the Infinite Royale `za_rank_inf*`), within a level range, and/or shuffles whole
# teams between trainers of the same rank. The trainer's identity (trid / trtype / zaRank) is left
# alone, so the Battle Zone spawns, dialogue and promotion matches keep working.
#
# It edits the game's `world/ik_data/trainer/trdata/trdata_array.bin` (a FlatBuffer) through its own
# reflection schema (`trdata_array.bfbs`, also shipped in the game) and flatc, exactly like SkyPy's
# trainer editor, then writes the loose file into `mods/<name>/romfs/...`. The recomp runtime
# (runtime/services/fs.c: trpfd_unlist) automatically serves a mod's file in place of the packed one.
#
# Inputs you need from your own game files (never shipped):
#   extracted/... the pack contains  arc/data.trpfs (files) + arc/data.trpfd (index).
#   trdata_array.bin / trdata_array.bfbs  ->  extract with tools/za/trdata_extract.py (or any
#   ONEPACK reader), then flatc makes the JSON:
#     flatc -t --strict-json --raw-binary --defaults-json -o work schema.bfbs -- bin
#
# Usage:
#   python tools/za/royale_randomizer.py --json work/trdata_array.json --flatc /path/flatc \
#       --bfbs work/trdata_array.bfbs --out "mods/ZA Royale Randomizer/romfs" --seed 1234 \
#       [--delta 2] [--min-level 1] [--max-level 100] [--pool rank|all] [--shuffle-teams]
#       [--include infinite] [--dry-run]
import argparse, json, os, random, subprocess, sys

NIGHT_PREFIXES = ("za_rank_",)          # the named Z-A Royale opponents (Z..B)
INFINITE_PREFIXES = ("za_rank_inf", "za_inf_", "dim_rank_")  # Infinite Z-A Royale pools
SLOTS = [f"poke{i}" for i in range(1, 7)]


def is_night(trid, include_infinite):
    if trid.startswith(NIGHT_PREFIXES):
        return True
    return include_infinite and trid.startswith(INFINITE_PREFIXES)


def mons(t):
    """The filled Pokemon slots of a trainer (in order)."""
    return [t[s] for s in SLOTS if t.get(s, {}).get("devId", "DEV_NULL") not in ("DEV_NULL", None)]


def rank_of(trid):
    """Shuffle bucket: same family + same tier, so teams only move within their own rank pool."""
    p = trid.split("_")
    if trid.startswith("za_rank_inf"):
        return "za_rank_inf" + p[2]   # Infinite Royale tiers inf1..inf4
    if trid.startswith("za_rank_"):
        return "za_rank_" + p[2]      # Royale rank letter Z..B
    if trid.startswith("za_inf_"):
        return "za_inf_" + p[2]
    if trid.startswith("dim_rank_"):
        return "dim_rank_" + p[2]
    return trid


def clamp(v, lo, hi):
    return max(lo, min(hi, v))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--json", required=True, help="trdata_array.json (from flatc -t)")
    ap.add_argument("--bfbs", required=True, help="trdata_array.bfbs (reflection schema from the game)")
    ap.add_argument("--flatc", default="flatc", help="path to flatc")
    ap.add_argument("--out", required=True, help="romfs output dir (…/romfs, or the mod folder's romfs)")
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--delta", type=int, default=2, help="level +/- around the original (default 2)")
    ap.add_argument("--min-level", type=int, default=1)
    ap.add_argument("--max-level", type=int, default=100)
    ap.add_argument("--pool", choices=["rank", "all"], default="rank",
                    help="species pool: 'rank' = species the Royale already uses (default), 'all' = every species any trainer uses")
    ap.add_argument("--include", default="", help="extra prefixes to include, e.g. 'infinite'")
    ap.add_argument("--shuffle-teams", action="store_true", help="permute whole teams between trainers of the same rank")
    ap.add_argument("--no-randomize", action="store_true", help="with --shuffle-teams: shuffle only, do not resample Pokemon")
    ap.add_argument("--dry-run", action="store_true", help="edit + report, but do not run flatc/write the bin")
    args = ap.parse_args()

    rng = random.Random(args.seed)
    include_inf = "infinite" in args.include.lower()

    with open(args.json, encoding="utf-8") as fh:
        data = json.load(fh)
    values = data["values"]
    night = [t for t in values if is_night(t["trid"], include_inf)]
    if not night:
        sys.exit("no night trainers matched")
    print(f"{len(values)} trainers, {len(night)} night trainers "
          f"({sum(1 for t in night if is_night(t['trid'], False))} Royale"
          f"{' + Infinite' if include_inf else ''})")

    # Species / nature pools: only values the game's own table already uses (always valid enums).
    if args.pool == "all":
        pool = sorted({p["devId"] for t in values for p in mons(t)})
    else:
        pool = sorted({p["devId"] for t in night for p in mons(t)})
    natures = sorted({p["seikaku"] for t in values for p in mons(t)})
    print(f"species pool: {len(pool)}  natures: {len(natures)}")

    # Optional: shuffle whole teams among trainers of the same rank.
    if args.shuffle_teams:
        by_rank = {}
        for t in night:
            by_rank.setdefault(rank_of(t["trid"]), []).append(t)
        moved = 0
        for r, group in by_rank.items():
            teams = [mons(t) for t in group]
            rng.shuffle(teams)
            for t, team in zip(group, teams):
                for s in SLOTS:
                    t.pop(s, None)
                for i, mon in enumerate(team):
                    t[SLOTS[i]] = mon
                moved += 1
        print(f"shuffled teams of {moved} trainers")

    if not args.no_randomize:
        for t in night:
            team = mons(t)
            if not team:
                continue
            base = [m["level"] for m in team]
            lo = clamp(min(base) - args.delta, args.min_level, args.max_level)
            hi = clamp(max(base) + args.delta, args.min_level, args.max_level)
            for m in team:
                m["devId"] = rng.choice(pool)
                m["formId"] = 0
                m["sex"] = "DEFAULT"
                m["item"] = "ITEMID_NONE"
                m["level"] = rng.randint(lo, hi)
                m["ballId"] = m.get("ballId", "MONSUTAABOORU")
                m["seikaku"] = rng.choice(natures)
                m["tokusei"] = "RANDOM_12"
                m["rareType"] = "NO_RARE"
                m["scaleValue"] = 128
                m["isOybn"] = False
                for w in range(1, 5):
                    m[f"waza{w}"] = {"wazaId": "WAZA_NULL", "isPlusWaza": False}
                for k in ("talentValue", "effortValue"):
                    m[k] = {a: 0 for a in ("hp", "atk", "def", "spAtk", "spDef", "agi")}

    if args.dry_run:
        print("dry run: no files written")
        return
    os.makedirs(args.out, exist_ok=True)
    out_json = os.path.join(args.out, "trdata_array.json")
    with open(out_json, "w", encoding="utf-8") as fh:  # (closed - flushed - before flatc reads it)
        json.dump(data, fh, ensure_ascii=False)
    subprocess.run([args.flatc, "-b", "-o", args.out, args.bfbs, out_json], check=True)
    os.remove(out_json)
    romfs_dir = os.path.join(args.out, "world", "ik_data", "trainer", "trdata")
    os.makedirs(romfs_dir, exist_ok=True)
    os.replace(os.path.join(args.out, "trdata_array.bin"), os.path.join(romfs_dir, "trdata_array.bin"))
    print(f"wrote {romfs_dir}/trdata_array.bin")


if __name__ == "__main__":
    main()
