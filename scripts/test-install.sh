#!/bin/bash
# Test script to simulate one-liner installation

set -e

echo "========================================="
echo "Testing InstantDB One-Liner Installation"
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
echo "Command: instantdb_server --help"
echo "-----------------------------------------"
instantdb_server --help | head -10
echo ""

echo "Test 3: Check installation path"
echo "Command: which instantdb_server"
echo "-----------------------------------------"
which instantdb_server
echo ""

echo "========================================="
echo "✅ Installation tests completed!"
echo ""
echo "To test the actual one-liner experience:"
echo "  curl -sSf https://install.instantdb.com | sh"
echo ""
echo "For local testing with downloaded script:"
echo "  ./scripts/install.sh --local"
echo ""
echo "To start the server:"
echo "  instantdb_server"
echo "========================================="