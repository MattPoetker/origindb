#!/usr/bin/env bash
# Build and run the benchmark suite; optionally compare against a baseline.
#
#   scripts/bench.sh                      # run, write benchmarks/latest.json
#   scripts/bench.sh --baseline           # also copy result to benchmarks/baseline.json
#   scripts/bench.sh --compare            # run + compare against benchmarks/baseline.json
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
BUILD_DIR="${BUILD_DIR:-build_new}"

mkdir -p benchmarks
cmake --build "$BUILD_DIR" --target instantdb_bench -j8 > /dev/null

"$BUILD_DIR/tests/performance/instantdb_bench" --out benchmarks/latest.json

if [[ "${1:-}" == "--baseline" ]]; then
    cp benchmarks/latest.json benchmarks/baseline.json
    echo "baseline saved to benchmarks/baseline.json"
elif [[ "${1:-}" == "--compare" ]]; then
    python3 scripts/bench_compare.py benchmarks/baseline.json benchmarks/latest.json
fi
