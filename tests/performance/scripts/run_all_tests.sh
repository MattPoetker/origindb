#!/bin/bash

# InstantDB Performance Testing Suite Runner
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/tests/performance"
REPORTS_DIR="${SCRIPT_DIR}/../reports"

echo "🧪 InstantDB Comprehensive Performance Test Suite"
echo "================================================"

# Create reports directory
mkdir -p "${REPORTS_DIR}"

# Ensure the build directory exists
if [ ! -d "${BUILD_DIR}" ]; then
    echo "❌ Build directory not found: ${BUILD_DIR}"
    echo "Please run 'make' from the project root first"
    exit 1
fi

# Check if performance test runner exists
if [ ! -x "${BUILD_DIR}/perf_test_runner" ]; then
    echo "❌ Performance test runner not found: ${BUILD_DIR}/perf_test_runner"
    echo "Please ensure the performance tests are built successfully"
    exit 1
fi

# Function to run a test and handle results
run_test() {
    local test_type="$1"
    local test_name="$2"
    local duration="$3"
    local threads="$4"
    local extra_args="$5"

    echo ""
    echo "🔥 Running $test_name..."
    echo "   Test Type: $test_type"
    echo "   Duration: ${duration}s"
    echo "   Threads: $threads"

    local output_file="${REPORTS_DIR}/${test_type}_$(date +%Y%m%d_%H%M%S).json"

    if "${BUILD_DIR}/perf_test_runner" \
        --test "$test_type" \
        --threads "$threads" \
        --duration "$duration" \
        --warmup 5 \
        --output "$output_file" \
        --verbose \
        $extra_args; then
        echo "✅ $test_name completed successfully"
        echo "   Results: $output_file"
    else
        echo "❌ $test_name failed"
        return 1
    fi
}

# Check if InstantDB server is running
if ! curl -s "http://localhost:9090" > /dev/null 2>&1; then
    echo "⚠️  InstantDB server not detected on localhost:9090"
    echo "   Starting server..."

    # Try to start the server
    if [ -x "${PROJECT_ROOT}/instantdb_server" ]; then
        "${PROJECT_ROOT}/instantdb_server" -p 9090 &
        SERVER_PID=$!
        echo "   Started server with PID: $SERVER_PID"
        sleep 5
    else
        echo "❌ Cannot find instantdb_server binary"
        exit 1
    fi
fi

# Configure test parameters
DEFAULT_DURATION=30
DEFAULT_THREADS=4

echo ""
echo "📊 Starting Performance Test Suite..."
echo "   Default Duration: ${DEFAULT_DURATION}s"
echo "   Default Threads: ${DEFAULT_THREADS}"

# Storage Engine Tests
echo ""
echo "🗄️  STORAGE ENGINE TESTS"
echo "========================"

run_test "storage-crud" "Storage CRUD Operations" $DEFAULT_DURATION $DEFAULT_THREADS
run_test "storage-batch" "Storage Batch Operations" $DEFAULT_DURATION $DEFAULT_THREADS "--batch-size 50"
run_test "storage-transaction" "Storage Transactions" $DEFAULT_DURATION $DEFAULT_THREADS
run_test "storage-concurrency" "Storage Concurrency Stress" $DEFAULT_DURATION 8

# WebSocket Tests (if WebSocket support is enabled)
echo ""
echo "🌐 WEBSOCKET TESTS"
echo "=================="

if command -v nc >/dev/null 2>&1 && nc -z localhost 9090; then
    run_test "websocket-scaling" "WebSocket Connection Scaling" $DEFAULT_DURATION $DEFAULT_THREADS "--connections 100"
    run_test "websocket-throughput" "WebSocket Message Throughput" $DEFAULT_DURATION $DEFAULT_THREADS "--message-size 512"
    run_test "websocket-latency" "WebSocket Latency Test" $DEFAULT_DURATION 2
else
    echo "⚠️  WebSocket server not available, skipping WebSocket tests"
fi

# gRPC Tests
echo ""
echo "⚡ gRPC TESTS"
echo "============"

# Check if gRPC server is available
if command -v grpcurl >/dev/null 2>&1; then
    if grpcurl -plaintext localhost:50051 list >/dev/null 2>&1; then
        run_test "grpc-sql" "gRPC SQL Queries" $DEFAULT_DURATION $DEFAULT_THREADS
        run_test "grpc-transaction" "gRPC Transactions" $DEFAULT_DURATION $DEFAULT_THREADS
        run_test "grpc-mixed" "gRPC Mixed Workload" $DEFAULT_DURATION $DEFAULT_THREADS
    else
        echo "⚠️  gRPC server not available on localhost:50051, skipping gRPC tests"
    fi
else
    echo "⚠️  grpcurl not available, skipping gRPC tests"
fi

# Integration Tests
echo ""
echo "🔧 INTEGRATION TESTS"
echo "===================="

run_test "integration-full" "Full System Integration" 60 8

# Generate comprehensive report
echo ""
echo "📋 GENERATING PERFORMANCE REPORTS"
echo "================================="

if [ -x "${BUILD_DIR}/benchmark_compare" ]; then
    echo "Creating HTML performance report..."
    "${BUILD_DIR}/benchmark_compare" \
        --input "${REPORTS_DIR}" \
        --output "${REPORTS_DIR}/performance_report.html" \
        --format html

    echo "Creating text summary..."
    "${BUILD_DIR}/benchmark_compare" \
        --input "${REPORTS_DIR}" \
        --output "${REPORTS_DIR}/performance_summary.txt" \
        --format text

    echo "Creating CSV export..."
    "${BUILD_DIR}/benchmark_compare" \
        --input "${REPORTS_DIR}" \
        --output "${REPORTS_DIR}/performance_data.csv" \
        --format csv

    echo "Creating top performers report..."
    "${BUILD_DIR}/benchmark_compare" \
        --input "${REPORTS_DIR}" \
        --output "${REPORTS_DIR}/top_performers.txt" \
        --format top

    echo ""
    echo "📊 Reports generated in: ${REPORTS_DIR}"
    echo "   📄 HTML Report: performance_report.html"
    echo "   📝 Text Summary: performance_summary.txt"
    echo "   📈 CSV Data: performance_data.csv"
    echo "   🏆 Top Performers: top_performers.txt"
else
    echo "⚠️  benchmark_compare tool not available, skipping report generation"
fi

# Cleanup
if [ ! -z "$SERVER_PID" ]; then
    echo ""
    echo "🧹 Cleaning up..."
    echo "   Stopping server (PID: $SERVER_PID)"
    kill $SERVER_PID 2>/dev/null || true
fi

echo ""
echo "✅ Performance test suite completed successfully!"
echo ""
echo "📊 SUMMARY"
echo "=========="
echo "   Reports Directory: ${REPORTS_DIR}"
echo "   Test Results: $(ls -1 ${REPORTS_DIR}/*.json 2>/dev/null | wc -l | tr -d ' ') test files"
echo ""
echo "🔍 To view results:"
echo "   open ${REPORTS_DIR}/performance_report.html"
echo "   cat ${REPORTS_DIR}/performance_summary.txt"
echo ""