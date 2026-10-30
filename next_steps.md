# ZA Recomp (Linux / Steam Deck): next steps

**IDENTIFIED: the crash is upstream's own open issue #22, not a new bug (2026-10-30 night, start here).** Found a
previous dev session's full transcript at `opencode/opencode.md` (2.5 MB, not ours - predates this session, kept for
reference, see its own note below) and it names the exact mechanism. Z-A skins characters with **transform feedback**
(~140 sessions/frame, `exec.c` ~line 4892 `xm_on`/`xm_lookup`/"transform feedback mirror") - draws read the skinned
output straight back as **vertex streams** (fixed-function vertex input, not a storage-buffer/SSBO read). A dedicated
"mirror" exists to make that read safe (copies the XFB output to a device-local buffer first), but it's **off by
default on AMD GPUs**, and the code comment names the reason verbatim: *"#22: an RX 9070 XT lost the device on an
invalid GPU read in exactly these skinning draws, with robust access on and submissions made one at a time"* -
upstream's own team tried hardening it (Vulkan robustness, serialized submissions) and it still crashed. Turning the
mirror off doesn't fix it either (already confirmed upstream, see §1 below: "still happens with the mirror off, and
is still being investigated") - with the mirror off, the skinned output just goes straight to the shared guest-heap
import instead, read back through `guest_direct()`'s plain vertex-buffer bind path, which (confirmed this session) has
**zero staleness/GPU-write protection** of its own outside of overlap-only machinery.

**What tonight adds that upstream's issue #22 doesn't have:** the exact crashing pipeline, decoded via
`LARECOMP_DEBUG_PIPELINE_HASH` - a **shadow-caster depth-only pass** (one position-only vertex attribute, 12-byte
stride, zero color targets, a real depth target - `shader_rec_format`'s `a=22 s=c/0 c=0,0,0,0,0,0,0,0 d=35`), and the
exact fault shape - `address read invalid 0xffff804002d6f000`, **~250 GB past the nearest tracked buffer** - which
reads as a shader dereferencing numerically-garbage data, not a tight race on otherwise-valid data. Both of tonight's
two captures hit this same pipeline, once with `gpu_overlap` off and once on - consistent with this being the
mirror-independent, overlap-independent bug the code comment already names, not anything new from this fork
(confirmed separately by two independent branch-vs-upstream reviews, see below).

**Likely the same bug's non-fatal end of the spectrum: the "flickering" symptoms reported earlier tonight.** Before
the crash, the user saw objects flickering in and out of existence (confirmed not frame generation - persisted with
it off), and later, with `gpu_overlap` explicitly on, missing objects and flickering shadows. Both happened
regardless of `gpu_overlap`'s state, same as the crash. Working inference (not confirmed with a captured log the way
the crash is): the same transform-feedback skinning readback race, when it lands on *stale-but-plausible* data
instead of a wildly-invalid address, doesn't fault - it just draws a wrong frame (degenerate/stale positions ->
a collapsed silhouette or a wrong shadow) and self-corrects next frame once the race happens to land right. One root
cause, a spectrum of severity from invisible to visibly glitchy to fatal, depending on how badly a given frame's race
lands. Worth keeping in mind if `LARECOMP_VK_XFB_MIRROR=1` (forcing the mirror on despite the AMD default) is ever
tried as an experiment - if it changes the flickering's frequency, that's corroborating evidence for this theory
without needing another crash.

**Recommendation: don't attempt a speculative fix for this tonight.** Upstream's own team has already tried the
obvious hardening (robustness extensions, serialized submissions) without success, and it's explicitly still open on
their end. The next useful step is probably reporting *this* session's specifics (the shadow-caster localization, the
precise fault-address magnitude) back to upstream issue #22, since it's more precise than what's in the issue now -
not guessing at a fix locally without their context or a way to validate it beyond "didn't crash this time."

**A note on `opencode/opencode.md`:** this file (and the `opencode/` folder it's in) is from a previous session using
a different tool (OpenCode, not Claude Code) - found tonight via `opencode session export` after the user recalled
a prior deep-dive existed. It is **not tracked by git and not created by this session** - leave it alone (don't
commit it, matches the existing untracked-files rule for `PROMPT.md`/`ZA-NEMESIS-RE.md`/`za_worklog.c`), but it's a
genuinely valuable reference: it has the `LARECOMP_GPU_OVERLAP=1` and `LARECOMP_FOCUS_REBUILD=1` experiments that
were very likely the user's two missing Steam launch-option flags (see "fps regression" note below), plus a lot of
prior Deck perf/crash investigation this file's §1/§1b/§4 sections didn't have.

---

**Our-branch-vs-upstream validation (2026-10-30, two independent reviews - start here for the night's three live
bugs below).** The user asked whether our fork's own changes (not upstream's pre-existing code) are responsible for
tonight's instability. Two independent reviews (this session's own code reading + a fresh agent with no shared
context, both diffing `linux` against the `v0.1.12` merge-base and reading `fd70be0` in full) agree:

1. **The VM-fault crash itself (the shadow-caster pipeline, `vh 9c1a6a0125797441 fh d4f2e43272162253`): likely
   pre-existing/inherited, not ours.** The GPU-write-tracking machinery a shadow pass would need
   (`vc_mark_gpu`/`vc_mark_sbufs`/`vc_page_gpu`/`vcache_lookup`) is verbatim identical to upstream's `v0.1.12` - we
   never touched it. Confirmed independently: on the Deck's actual runtime path (`gpu_overlap` off, vertex cache off
   on this integrated GPU - `vcache_lookup`'s own "integrated GPU: no bus to save" exclusion), `guest_direct()`'s
   vertex/index bind (`exec.c` ~line 5579) skips `hc_lookup` and `vcache_lookup` entirely and does a bare direct
   bind with **zero GPU-write awareness**, relying purely on barrier placement - which predates this fork and which
   this crashed through with `gpu_overlap` off, where none of today's overlap-only code even runs. Matches upstream's
   own independently-reported crash class (issues #39, #72).
2. **The `gpu_overlap`-on rendering corruption (missing objects, flickering shadows): a real, previously-unflagged
   fork-specific suspect exists, not confirmed.** `overlap_defer()` (`exec.c` ~line 700, used by `renderer_wait_seq`)
   was rewritten by this fork in `665a201` (2026-10-29) - a full algorithm change (per-fence deferral ->
   per-frame-boundary, `deferred_seq`/`present_seq` each going from one value to two) to fix a real Deck hitch. That
   code decides exactly what the CPU may touch while the GPU is still working on prior frames, and until tonight it
   had only ever been validated by 5 minutes of standing idle over SSH (no controller input possible) - never under
   real sustained movement. This is a concrete, plausible, *untested-at-scale* candidate, independent of item 1's
   shared/inherited bug and separate from today's `hc_lookup` fix.
3. **The fps regression (15 fps in a 56-58k-draw scene vs. a remembered 30/50+): still unresolved, resolves to an
   A/B nobody has run.** If `gpu_overlap` was actually active during the remembered baseline, `hc_lookup`'s per-draw
   content-hash probe (today's new code, `fd70be0`) has only ever been exercised at a few hundred draws/frame (the
   170 s local run, the idle Deck test) - scaling that unconfirmed cost to 56,000+ draws is a real, cheap-to-isolate
   suspect. If overlap was actually off that whole time (very possible - see `overlap_on()`'s unconditional AMD
   default-off below), the regression more likely belongs to the untested `v0.1.12` rebase base itself, unrelated to
   this fork's 29 commits. Nobody has done a controlled A/B (old AppImage vs. new, same spot, same confirmed
   `gpu_overlap` state) to tell these apart.
- **Next real step, not done yet:** before touching code further, confirm what `gpu_overlap` actually was (the log
  always says so at boot, `"[vk] GPU overlap on/off"`) during each of tonight's three observations (the 15 fps
  scene, the corruption, the baseline the user remembers) - most of the uncertainty above collapses once that's
  known for certain instead of inferred.

---

**State as of 2026-10-30 evening - start here.** Branch `linux` at `beb06a4`, fully committed and pushed to both
`github` and `gitlab`. `ZA-WORKLOG.md` has the longer history. This paragraph and the next supersede the "no Deck
access" paragraph below (same day, later session, real SSH access to the Deck this time) - read this first, then
§1c/§1d/§3/§4/§5 and "Other open items".

**First real Deck validation of the three runtime fixes below, 2026-10-30 evening.** Built a fresh AppImage from
`beb06a4` (`make appimage`, podman wrapper, ~5 min; version string is `v0.1.12`, GAMELIB_ID unchanged so no player
recompile needed) and pushed it to the Deck (`run.sh` updated to point at the new file). No physical presence at
the Deck - driven entirely over SSH, launched into the real gamescope session (`DISPLAY=:0` from
`/run/user/1000/gamescope-environment`, run as the `deck` user) so it's a real window, not a headless one.

- **Operational note for next time:** the launcher's title screen blocks on a mouse click and SSH has no mouse -
  pass `--play` on the command line (`./run.sh --play`) to skip straight into the game, same as
  `LARECOMP_NO_LAUNCHER=1`/`LARECOMP_INPUT` already do for scripted runs (`main.c` ~line 305).
