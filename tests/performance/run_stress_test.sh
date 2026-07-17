#!/bin/bash

# OriginDB Stress Test Runner
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build/tests/performance"
REPORTS_DIR="${SCRIPT_DIR}/../reports"

echo "🔥 OriginDB Stress Test Runner"
echo "==============================="

# Create reports directory
mkdir -p "${REPORTS_DIR}"

# Check if performance test runner exists
if [ ! -x "${BUILD_DIR}/perf_test_runner" ]; then
    echo "❌ Performance test runner not found: ${BUILD_DIR}/perf_test_runner"
    echo "Please ensure the performance tests are built successfully"
    exit 1
fi

# Check if OriginDB server is running
if ! curl -s "http://localhost:9090" > /dev/null 2>&1; then
    echo "⚠️  OriginDB server not detected on localhost:9090"
    echo "   Starting server..."

    # Try to start the server
    if [ -x "${PROJECT_ROOT}/origindb_server" ]; then
        "${PROJECT_ROOT}/origindb_server" -p 9090 &
        SERVER_PID=$!
        echo "   Started server with PID: $SERVER_PID"
        sleep 5
    else
        echo "❌ Cannot find origindb_server binary"
        exit 1
    fi
fi

echo ""
echo "🔥 Running STRESS TESTS - High load scenarios"
echo "Duration: 300 seconds (5 minutes)"
echo "Threads: 16-32 concurrent threads"
echo ""

# High-intensity storage stress test
echo "1. 🗄️  Storage Engine Extreme Load"
"${BUILD_DIR}/perf_test_runner" \
    --test storage-concurrency \
    --threads 32 \
    --duration 300 \
    --warmup 30 \
    --output "${REPORTS_DIR}/stress_storage_$(date +%Y%m%d_%H%M%S).json" \
    --verbose || echo "Storage stress test failed"

# High-connection WebSocket test
echo ""
echo "2. 🌐 WebSocket Connection Storm"
"${BUILD_DIR}/perf_test_runner" \
    --test websocket-scaling \
    --threads 16 \
    --duration 300 \
    --connections 2000 \
    --output "${REPORTS_DIR}/stress_websocket_$(date +%Y%m%d_%H%M%S).json" \
    --verbose || echo "WebSocket stress test failed"

# gRPC load test (if available)
if command -v grpcurl >/dev/null 2>&1; then
    if grpcurl -plaintext localhost:50051 list >/dev/null 2>&1; then
        echo ""
        echo "3. ⚡ gRPC High-Throughput Load"
        "${BUILD_DIR}/perf_test_runner" \
            --test grpc-mixed \
            --threads 20 \
            --duration 300 \
            --target-ops 1000 \
            --output "${REPORTS_DIR}/stress_grpc_$(date +%Y%m%d_%H%M%S).json" \
            --verbose || echo "gRPC stress test failed"
    else
        echo "⚠️  gRPC server not available, skipping gRPC stress test"
    fi
else
    echo "⚠️  grpcurl not available, skipping gRPC stress test"
fi

# Memory pressure test
echo ""
echo "4. 🧠 Memory Pressure Test"
"${BUILD_DIR}/perf_test_runner" \
    --test storage-memory \
    --threads 16 \
    --duration 240 \
    --output "${REPORTS_DIR}/stress_memory_$(date +%Y%m%d_%H%M%S).json" \
    --verbose || echo "Memory pressure test failed"

# Cleanup
if [ ! -z "$SERVER_PID" ]; then
    echo ""
    echo "🧹 Cleaning up..."
    echo "   Stopping server (PID: $SERVER_PID)"
    kill $SERVER_PID 2>/dev/null || true
fi

echo ""
echo "✅ Stress testing completed!"
echo ""
echo "📊 Results saved in: ${REPORTS_DIR}"
echo "   $(ls -1 ${REPORTS_DIR}/stress_*.json 2>/dev/null | wc -l | tr -d ' ') stress test results"
echo ""