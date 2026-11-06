#!/usr/bin/env bash
# runtime/saveserv.cpp against a throwaway copy of the SaveServ server: two devices, on their own folders. Touches neither
# a real server nor a real save. Usage: SAVESERV_PY=<SaveServ>/server/saveserv.py tools/test_saveserv.sh
set -u
: "${SAVESERV_PY:?path to SaveServ/server/saveserv.py}"
R=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
PORT=${PORT:-18787}
URL=http://127.0.0.1:$PORT
fails=0
trap 'kill $SRV $OTHER 2>/dev/null; rm -rf "$T"' EXIT
g++ -std=c++17 -O1 -Wall -I"$R/runtime" "$R/tools/test_saveserv.cpp" "$R/runtime/saveserv.cpp" -lpthread -o "$T/drv" || exit 1
mkdir -p "$T/srv" "$T/web"
SAVESERV_DATA=$T/srv SAVESERV_PORT=$PORT SAVESERV_THUMBNAILS=0 python3 -I "$SAVESERV_PY" >"$T/srv.log" 2>&1 &
SRV=$!
(cd "$T/web" && exec python3 -I -m http.server $((PORT + 1)) >/dev/null 2>&1) &
OTHER=$!
for _ in $(seq 50); do curl -sf "$URL/api/hello" >/dev/null && break; sleep 0.1; done
J=$T/jar
curl -sS -c "$J" -H 'X-SaveServ: 1' -H 'Content-Type: application/json' -d '{"name":"tester","password":"testtest1"}' "$URL/web/register" >/dev/null

ok() { if [ "$2" == "$3" ]; then echo "ok   $1"; else echo "FAIL $1: wanted [$3], got [$2]"; fails=$((fails + 1)); fi; }
dev() { local d=$1; shift; LARECOMP_DATA_ROOT=$T/$d SAVESERV_CONFIG=$T/$d.cfg "$T/drv" "$@"; }
pair() {
    mkdir -p "$T/$1"
    dev "$1" pair "$URL" >"$T/$1.pair" &
    local p=$!
    for _ in $(seq 50); do grep -q PIN "$T/$1.pair" && break; sleep 0.1; done
    curl -sS -b "$J" -H 'X-SaveServ: 1' -H 'Content-Type: application/json' -d "{\"pin\":\"$(awk '/PIN/{print $2}' "$T/$1.pair")\"}" "$URL/web/pair" >/dev/null
    wait $p
    tail -1 "$T/$1.pair"
}
put() { mkdir -p "$T/$1/save/save_0"; printf %s "$3" >"$T/$1/save/save_0/$2"; sleep 1.1; }  # (a new mtime)
get() { cat "$T/$1/save/save_0/$2" 2>/dev/null; }
revs() { curl -sS -H "Authorization: Bearer $(sed -n 's/^token=//p' "$T/${REVS_CFG:-a.cfg}")" "$URL/api/saves/za" | python3 -I -c "import json,sys; d=json.load(sys.stdin); print(d.get('save',{}).get('rev',0))"; }

mkdir -p "$T/x"
ok "not paired: no network, nothing happens" "$(dev x sync)" "OFF nothing "
ok "a server that isn't SaveServ is refused" "$(dev x pair http://127.0.0.1:$((PORT + 1)))" "ERROR nothing http://127.0.0.1:$((PORT + 1)) isn't a SaveServ server."
ok "no such server" "$(dev x pair http://127.0.0.1:9 | cut -d' ' -f1)" "OFFLINE"
ok "bad address refused" "$(dev x pair 'a"b' | cut -d' ' -f1)" "ERROR"

put a main M1; put a backup B1
ok "A pairs and uploads" "$(pair a)" "SYNCED uploaded "
ok "token file is private" "$(stat -c %a "$T/a.cfg")" "600"
ok "main on the server" "$(revs)" "1"
ok "B, new, pairs and downloads" "$(pair b)" "SYNCED downloaded "
ok "B has main" "$(get b main)" "M1"
ok "B has backup" "$(get b backup)" "B1"
ok "nothing changed" "$(dev a sync)" "SYNCED nothing "
ok "no new revision" "$(revs)" "1"

put b main M2; put b backup M1
ok "B's new save goes up" "$(dev b sync)" "SYNCED uploaded "
ok "A takes it" "$(dev a sync)" "SYNCED downloaded "
ok "A has it" "$(get a main)" "M2"
ok "A's old save kept as a backup" "$(cat "$T"/a/save/backup-*-sync/main)" "M1"

put a main A3; put b main B3
ok "A uploads first" "$(dev a sync)" "SYNCED uploaded "
ok "B: conflict" "$(dev b sync)" "CONFLICT nothing "
ok "B's save untouched by a conflict" "$(get b main)" "B3"
ok "B keeps the server's" "$(dev b resolve server)" "SYNCED downloaded "
ok "B now has A's" "$(get b main)" "A3"
ok "B's own copy is a backup" "$(cat "$T"/b/save/backup-*-sync/main | tail -c 2)" "B3"

