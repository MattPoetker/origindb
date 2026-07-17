#!/usr/bin/env bash
# End-to-end verification of the WASM pipeline:
#   build -> start server -> deploy module -> execute reducer -> SQL verify ->
#   WHERE-filtered websocket delivery -> persistence across restart -> undeploy
#
# Usage: scripts/e2e_verify.sh [BUILD_DIR]   (default: build_new)
set -euo pipefail

BUILD_DIR="${1:-build_new}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

WS_PORT=18080
GRPC_PORT=15051
DATA_DIR="$(mktemp -d)"
SERVER_LOG="$DATA_DIR/server.log"
CLI="$BUILD_DIR/origindb_client"
SERVER_PID=""

cleanup() {
    [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null || true
    rm -rf "$DATA_DIR"
}
trap cleanup EXIT

fail() { echo "❌ FAIL: $1"; exit 1; }
step() { echo; echo "=== $1 ==="; }

step "Build"
cmake --build "$BUILD_DIR" --target origindb_server origindb_client wat2wasm_tool -j8 > /dev/null

step "Compile test module (WAT -> wasm)"
"$BUILD_DIR/wat2wasm_tool" tests/wasm/fixtures/test_module.wat "$DATA_DIR/test_module.wasm"

BOOT=0
start_server() {
    BOOT=$((BOOT + 1))
    SERVER_LOG="$DATA_DIR/server_boot$BOOT.log"
    "$BUILD_DIR/origindb_server" -d "$DATA_DIR/db" -p $WS_PORT -g $GRPC_PORT \
        > "$SERVER_LOG" 2>&1 &
    SERVER_PID=$!
    for _ in $(seq 1 50); do
        grep -q "gRPC Server ready" "$SERVER_LOG" && return 0
        sleep 0.2
    done
    fail "server did not start (see $SERVER_LOG)"
}

step "Start server"
start_server

step "Deploy module"
"$CLI" -s localhost:$GRPC_PORT deploy testmod "$DATA_DIR/test_module.wasm" 1.0.0 \
    || fail "deploy"
"$CLI" -s localhost:$GRPC_PORT modules | grep -q testmod || fail "module not listed"

step "Execute reducer + verify via SQL"
"$CLI" -s localhost:$GRPC_PORT call testmod w || fail "reducer execution"
"$CLI" -s localhost:$GRPC_PORT exec "SELECT * FROM t" | grep -q "k1" \
    || fail "row not visible via SQL"

step "WHERE-filtered websocket delivery"
python3 scripts/ws_filter_check.py "$ROOT/$CLI" $WS_PORT $GRPC_PORT \
    || fail "websocket WHERE filtering"

step "Persistence across restart"
kill "$SERVER_PID"; wait "$SERVER_PID" 2>/dev/null || true; SERVER_PID=""
start_server
grep -q "Restored persisted module: testmod" "$SERVER_LOG" \
    || fail "module not restored after restart"
"$CLI" -s localhost:$GRPC_PORT exec "SELECT * FROM t" | grep -q "k1" \
    || fail "table data not recovered from WAL"

step "Undeploy"
"$CLI" -s localhost:$GRPC_PORT undeploy testmod || fail "undeploy"
[ ! -d "$DATA_DIR/db/modules/testmod" ] || fail "module files not removed"

echo
echo "🎉 ALL E2E CHECKS PASSED"
