#!/bin/bash
# The fast LBM layer (docs/canned-tests-plan.md, step LF): LBM6's headless arms on the coordinator's one
# virtual time, each as off/on twins like the real-time gate; then scripts/lbm_fast_judge.py.
#   PREFIX=lf1 [ARMS="a0_first:A0 a11:A11 ..."] [MASK=0xFFFF] [ROOT=game dir] bash scripts/lbm_fast.sh
# The first arm should be an A0 (the control the others are judged against, CONTROL=<its _off dir> otherwise).
# Run from a build tree with BallanceMMOServer, BallanceMMOSessionClient, BallanceMMOCoordinator and SimTool in
# build-agent/BallanceMMOServer (BIN=...). The server runs as a copy (BallanceMMOServerT1): the original may
# have page heap switched on through IFEO.
W=$(cd "$(dirname "$0")/.." && pwd)
A=$W/maps/arena-test
BIN=${BIN:-build-agent/BallanceMMOServer}
ROOT=${ROOT:-C:/Users/geekerwan/Desktop/Ballance-Agent}
if [ -r /proc/$$/winpid ]; then   # Windows (Git Bash): the given cores, below normal
  winpid=$(cat /proc/$$/winpid)
  powershell -NoProfile -Command "\$p=Get-Process -Id $winpid; \$p.ProcessorAffinity=${MASK:-0xFFFF}; \$p.PriorityClass='BelowNormal'"
fi
cd "$W"
EXE=; [ -f "$BIN/BallanceMMOServer.exe" ] && EXE=.exe
cp -f "$BIN/BallanceMMOServer$EXE" "$BIN/BallanceMMOServerT1$EXE"
P=${PREFIX:-lf}
ARMS=${ARMS:-"a0_first:A0 a0_second:A0 a6b:A6B a6m:A6M a6m_death:A6M a6m_superseded:A6M a6_drop:A6 a4_swing_seed0:A4 a4_swing_seed1:A4 a4_swing_seed2:A4 a8_r1:A8 a8_r2:A8 a8_r3:A8 a11:A11 a11u:A11U a5d_resync:A5D a10x:A10X a5ub_r1:A5UB a5ub_r2:A5UB"}
H="python -X utf8 -I scripts/arena_map_test.py --bin $BIN --root $ROOT --port ${PORT:-26997} --coordinated --server-exe BallanceMMOServerT1 --client-arg=--final-checkpoints --client-arg=--ignore-snapshots --client-arg=--record-by-tick --seconds 60 --startup-timeout 90 --drive-stop 1750 --twins"
CONTROL=${CONTROL:-$A/${P}_a0_first_off}
labels=
for spec in $ARMS; do
  key=${spec%%:*}
  scenario=${spec#*:}
  label=${P}_$key
  case $key in   # LBM6's arguments per arm (.local-tools/work/LBM6/commands.jsonl)
    a0_*) extra="";;
    a6b) extra="--control $CONTROL --client-arg-for '2:--beam 74 4 15 900 --beam 104 4 15 1500' --window-origin 1500";;
    a6_drop) extra="--control $CONTROL --hold drop --client-arg-for '2:--beam 74 4 15 1500' --window-origin 1500";;
    a6m_death) extra="--control $CONTROL --client-arg-for '2:--beam 74 4 43 1500 --hold-event unphysicalize:2:300'";;
    a6m_superseded) extra="--control $CONTROL --client-arg-for '2:--beam 74 4 43 1500 --hold-event pair:1:300' --window-origin 1500";;
    a6m) extra="--control $CONTROL --client-arg-for '2:--beam 74 4 15 1500' --window-origin 1500";;
    a4_swing_seed*) extra="--control $CONTROL --variant swing --loss-pct 10 --loss-burst 8 --loss-seed ${key##*seed}";;
    a5d_resync) extra="--control $CONTROL --client-arg-for '2:--resync-at 2900'";;
    *) extra="--control $CONTROL";;
  esac
  start=$(date +%s)
  echo "== $label ($scenario) $(date +%T)"
  eval $H --scenario $scenario --label $label $extra > "$A/$label.harness.out" 2>&1
  echo "   rc $? in $(( $(date +%s) - start )) s"
  grep -E "coordinator: done|Traceback" "$A/$label.harness.out" | cut -c1-160
  labels="$labels $label"
done
python -X utf8 -I scripts/lbm_fast_judge.py --root "$ROOT" --simtool "$BIN/BallanceMMOSimTool$EXE" $labels
