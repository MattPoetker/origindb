#!/usr/bin/env bash
# Deterministic Marble Clash bring-up on the rack. Two-phase so the native tick
# scheduler sees the modules at boot: empty boot -> deploy -> restart with tick.
# Usage: run_rack.sh <arenas> <hz> <substeps> <threads> <loglevel>
set -u
cd ~/instant_db
BIN=./build/origindb_server
CLI="./build/origindb_client -s localhost:50054"
ARENAS=${1:-8}; HZ=${2:-60}; SUB=${3:-1}; THREADS=${4:-8}; LL=${5:-info}
WASM=sdk/typescript/build/marbles.wasm

echo "[run] killing old + wiping"
pgrep -f "origindb_server -d marbles" | xargs -r kill -9 2>/dev/null || true
sleep 2
rm -rf marbles_data && mkdir -p marbles_data/db

echo "[run] phase1: empty boot + deploy $ARENAS modules"
setsid nohup $BIN -d marbles_data/db -p 8790 -g 50054 --no-auth --sync-mode none -l warn </dev/null >marbles_run.log 2>&1 &
until ss -ltn | grep -q ":8790 "; do sleep 1; done
for i in $(seq 0 $((ARENAS-1))); do $CLI deploy marble_$i $WASM 1.0.3 >/dev/null 2>&1; done
N=$($CLI modules 2>/dev/null | grep -c marble_)
echo "[run] deployed=$N"
pgrep -f "origindb_server -d marbles" | xargs -r kill -9 2>/dev/null || true
sleep 2

echo "[run] phase2: tick server arenas=$ARENAS hz=$HZ sub=$SUB threads=$THREADS ll=$LL"
setsid nohup $BIN -d marbles_data/db -p 8790 -g 50054 --no-auth --sync-mode none -l $LL \
  --tick-modules marble_ --tick-count $ARENAS --tick-hz $HZ --tick-substeps $SUB --tick-threads $THREADS \
  </dev/null >marbles_run.log 2>&1 &
until ss -ltn | grep -q ":8790 "; do sleep 1; done
sleep 1
echo "[run] TICK SERVER UP (pid $(pgrep -f 'origindb_server -d marbles' | head -1))"
