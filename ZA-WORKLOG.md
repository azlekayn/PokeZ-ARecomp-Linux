# ZA Recomp (PokeZ-ARecomp) — worklog / handoff notes

Repo (working dir): `/mnt/SSD Storage/Games/Ports/Switch/PokeZ-ARecomp-pokemon-za`
Session date: 2026-10-28. Read this first in a new session.

## CURRENT STATE — build #20 (2026-10-28)
- Branch **`fg`**. AppImage `build/dist/LAS Recomp-v0.1.10-linux-x86_64.AppImage` = **build #20**.
  Game code `extracted/lasgame.so.id = e0a36a7d6da5179b-B1F12FD919EAE86A`; everything since is runtime-only,
  so **do not recompile** the game lib.
- Done this session:
  - **Build #19:** `framegen` / `fg_strength` / `fg_thresh` / `frame_dt` are now `larecomp.ini` + F1 settings
    (env vars still override at load); `[fg]` spam gated behind `LARECOMP_FG_LOG=1`; added the
    `LARECOMP_FOCUS_REBUILD=1` lever for the post-alt-tab sag.
  - **Build #20:** the decouple (`rt_za_frame_dt`) was fed the game's CPU-busy time, so it did nothing — it now
    accumulates real wall-clock and reports `owed - 1` (its "vsyncs - 1" convention). Verified with an A/B
    screenshot. Frame generation no longer does a **blocking** second acquire/present (timeout 0, dropped if no
    image) and goes **idle** when there are no motion vectors at the presented size (Dynamic res / not 1x).
  - **Steam Deck bundle:** `build/dist/ZA-SteamDeck-linux-x86_64.zip` (6.15 GB,
    sha256 `79cd92d34d8c4be332734985ec1fbdecd6a29b433509b56f3274adfe75424d2e`) — self-contained
    (AppImage + `run.sh` + game files, no compile on the Deck).
- **OPEN (verify):**
  1. On the PC: does the build #20 frame_dt fix the below-60 "under water"? (main unverified item)
  2. Frame gen at `resolution = 1x` + `dynamic_res = off` (the ini had `dynamic_res = game`).
  3. `LARECOMP_FOCUS_REBUILD=1` for the post-alt-tab sag (lever added, untested).
  4. Sideload the Deck zip and test.
- Levers: `LARECOMP_FRAME_DT_LOG`, `LARECOMP_FRAME_DT_FORCE`, `LARECOMP_FG_LOG`, `LARECOMP_FOCUS_REBUILD`.
  (See "Tests to run (build #20)" and "Build #20" below.)

## Environment
- Fedora 44, Ryzen 7 7800X3D, **AMD RX 7800 XT (RADV, Mesa 26.2.4)**, 30 GB RAM.
- Displays: DP-1 LG UltraGear **2560x1440 @ 165 Hz** (primary), HDMI Dell 1080p @ 60 Hz.
- The port is upstream-tested on **Windows + NVIDIA only**; we are on the untested **Linux + RADV** path.
- **SELinux is Enforcing.** `make appimage` runs podman and the repo bind-mount fails without a label
  workaround. A scoped wrapper exists at `/tmp/opencode/bin/podman` (adds `--security-opt label=disable`
  to `podman run` only). Build with: `PATH="/tmp/opencode/bin:$PATH" make appimage`.
  If the wrapper is gone after a restart, recreate it (content is trivial: loop args, insert the flag after `run`).

## Data root / artifacts
- Data root: `~/.local/share/lasrecomp` (game files, lasgame.so, caches, ini, saves, log `lasrecomp.log`).
- AppImage: `build/dist/LAS Recomp-v0.1.10-linux-x86_64.AppImage` (rebuilt many times this session).
- Original (pre-PR) AppImage backup: `/tmp/opencode/ZA-AppImage-original.AppImage`.
- Launcher script: `~/ZA-Recomp.sh` (sets the **RADV fix**, see below).

## Game files (extraction)
- `zaimport` (LibHac importer) is **Windows-only** in the repo; the Linux AppImage does not bundle it, so the
  start screen can't import packages on Linux. We built it ourselves:
  - `podman run ... mcr.microsoft.com/dotnet/sdk:8.0 dotnet publish tools/zaimport -c Release -r linux-x64
    --self-contained ... -o build/zaimport` → `build/zaimport/zaimport`.
  - Ran it: base v1.0.0 + update **v2.0.2** → `~/.local/share/lasrecomp/extracted/{exefs/,romfs.bin}` (6 GB).
  - **DLC not included**: its NSP has no ticket and `title.keys` has no `0100F43008C45002` entry.
- Game code (`extracted/lasgame.so`) was built once by the AppImage start screen; id `29a5b3a30e545c4a-B1F12FD919EAE86A`.

## THE RADV BLACK-WORLD FIX (important)
- Symptom: game runs (HUD/minimap draw) but the **3D world and all text are black**; game logic alive; ~1340 draws/frame.
- Cause: the **guest heap imported as driver memory (dma-buf / placed map)** is broken on this RADV setup.
- Fix (in `~/ZA-Recomp.sh`): `LARECOMP_VK_HEAP_DMABUF=0` + `LARECOMP_VK_GMEM_WINDOW=256`.
  - User confirmed: stutter gone, world renders at 60 fps.