put a main A4; put b main B4
dev a sync >/dev/null
ok "B: conflict again" "$(dev b sync)" "CONFLICT nothing "
ok "B keeps its own" "$(dev b resolve local)" "SYNCED uploaded "
ok "A gets B's" "$(dev a sync)" "SYNCED downloaded "
ok "A has B's" "$(get a main)" "B4"

out=$(dev a ingame-write G1)
ok "in game: a save is sent once still" "$(echo "$out" | tail -1)" "SYNCED uploaded "
ok "B gets it" "$(dev b sync)" "SYNCED downloaded "
ok "B has G1" "$(get b main)" "G1"
out=$(dev a ingame-flush G2)
ok "quit straight after a save: flushed" "$out" "SYNCED uploaded "
dev b sync >/dev/null

put b main B5
dev b sync >/dev/null
put a main A5
before=$(revs)
out=$(dev a ingame-write A6)
ok "in game, the server's newer: said so" "$(echo "$out" | tail -1 | cut -d" " -f1)" "NEWER"
ok "in game: no upload over it" "$(revs)" "$before"
ok "in game: no download under the game" "$(get a main)" "A6"
ok "the start screen asks" "$(dev a sync)" "CONFLICT nothing "
dev a resolve local >/dev/null

mkdir -p "$T/c"; pair c >/dev/null
printf '' >"$T/c/save/save_0/main" 2>/dev/null
ok "empty local file isn't uploaded, but replaced" "$(dev c sync | cut -d' ' -f1)" "SYNCED"

ID=$(curl -sS -b "$J" "$URL/web/devices" | python3 -I -c "import json,sys; print([d['id'] for d in json.load(sys.stdin) if True][-1])")
curl -sS -b "$J" -H 'X-SaveServ: 1' -X POST "$URL/web/devices/$ID/remove" >/dev/null
for d in a b c; do st=$(dev $d sync); [ "${st%% *}" == OFF ] && gone=$d; done
ok "a device removed on the website goes unpaired" "$(dev "${gone:-none}" sync 2>/dev/null)" "OFF nothing "
ok "and its token is forgotten" "$(grep -c '^token=' "$T/${gone:-none}.cfg")" "0"

# The saveserv tool beside this client (SAVESERV_BIN=<its binary>, optional): the same save must hash the same from both,
# and each must take the other's uploads. Its settings are a throwaway copy, checked to name this server before use.
if [ -n "${SAVESERV_BIN:-}" ]; then
    survivor=a; [ "${gone:-}" == a ] && survivor=b
    grep -q '^token=' "$T/$survivor.cfg" || survivor=c
    cp "$T/$survivor.cfg" "$T/cli.cfg"
    REVS_CFG=cli.cfg  # (a.cfg's token may be the one removed above)
    cli() { SAVESERV_CONFIG=$T/cli.cfg "$SAVESERV_BIN" "$@"; }
    if cli status 2>&1 | grep -q "server:  $URL\$"; then
        dev $survivor sync >/dev/null; dev $survivor resolve local >/dev/null
        put $survivor main T1; put $survivor backup T2
        ok "tool test: client uploads" "$(dev $survivor sync)" "SYNCED uploaded "
        cli add za "$T/$survivor/save/save_0" --files main,backup >/dev/null
        before=$(revs)
        cli sync-folder za >/dev/null 2>&1
        ok "tool: same save hashes the same (no new revision)" "$(revs)" "$before"
        put $survivor main T3
        cli sync-folder za >/dev/null 2>&1
        ok "client: takes the tool's upload (same bytes, nothing to do)" "$(dev $survivor sync)" "SYNCED nothing "
        ok "tool's upload is a new revision" "$(revs)" "$((before + 1))"
        mkdir -p "$T/d"; cli remove za >/dev/null; cli add za "$T/d/save/save_0" --files main,backup >/dev/null
        mkdir -p "$T/d/save/save_0"; cli sync-folder za >/dev/null 2>&1
        ok "tool: downloads the client's save, main" "$(get d main)" "T3"
        ok "tool: downloads the client's save, backup" "$(get d backup)" "T2"
    else echo "FAIL the tool's settings don't name the test server: skipped"; fails=$((fails + 1)); fi
fi

kill $SRV; wait $SRV 2>/dev/null
put a main A7
ok "server down: offline" "$(dev a sync 2>/dev/null | cut -d' ' -f1)" "OFFLINE"
ok "server down: the save's untouched" "$(get a main)" "A7"
ok "no temp files left" "$(ls "$T/a/cache" 2>/dev/null | wc -l)" "0"
ok "temp files gone from save_0" "$(ls -A "$T/a/save/save_0" | tr '\n' ' ')" "backup main "
[ $fails == 0 ] && echo PASSED || { echo "$fails FAILED"; cat "$T/srv.log" | tail -5; exit 1; }