- **5+ minutes (317 s, ~9500 frames) steady 30 fps, zero crashes, zero hangs, zero device faults, zero dmesg ring
  timeouts** - idle at the post-load overworld (no controller input was possible over SSH, so no walking/battles,
  just standing). This clears **both** of the historically fatal windows on **real Vangogh/Mesa hardware** for the
  first time: the ~20-75 s / frame 300-400 pipeline-compile hang (§1/§1b - the suspect UI shader
  `b3a5396059fd6d6c` built at frame ~240 this run, same as every past capture, and nothing bad followed) and the
  ~90 s `gpu_overlap` vertex/index device-lost race (§4, `gpu_overlap = on` + `framegen = 1` both still on in the
  ini). Clean `SIGTERM` shutdown twice in this session (once from the launcher screen, once after the 317 s run) -
  no stuck-shutdown repeat either time, though that's not a real test of §5's device-lost path (no crash happened
  to trigger it).
- **§3 frame-gen still not confirmed** - `[fg]` never logged once despite `framegen = 1` and `LARECOMP_FG_LOG=1`.
  Root cause confirmed this time (not just suspected): `ui_window_focused()` reads SDL's actual
  `SDL_WINDOW_INPUT_FOCUS` flag, and nothing sets it when the window is opened this way - gamescope doesn't hand a
  raw SSH-launched client real input focus the way it would a game Steam itself launched, so `vk_fg_build`'s focus
  gate (`exec.c` ~line 9077) never passes. **Same root blocker as the local dev-PC headless testing, just
  reproduced on the Deck too** - this one genuinely needs a human physically at the Deck (or at least a real input
  event reaching the window) to validate, not just Deck hardware.
- **Not yet measured: whether `gpu_overlap = on` is an actual fps win on the Deck**, since idle standing still
  isn't a real workload (§4's whole open question). Needs the same thing as §3: someone actually walking around.
