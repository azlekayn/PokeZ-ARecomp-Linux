#!/bin/bash
# Windows (Git Bash): a scripted play for performance. IN="frame:BUTTON:frames,..." (LARECOMP_INPUT), T=timeout s,
# on a scratch data root, build/perf/: the install's game files and game code linked in, its own copies of the
# settings, the save (the game autosaves) and the shader cache - the player's are never touched. Then the [slow] lines
# (frames over 50 ms, runtime/nvn/slowframe.c) summed up.
#   GAME=<install folder> (with extracted/, save/save_0, larecomp.ini: the game code built and a save to start from)
#   EXE=<lasrecomp.exe> (default: build/dist-za/ZA Recomp/lasrecomp.exe, with its DLLs next to it)
#   CACHE=cold (empty shader caches: every shader is new, as the first time a move is seen; default) | warm (the
#   install's caches) | keep (the previous run's: what a second session finds)
#   SHOT=<n>: the window every n frames in build/perf/build/frame_*.png (0: none, default)
#   SAVE=<save_0 folder> to start from (default: the install's, GAME/save/save_0)
#   INI=<larecomp.ini> to play with (default: the install's, GAME/larecomp.ini)
#   IN=@<file>: the entries in a file, e.g. a player's recording (LARECOMP_RECORD_INPUT, runtime/hle/input.c)
# Log: build/perf/perf.log. Like auto-win.sh: no MSYS timeout (it stalls the window) - killed after T s or 25 s with
# no new frames.
cd "$(dirname "$0")/../.." || exit 1
[ -n "$GAME" ] || { echo "GAME=<the ZA Recomp folder> needed"; exit 1; }
EXE=${EXE:-build/dist-za/ZA Recomp/lasrecomp.exe}
R=build/perf
[ -f "$GAME/extracted/lasgame.dll" ] || { echo "no game code in $GAME/extracted"; exit 1; }
rm -rf "$R/build" "$R/save" "$R/logs" "$R/perf.log"
[ "$CACHE" = keep ] || rm -rf "$R/cache"
mkdir -p "$R/save" "$R/build"
# (a junction: no admin rights needed, and nothing copied - romfs.bin is GBs. Made again each run: GAME may have
# changed. rmdir removes the junction only - never rm -rf it, that empties the install's folder)
[ -e "$R/extracted" ] && cmd //c rmdir "$(cygpath -w "$R/extracted")"
cmd //c mklink //J "$(cygpath -w "$R/extracted")" "$(cygpath -w "$GAME/extracted")" > /dev/null
cp -r "${SAVE:-$GAME/save/save_0}" "$R/save/save_0"
cp "${INI:-$GAME/larecomp.ini}" "$R/larecomp.ini"
[ "${CACHE:-cold}" = warm ] && cp -r "$GAME/cache" "$R/cache"
# (@<file>: as a full Windows path - the game runs in the data root and knows no /e/... paths)
[ "${IN#@}" != "$IN" ] && IN="@$(cygpath -aw "${IN#@}")"
LARECOMP_DATA_ROOT="$(cygpath -aw "$R")" LARECOMP_SETTINGS="$(cygpath -aw "$R/larecomp.ini")" LARECOMP_INPUT="$IN" \
    LARECOMP_SCREENSHOT_EVERY=${SHOT:-0} LARECOMP_FPS_INTERVAL=1 "$EXE" "$(cygpath -w "$R/extracted")" --play > "$R/perf.log" 2>&1 &
G=$!
start=$SECONDS; last=""; stuck=0
while kill -0 $G 2>/dev/null; do
    sleep 5
    if [ $((SECONDS - start)) -ge ${T:-180} ]; then echo "timeout ${T:-180}s"; break; fi
    [ $((SECONDS - start)) -lt 40 ] && continue
    cur=$(grep -a "^\[vk\] t=" "$R/perf.log" | tail -1 | cut -c1-30)
    if [ "$cur" = "$last" ]; then stuck=$((stuck+5)); else stuck=0; last="$cur"; fi
    if [ $stuck -ge 25 ]; then echo "HANG at: $cur"; break; fi
done
# (by its own pid: a game the player has open is left alone)
taskkill //F //T //PID "$(cat /proc/$G/winpid 2>/dev/null)" > /dev/null 2>&1
wait $G 2>/dev/null
# (and any left from this EXE - the pid lookup can miss it, and a leftover game skews the next run's numbers; the
# player's install is another path)
for p in $(powershell -NoProfile -Command "Get-Process lasrecomp -EA 0 | ? Path -eq '$(cygpath -aw "$EXE")' | % Id" | tr -d '\r'); do
    taskkill //F //PID "$p" > /dev/null 2>&1
done
grep -a -m3 "FATAL\|EXCEPTION\|access violation" "$R/perf.log"
grep -a "^\[vk\] t=" "$R/perf.log" | tail -1 | cut -c1-60
# the [slow] lines (runtime/nvn/slowframe.c: a frame over LARECOMP_SLOW_FRAME_MS, 50 by default, and what it did)
awk '/^\[slow\] frame/ {
        n++; ms = $4 + 0; tot += ms; if (ms > mx) mx = ms
        if (match($0, /render cpu [0-9]+/)) r += substr($0, RSTART + 11, RLENGTH - 11)
        if (match($0, /game cpu [0-9]+/)) g += substr($0, RSTART + 9, RLENGTH - 9)
        if (match($0, /gpu wait [0-9]+/)) gw += substr($0, RSTART + 9, RLENGTH - 9)
        if (match($0, /queue wait [0-9]+/)) q += substr($0, RSTART + 11, RLENGTH - 11)
     }
     END { if (!n) { print "no frames over 50 ms"; exit }
           printf "%d frames over 50 ms, %.0f ms in all (longest %.0f): render cpu %.0f, game cpu %.0f, gpu waits %.0f, " \
                  "queue waits %.0f\n", n, tot, mx, r, g, gw, q }' "$R/perf.log"
