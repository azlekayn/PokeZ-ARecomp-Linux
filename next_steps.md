# ZA Recomp (Linux / Steam Deck): next steps

State as of 2026-10-29. Branch `linux` (tracks `gitlab/main`); both remotes (`github`, `gitlab`) are at `a957e0e`.
Push with `git push github linux:main` and `git push gitlab linux:main`. `ZA-WORKLOG.md` has the longer history.

## Uncommitted work (in the working tree, built, running on the Deck as AppImage 59aa0269)

8 files, about +131/-8. Commit these once the GPU hang below is fixed (or as a WIP now, so they aren't lost):

- **HUD split ("UI scale")**: the HUD renders at the presented size while the scene is scaled.
  - `NvnTexture.window` is set in `objects.c` `nvnWindowInitialize`.
  - `TexBackend.ui` is in `vk.h`.
  - `textures.c`: `g_ui_x`/`g_ui_y`, `renderer_set_ui_scale`, and `tex_ui()`, used in `create_image`, the
    `tex_backend` reallocation check, and `renderer_mark_gpu_owned`. `LARECOMP_UI_NATIVE=0` turns it off.
  - `device.c` computes `uy = vh / present crop height`, active only when it is above the scene scale, and logs
    `[scale] UI ...`.
  - The user said it was "much sharper"; flyouts are fine.
- **Handheld 480p fix** (`output_scale.c`): `h = mode==1 && g_gpu_integrated && !hh ? 720/1080 : mode`. In
  handheld, 1x was scaling twice (0.667 of the 720 crop gave 854x480).
- **Menu "Rendering at"** (`menu.mm`): reports the picture size from `g_present_crop`.
- **Diagnostics** (can be removed or kept behind their env vars):
  - `LARECOMP_VK_LOG_PRESENT_PASSES` (begin_pass)
  - `LARECOMP_VK_LOG_UI_SOURCES` (texture_info)
  - `LARECOMP_MV_LOG=<frame>` (`exec.c` `mv_note_draw`)

Untracked files that are **not ours** (from another session): `ZA-NEMESIS-RE.md`, `runtime/hle/za_worklog.c`,
`.claude/`. Never commit them.

## 1. GPU hang about 20 s into gameplay (top priority)

This started with the true-720p fix. Frame generation was on in every crashed run.

- **Symptoms:**
  - dmesg shows `ring gfx_0.0.0 timeout ... Process pokemon-za`, then `Ring gfx_0.0.0 reset failed`, then a MODE2
    reset.
  - The log shows `FATAL: r failed: -4 (record.c:584)` and `vkWaitSemaphores: -4`.
  - The `LARECOMP_VK_DEVLOST_TRACE` output names only the last ops the CPU queued, not the culprit.
- **Suspect:** frame gen's compute-queue path (fgq, `device.c`). At 720p the motion-vector and generated images match
  the presented size now, so more of its path runs. A race is likely between fgq's queue and the game's queue (image
  layouts, shared images, `rec_sc_lock`, `fgq_drain`).
- **Plan:**
  1. **Bisect.** Play once with frame gen off (F1, or `framegen = 0` in the ini), and once with
     `LARECOMP_FG_QUEUE=0` (frame gen on, presented from the main queue).
  2. **Run RADV's hang debugger.** Add `export RADV_DEBUG=hang` to `run.sh` for one run. It is slow; on a hang it
     writes `~/radv_dumps_*` naming the draw and shaders. If the hang does not happen under it, that points to a
     cross-queue sync race.
  3. **Check for a devcoredump.** The Deck's `gpucore-catch` loop copies
     `/sys/class/drm/card0/device/devcoredump/data` to `/tmp/gpucore-HHMMSS.txt`. It does not survive a reboot,
     so restart it after one.
  4. Fix, then commit the uncommitted work above.

## 2. Recurring GPU page fault at GPU VA 0x80010c800000

- **Symptoms:** a CB/DB (colour/depth) write gets a permission fault, then the ring times out and a MODE2 reset
  follows.
- **Seen with:** several builds, with the overlap both on and off.
- **Likely cause:** a render target bound after its memory was freed or reallocated (texture realloc / `tex_backend`
  reallocation, or gwin aliasing).
- **Next:** a devcoredump or `RADV_DEBUG=hang` run will show which target it is.

## 3. Frame-gen motion vectors are about 0 (the real frame-gen blocker)

- **Symptoms:** the largest motion vector is about 0.2 px, which is just the TAA jitter. With
  `LARECOMP_FG_DUMP`, `fg_gen.png` was byte-identical to `fg_cur.png`.
- **What `mv_note_draw` does:** it takes `st.ubuf[0][2]` (vertex cb5) from the first draw each frame that has 4+
  colour targets, and treats it as the view-projection. That is probably the wrong matrix, or a fixed-camera
  pass.
- **Better lead:** GTAO reads the lighting pass's fragment cb2 (`st.ubuf[1][2]`):
  - rows 12-15 are a matrix
  - row 19 is the camera position
  - rows 20-21 hold tan(fov)
- **Next:**
  1. Do a `LARECOMP_MV_LOG=1500` run, turning the camera between about 45 s and 2 min.
  2. Find the cbuf/row whose matrix changes with the camera, and switch `mv_note_draw` to it.
  3. Restrict the motion-vector pass to the content region: the scene fills only part of its targets
     (0.667 scale).

## Other open items

- **Overlap's vertex/index race:** device-lost after about 90 s; unfixed.
- **Royale randomizer probably never fires:** `fs.c` `za_royale_read` looks for a loose `trdata_array.bin`, but
  that file is inside `arc/data.trpfs`.
- **Unmerged reviewer branch** `worktree-agent-ad664600080292dcf`:
  - Has a settings clamp, a duplicate `temporal_aa` line, and Python tidy-ups. Its two frame-gen fixes are
    already applied.
  - Notes from it: `_za_frame_measured` is dead code, and the Metal backend still needs the UI scale.
- **Small flyouts** (916x172, 340x164...) are unscaled layers. They are fine at 1:1 but would stay soft at higher
  desktop scales.
- **Deck GPU clock:** auto DPM holds 1040 MHz; forcing high gives +22%.
- **Deck `run.sh` diagnostics to remove later:** `LARECOMP_VK_LOG_PRESENT_PASSES`, `LARECOMP_VK_LOG_UI_SOURCES`,
  `LARECOMP_MV_LOG`, `LARECOMP_FG_DUMP`, `LARECOMP_VK_PROF`, `LARECOMP_GPUPROF_AT`.

## Steam Deck reference

- **Host:** deck@wingdeck, 192.168.1.42. The old PC (192.168.1.128) reached it through a tmux window
  `steamdeck:0` (ssh, then sudo) and had no ssh key. A new machine needs its own way in, e.g. `ssh deck@192.168.1.42`.
- **Install:** `/run/media/deck/SD Card/Games/ZA-SteamDeck/`, containing:
  - `run.sh`
  - `LAS Recomp-v0.1.11-linux-x86_64.AppImage`
  - `data/`: `larecomp.ini`, `lasrecomp.log`, `extracted/lasgame.so` (id `aa027aae6a7f3adc-B1F12FD919EAE86A`),
    and `build/`
  - `.backup-20261029/`: old AppImages and the old `lasgame.so`
- **`run.sh` must keep** `LARECOMP_VK_HEAP_DMABUF=0` and `LARECOMP_VK_GMEM_WINDOW=256` (the RADV fix), plus
  `LARECOMP_NO_DISPLAY_TIMING=1`.
- **ini:** `gpu_overlap = on`, `fps60 = 0`, `framegen = 1`, `dynamic_res = off`, `resolution = 1x`,
  `upscaler fsr1`.
- **Build (Linux PC):** `PATH="/tmp/opencode/bin:$PATH" make appimage`. It needs the podman wrapper (SELinux) and
  takes about 5 min. The output is `build/dist/LAS Recomp-v0.1.11-linux-x86_64.AppImage`.
- **Pushing a build to the Deck:**
  1. Serve the AppImage and a `SHA256SUMS` over `python3 -m http.server`.
  2. On the Deck, check first that the game is not running (`pgrep -x pokemon-za`). Never replace the AppImage
     under a running game.
  3. curl it to `.incoming` and run `sha256sum -c`.
  4. `mv` it over the AppImage, then `chmod +x` and `chown deck:deck`.
- The process is named `pokemon-za`.