- Session's new AppImage and a named copy of its log are on the Deck (`data/lasrecomp.log.run20-v0112-fixes-5min-
  clean-17h19`) for reference.

**Then the user showed up in person and actually played - real findings, 2026-10-30 evening continued:**

- **Found and fixed a real ini bug that was making the game feel "slow as hell": `frame_dt = 0` in
  `data/larecomp.ini`**, against the documented default of `true` (`runtime/settings.c` line 53). With it off,
  Pokémon Z-A reads its own raw per-frame duration and buckets it into fixed vsync bands (16.67/33.33/50/66.67 ms,
  `runtime/hle/framepace.c` `rt_za_frame_dt`) instead of a real wall-clock accumulator - ordinary frame-time jitter
  then constantly under-counts vsyncs (a 33.0 ms frame reads as only 1 vsync's worth of world-time instead of 2),
  so the game runs in genuine slow motion even while holding a steady 30 fps, and far worse once fps actually drops
  (16 fps in a busy town - see below). **Fixed by editing the ini directly** (`frame_dt = 0` -> `1`) rather than
  chasing a launch flag, per the "prefer the automatic/default fix" rule - `true` is already the intended default,
  this was just a stale toggle. Not yet re-tested after the fix (session moved on to the crash). This does **not**
  change the `fps60 = 0` (30 fps) target - it only stops the slow-motion that happens when the target isn't held.
  - The user also mentioned losing 2 Steam launch-option flags that had been present "every day before today" and
    couldn't recall them. Ruled out `LARECOMP_VK_HEAP_DMABUF`/`LARECOMP_VK_GMEM_WINDOW` as candidates - both were
    confirmed still active from the crash logs' own boot sequence, and `GMEM_WINDOW` auto-defaults to 256 MB on an
    integrated GPU regardless (`exec.c` `gmem_window_init`, ~line 3949) so it never needed a flag on this hardware
    anyway. Best guess is one of the two was `LARECOMP_FRAME_DT=1`, silently overriding the ini bug above every day
    until it was dropped - now moot since the ini itself is fixed. The second flag is still unidentified.
- **A real crash happened live, with real forensic value for §1/§1b.** Sequence across three launches in ~3
  minutes: (1) a clean run the user closed normally; (2) a hard crash - dmesg confirms the **same long-standing
  §1 ring-timeout hang** (`ring gfx_0.0.0 timeout`, `Ring gfx_0.0.0 reset failed` -> MODE2 reset,
  `GPU reset(4) succeeded!`, pid 40156, ~25 s into the loading-screen -> town transition, same timing/signature as
  every past capture) - **not** anything from today's fixes; (3) relaunched, and this time the user watched it
  happen: **"a few vertex explosions, then everything turned to black and white dots, then the game closed."**
  - **This is the clearest direct evidence yet for §1b's "stale/garbage buffer read" theory over a missing-barrier
    theory.** The saved log (`data/lasrecomp.log.run21-v0112-vafault-vertexcorruption-17h26-pid40743` on the Deck)
    shows the actual fault: `address read invalid 0xffff804002d6f000`, `no object ever held that page in the 5564
    kept binding events`, **`the fault past its end by 262452668 KB` (~250 GB)**. That magnitude rules out a tight
    off-by-one or a slightly-stale-but-valid previous frame's data (which a missing barrier would produce) - a
    read 250 GB past the nearest real allocation looks like a shader dereferencing a numerically-garbage value it
    found in an uninitialized or wrongly-bound buffer slot, not a synchronization race on otherwise-valid data. The
    visual symptom (exploded geometry, then static) fits: garbage vertex positions, then garbage pixel data, then
    the GPU faults on a garbage pointer a shader tried to read through.
  - **`GPU overlap` was explicitly logged as `off`** in this fault's state dump (`F1 > Rendering > Compatibility`
    line) - so this specific crash is independent of §4's overlap work, consistent with §1's existing "reproduces
    with `gpu_overlap` both on and off" finding. Not a regression from today.
  - **§5's crash-dialog fix fired correctly for the first time on a real crash** - the log has the full
    `vk_fatal_device_lost` sequence (`[vk] r: device lost`, the device fault report, the devlost trace dump) right
    before the process exited, matching the user's own description exactly (froze briefly - the new up-to-60s
    patient poll - then closed itself, no dialog, nothing to force-quit). This is the first real-world confirmation
    §5 works as intended.
  - **Sequence leading to the fault** (from the devlost trace, same log): a long run of GPU-indirect foliage/culling
    draws (`args now: <count> 0 <first> 0 <binst>`, binst in the 200-260k range, matching §1b's "grass/foliage-style
    instanced draws" pattern) -> a compute dispatch (`dispatch 244x1x1 cs ... gmem 1`) -> a small `256x256` pass ->
    the main `1920x1080` color pass -> **a depth-only pass (`color 0x0 fmt 0`, likely a shadow map)** -> the fault,
    on a **non-indirect** draw (pipeline `vh 9c1a6a0125797441 fh d4f2e43272162253`) inside that depth-only pass.
    This refines §1b's earlier "compute dispatch then the main pass" capture: here the fault is specifically in a
    *shadow/depth-only* pass that comes *after* the main color pass, not in the main pass itself. Read `begin_pass()`
    (`exec.c` ~line 1550) and `dispatch_compute()` (~line 6351) during this session: `dispatch_compute` correctly
    calls `vk_end_pass()` before dispatching and `vk_full_barrier()` after, and that `vk_end_pass()` clears `in_pass`
    so `begin_pass()`'s same-render-target early-return (`if (in_pass && !st.rt_dirty) return;`, line 1551) can't
    skip the barrier on the next draw either - the barrier placement around dispatches looks correct on inspection,
    which doesn't rule out a narrower case but didn't turn up an obvious gap this time.
  - **Not yet identified: what `vh 9c1a6a0125797441 fh d4f2e43272162253` actually is.** It wasn't a first-visit this
    session (already warm from the pipeline cache, 8991552 bytes loaded at boot), so `LARECOMP_DEBUG_PIPELINE_HASH`
    didn't catch its shape this time. Next real repro: clear `cache/shader_seen.txt` and `cache/shader_code.bin` (or
    just get a first-visit log) with `LARECOMP_DEBUG_PIPELINE_HASH=9c1a6a0125797441` set, to decode what this
    pipeline actually draws (shadow caster geometry is the obvious guess given the pass shape, not confirmed).
  - All three logs from this live session preserved on the Deck: `lasrecomp-backup-20261030-172510-38299.log` (clean
    close), `lasrecomp-backup-20261030-172538-40156.log` (the ring-timeout hang, cuts off with no FATAL - the actual
    GPU-level crash, not caught by `vk_fatal_device_lost` since the ring reset path doesn't go through it the same
    way), `lasrecomp.log.run21-v0112-vafault-vertexcorruption-17h26-pid40743` (the VA-fault crash with the full
    device-lost report, described above).

**IDENTIFIED THE CRASHING PIPELINE'S SHAPE (2026-10-30, same evening, continued) - it's a shadow-caster pass.**
After the Deck rebooted (Steam's own gamescope session kept crashing after our GPU resets - worth remembering as a
symptom: our crash doesn't just kill our process, it can take the whole Steam UI down with it), cleared
`cache/shader_seen.txt`/`shader_code.bin` (backed up, not deleted - `*.bak-pre-debughash`) and set
`LARECOMP_DEBUG_PIPELINE_HASH=9c1a6a0125797441` in `run.sh` to force a full decode on this pipeline's next first-visit
(it was `vh 9c1a6a0125797441 fh d4f2e43272162253` in both of the night's two crashes - see above). Caught it:

    G vs=9c1a6a0125797441 fs=d4f2e43272162253 ph cb8fe2e386bb326b | a=22 s=c/0 c=0,0,0,0,0,0,0,0 d=35 n=1 m=0 b=0 B= k=0 t=2 p=1

Decoded against `shader_rec_format`'s field order (`shader_prep.c` line 64): **one vertex attribute** (`a=22`, no
comma so `nattrib=1`), **one stream, 12-byte stride, not instanced** (`s=c/0`) - a bare `vec3` position and nothing
else, **zero color targets** (`c=0,0,0,0,0,0,0,0`, all `VK_FORMAT_UNDEFINED`), **a real depth target** (`d=35`), no
blending (`b=0`). Unambiguous: **this is a shadow-caster depth-only pass** (position-only vertex data, no shading
inputs needed for depth-only rendering) - Z-A's sun shadow maps per the earlier `CLAUDE.md`/code comment reference.

- **Confirmed this session: the SAME pipeline crashed twice in a row, once with `gpu_overlap off` and once with
  `gpu_overlap on`** (the ini was changed to `on` between the two crashes, chasing a separate fps regression - see
  below). That rules out an overlap-specific cause for *this* crash: whatever's wrong with the shadow-caster pass's
  vertex data is wrong in the base path shared by both overlap states, not something `hc_lookup`/`vtx_must_copy`
  introduced or missed.
- **Working theory, not yet verified:** Z-A's shadow casters very plausibly read a skinned character's
  GPU-computed position buffer straight back as the shadow pass's vertex input - the exact "skinned character
  output read straight back as vertex data" case already flagged as a known hazard in §4's `vc_mark_sbufs` comment
  (`exec.c`). If that compute-skin -> shadow-caster-vertex-read handoff has a gap in the *general* tracking path
  (not the overlap-only `hc_lookup` one), a shadow pass drawing before the skin compute's write is actually visible
  (or reading a since-recycled buffer) would produce exactly this: a position stream full of garbage, which is
  consistent with the "vertex explosions" the user watched happen live before the black-and-white static and the
  fault. **Not yet confirmed** - next step is finding where shadow-caster draws bind their vertex buffer and
  checking whether it goes through the same GPU-written-range awareness as the main pass's skinned draws do.
- Preserved as `data/lasrecomp.log.run22-overlapon-fault-17h51` on the Deck (the overlap-on crash) and the
  pipeline-detail line is in `lasrecomp-backup-20261030-175007-5092.log` line 748.

**Also found live tonight, a real fps regression independent of all of the above - not yet explained:** the user
(playing for real, in town, ~56-58k draws/frame) got 15 fps where they're used to a solid 30 with `gpu_overlap`
off and 50+ with it on. Ruled out: `frame_dt` (fixed separately, see above, doesn't affect raw fps) and the cleared
shader cache (fps stayed at 15 even once `compiles 0 pending` showed the cache had caught up). Two real suspects,
neither confirmed:
  1. **`gpu_overlap` may not have actually been running most nights either** - `overlap_on()` (`exec.c` line 130)
     caches its decision once per process (confirmed: toggling it in F1 mid-session does nothing, matching what the
     user already knew from experience) and **defaults to off unconditionally on any AMD GPU** regardless of the
     `auto` setting (added in v0.1.10 after a different AMD-only crash, issues #39/#54 - `read invalid
     0xbeefdeadc000`). The ini had `gpu_overlap = auto` tonight, not `on` as `next_steps.md`'s own "Steam Deck
     reference" section documents - if the user's daily setup normally forces it via a launch flag
     (`LARECOMP_GPU_OVERLAP=1`, possibly one of the two missing flags from earlier tonight), losing that flag would
     silently fall back to off on this AMD GPU, which would track with "better with overlap explicitly set to on"
     (confirmed better fps when the ini was changed to `on` directly, see above) - but overlap being on introduced
     new symptoms (next bullet), so this isn't a clean win yet.
  2. **The v0.1.12 upstream rebase itself (§1c/§1d) has never been tested on the Deck with real heavy content until
     tonight** - only idle/light content locally and in this session's own idle Deck test earlier. It's entirely
     possible upstream's own changes between the old base and v0.1.12 cost real fps in a busy scene, independent of
     any of this session's three fixes. Not isolated from the overlap question above - needs a proper A/B (old
     AppImage vs new, same spot, same settings) to separate "missing launch flag" from "the rebase is slower here"
     from "today's fixes cost more in heavy scenes than the 170 s/5 min tests ever exercised."
- **With `gpu_overlap = on`, the user also reported real rendering corruption under sustained real play**: missing
  objects and flickering shadows, before the second shadow-caster crash above. Given the SAME pipeline crashed with
  overlap both on and off, this specific corruption may be the live, non-fatal version of the same underlying bug
  rather than a new overlap-specific one - but that's not confirmed either, and it was *worse* (visually) with
  overlap on. **Recommendation: leave `gpu_overlap = auto` (effectively off on this AMD GPU) until the shadow-caster
  theory above is actually run down** - the rendering corruption is a worse problem than lower fps, and forcing
  overlap back on to chase the fps regression risks making a known-bad situation more visible, not fixing it.

---

Previous paragraph (same day, earlier session, dev PC only) for context:

Session was entirely on the dev PC (KnightWing, RX 7800 XT) - no Deck access that session (it's at home, the user
was at work). Three real fixes landed, all local-only validated at the time:

1. **§3 - Frame-gen motion vectors fixed.** The root cause (reading the wrong constant buffer) is fixed and
   validated (camera data now correctly tracks movement, cross-checked against GTAO's trusted extraction). **Not**
   validated: the actual interpolated-frame pipeline (`vk_fg_build`) never ran even once locally - it needs a
   really-focused window, which neither `LARECOMP_HIDDEN=1` nor a real window opened headlessly ever got.
2. **§4 - GPU overlap's vertex/index device-lost race fixed**, via a new content-hash cache (`hc_lookup`) for when
   the heap import doesn't start at offset 0 (`gmem_lo != 0`, which `LARECOMP_VK_GMEM_WINDOW` - required on the Deck
   - always causes). 170 s local runs with overlap forced on are clean and fast (steady 60 fps, bandwidth back to
   baseline).
3. **§5 - The "stuck after a crash, needs SIGKILL" bug fixed** (skip the blocking crash dialog once a device-lost
   GPU is confirmed, poll longer first since a real driver reset isn't instant). Logically sound, cross-checked
   against a sibling project's identical fix for the identical symptom, but the device-lost path itself can't be
   tested without a real hang.

Also: tried and **disproved** a theory that the Deck's documented slow fps drift was caused by the vertex cache
being disabled (bandwidth) - see "Other open items" below. Don't re-investigate that angle without new evidence.

Two new standing rules from the user that session (now in `CLAUDE.md` too): prefer an automatic fix over a new
`LARECOMP_*` env flag, and commit + push every major change rather than letting it sit in the working tree.

Untracked files that are **not ours** (from another session, still untouched): `PROMPT.md`, `ZA-NEMESIS-RE.md`,
`runtime/hle/za_worklog.c`. Never commit them. (A related mod idea the user is tracking separately, not part of
this repo's code: a "Nemesis system" for trainer AI - see this session's memory, not here.)

## 1. GPU hang about 20-60 s into a run (top priority)

This started with the true-720p fix. **2026-10-29 evening session: frame generation is NOT required** - ruled out
by a clean single-instance repro with `framegen = 0`. Also reproduces with `gpu_overlap = off`. See findings below;
the frame-gen/overlap suspicions from the morning session are superseded.

- **Symptoms:**
  - dmesg shows `ring gfx_0.0.0 timeout ... Process pokemon-za`, then either `Ring gfx_0.0.0 reset succeeded` (game
    sometimes survives, see below) or `reset failed` -> MODE2 reset (`GPU reset(N) succeeded!`, `[drm] device
    wedged, but no recovery needed`).
  - The log shows `FATAL: r failed: -4 (record.c:584)` (submit_run's `VK_CHECK` after `vkQueueSubmit2`) and/or
    `FATAL: vkGetSemaphoreCounterValue(...) failed: -4 (exec.c:166)` (`vk_completed_seq`, called from
    `vk_collect_dead`). **Both are downstream symptoms** - the device is already lost by the time these fire; don't
    chase them as the root cause. A `[vk] device fault:` line (device.c) logs just before them when it happens.
  - Before that: `[hang] no new frame for 20 s (frame N): the game looks stuck` (the runtime's own watchdog) with a
    dump of every guest thread's futex/condvar state (`arb-lock`/`cv-wait` lines) - this looks like normal
    background-thread churn still running while the render path is stuck on the dead GPU, not itself the cause.
  - Hit at a fairly **fixed early frame (~300-400)**, reproduced **before any scripted movement started** (our
    input script's first walk command is at frame 1020) - so it is not tied to player input/movement, more likely
    the loading-screen -> overworld transition or an early fixed intro/cutscene pass.
  - `LARECOMP_VK_DEVLOST_TRACE` output names only the last ops the CPU queued, not the culprit.
  - **Secondary bug found:** after the crash, the process can get stuck *again* after printing
    `[vk] pipeline cache: saved ...` (clean-shutdown path) - alive at ~15% CPU, no new log lines, needs `SIGKILL`
    (`SIGTERM` does nothing). Likely a cleanup call (`vkDeviceWaitIdle`-style) spinning/polling a device that's
    already lost instead of bailing out on `VK_ERROR_DEVICE_LOST`.
- **RADV_DEBUG=hang evidence (two captures, `/home/deck/radv_dumps_<pid>_<timestamp>/`):**
  1. First capture (launched as the `deck` user): `registers.log` showed `CP_CPF_STALLED_STAT1`:
     `RING_FETCHING_DATA=1`, `INDR1_FETCHING_DATA=1` - the command processor stuck mid-fetch of a command/indirect
     buffer. `vm_fault.log` was empty (no reported fault - a true stuck read). `umr_waves.log` failed outright
     (needs `debugfs`, which the `deck` user doesn't have).
  2. Second capture (relaunched as **root**, same repro): got the full data. **`vm_fault.log` showed a real
     permission fault**: `CLIENT_ID: SQC (data)`, failing VM page `0xf7d4fefd0000` - **not** in the guest heap's
     address family (`0x8000...`/`0x8001...` everywhere else in this codebase) and **not a tracked buffer at all**
     (`VA not found!` in the address-binding report). Reads like a shader using a garbage/stale value as a GPU
     pointer rather than a legitimate freed-buffer reuse.
  - `bo_history.log` both times: continuous small-buffer (16-288 KB) alloc/destroy churn right up to the hang, no
    gap - consistent with per-frame descriptor/uniform/storage recycling under contention.
  - `umr`'s database is incomplete for Van Gogh (lots of `[BUG]: reg [...] not found on asic [vangogh]`) - wave-level
    shader identification did not come through even as root. Might need a newer `umr`/`amdgpu` on the Deck, or a
    different capture tool.
- **Working theory:** `[plat] page write tracking off: the guest heap is a userptr import` (always logged on the
  Deck, because of the required `LARECOMP_VK_HEAP_DMABUF=0` RADV fix) - this doesn't just affect `gpu_overlap`
  (which still logs "forced without page write tracking: the hash probe instead of the write watch" and stays on
  when the ini asks for it); the same gap affects the general in-place storage-range protection (`exec.c`
  `RS_PROBE`/`RS_COPY`, around line 3389) and the vertex cache. Any of these falling back to the weaker hash-probe
  (or missing a case it doesn't probe) could produce exactly this class of stale/garbage GPU read. This is a
  broader, pre-existing hazard from the RADV workaround, not something `gpu_overlap` introduced by itself.
- **Ruled out this session (each its own clean single-instance repro, same `record.c:584` / device-lost signature,
  same ~20-75 s / ~frame 300-400 timing every time):**
  - Frame generation (`framegen = 0`).
  - `gpu_overlap` (`gpu_overlap = off`).
  - GTAO / SSGI / water reflections (`ambient_occlusion = indirect_light = water_reflections = 0`).
  - Texture prefetch (`LARECOMP_TEX_PREFETCH=0`).
  - **The write-watch theory itself** (see below) - the single biggest result of the session.
  - Player input/movement: hangs before the scripted walk even starts.
  - Dual-instance contention: first repro was confounded by two GPU clients: all repros above are single-instance.
- **Write-watch theory DISPROVED:** temporarily removed the Deck's RADV fix (`LARECOMP_VK_HEAP_DMABUF` /
  `LARECOMP_VK_GMEM_WINDOW`, commented out in `run.sh` for one test only, restored after) so the guest heap goes back
  to a real dma-buf import with OS page-write tracking available (confirmed: no more "page write tracking off: ...
  userptr import" log line). **The hang still happened** (ring timeout, same crash signature, ~21 s in - this run
  also had the black-world bug as expected, which doesn't matter for this test). So the missing write-watch
  (§1's leading theory earlier in this session) is **not** the cause of this particular hang. It may still be a real,
  separate bug (the `gpu_overlap` vertex/index race documented elsewhere in this file is probably still real), just
  not this one.
- **New lead: a freshly-compiled pipeline/shader, not a buffer race.** The `LARECOMP_TEX_PREFETCH=0` run's log shows
  a tight cluster of `[shader] first visit N: pipeline <hash> <hash>` lines (brand-new pipeline compiles, not cache
  hits) immediately before the hang, including the **same pipeline hash recompiled three times with different
  second-hash variants** in a few lines (`b3a5396059fd6d6c`, visits 46/50/51). This fits every observation so far:
  it is independent of every feature toggle tried (none of them control whether THESE specific pipelines get
  compiled), and it naturally happens at a fixed early point (the first time the loading-screen/early-UI shaders are
  used, before they're warm in `cache/shader_seen.txt` / `shader_code.bin` - see `shader_prep.c`/`shader_seen.c`).
  Worth checking whether repeated crashes are preventing the shader cache from ever fully warming (every one of this
  session's crashed runs needed `SIGKILL`, which may skip the normal `shader_seen.txt` save) - if so, a clean full
  playthrough once might stop reproducing it, which would itself be a strong clue (a specific one-time shader
  compile is the trigger, not a shader that's bad on every use).
- **Secondary bug (independent of the above):** after any of these crashes, the process can get stuck *again* right
  after printing `[vk] pipeline cache: saved ...` (the clean-shutdown path) - alive at ~15% CPU, no new log lines for
  5+ s, needs `SIGKILL` (`SIGTERM` does nothing). Likely a device-lost cleanup call spinning/polling instead of
  bailing out on `VK_ERROR_DEVICE_LOST`. Worth its own fix regardless of the GPU hang's root cause, since it turns
  every crash into a wedged process on the Deck.
- **Mesa note:** this Deck's driver is `Mesa 26.2.0-devel (git-5c65d4fc31)` - a development snapshot, not necessarily
  a stable release (may just be SteamOS's own branch; not confirmed either way). Not chased further this session.
- **FOUND THE PIPELINE (2026-10-29 night).** Added `LARECOMP_DEBUG_PIPELINE_HASH=<hex vs/fs hash>` (`exec.c`, next to
  the `shader_first_visit("pipeline", ...)` call, ~line 6736): on a matching first-visit pipeline it logs the full
  `shader_rec_format` record. Built (runtime-only, no `GAMELIB_ID` change - no Deck recompile needed) and pushed.
  Ran with `LARECOMP_DEBUG_PIPELINE_HASH=b3a5396059fd6d6c` (the vertex hash seen 3x in every earlier capture):
  - All 3 first-visit variants at **frame 367** decode to the same shape, just a different fragment shader each
    time: `G vs=b3a5396059fd6d6c fs=<4c2e8481137283ea|78d53b502c665b45|2c2e27deec6d86ed> f=0 a=16 s=8/0 c=25 d=0 n=1
    m=f b=1 B=0005060506010100 k=0 t=2 p=1`. That's a **plain 2D UI quad**: one vertex attribute, one 8-byte-stride
    stream, no depth, alpha blending on, render target format `0x25` = **`VK_FORMAT_R8G8B8A8_UNORM`** (matches the
    `fmt 0x25` seen in `[vk] UI source: ...` lines elsewhere in the same log) - three UI elements sharing a vertex
    shader with three different fragment-shader variants, built back-to-back.
  - **~30 frames later** (frame 397/398): `[slow] frame 398 161 ms ... gpu wait 155 ms` /
    `[hitch] frame 397: 180 ms ... GPU waits 155.0`. That is the last thing the log shows - **the process was killed
    outright with no FATAL/crash line this time** (matches the already-documented "killed during the hang before it
    logs" behaviour). dmesg: ring timeout -> `GPU reset(19) succeeded!` right after.
  - So: a fresh UI pipeline (vertex shader `b3a5396059fd6d6c`, one of its 3 fragment variants) is built, and within
    half a second the GPU stalls for 155 ms and then the ring times out. This is consistent across every earlier
    capture (RADV_DEBUG=hang's register dump, the VM-fault capture) and independent of every setting ruled out
    above - **this specific shader/pipeline is the leading suspect for the hang itself**, not a buffer-lifetime race.
- **IDENTIFIED THE UI ELEMENT (2026-10-29 night, same session).** Reran with `LARECOMP_VK_LOG_UI_SOURCES=1` added
  alongside `LARECOMP_DEBUG_PIPELINE_HASH`. Right before the target pipeline's first visit (same frame, 366), the log
  shows a tight cluster of small-rectangle UI panels going up as `[vk] UI source: ...]` entries - `340x164`,
  `276x100`, `556x164`, `492x100`, `828x188`, `764x124`, all format `0x25`, all `NOT SCALED` - six similarly-shaped
  panels in a row, right after `[launcher] team: 6 in the party` is logged at boot. That is almost certainly **the
  game's own loading-screen team-roster display (the 6 Pokémon icon/info plates shown while the world loads)**, and
  the target pipeline (`vh b3a5396059fd6d6c`, 3 fragment variants, the same plain textured-quad shape as those
  panels: `a=16 s=8/0 c=25 d=0 ... b=1`) is one of that roster's elements, built in the same pipeline-creation batch.
  (This run also showed two separate ~160 ms GPU-wait stalls, frames 377/378 *and* 393/394, before the process was
  finally killed around frame 406+ - so a stall doesn't necessarily kill it immediately; something cumulative across
  stalls likely triggers the eventual ring timeout.)
- **CORRECTION, same night: it is not one specific UI shader.** Extended `LARECOMP_VK_DEVLOST_TRACE`'s draw trace to
  also print each draw's content hash (`vh`/`fh`, the same ones `shader_first_visit`/`LARECOMP_DEBUG_PIPELINE_HASH`
  use - `exec.c`, `dl_note("draw vs ... fs ... vh ... fh ...")`, gated by a new `dl_active()` helper so the extra
  `shader_stage_hash` work only happens when the trace is on). Built, pushed, reran. **This run's devlost trace shows
  a completely different draw sequence than the UI-flyout one**: a long run of GPU-indirect draws (Z-A's culling
  compute writing instance counts - `exec.c` ~line 7333: "the GPU fills in parts of the parameters: the instance
  count, Pokémon Z-A's culling compute" - grass/foliage-style instanced draws, `args now: <count> <inst> <first>
  <base> <binst>` with large binst values), then a compute `dispatch`, then the main `1920x1080 depth-buffered` world
  pass. No UI panels this time, and a different pipeline hash entirely (`fb9f69aa13db58a1`/`5a70f0599d0b1105`, not
  `b3a5396059fd6d6c`).
  - So the earlier "flyout" identification was real for *that* capture, but isn't *the* cause - different runs die on
    different content. The actual common thread across every capture so far is **timing** (the loading-screen ->
    world transition, always early, always some mix of fresh pipeline compiles / heavy texture uploads / GPU-indirect
    draws), not one broken shader.
  - Read `vk_full_barrier` (`exec.c` line 454: `ALL_COMMANDS -> ALL_COMMANDS`, inserted at every render-pass boundary
    via `begin_pass()`, line 1570) - looks correctly conservative for the common case, and Vulkan doesn't allow a
    `vkCmdDispatch` mid-render-pass anyway, so a missing compute-write -> indirect-draw-read barrier isn't obviously
    confirmed just by reading the code. Could still be a narrower case (e.g. a compute dispatch issued through a path
    that skips `begin_pass()`'s barrier) - unverified.
  - **`VK_LAYER_KHRONOS_validation` is on the Deck** (bundled inside Steam's own runtime -
    `~/.local/share/Steam/{ubuntu12_64,steamrt64}/libVkLayer_khronos_validation.so`) **but has no manifest anywhere**
    on the system, so the Vulkan loader can't discover it via `VK_INSTANCE_LAYERS` without hand-writing one and
    pointing `VK_LAYER_PATH` at it. Not attempted this session (a bit of a side quest on its own); doing this and
    turning on GPU-assisted / synchronization validation for one run is probably the single most direct way to catch
    a missing barrier if that's the real cause, instead of more manual log correlation.
- **Plan:**
  1. Write a `VkLayer_khronos_validation.json` manifest next to the existing `.so` (or anywhere, with `VK_LAYER_PATH`
     pointed at it) and run once with `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` and synchronization validation
     enabled (`VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT`, or a
     `vk_layer_settings.txt`) - if there's a missing barrier anywhere in the hot path, this should name the exact two
     commands.
  2. Failing that, keep using `LARECOMP_DEBUG_PIPELINE_HASH` + the now-hash-annotated `LARECOMP_VK_DEVLOST_TRACE` on
     more captures to build up a corpus of "what was drawing when it died" - if a pattern emerges (e.g. always a
     compute dispatch shortly before, or always a specific render-target format) that's useful even without
     validation layers.
  3. Bind `LARECOMP_SYNC_SHADERS=1` (existing env var per `exec.c`) for one run to see if forcing synchronous shader
     compilation changes the failure point.
  4. Try a full, uninterrupted playthrough past this point (clean exit, not `SIGKILL`) to see if `shader_seen.txt` /
     `shader_code.bin` getting fully warmed makes the hang stop reproducing on a later run.
  5. Get `umr` working for Van Gogh (lots of `[BUG]: reg [...] not found on asic [vangogh]` even as root) or find an
     alternative wave-inspection tool.
  6. Fix the secondary shutdown-hang bug regardless (see above) - reproduced again this session (process alive at
     ~14% CPU, no progress, needed `SIGKILL`).
  7. Fix the main hang, then commit the uncommitted work from the morning session (above).
- Both `LARECOMP_DEBUG_PIPELINE_HASH` and the hash-annotated `LARECOMP_VK_DEVLOST_TRACE` stay in the tree (cheap,
  opt-in, matches house style) for the next capture.

## 1b. The validation-layer run and the resource-recycling theory (2026-10-29 night, continued)

Got a second opinion (an independent review of all the above) that pushed back on "missing compute->indirect-draw
barrier": a shader dereferencing an address that isn't in any tracked buffer's family at all (`0xf7d4fefd0000`, "VA
not found") fits **a resource recycled while the GPU might still reference it** better than a GPU-side ordering gap -
and specifically a **cross-submission** lifetime bug (frame N+1 recycling a descriptor set / vertex-cache slot /
command-buffer region while frame N's GPU work is still in flight), not something `vkCmdPipelineBarrier` inside one
submission would even address. Two things followed from that:

- **Hand-wrote a Vulkan loader manifest** for `VK_LAYER_KHRONOS_validation` (present on the Deck inside Steam's own
  runtime - `~/.local/share/Steam/ubuntu12_64/libVkLayer_khronos_validation.so` - but with no manifest anywhere, so
  undiscoverable via `VK_INSTANCE_LAYERS` without one). `vulkaninfo` confirmed the loader picks it up. Ran the actual
  game with synchronization validation only (core/API-param/object-lifetime/thread-safety checks disabled via
  `vk_layer_settings.txt` to cut noise/overhead).
  - It DID catch a real, repeated hazard: `SYNC-HAZARD-READ-AFTER-WRITE` - a depth image is sampled as a
    `COMBINED_IMAGE_SAMPLER` (binding #32) right after being written as a depth/stencil attachment, `write_barriers:
    0`; and the reverse, `SYNC-HAZARD-WRITE-AFTER-READ` - the same depth image is written again (at
    `vkCmdEndRendering`'s store) right after a shader sampled it, `read_barriers: 0`. ~3600 of each, though - this
    happens **every frame**, not once, so it's very likely a pre-existing, mostly-benign pattern (this engine
    evidently survives thousands of these in every normal run) rather than the fatal trigger. Worth fixing on its own
    merits (it's a real spec violation and could be the "0x80010c800000" CB/DB fault in section 2 below, or cause
    subtle visual bugs), but didn't look like *the* hang.
  - The run itself became impractical to finish: sync validation's per-resource tracking overhead grew the longer the
    engine ran (this engine does thousands of draws/sets per frame), and the process ended up stuck at ~50% CPU with
    zero new log output for 2+ minutes, far past where it would normally have hung or survived. Never reached a
    verdict on whether the *real* bug also reproduces under validation; killed it rather than wait indefinitely.
- **Tested the cross-submission-recycling theory directly** with the three existing "kill switches" named in
  CLAUDE.md's bisect list, one at a time, each a clean single-instance run: `LARECOMP_VK_NO_SET_REUSE=1`,
  `LARECOMP_VK_NO_KEY_MEMO=1`, `LARECOMP_VK_NO_SLOT_MEMO=1`. **All three still hang**, same `record.c:584` /
  ring-timeout signature, same ~25-45 s window. So it is not descriptor-set reuse, pipeline/key memoization, or
  whatever "slot memo" covers, specifically - at least not alone. (The secondary shutdown-hang bug reproduced on two
  of these three runs too: alive at 15-30% CPU, zero progress, needs `SIGKILL`, even after the ring reset itself
  *succeeded* at the kernel level - whatever this process gets stuck in post-reset doesn't care whether the reset
  "worked.")
- **State after this round: every specific mechanism tried (frame gen, overlap, GTAO/SSGI/water, texture prefetch,
  write-watch availability, set reuse, key memo, slot memo) is now ruled out as the sole cause**, each via a clean
  repro. What's left standing: the timing correlation (always the loading-screen -> world transition, always early),
  the two different-content devlost traces (UI panels once, GPU-indirect foliage+compute dispatch another time), and
  the real-but-seemingly-benign depth sync hazard above. The GMEM_FAST compute-copy path (`LARECOMP_VK_GMEM_FAST=0`,
  `LARECOMP_VK_GFAST_MERGE=0` - not yet tried) and the vertex cache (already off on this integrated GPU per
  `[vk] vertex cache off: integrated GPU` seen in every log - so not a live suspect) are the remaining named kill
  switches from CLAUDE.md's list not yet tested one at a time.
- **`LARECOMP_VK_GMEM_FAST=0` and `LARECOMP_VK_GFAST_MERGE=0` tested individually, same night: both still hang.**
  Same `record.c:584` / ring-timeout signature both times (~25-30 s in, clean single-instance runs). `GMEM_FAST=0`'s
  run died cleanly (no stuck-shutdown this time); `GFAST_MERGE=0`'s hit the usual `vk_collect_dead`/`vk_submit_extra`
  crash stack.
- **So every named kill switch in CLAUDE.md's bisect list has now been tried individually, and none of them is the
  sole cause:** frame gen, GPU overlap, the storage write-watch's availability, set reuse, key memo, slot memo,
  GMEM_FAST, GFAST_MERGE. (The vertex cache is already off on this integrated GPU by default - not a live suspect.)
  This is a meaningfully different situation than a normal "disable the suspect feature" bisect: **no single
  opt-in/opt-out subsystem is responsible**, which argues more strongly for either (a) something always-on and
  structural (the base draw/submit/recycle path itself, not any of its optional accelerations), or (b) the
  driver/hardware explanation the second opinion rated low-but-nonzero (a genuine Mesa/RADV bug on this git-snapshot
  build, workload-shaped rather than workload-agnostic, which would explain why it's insensitive to all these
  application-side toggles).
- **Next, not yet tried (heavier than a kill-switch toggle):**
  1. A RenderDoc capture (the layer is already on the Deck - `VK_LAYER_RENDERDOC_Capture` in the `vulkaninfo`
     listing) of a run right up to the hang - lets a human actually see the frame and its resource states, instead
     of reconstructing them from logs.
  2. Instrument the engine's own resource-recycle fence checks directly (log the fence value a slot/set/pipeline/
     command-buffer is freed at vs. the fence value actually completed by the GPU, looking for an off-by-one) -
     more surgical than disabling whole subsystems, and would catch the recycling theory even where `vkCmdPipeline
     Barrier`-level validation can't see it.
  3. Try `RADV_DEBUG=` feature toggles (NGG, DCC) per the second opinion's suggestion, to localize to a driver
     subsystem rather than rule application code in or out definitively - cheap, and doesn't require touching our
     own code at all.
  4. If practical, a different/older Mesa build on the Deck purely as a driver-vs-app discriminator (SteamOS's Mesa
     is usually not user-swappable without real effort, so this is a last resort, not a quick test).

## 1c. This is upstream's bug too, independently (2026-10-29 night)

Upstream (`Hexveil404/PokeZ-ARecomp`, our fork's base) has the **exact same crash class**, found and reported
completely independently on different hardware: Windows 11 + AMD RX 9070 XT, `FATAL: r failed: -4 (record.c:578)`,
`GPU page fault detected`, ~31 s into a session (issue #39, also reproduced on RX 7700 XT and RX 7600 - issue #72).
This rules out "maybe it's a buggy Mesa git-snapshot driver on this one Deck" for good: it happens on the proprietary
Windows AMD driver too. One of their crash addresses was literally `0xbeefdeadc000` - a poison/sentinel pattern,
strong evidence of a genuine use-after-free rather than a driver quirk. Their most reliable trigger: **defeating a
Bunnelby in the first battle** - worth trying as a much more targeted repro than our loading-screen taps (our test
save already has a Bunnelby in the party). Their v0.1.12 shipped an "instanced vertex buffer copy size" fix for a
related but **not confirmed as the root cause** bug (their own A/B test with the old buggy behavior forced back via
`LARECOMP_VK_OLD_INSTANCE_COPY=1` did not reproduce the crash either way) - issue #39 is still open upstream as of
v0.1.12. They also shipped a real diagnostic win worth having regardless: `[vk] GPU address binding report: on` names
the GPU object at a device-fault address directly, no `umr`/RADV_DEBUG=hang needed.

**Rebased our `linux` branch onto v0.1.12** (`git rebase --onto v0.1.12 cff8364 linux`, backup at
`linux-backup-pre-v0.1.12-rebase`). Two real conflicts in `device.c` (merging our frame-gen compute-queue addition
with upstream's new address-binding-report feature), both resolved by hand and it builds clean. **Not yet pushed to
GitHub/GitLab or the Deck** - validated only that Vulkan device creation succeeds locally (RX 7800 XT) with both
merged features initializing correctly; never got a full gameplay frame locally (the cached `lasgame.so` needs a
fresh compile under the new `GAMELIB_ID`, which needs the launcher's one-time UI click - `LARECOMP_FORCE_LAUNCHER=1`
gets the launcher to show despite `LARECOMP_INPUT` being set, but its "Compile now" button needs a real mouse click;
no `xdotool`/passwordless sudo available to automate it). Next session: either click through that by hand once, or
find another way to drive the launcher headlessly, before trusting this build further.

**Also found tonight: even the older, previously-"stable" `031cf13` (pre-investigation UI-split) build is unstable in
real play** - the Deck crashed hard enough once to need a full physical reboot while testing (though that was with
`ENABLE_VULKAN_RENDERDOC_CAPTURE=1` active, a confound worth noting), and a separate casual-play session on the same
build was reportedly unusable. Given §1c above, this isn't surprising: the bug isn't tied to any specific commit in
our own history, so no older build is immune.

## 1d. The v0.1.12 rebase runs clean for 3 minutes on the dev PC (overnight, 2026-10-29/30)

Picked back up on the §1c smoke-test blocker (needed a real mouse click to get through the launcher's one-time
"Compile now" UI, no `xdotool`/sudo available). Solved it properly instead of fighting the launcher: built the game
code directly via the documented dev workflow (`tools/gen.py` + `make`), bypassing the launcher UI entirely.

- **Set up `tools/venv`** (didn't exist locally) with `capstone`, `lz4`, `unicorn` - `gen.py`'s actual dependencies.
- **`make gen`** (pure Python codegen against the repo's own already-present `extracted/exefs`) succeeded in ~100 s,
  no issues.
- **`make -j6 gen-objs` and the final link both needed workarounds**, all environmental, none code problems:
  1. `libavcodec/avcodec.h` not found - ffmpeg-dev is vendored at `build/linux/x86_64/deps` (from the earlier podman
     `make appimage` runs) but the native build needs `DEPS=` pointed at it explicitly (the podman build's container
     wires this up internally).
  2. `-I$(DEPS)/include` / `-L$(DEPS)/lib` broke because **this repo's own path contains a space** ("SSD Storage") -
     the Makefile doesn't quote `$(DEPS)` in its flag construction, so make/the shell word-split it. Fixed by
     symlinking the deps dir to a space-free path (`/tmp/za-deps`) and pointing `DEPS` there instead - not a bug in
     the project's code, just a local-path quirk; the podman build never hits it because the container mounts at
     `/src`.
  3. `-fuse-ld=lld`: `lld` isn't installed on this host at all (only in the podman image). Manually re-ran the two
     link commands (`build/lasrecomp` and `build/lasgame.so`) with that flag stripped, using the system default
     linker (`ld.bfd`) instead - fine for a local test binary, not something to fix upstream.
  4. `-lshaderc`: the vendored lib is named `libshaderc_shared.so`, not `libshaderc.so` - symlinked it to match.
  - None of these are real build-system bugs worth fixing in the Makefile; they're all "native host build without
    the full dep set the podman image provides" issues. Worth remembering for next time rather than re-discovering.
- **Result: a genuine native `build/lasrecomp` + `build/lasgame.so`** for this exact v0.1.12-rebased tree, GAMELIB_ID
  `44da472c7706254e-B1F12FD919EAE86A`. Copied into `~/.local/share/lasrecomp/extracted/` (overwriting the old
  pre-rebase `lasgame.so` there - fully reproducible from source + the same game files, so no real loss) and ran it
  directly (`LD_LIBRARY_PATH=/tmp/za-deps/lib`, the usual `LARECOMP_VK_HEAP_DMABUF=0`/`GMEM_WINDOW=256`,
  `LARECOMP_HIDDEN=1`, scripted `LARECOMP_INPUT` taps then idle).
  - **Ran clean at a steady 60.0 fps for just over 3 minutes (frame 10290, t=179.9 s) with zero crash**, on this
    machine's RX 7800 XT - well past the ~20-75 s window where **every single Deck test tonight crashed**. Memory
    stayed flat and healthy the whole time (no leak).
  - **Caveat, don't over-read this result:** the scripted input only walked for a few seconds; the rest of those 3
    minutes was the character standing idle, not exploring new content or hitting loading transitions. Since every
    Deck crash tonight was tied to *content* (the loading-screen -> world transition, specific UI panels, or
    GPU-indirect foliage draws), an idle scene not reproducing the hang doesn't mean this build is fixed - it may
    just mean this particular run never exercised the trigger. `LARECOMP_HIDDEN=1` (no real window/compositor) is
    also a different presentation path than the Deck's windowed, gamescope-composited one, which could itself matter
    for a timing-sensitive bug.
  - Still a genuinely useful data point: the rebase is demonstrably playable and stable-enough-to-navigate-menus on
    real hardware, not just "compiles."
- **Next session:** rerun this same local setup with a LONGER, content-varied `LARECOMP_INPUT` script (actual
  walking into new areas, ideally toward a battle - §1c's "defeat a Bunnelby" recipe) rather than mostly-idle time,
  to get a real apples-to-apples test against the Deck's failure mode. If it's *still* clean after exploring real
  content for several minutes, that would be a much stronger signal (either this GPU/driver genuinely doesn't hit it,
  or the v0.1.12 rebase's bundled fixes help more than upstream's own inconclusive A/B test suggested).
- **Cleanup done:** killed the test process cleanly (`SIGTERM`, exited fine, no stuck-shutdown repeat this time),
  killed two leftover `python3 -m http.server` processes from earlier pushes that were missed, memory fully back to
  normal (17 GB available). The v0.1.12 rebase itself is still **local-only** - not pushed to GitHub/GitLab or the
  Deck.

## 2. Recurring GPU page fault at GPU VA 0x80010c800000

- **Symptoms:** a CB/DB (colour/depth) write gets a permission fault, then the ring times out and a MODE2 reset
  follows.
- **Seen with:** several builds, with the overlap both on and off.
- **Likely cause:** a render target bound after its memory was freed or reallocated (texture realloc / `tex_backend`
  reallocation, or gwin aliasing).
- **Next:** a devcoredump or `RADV_DEBUG=hang` run will show which target it is.

## 3. Frame-gen motion vectors were about 0 - fixed locally 2026-10-30, not yet Deck-tested

- **Was:** `mv_note_draw` read `st.ubuf[0][2]` (vertex cb5) off the first draw each frame with 4+ colour targets (a
  G-buffer heuristic) and treated it as the view-projection. Motion vectors came out ~0 - either the wrong matrix
  or a fixed-camera pass.
- **Fix:** `mv_note_draw` (`exec.c`) now triggers on `gtao_is_lighting(pr)` (the same lighting-pass detection GTAO
  uses - works whether or not ambient_occlusion/indirect_light/water_reflections are even on, via the hardcoded
  `GTAO_LIGHTING` fragment-shader hash) and reads the exact same cbuf GTAO already trusts (`st.ubuf[1][2]`,
  fragment cb2 of that draw): rows 12-15 (`m0..m3`), row 19 (camera position), rows 20-21 (tan half fov x/y).
  That block is **not a conventional view-projection** - it's the game's own `world_pos = ndc.x*m0 + ndc.y*m1 +
  depth*m2 + m3` reconstruction, i.e. already the *inverse* of a view-projection, column-packed. `fsr2_copy`
  (which builds `fsr2_mv.frag`'s `inv_cur`/`prev` uniforms) had to flip which matrix gets `inv4()`'d: `inv_cur` is
  now a straight copy of `mv_vp[0]` (no inversion - it already is the inverse), and `prev` is `inv4()` of the
  *other* frame's stored matrix (to turn its inverse-VP back into a real forward VP). The camera-cut heuristic
  (dot product threshold for an FSR 2 history reset) now reads the `m2` depth-basis column's xyz instead of the
  old matrix's w-row, and `fov_y` uses the captured tan(half fov) directly instead of guessing it from a matrix
  row's length; `near_z` has no clean source in this data any more, so it's just the existing 0.1 fallback.
- **Known gap:** `gtao_is_lighting`'s *heuristic* fallback (for lighting-pass variants during moves/Pokémon
  send-outs, per its own comment) only runs when `gtao_on()||ssgi_on()||water_on()` is true - with all three off,
  motion vectors may briefly stop updating during those specific animations (the hash-based fast path still
  covers the common case). Not fixed; narrow enough to leave for now.
- **Tested locally (dev PC, RX 7800 XT, v0.1.12 rebase, scratch save/ini - never the real ones):** three headless
  native runs, zero crashes. `LARECOMP_MV_LOG` showed the captured camera position tracking real movement frame
  to frame (a ~700-unit jump right after spawn/load, then small smooth per-frame deltas while walking) and
  matched GTAO's own independently-logged camera position exactly (`[gtao] on: ... camera (-0.0, 0.5, 2.5)` vs.
  `[mvlog]`'s same value same frame) - strong cross-confirmation the right cbuf is being read.
- **Not yet confirmed:** `vk_fg_build` (the actual interpolated-frame draw) needs `ui_window_focused()==true`, and
  neither a `LARECOMP_HIDDEN=1` run nor a real on-screen window opened headlessly on this dev box ever received
  real input focus (`[fg]`'s own frame-by-frame log, which fires unconditionally for the first 6 frames once
  `vk_fg_build` runs past that check, never printed once in any run). So the motion-vector *data* is verified
  correct; the full presentation path (frame actually gets interpolated and shown) is not, and needs either a
  real focused window (someone at the keyboard) or the Deck.
- **Also added:** the F1 "Show FPS" overlay now shows `base + generated = total` fps when frame generation is on
  (`device.c`'s `fg_rate_note`/`vk_fg_present_rate`: a rolling rate of the generated frame's own successful
  presents, 0 once they stop landing rather than a stale number) instead of just the base rate - for exactly this
  kind of "is it actually doubling the frame rate" check, without reading the log.
- **Next:** play it for real (Deck or local with focus) with `LARECOMP_FG_LOG=1` and watch for `[fg] frame N:
  on=1 ... valid=1` and a doubled fps in the corner counter; `LARECOMP_FG_DUMP=<frame>` to compare `fg_cur.png`
  vs `fg_gen.png` (should differ now, not be byte-identical).

## 4. CPU/GPU overlap's vertex/index race - fixed locally 2026-10-30, not yet Deck-tested

GPU overlap (`gpu_overlap`/`LARECOMP_GPU_OVERLAP`) is the single biggest known Deck fps lever (ZA-WORKLOG.md: 44 ->
51-55 fps), but ships off by default because of a real device-lost bug after ~90 s: without the storage write watch
(the Deck's RADV fix always means no watch - `LARECOMP_VK_HEAP_DMABUF=0`), nothing caught the game rewriting its
per-frame vertex/index data while the GPU still read the OLD frame's *in-place* binding of it - the same unwatched-
rewrite race `snap_storage`'s `RS_PROBE` already had to solve for storage buffers, just never extended to vertex /
index data at all (it was bound straight from the guest heap unconditionally, every draw, no protection whatsoever).

- **First attempt (`vtx_must_copy`, `exec.c`):** when overlap is on and there's no write watch, refuse the in-place
  bind and force the existing `vk_transient_copy` fallback instead - correct (170 s clean run locally, no crash,
  vs. the historical ~90 s), but **~15x the upload bandwidth** on a busy scene (measured: ~750-1100 MB/2s baseline
  vs. ~15,000 MB/2s with this on) and a real local fps hit (60 -> 42-50), because it copies *everything* every
  frame, including world geometry that's static forever. Kept as the fallback for ranges the smarter path below
  can't handle (oversized, or its table is full this submission).
- **Second attempt (`vc_hash_mode`, inside the existing `vcache_lookup`/`vc_tab`):** reuse the vertex cache's own
  buffer pool, but decide staleness with a content hash (`range_hash`, already used by `snap_storage`) instead of
  OS dirty-page bits, so a genuinely static range costs one copy ever, not one a frame. **Turned out to be dead
  code on the Deck's own configuration**: `vcache_lookup` has a pre-existing `|| gmem_lo ||` early-exit, because its
  offset math (`lo = addr - gwin_base`) was never adjusted for a non-zero `gmem_lo` - and `LARECOMP_VK_GMEM_WINDOW`
  (required on the Deck) always makes `gmem_lo != 0` (confirmed via `LARECOMP_VK_PROF=1` telemetry: `gmem_lo` was
  2965 MB in a local repro of the Deck's settings, and `vcache N hit N copied` sat at `0 0` for an entire run).
  Left in place anyway (harmless, and still useful on any Linux/AMD config that doesn't need `GMEM_WINDOW`), but it
  doesn't help the one configuration this was for.
- **Third attempt (`hc_lookup`/`hc_flush`/`hc_reset`, `exec.c`), the one that actually works there:** a second,
  independent content-hash cache, keyed on `GM_OFF`-adjusted offsets (i.e. correct under `gmem_lo != 0`), mirroring
  `vc_tab`'s buffer-lifetime pattern (LRU, a dead-list freed only once `vk_completed_seq()` passes a buffer's last
  use) but with its own pool - the two offset spaces aren't comparable, so it isn't shared with `vc_tab`.
  - An independent review (a fresh agent, no shared context, asked to adversarially check address math, buffer
    lifetime, and the "GPU-ordered copy can't race its own reads" safety argument) caught a real bug before this
    shipped: `hc_flush`'s content hash is a raw CPU pointer dereference, and `gmem_copy`'s `vkCmdCopyBuffer` has no
    fence of its own - both fine for a CPU rewrite (that's exactly the race they protect against), but **not safe
    against a range a compute/vertex shader is still writing** (Z-A does this for real: skinned character output
    read straight back as vertex data - see the `exec.c` comment on `vc_mark_sbufs`). The *first* blunt
    `vtx_must_copy` attempt had this identical bug too, silently, from the start (a plain `memcpy`, zero GPU-write
    awareness) - it just never showed up as a crash in testing, only as a possible stale/torn pose for a frame.
  - Fixed: `hc_lookup` now checks the vertex cache's own GPU-written page table (`vc_page_gpu`, the same one
    `vcache_lookup`'s `e->gpu` bypass reads, translated to heap-relative via `+ gmem_lo`) before touching anything,
    and for a known-GPU-written range just binds the import directly - free, and correct by construction (a GPU
    write and a later GPU read of it are already ordered by the queue on their own, no CPU involvement needed).
  - **Result after the fix, 170 s local run, overlap forced on, no write watch:** zero crashes, upload bandwidth
    ~700-1000 MB/2s (matching the no-overlap baseline almost exactly, down from the blunt version's ~15,000), and
    **60 fps steady** for most of the run (vs. 42-54 fps with either earlier attempt) - the GPU-written bypass alone
    recovered real cost, since Z-A's skinned meshes would otherwise have been needlessly hash-cached or copied.
- **Not yet confirmed:** all of this was tested on a dev PC (RX 7800 XT, Linux/AMD so genuinely has no write watch,
  matching the Deck's situation) which has no CPU/GPU-serialization bottleneck for overlap to relieve in the first
  place - a local fps measurement with overlap on literally cannot show the win this exists for, only the cost of
  keeping it safe (which is now small). The Deck is the only place that can confirm whether turning `gpu_overlap`
  on, with this fix, is a net win over 30 fps default. Try it there before changing `gpu_overlap`'s shipped default.
- **`guest_direct`'s `gc_lazy` branch (macOS's lazy heap-chunk path) still has the plain blunt `vtx_must_copy`
  fallback, unprotected against the same GPU-write case** - not fixed, since it's not Linux/Deck and untestable
  from here; worth the same `hc_maybe_gpu`-style check if someone picks this up on macOS.

## 5. The "stuck after a crash, needs SIGKILL" bug - fixed 2026-10-30 (via totk-recomp cross-reference)

**totk-recomp** (github.com/Muhammad1259-sigma/totk-recomp, Tears of the Kingdom on Steam Deck) ports its NVN-on-
Vulkan layer from the same upstream LAS Recomp we do - a sibling project, not a fork of ours, but close enough in
lineage that its own extensive GPU-hang debugging (same timeframe as ours) is worth cross-checking. Its hang handler
goes straight to `_exit()` on a confirmed device-lost GPU, no crash dialog. That's the fix for our own long-standing
"stuck after a crash, needs SIGKILL" symptom: `FATAL()`'s `plat_fatal_notice()` shows a blocking
`SDL_ShowSimpleMessageBox` on *every* fatal error, including device-lost - and right after a GPU reset the
window/surface can be in a state where that dialog never actually displays or can't be dismissed, which looks
exactly like the whole process hanging (and explains why every case we've hit it in was an unattended/scripted run
or a fullscreen game nobody was watching for a hidden dialog).

- **New `vk_fatal_device_lost(what)`** (`device.c`): logs the device fault report and the devlost trace/checkpoints
  (unchanged, already reused), then `_exit(5)` directly - no notice dialog, no further Vulkan or window-system calls
  of any kind once the device is confirmed gone.
- **Wired into every confirmed-`VK_ERROR_DEVICE_LOST` site**: the generic `VK_CHECK` macro (`vk.h` - this alone
  covers every Vulkan call in the codebase that can return it), `vk_wait_seq`'s device-lost branch, the present path
  (`record.c`), and frame generation's queue submission (`device.c`).
- **`vk_wait_seq`'s plain `VK_TIMEOUT` case also changed**, separately from the confirmed-lost case above: a 10 s
  `vkWaitSemaphores` timeout is ambiguous, not a confirmed loss - an amdgpu ring-timeout reset or a Windows TDR isn't
  instant and can still be mid-recovery past 10 s (totk-recomp's own handler measured a ~100 s Windows TDR). Old
  behavior: fatal immediately at 10 s, possibly while the driver is still resetting - likely making the dialog-hang
  problem worse, not just coincidental with it. New: poll up to 60 s more (checking `vkGetSemaphoreCounterValue` for
  a confirmed loss along the way); if the GPU actually finishes in that window, log it and carry on instead of
  killing a session that would have recovered on its own.
- **Not device-lost-specific, but checked while here: we already have `VK_NV_device_diagnostic_checkpoints`**
  (`device.c` `vk_checkpoint`/`vk_checkpoints`, wired into `exec.c`'s draw/dispatch call sites, read by
  `vk_devlost_dump` under `LARECOMP_VK_DEVLOST_TRACE`) - equivalent to totk-recomp's own checkpoint use, already in
  place and already central to this week's §1/§1b GPU-hang investigation. Nothing to add there.
- **Tested:** compiles clean, a normal 30 s local smoke run is unaffected (no behavior change on the happy path, as
  expected - this only touches the already-fatal device-lost/hang paths). The actual device-lost path itself is
  **not** independently verified - that needs a real GPU hang to hit, which isn't something to deliberately trigger
  locally. Next real Deck crash is the first live test of whether this actually stops the stuck-shutdown symptom.
- **Open question, not acted on (no Deck access to test it)**: totk-recomp's comment says userptr heap imports cause
  "Deck v2's 6 s kernel stall" via the MMU notifier, and defaults to dma-buf instead - the *opposite* of our own
  Deck fix, which forces `LARECOMP_VK_HEAP_DMABUF=0` (userptr) because dma-buf was what hung for us. Could be a
  hardware-revision (LCD vs OLED Deck) or Mesa-version difference, or two genuinely different bugs each project
  independently routes around. Don't change our default on this alone; worth asking if the hang ever shows up on an
  OLED Deck specifically.
- **totk-recomp's "device window" (`gw_overlap`/`gdev_reg`) - read in full, tried porting its core idea, measured it
  worse, reverted.** It's a single pre-allocated device-local buffer split into two halves used in turn: a region
  gets offsets carved out of the current half, and when a half fills, the *other* half is reclaimed as a whole (wait
  once for its last user, then every region in it is simply forgotten) - no per-range VkBuffer/VmaAllocation churn,
  and a small linear region list instead of a hash table (their own choice, measured ~1us/draw at 256 entries).
  Rebuilt `hc_lookup`/`hc_flush` this way (2026-10-30) to avoid `hc_lookup`'s original per-range VMA buffer
  create/destroy cost. An independent review of the port found one real bug (`hc_npend` could overflow without the
  caller noticing, serving a region with stale/uninitialized contents for one frame - the same fix applies to the
  reverted-to version below, see next bullet) and confirmed the rest correct. **But a same-test A/B measured it
  clearly worse**: ~14 GB/2s upload bandwidth, almost back to the blunt always-copy version's, vs. ~800 MB/2s before
  and after. Root cause understood, not just observed: totk-recomp's window protects a handful of small, short-lived
  storage buffers, where "presume a whole half's regions stale on every flip" is a fine tradeoff; this one needs a
  large, mostly-*static* vertex/index working set to stay resident for as long as it keeps being drawn, and
  discarding half of it on every flip - whether or not anything in it actually changed - defeats exactly the
  caching this exists for. **Reverted to the original per-range VMA buffer + true LRU** (only ever evicts the
  genuinely least-recently-used entry, never a whole batch): confirmed back to ~800-1000 MB/2s, zero crashes, and
  this time a steady 60 fps for the full 170 s run (better than either earlier attempt - the LRU design turned out
  to also scale better than the blunt version's heavier, evenly-spread copy cost). Also applied the overflow fix
  found during the review to this reverted version too (`hc_lookup`'s re-used-entry path now also refuses rather
  than silently skipping the queue when `hc_pend` is full - a latent gap in the very first version of this, not
  introduced by the ping-pong attempt, just surfaced by the same review).
  - **Lesson for next time an architecture from a sibling project looks appealing**: port the idea, but re-derive
    whether its *tradeoffs* (not just its mechanism) fit this codebase's actual workload before trusting it - and
    measure, don't just reason about it. This was caught by running the same test script before and after, not by
    the correctness review (which passed the ping-pong version with only the one unrelated bug).
  - Their broader "device window" is still a real, more unified design than this codebase's separate
    `vcache_lookup`/`snap_storage`/`hc_lookup` mechanisms for the GPU-written-storage-buffer case specifically
    (which does need read-back, unlike vertex/index data) - just not a fit for `hc_lookup`'s own job. Still worth a
    look for `snap_storage` specifically, another time, not tonight.

## Other open items
- **Disproven 2026-10-30, don't re-chase it:** the Deck's documented slow fps drift ("cached ~18000 MB host" over
  9 min, 30.4 -> 28.9 fps) is *not* "the vertex cache is disabled, so reads are uncached and costly." `stat_vtx_host`
  is the free, zero-copy direct-bind path (no upload, no GPU copy - just pointing the GPU at existing memory); tried
  widening the overlap-only hash cache (§4) to cover it unconditionally (no new env flag, per the standing rule) and
  measured it a clear *regression* even on a discrete GPU with real PCIe bandwidth to save (steady 60 fps -> 44-49,
  same test script, local): the CPU-side hashing this adds costs more than the bus-crossing it avoids, because the
  direct-bind path was never actually expensive. Reverted (no commit - the working tree was already back at HEAD).
  On the Deck specifically it would have been worse than useless regardless: `vcache_lookup` already has a deliberate
  "integrated GPU: no bus to save, caching would only cost" exclusion (same physical RAM as the CPU), which a
  first pass at this missed entirely. The real cause of the Deck's drift is still open - thermal, actual RAM
  capacity pressure on a 15 GB device, or something else - just confirmed to not be this.
- **Royale randomizer probably never fires:** `fs.c` `za_royale_read` looks for a loose `trdata_array.bin`, but
  that file is inside `arc/data.trpfs`.
- **Unmerged reviewer branch** `worktree-agent-ad664600080292dcf`: stale (forked long before the v0.1.12 rebase -
  diffing it against `linux` is mostly *missing* work, not ahead of it; don't merge it wholesale). Its small,
  still-applicable fixes were cherry-picked by hand 2026-10-30: the `za_royale_delta` load-time clamp and the
  duplicate `temporal_aa` save line (`settings.c`), `royale_randomizer.py`'s two unclosed-file opens (the second
  one was a real race - flatc could read the json before Python flushed it), the env-override comment, and a
  `black2`/`black` duplicate-constant dedupe in `device.c`'s frame-gen present path. Nothing left to pull from it.
  - Notes from it, not yet looked at: `_za_frame_measured` is dead code, and the Metal backend still needs the UI
    scale.
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
