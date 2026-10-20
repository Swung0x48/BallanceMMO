#!/bin/bash
# $1 label  $2 latency_ms, rest = extra session-client args.
# The 4-player networked run, with client args so the v11 halves can be A/B'd
# on the real path and not only under the solo probe.
set -u
cd /d/repos/BallanceMMO
L=$1; LAT=$2; shift 2
ARGS=""
for a in "$@"; do ARGS="$ARGS --client-arg=$a"; done
powershell.exe -NoProfile -Command "wsl -d Arch -- bash /mnt/c/Users/geekerwan/AppData/Local/Temp/claude/D--repos-BallanceMMO/c8d36a4f-80ba-4c5e-971f-a7c8bf697232/scratchpad/arch_stop.sh" > /dev/null 2>&1
powershell.exe -NoProfile -Command "wsl -d Arch -- bash /mnt/c/Users/geekerwan/AppData/Local/Temp/claude/D--repos-BallanceMMO/c8d36a4f-80ba-4c5e-971f-a7c8bf697232/scratchpad/arch_server_snap.sh wsl 26700 600 66" > /dev/null 2>&1
sleep 4
echo "##### $L (lat=${LAT}ms) $* #####"
python -I scripts/arena_map_test.py --bin build-agent/BallanceMMOServer \
    --root "C:/Users/geekerwan/Desktop/Ballance-Agent" --level 13 \
    --external-server 127.0.0.1:26700 --journal-dir maps/arena-test/wsl/journals \
    --checkpoint-ticks 2 --label "$L" \
    --sectors 2 --players 4 --seconds 40 --drive --beam-bays 9,9,9,9 \
    --beam-spread 3 --latency-ms "$LAT" $ARGS 2>&1 \
  | grep -E "VERDICT|replay rc|client[0-9] rc"
S=$(ls -t maps/arena-test/wsl/journals/*.bmjr 2>/dev/null | head -1)
[ -n "$S" ] && [ -d "maps/arena-test/$L" ] && cp "$S" "maps/arena-test/$L/server.bmjr" && echo "server journal kept as maps/arena-test/$L/server.bmjr"
echo "=== done $L"
