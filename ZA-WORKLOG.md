# ZA Recomp (PokeZ-ARecomp) — worklog / handoff notes

Repo (working dir): `/mnt/SSD Storage/Games/Ports/Switch/PokeZ-ARecomp-pokemon-za`
Session date: 2026-10-28. Read this first in a new session.

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
- FG off does NOT fully hold 60 either: the 17:25 FG-off run held a clean 60.0 for ~2.5 min, then dipped
  (`56.9 -> 31.5 (max 96 ms) -> 59.5`) around focus events (~t=149-177). So the dip is partly general, not only FG.
- The user enabled the heavy #57 options: `ambient_occlusion=2`, `indirect_light=3`, `water_reflections=1`
  (`operation_mode=docked`, `resolution=1x`, `tex_hd=1`). Log shows ~360 dispatches + 720 flushes / 2 s (~6 compute
  passes/frame) -> lots of render-thread work; suspected cause of the late "frame times climbing".
  Try them OFF (F1 > Rendering) and/or `operation_mode=handheld`.
- FG present count is 0 in FG-off logs; `[fg] valid=1` + "presenting the generated frame" confirm FG when on.

## PENDING / NEXT
1. ~~Rebuild the AppImage after the band fix~~ — **DONE (build #16 succeeded, 2026-10-28)**. The current
   `build/dist/LAS Recomp-v0.1.10-linux-x86_64.AppImage` includes the decoupling patch + band fix.
2. User launches the AppImage → click **"Compile now"** once (generator changed).
3. Test: `LARECOMP_FRAMEGEN=1 ~/ZA-Recomp.sh` (ini: `resolution=1x`, `temporal_aa=game`, `dynamic_res=off`).
   Compare with/without `LARECOMP_FRAME_DT=0` for the slow-motion fix; check FG smoothness/sharpness
   (`LARECOMP_FG_STRENGTH`, `LARECOMP_FG_THRESH`).
4. If good: make FG + strength a proper **F1 menu / ini setting** (`framegen = 1`).
5. Optional: if the in-game HUD (not the ImGui overlay) ever warps under FG, composite the game's own HUD
   onto generated frames (needs it kept separate — a larger change).
