#!/bin/bash
# Windows (Git Bash): follows a player's logs/play.log while they play and prints only what needs a look - for a
# monitor (each line an event): the [slow] lines (slow frames, runtime/nvn/slowframe.c), fps drops,
# errors and crashes, and when the game starts and exits. Each line has its line number in play.log (L<n>) to read
# around it. Polled, the log never held open: the game renames it to play-anterior.log at each start (runtime/main.c).
#   GAME=<install folder> (needed: the ZA Recomp folder with lasrecomp.exe and logs/)
#   FROM=start: the current log from its first line (default: only what is written from now on)
[ -n "$GAME" ] || { echo "GAME=<the ZA Recomp folder> needed"; exit 1; }
L="$GAME/logs/play.log"
size() { stat -c %s "$L" 2>/dev/null || echo 0; }
# (the log so far ends a line: a marker line between the log's own)
endsnl() { [ "$off" = 0 ] || [ "$(tail -c +"$off" "$L" | head -c 1 | od -An -tx1 | tr -d ' ')" = 0a ]; }
# (this install's game only: not a test's (tools/za/perf-win.sh) from build/dist-za)
EXEW=$(cygpath -aw "$GAME/lasrecomp.exe")
# (compared in PowerShell: -eq ignores case; Git Bash's grep -iF aborts on such a path)
running() { powershell -NoProfile -Command "if (Get-Process lasrecomp -EA 0 | ? Path -eq '$EXEW') {1} else {0}" |
            tr -d '\r'; }
off=$(size); [ "$FROM" = start ] && off=0
base=0; [ "$off" -gt 0 ] && base=$(head -c "$off" "$L" | wc -l)
run=$(running)
{
    echo $'\001BASE' "$base" "$run"
    n=0
    while true; do
        s=$(size)
        # (smaller: the game started again with a new log; the marker on a line of its own - the old log may end
        # mid-line, a crash)
        if [ "$s" -lt "$off" ]; then off=0; printf '\n\001NEWLOG\n'; fi
        if [ "$s" -gt "$off" ]; then tail -c +$((off + 1)) "$L" | head -c $((s - off)); off=$s; fi
        if [ $((n++ % 5)) = 0 ]; then
            # (exited: the log is final - a line it left unended (a crash) is ended here)
            r=$(running)
            if [ "$r" != "$run" ] && { [ "$r" = 0 ] || endsnl; }; then
                [ "$r" = 0 ] && ! endsnl && echo
                run=$r; echo $'\001RUN' "$r"
            fi
        fi
        sleep 1
    done
} | awk '
    function out(s) { printf "L%d %s\n", NR - skip + base, substr(s, 1, 400); fflush() }
    /^\001BASE/   { base = $2; skip = NR; print "[watcher] following play.log from line " base + 1 ($3 ? ", the game is running" : ", the game is not running"); fflush(); next }
    /^\001NEWLOG/ { base = 0; skip = NR; best = 0; last = 0; closed = 0; print "[watcher] new play.log: the game started again"; fflush(); next }
    /^\001RUN/    { if ($2) print "[watcher] lasrecomp.exe started"
                    else print "[watcher] lasrecomp.exe exited" (closed ? " (window closed)" : " WITHOUT \"window closed\": a crash? the log ends at L" NR - skip - 1 + base)
                    fflush(); skip++; next }
    /^window closed/ { closed = 1; out($0); next }
    /^\[slow\]|^\[hitch\]|^\[vtxstate\]|^\[black\]|^\[shader\]|full-screen draw waited/ || /^FATAL|^\*\*\* |EXCEPTION|access violation|[Aa]ssert|VK_ERROR|device lost|out of memory|gave up|unimplemented/ { out($0); next }
    /^\[launcher\]|^\[gamelib\]|^\[shot\]/ { out($0); next }
    # fps: below 85 % of the best so far (30 or 60 fps, the settings), at most once per 10 s of the log
    /^\[vk\] t=[0-9.]+ frame [0-9]+ [0-9.]+ fps/ {
        t = substr($2, 3) + 0; f = $5 + 0
        if (f > best) best = f
        if (best >= 25 && f < best * 0.85 && t - last >= 10) { last = t; out($0) }
    }'