- Related upstream open issues: Hexveil404/PokeZ-ARecomp **#50, #51** (RADV). Files one with `vulkaninfo --summary`.

## Imported upstream PRs (git branch `pr57`)
Repo is now a git repo (base commit `6c9e603` = upstream v0.1.10 / 51bd927 local tree).
Merged cleanly (only `menu.mm` needed a union resolve for #46 vs #52):
`#41` (fractional occlusion), `#45`, `#46`, `#48`, `#49`, `#52`, `#55`, and **`#57`** (big: FSR2 + HDR + SSGI +
water + GTAO; union-resolved 9 conflicts).
### PR #57 Linux build fixes (it only ever built on Windows)
- Makefile FSR2 rules: add `-DFFX_GCC` and `-include stddef.h` (SDK headers use `__declspec`, `size_t`).
- `third_party/fsr2/vk/ffx_fsr2_vk.cpp`: portable `wcscpy_s` shim for non-Windows.
- `settings.c`: save `temporal_aa` (PR#57 parsed it but never persisted it).
- **FSR2 now builds and runs on Linux/RADV** (temporal AA resolves frames — never worked before).
- Known: **FSR2 flickers the in-game UI** (PR#57 is experimental). FG no longer needs it (below).

## Frame generation (git branch `fg`; `LARECOMP_FRAMEGEN=1`)
Implemented from scratch on top of #57's depth-derived motion vectors.
- `shaders.c`: `fg_interp.frag` — **motion-faded** midpoint: `t = clamp(|mv|/thresh,0,1)*strength`,
  `mid = mix(cur(uv-0.5m), prev(uv+0.5m), 0.5)`, `out = mix(cur, mid, t)`. Static pixels/HUD = current frame.
  Env knobs: `LARECOMP_FG_THRESH` (px, default 2), `LARECOMP_FG_STRENGTH` (0..1, default 1).
- `exec.c`: `vk_fg_build()` keeps prev/cur and runs the pass; MVs come from `fsr2_mv.frag`.
  **MVs are now built with FSR2 OFF too** (the copy site runs when `fsr2_wanted() || vk_fg_on()`), so FG works
  without FSR2 and avoids its HUD flicker.
- `device.c`: presents the generated frame **after** the game's own (reverted an experiment that put it *before*
  — that made stutter worse). **Overlay fix**: `vk_overlay_replay()` replays the ImGui overlay's secondary command
  buffer on the generated frame, so the port's FPS counter/menu (drawn after the game image) don't flicker.
- **Requirements**: `resolution = 1x` and Dynamic off (so scene size == presented texture size), FSR2 optional.
- Verified here: `[fg] frame N: valid=1`, presents the generated frame, no errors.

## Slow motion / "decouple the world step from the frame rate" (in progress)
> **SUPERSEDED by Build #20** (below). The analysis here is wrong on the key point: `[x1,#8]` is the game's
> **CPU-busy** time (~8.5 ms at 60 fps), not its frame period, and the subsystems' step is `w20 + 1` vsyncs. The
> accumulator built on that value therefore did nothing. See "Build #20" for the real bug and fix. Kept for history.
- By design: the game advances its world **1/60 s per presented frame**; below 60 fps it slow-motions
  (~57 fps = 95 % speed). `fps60_lock=1` keeps 60 and accepts slow motion.
- **Decompiled** the step function (2.0.2, `f_7001b29660`, via capstone on `extracted/exefs/main`):
  - `0x1b29670 ldr x8,[x1,#8]` = frame duration (ns).
  - Bands (strict `>`): >16,666,667→1, >33,333,366→2, >50,000,000→3, >66,666,667→4 vsyncs.
  - `0x1b296d8 ldrsb w20,[x9,#0x20]` = "vsyncs per frame − 1" (already patched by `rt_za_frame_cfg`).
  - `w20 = max(measured_vsyncs, configured)` = **the world step** (integer vsyncs → why 57 fps is 95 %).
- **Fix implemented**: `runtime/hle/framepace.c` `rt_za_frame_dt()` = fixed-timestep accumulator (step = whole
  vsyncs owed, fraction carried), returning a duration in the game's own band (explicit `band[5]` table — the
  thresholds are strict/rounded, so `n*step+1` was one band low). `LARECOMP_FRAME_DT=0` disables it.
- `tools/za_patches.py`: new patch at `FRAME_DT[build] = ZA_BUILDS[build] - 0x68` replacing the duration load
  with `rt_za_frame_dt(LD64(c->x[1] + 8))` (falls back to the plain load if the instruction isn't `0xf9400428`).
- **This changes the generator → GAMELIB_ID changes → the start screen will ask "Compile now" once (~10–30 min).**
  The user did this: `extracted/lasgame.so.id = e0a36a7d6da5179b-B1F12FD919EAE86A` (new id). No recompile needed
  again for runtime-only fixes below.
- **BUGFIX (user hit it):** the first `rt_za_frame_dt` kept a stall's backlog (`if (n>4) acc += (n-4)*step`), so the
  loading stalls left a permanent surplus → 4 world steps/frame → fast-forward/"lag out". Fixed: account the whole
  backlog (`acc -= n*step`) then clamp `n` to 4 (drop the excess). Build #17 (runtime-only; game lib unchanged).

## Frame-rate dips (diagnosed)
- Not shader compilation (steady state logs `compile 0`).
- The dips are **compositor swapchain-acquire stalls**: `vkAcquireNextImageKHR … 645 ms`,
  `recorder (present) 471 ms`, `queue wait` up to 660 ms. Happen when the window is windowed/occluded
  (Plex open beside it). **Run fullscreen + focused.**
- Compositor-heavy context: another agent runs a Xenoblade Chronicles 3 port with a host-wide RAM guard that
  SIGTERMs the largest process at ≥20 GiB (it killed our build and game earlier). Not our bug; coordinate/pause it.

## Test harness (as used this session)
- Run: `LARECOMP_DATA_ROOT=~/.local/share/lasrecomp LARECOMP_NO_AUDIO=1 LARECOMP_HIDDEN=1
  LARECOMP_SAVE_DIR=/tmp/opencode/za-test/save LARECOMP_VK_HEAP_DMABUF=0 LARECOMP_VK_GMEM_WINDOW=256
  LARECOMP_INPUT="300:A:6,600:A:6,900:A:6,1200:A:6,1500:A:6,1800:A:6,2200:STICK_UP:300"
  LARECOMP_SHOT_AT=3000 <AppImage> --play`
- `--play` skips the start screen. `LARECOMP_SHOT_AT` writes `build/frames/frame_NNNNN.png` (pre-create `build/frames`).
- `LARECOMP_INPUT="frame:BUTTON:frames,..."` (A/B/X/Y/L/R/ZL/ZR/PLUS/MINUS/arrows/STICK_*/ls:x/y).
- Scratch save copy at `/tmp/opencode/za-test/save` (never touch the player's real save).

## Git state
- Base: `6c9e603`. Branch **`pr57`** = PRs + FSR2 (the safe build). Branch **`fg`** = frame gen + decoupling (+ all of pr57).
- Key `fg` commits: FSR2 Linux fixes → `612d9e6` FG WIP → `9bbfec9` diag → `f2af20f` order (later reverted `fe4e11e`) →
  `14ba0fa` temporal_aa save → motion-fade → overlay replay → decoupling patch + band fix.
- `/tmp/opencode/shots/` holds A/B screenshots (black world, fixed, FSR2, FG).


## Frame gen + alt-tab (FIXED, build #18)
Measured: FG ON, a focus change (alt-tab) dropped a steady 60 to ~52 for the rest of the session (the render thread's
`wait` grew 7->91 ms; `recorder (present) 1103 ms`, `vkAcquireNextImageKHR 62 ms`). FG OFF stayed 60.0 across focus
events. Fix: `vk_fg_build` presents only the game's own frame while `!ui_window_focused()` (and drops the gap so the
first frame back isn't interpolated across it). Runtime-only. If it still sags after refocus, next lever: recreate the
swapchain / reset pacing on focus gain (the port's present-watch already does this for a stale window).


## UPDATED diagnosis (end of session)
> **SUPERSEDED** — the heavy #57 options were already off in the 17:30 run (see "Dips: the heavy #57 options are
> NOT the cause" below). The dip is a focus-transition / present-backpressure stall, not those effects.
- FG off does NOT fully hold 60 either: the 17:25 FG-off run held a clean 60.0 for ~2.5 min, then dipped
  (`56.9 -> 31.5 (max 96 ms) -> 59.5`) around focus events (~t=149-177). So the dip is partly general, not only FG.
- The user enabled the heavy #57 options: `ambient_occlusion=2`, `indirect_light=3`, `water_reflections=1`
  (`operation_mode=docked`, `resolution=1x`, `tex_hd=1`). Log shows ~360 dispatches + 720 flushes / 2 s (~6 compute
  passes/frame) -> lots of render-thread work; suspected cause of the late "frame times climbing".
  Try them OFF (F1 > Rendering) and/or `operation_mode=handheld`.
- FG present count is 0 in FG-off logs; `[fg] valid=1` + "presenting the generated frame" confirm FG when on.

## PENDING / NEXT
> **SUPERSEDED — see "Tests to run (build #20)" and "CURRENT STATE" above.** The list below is from build #16.
1. ~~Rebuild the AppImage after the band fix~~ — **DONE (build #16 succeeded, 2026-10-28)**. The current
   `build/dist/LAS Recomp-v0.1.10-linux-x86_64.AppImage` includes the decoupling patch + band fix.
2. User launches the AppImage → click **"Compile now"** once (generator changed).
3. Test: `LARECOMP_FRAMEGEN=1 ~/ZA-Recomp.sh` (ini: `resolution=1x`, `temporal_aa=game`, `dynamic_res=off`).
   Compare with/without `LARECOMP_FRAME_DT=0` for the slow-motion fix; check FG smoothness/sharpness
   (`LARECOMP_FG_STRENGTH`, `LARECOMP_FG_THRESH`).
4. If good: make FG + strength a proper **F1 menu / ini setting** (`framegen = 1`). — **DONE (build #19, below)**
5. Optional: if the in-game HUD (not the ImGui overlay) ever warps under FG, composite the game's own HUD
   onto generated frames (needs it kept separate — a larger change).

## Build #19 — FG + frame_dt as settings, focus lever (2026-10-28)
- Env-only knobs are now settings + F1 rows (runtime/settings.{c,h}, runtime/ui/menu.mm):
  - `framegen` (bool), `fg_strength` (0..1; the menu shows 20–100%), `fg_thresh` (px) — F1 > Video > Rendering,
    under Temporal anti-aliasing. `resolution=1x` + Dynamic off are shown as a warning when unmet.
  - `frame_dt` (bool, default **on**) — F1 > Video > "Constant game speed", under Frame rate.
  - `LARECOMP_FRAMEGEN` / `LARECOMP_FRAME_DT` / `LARECOMP_FG_STRENGTH` / `LARECOMP_FG_THRESH` still work: they
    override the settings **at load** (so the menu shows/edits what they set), and are not read afterwards.
    Runtime reads g_settings (`exec.c vk_fg_on`/`vk_fg_build`, `hle/framepace.c rt_za_frame_dt`).
- `[fg]` per-frame log spam removed (both "frame N: valid=1" and "presenting the generated frame"): set
  `LARECOMP_FG_LOG=1` to get it back.
- New env lever for the post-alt-tab sag: `LARECOMP_FOCUS_REBUILD=1` rebuilds the swapchain on the first frame back
  with the focus (device.c vk_frame_begin). **Off by default.**
- AppImage rebuilt (build #19). Smoke run (hidden, scratch save) boots at **60.0 fps**, settings load, no [fg] spam.

## Dips: the heavy #57 options are NOT the cause (log evidence)
- The 17:30 FG-off run's `larecomp.ini` already had `ambient_occlusion=0`, `indirect_light=0`, `water_reflections=0`,
  `temporal_aa=game`, `resolution=1x` — and it still dipped at **t≈157–169** (60 → 56.9 → 31.5 → 19.9 → ~58). So the
  ~360 dispatches / 720 flushes per 2 s are the game's own (or the MV build), not the #57 effects. No need to retest
  with them off; the earlier "enabled" reading predates the 17:25 ini.
- The dip lines up with `[window] focus lost` (t=160.3); the recovery with `[window] focus gained` (t=173.0). The
  `[slow]` frames just before (9519–9565) are `vsync/pace wait` (the pacer) + `full texture hash` + `queue wait`
  (the render thread waiting for the recorder, which is stuck in the present). Focus was also lost for ~110 s earlier
  (t=38→149) **with a clean 60**, so it is the *transition*, not "unfocused" per se.
- Separate hard stall: `vkAcquireNextImageKHR … 1391 ms` at frame 872 (startup) — a compositor swapchain-acquire stall.
- Next levers to test: `LARECOMP_FOCUS_REBUILD=1`; and staggering the periodic texture full-hash recheck
  (textures.c `frame - b->full_frame >= recheck`, every 600 frames) so the 10 s safety-net bursts don't cluster.

## Tests to run (build #19)
1. FG **off** (open F1 > Video, ensure Frame generation is Off; remove `LARECOMP_FRAMEGEN` from the launcher so the
   menu controls it), fullscreen + focused, no alt-tab, 5 min → does it hold 60?
2. FG off, alt-tab away ~10 s and back a few times, with and without `LARECOMP_FOCUS_REBUILD=1` → does it recover?
3. FG **on** (`framegen=1` in F1 or `LARECOMP_FRAMEGEN=1`), alt-tab → does build #18's "present once while unfocused"
   hold 60? (`LARECOMP_FG_LOG=1` shows `[fg] presenting the generated frame` again.)
4. FG on, check the F1 rows (strength / threshold) change the picture live.

## Build #20 — the decouple's real bug, and FG's pure-overhead paths (2026-10-28)
### frame_dt: it was fed the wrong duration (this is why 40 fps felt "under water")
- Disassembled the game's frame-clock (2.0.2 `f_7001b29660`) with capstone this session (tools/nso.py + lz4, base
  0x7000000000, offsets 0x1b29660..):
  - `ldr x8,[x1,#8]` -> the strict bands -> **w8 = vsyncs − 1** (0 at a 16.67 ms frame).
  - `ldrsb w20,[frameclock+0x20]` -> configured (rt_za_frame_cfg makes it **0** for 60 fps).
  - `w20 = max(chosen, configured)`; the subsystems then get `update(i, w20)` — and **0 means one vsync** (the game's
    own measured value is 0 at 60 fps, yet the world moves; the 30 fps mode's configured 1 gives exactly two steps).
- MEASURED with the new `LARECOMP_FRAME_DT_LOG=1`: the value the game passes in (`[x1,#8]`) is its **CPU-busy time,
  ~8.5 ms at 60 fps**, NOT the 16.67 ms frame period. So the game's own bands read 0 every frame, and the old
  accumulator - which added that same ~8.5 ms - could never reach 2 at 40 fps. **That is why the decouple did nothing.**
- FIX (runtime/hle/framepace.c): accumulate the real wall-clock time between calls (`plat_now_ns`) and report
  `owed − 1` (the game's "vsyncs − 1" convention): 60 fps -> 0 (one step, unchanged), 40 fps -> 0,1,0,1 (1.5 = full
  speed), 30 fps -> 1 (two steps). `LARECOMP_FRAME_DT=0` still hands the game's own value back.
- VERIFIED: an A/B screenshot (same scripted walk, hidden, scratch save) with `LARECOMP_FRAME_DT_FORCE=1` (hand the
  game w20=1 => two steps) puts the character at a **clearly different point of the walk** vs. the default — so this
  value does drive the world, and the fix targets the right place. (Screenshots: /tmp/opencode/za-test/{A,B}_*.png.)
- New test levers: `LARECOMP_FRAME_DT_LOG=1` (`[framedt] call .. dt .. (game ..) -> step ..`),
  `LARECOMP_FRAME_DT_FORCE=<n>` (always hand the game band[n] = w20 n; its step is n+1).

### Frame generation: no more stalling / pure-overhead work
- The 20:05 "FG tanks" run was heavily confounded (water/AO/SSGI/upscaler/FSR2 toggled mid-run; `dynamic_res=game`),
  but its FG-specific costs were clear: a **second** `vk_frame_begin`/acquire + present per game frame, and - with
  Dynamic resolution on (the ini) - the scene size never matches the presented size, so FG produced **nothing** while
  still running the MV build + a full-res copy + two full pipeline barriers every frame.
- FIXES (runtime/nvn/vulkan/{device,exec}.c):
  - the generated frame now acquires with **timeout 0** (`vk_frame_begin_to`) and is **dropped** when no window image
    is free - it can no longer stall the render thread (a compositor that can't take two presents a refresh);
  - `vk_fg_build` returns before the copy when there are no motion vectors at the presented size (Dynamic resolution
    on / not 1x): FG goes idle instead of pure overhead, with one log line saying why;
  - `[fg]` logging: the first few lines always, the periodic ones only with `LARECOMP_FG_LOG=1`.
- **FG still requires `resolution = 1x` and `dynamic_res = off`.** With `dynamic_res = game` (as the ini had) it now
  stays idle rather than costing frames.

## Tests to run (build #20)
1. **frame_dt** (the "under water" fix): F1 > Video > Frame rate > Constant game speed **on** (default). Play where
   the frame rate dips below 60 (e.g. a heavy area) → is the game speed now correct (was ~40/60 speed)? Compare with
   Constant game speed **off**. `LARECOMP_FRAME_DT_LOG=1` shows `[framedt] dt 27.4 ms -> step 0/1` at ~36 fps.
2. **FG**: set **Internal resolution 1x** and **Dynamic resolution Off** first, then turn Frame generation on. Does it
   now hold the frame rate (a dropped generated frame is better than a stalled one)? `LARECOMP_FG_LOG=1` shows
   `[fg] ... valid=1` and `presenting the generated frame`. With Dynamic on, expect the one-time
   `[fg] no motion vectors ... frame generation is idle` instead.
3. Alt-tab (with `LARECOMP_FOCUS_REBUILD=1` if the post-refocus sag remains).

## Steam Deck bundle (build #20)
- The Deck is x86-64, so it uses the same AppImage (glibc 2.35 need; SteamOS 3.x is newer; AVX2/BMI2 = x86-64-v3:
  Zen 2 has it). `settings_on_steam_deck()`/`am_handheld()` auto-pick handheld 720p.
- **`zaimport` is Windows-only**, so the Deck's start screen cannot import NSPs: the bundle ships the already
  extracted game files (`extracted/` incl. `lasgame.so`, so no "Compile now") in a self-contained data root.
- Bundle: `build/dist/ZA-SteamDeck-linux-x86_64.zip` (unzip -> `ZA-SteamDeck/{run.sh, README.txt,
  LAS Recomp-...AppImage, data/{extracted,cache,mods,save,larecomp.ini}}`). `run.sh` sets
  `LARECOMP_DATA_ROOT`/`LARECOMP_SETTINGS` to the bundle's `data/`, the RADV fix, and
  `APPIMAGE_EXTRACT_AND_RUN=1` when there is no `/dev/fuse`. `data/larecomp.ini` is Deck-tuned
  (`operation_mode=auto`, `fps60=0`, `frame_dt=1`, `framegen=0`, effects off).

## v0.1.11 fork + Deck findings (2026-10-29)
- Ported our Linux work onto upstream **v0.1.11** (tag cff8364): branch **`linux`** = v0.1.11 + frame gen + `frame_dt`
  decouple + settings/F1 rows + FSR2 Linux build fixes; 3-way applied our 677-line delta, only `settings.h`/`menu.mm`
  conflicted (kept both, against upstream's new **Compatibility** page). Published as **PokeZ-ARecomp-Linux** on
  GitHub (`azlekayn/PokeZ-ARecomp-Linux`, branch `main`) and GitLab (`gitlab.com/azlekayn/PokeZ-ARecomp-Linux`).
- **Per-frame cost is the wall**: uncapped ~44 fps (`~22 ms`); the CPU/GPU **overlap** is the lever (44 -> 51, GPU
  30% -> 85%, F1 > Compatibility > CPU/GPU overlap = On or `LARECOMP_GPU_OVERLAP=1`). It needs the write watch, which
  the **userptr heap** (our RADV fix `LARECOMP_VK_HEAP_DMABUF=0`) turns off - upstream #181. Made the no-watch fallback
  **always copy** storage ranges (`exec.c` RS_PROBE -> RS_COPY, crash-2's torn instance count) - fixed that, **still
  device-lost after ~90 s**: the remaining race is **vertex/index**, which the vertex cache can't protect without the
  watch. So overlap on = ~51 fps **but not shippable**.
- **Frame gen**: presents the generated frame back-to-back with the game's -> gamescope coalesces (min 0.3 ms / max
  34 ms pairs) -> still 30 fps look. Spacing it half a game frame (`device.c`, `LARECOMP_FG_MID=half|<ns>`) is
  **opt-in**, off by default: on the Deck it sleeps the render thread (`~24 ms` work in a 33 ms frame) -> 30 -> 25 fps.
  Only helps where the per-frame cost leaves ~half a frame free.
- **`frame_dt` decouple**: works in the game's **30 fps mode** (dt 34 ms -> step 1 -> world 2/frame = full speed).
  In **60 fps mode** the frame clock ignores the reported duration (`[frameclock+0x21] == 0` -> takes `configured`),
  so it steps 1/60 s/frame and slow-motions below 60. Tried forcing the measured branch (`za_patches.py`, load 1 for
  the flag compare): **over-corrected (double speed) and made the game's controller flip 60/30 modes** - reverted.
  Correct fix: override the **final per-subsystem step** (`mov w2, w20` in `f_7001b29660`'s update loop), leaving the
  mode logic alone. Validate on the PC (screenshot-distance A/B) before a Deck recompile.
- **Deck stable config** (set in its `data/larecomp.ini`): `fps60=0` (30 fps Switch, decouple gives full speed),
  `gpu_overlap=off` (no crashes), `framegen=0`, `dynamic_res=off`, `resolution=1x`. Also `run.sh` keeps the RADV fix
  (`HEAP_DMABUF=0` + `GMEM_WINDOW=256`) - commenting it out black-screens/crashes the Deck.
- Slow drift seen (`30.4 -> 28.9` over 9 min) with `cached ~18000 MB host` on a 15 GB Deck (memory pressure), plus
  thermal. Separate from the above.

## Deck performance: it is LATENCY-bound, not clock/workload (2026-10-29 pm)
- Symptom (final, unambiguous): **static scene, not moving** - the game starts a run at **50-55 fps** and decays to
  **~35 fps over ~1-2 min**, then flat; **restarting resets it**. Even a trivial view ("staring at the ground") tops
  out at **~44 fps** -> a **fixed ~22 ms/frame floor**.
- Eliminated by measurement (all with the game running; `perf` + sysfs via the tmux `steamdeck` root shell):
  - **CPU clock** - governor `powersave` pinned it to 2425 MHz (max 3501); forcing `performance` -> 3475-3500 MHz ->
    34 -> 37 fps only. Reverted.
  - **GPU clock** - DPM parked at the middle state (1040 MHz, ~8-15 W); forcing
    `power_dpm_force_performance_level=high` -> 1600 MHz -> **still ~44 fps**. Reverted to `auto`.
  - **Thermal** - 65-77 C, no throttle.
  - **Resolution/effects** - `dynamic_res = game` made no difference (the GPU is not the wall).
  - **Workload** - on the static scene the draw count FELL (125135 -> 84248) while the fps fell.
  - **Host cache / RAM** - plateaued ~7-8 GB, no swap (`zram` + a 16 GB swapfile, 0 used).
- `perf record -p <pid>` on the live process (module split): **lasgame.so (the game's own recompiled code) 55.3 %**,
  lasrecomp 20 %, kernel 13.2 % (notably `hmm_range_need_fault` / `hmm_vma_walk_pmd` - HMM page-table walks for the
  amdgpu **userptr** heap, i.e. our RADV path - and `amdgpu_cs_ioctl`), libvulkan_radeon 5 %, vdso 2.1 %
  (`__vdso_clock_gettime`, largely `VK_PROF`), libc 3 %. Otherwise very flat (no hot symbol > ~2 %).
- Prof split (render thread, per frame): at 30 fps exec ~13.7 ms (draw 10.5: pre 2.6, sets 3.1, tex 2.2, key 0.5,
  dyn 0.5, vtx 0.5) + submit 3.5 ms; recorder thread busy ~8.5 ms; ~1222-1991 draws/frame, ~69-71 passes,
  ~1000-1500 descriptor-set writes, ~81-110 MB of command packets per 2 s, pipeline memo ~84 %. GPU span ~22-25 ms
  (at interval 2 this includes the vsync wait, so real GPU work is far less). With overlap ON + interval 1 the GPU
  span is ~22 ms -> the ~44-50 ceiling.
- **NEXT PROBE (already armed in the Deck's `run.sh`): `LARECOMP_GPUPROF_AT=2000,5000,10000,13000`** - leave the game
  on the static scene and compare the per-pass GPU ms at an early (fast) vs a late (slow) frame; the pass that grows
  names the decay.
- Deck `run.sh` now: RADV fix (`LARECOMP_VK_HEAP_DMABUF=0` + `LARECOMP_VK_GMEM_WINDOW=256` - load-bearing, removing it
  black-screens/crashes the Deck), `LARECOMP_VK_PROF=1`, `LARECOMP_GPUPROF_AT=...`, `LARECOMP_FRAME_DT*` unset. Deck
  `larecomp.ini`: `fps60=0`, `gpu_overlap=on` (the user enabled it - 44 -> 51, but it device-losts after ~90 s),
  `framegen=0`, `frame_dt=1`, `dynamic_res=off`, `resolution=1x`.
- **The through-line: the Deck's ~44 fps ceiling is the port's per-frame pipeline latency (~22 ms fixed), not CPU,
  not GPU, not settings.** The overlap (defer a fence) recovers 44 -> 51; making it **safe** (vertex/index snapshots,
  not just storage) is the main lever. Fix the pipeline's fixed ~6 ms + missed vsync (profile game -> render -> record
  -> present at a trivial view) and it lifts every view.

## The Deck's "decay" was the overlap flipping into series - fixed (2026-10-29 late)
- NOT latency or a gradual decay: a **step**. Caught live (GPUPROF + the fps line's `deferred` / `wait`): 54 fps until
  one ~44 ms hitch, then 31-35 for good. Z-A waits on **two fences a frame** (mid-frame A, and B right after its
  present); the overlap deferred by one *fence*, so B waited for A - the same frame. Fine while the GPU had A done by
  then; after one slow frame never again: `deferred` 2/frame -> 1/frame, GPU wait 13 -> ~700 ms per 2 s, the GPU
  idling while the CPU built (a GPUPROF row "5 ms" on a 93-draw depth pass was that idle gap: a row runs to the next
  submission's first timestamp). The GPU clock parking at 1040 MHz afterwards was an effect, not the cause.
- **Fix (exec.c `overlap_defer`): one frame late, not one fence.** A wait is for the newest game-frame end older than
  the fence (`present_seq`, set by `vk_overlap_frame_end` at the end of the present item - after frame generation's
  frame: B resolves to the item's last submission), plus the fence two waits back as the bound where nothing presents
  (loading). Verified on the Deck: 54-55 fps held through 90 ms load hitches and F1 round trips; unit test
  `tools/test_vulkan_overlap.py` (Z-A's two fences, presents before/after the wait, frame generation, loading).
- Recording *every* present as a frame end broke it again with frame generation on (46 -> 27): fixed as above.
- Deck at 1x / dynamic res off, overlap on: ~50 fps steady. Overlap's own device-lost after ~90 s (vertex/index race)
  was NOT seen in ~6 min runs today, but isn't fixed either.
- `auto` DPM keeps the GPU at 1040 MHz even 99 % busy at 7-8 W of 15: forcing `high` (1600) = +22 % with overlap off.
- GPU page fault at GPU VA **0x80010c800000** (CB/DB write, permission fault) -> ring timeout -> MODE2 reset: same
  address on the old build with overlap off and on today's; clustered after earlier resets, gone after a reboot
  (several clean 2-6 min runs). The process is killed during the hang before it logs (line-buffered: nothing lost),
  so `LARECOMP_VK_DEVLOST_TRACE` never prints. Unsolved.

## Also this session
- **Step-override** (za_patches.py `_za_frame_step`, 2.0.2 only, asserts `mov w2, w20` at 0x7001b29778; framepace.c
  `rt_za_frame_step`): subsystems get max(the game's step, the accumulator's) - 60 fps mode no longer slow-motions
  below 60. Changes GAMELIB_ID (players recompile). `LARECOMP_FRAME_STEP=0` off. Deck's game code:
  `aa027aae6a7f3adc-B1F12FD919EAE86A` (compiled on the PC via `LARECOMP_LAUNCHER_EXTRACT=` against a scratch data
  root with the old `.gamebuild` cache: 2 objects, 110 s).
- **Mods tab** (menu.mm id 11): the Z-A Royale rows moved there from Video.
- **Select / Start held back** (input.c `combo_hold`): alone, they reach the game on release (a tap) or after 500 ms
  held; with the other one pressed meanwhile it's the F1 combo and neither reaches the game. 150 ms still leaked on the
  Deck. `LARECOMP_MENU_COMBO_HOLD=<ms>`. Confirmed by the user.
- **Frame generation** (still looks 30 at a 30 cap):
  - the generated frame is presented half a game frame after the game's, the wait on the recorder (record.c
    `present_gap_wait`; `LARECOMP_FG_MID=0` off); `[fg] spacing` log line. Measured: presents 16.7 ms apart, exactly -
    yet MangoHUD's graph stays a 0.3 / 34 ms zig-zag: gamescope shows a frame when its GPU work is done, and the game
    frame (~21 ms of GPU at 30 fps) finishes a moment before the generated blit queued behind it.
  - `VK_GOOGLE_display_timing` ("not before" times): gamescope's layer lists it, RADV doesn't; enabled, the first
    submission after the start screen crashed inside RADV every launch. Now opt-in (`LARECOMP_DISPLAY_TIMING=1`).
  - Reviewer fixes: the overlay's secondary buffer was ONE_TIME_SUBMIT yet replayed on the generated frame, and its
    pool reset under the replay's pending submission; motion vectors were tagged as this frame's before an early return.
- device.c's device extension array was 16 with up to ~23 appends: now 32.
- Deck `run.sh` extras now: `LARECOMP_VK_DEVLOST_TRACE=1`, `LARECOMP_NO_DISPLAY_TIMING=1` (both removable). Deck ini:
  `gpu_overlap = on`, `fps60 = 1`, `dynamic_res = off`, `resolution = 1x`. Old AppImages / game code in
  `ZA-SteamDeck/.backup-20261029/`. Pushing builds: `make appimage` (podman wrapper), serve `build/dist` over HTTP on the
  LAN, `curl` from the Deck's root shell (tmux `steamdeck`), check sha256, swap.

## Open items / next session
1. **Frame generation: presentation solved, content isn't (2026-10-29 evening).**
   - DONE: the generated frame is drawn into an image of its own (fgq) and presented by a thread from a compute queue
     (the Deck has one that can present: `[vk] frame generation's queue: a compute queue...`), half a measured game
     frame after the game frame's GPU work finished. Verified: `[fg] queue:` 18 ms after (of 33.4), ~0 dropped,
     MangoHUD's graph flat (no more 0.3 / 34 ms zig-zag). Swapchain lock `rec_sc_lock` (render thread's acquire in 2 ms
     slices, recorder's present, the thread). `LARECOMP_FG_QUEUE=0` off.
   - The generated frame is now extrapolated (current frame + half its motion) - the midpoint of the last two shown
     *after* the current one stepped back half a frame each time. `LARECOMP_FG_MODE=interp` = old.
   - STILL looks 30: `LARECOMP_FG_DUMP=<frame>` (-> `<data root>/build/fg_{cur,gen,mv}.png`; create build/) showed
     fg_gen.png byte-identical to fg_cur.png and max |mv| 0.2 px while the camera turned = the TAA jitter only. The
     matrix mv_note_draw takes (G-buffer pass, `st.ubuf[0][2]` = cb5) has no camera rotation in Z-A (projection only?).
     NEXT: log the G-buffer pass's constant-buffer matrices over frames while the camera turns, find the one that moves
     (view-projection). This is FSR 2's input too - it has never had camera motion in Z-A.
   - Also: the scene occupies the top-left 854x480 of 1280x720 (see 5.) - the mv pass must use that region.
2. **Royale randomizer probably never fires**: fs.c `za_royale_read` looks for `trdata_array.bin` among loose RomFS
   files, but the game keeps it Oodle-compressed inside `arc/data.trpfs` (FNV-1a path hash -> pack entry, as
   tools/za/trdata_extract.py reads it). Confirm (`[royalerandom]` never logs with the toggle on), then hook the pack read.
3. Review branch `worktree-agent-ad664600080292dcf` (settings clamp for za_royale_delta, duplicate `temporal_aa` save
   line, Python tidy-ups; its two frame-gen fixes are already in). Its notes: `_za_frame_measured` is dead code
   (za_patches.py - touching it forces a recompile); fs.c royale read past EOF zero-fills.
4. The 0x80010c800000 GPU page fault (above), and the overlap's vertex/index race.
5. **Handheld renders at 480p, HUD included** (the UI blur): Z-A handheld keeps 1920x1080 targets and draws 1280x720
   (`[present] ... crop 0,0 1280x720`); output_scale.c assumes handheld targets are 1280x720 and "1x on an integrated
   GPU" = 720 of 1080 lines -> scale 0.667 -> 854x480. Options: handheld 1x = 720p (`h = 1` when hh: ~2.25x the
   pixels, the Deck's GPU is near its limit at 30); or the real fix, the HUD at the window's resolution and the 3D
   scene low: the shown image (scene copied in, HUD drawn over: the FSR 2 / HDR boundary) gets its own scale.
