#!/bin/bash
# Test script to simulate one-liner installation

set -e

echo "========================================="
echo "Testing OriginDB One-Liner Installation"
echo "========================================="
echo ""

# Test local installation
echo "Test 1: Local installation (for development)"
echo "Command: ./install.sh --local"
echo "-----------------------------------------"
./scripts/install.sh --local
echo ""

# Verify installation
echo "Test 2: Verify installation"
echo "Command: origindb_server --help"
echo "-----------------------------------------"
origindb_server --help | head -10
echo ""

echo "Test 3: Check installation path"
echo "Command: which origindb_server"
echo "-----------------------------------------"
which origindb_server
echo ""

echo "========================================="
echo "✅ Installation tests completed!"
echo ""
echo "To test the actual one-liner experience:"
echo "  curl -sSf https://install.origindb.com | sh"
echo ""
echo "For local testing with downloaded script:"
echo "  ./scripts/install.sh --local"
echo ""
echo "To start the server:"
echo "  origindb_server"
echo "========================================="