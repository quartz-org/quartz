#!/bin/bash
# Test script to verify JIT compilation is working

set -e

echo "=========================================="
echo "Testing JIT Compilation"
echo "=========================================="
echo ""

# Check if quartz is built
if [ ! -f "./build/quartz" ]; then
    echo "Error: quartz binary not found. Please build first."
    echo "Run: ./build.sh"
    exit 1
fi

# Enable JIT debug output
export QZ_JIT_DEBUG=1

echo "1. Compiling test_jit.qz to bytecode..."
./build/quartz --compile test_jit.qz -o test_jit.qzb
echo "✓ Compilation successful"
echo ""

echo "2. Running with JIT (should see JIT compilation messages)..."
echo "Expected output:"
echo "  [JIT] Loop detected in function #X, forcing immediate compilation"
echo "  [JIT] Compiling function #X"
echo "  [JIT] Successfully compiled function #X -> Y bytes native code"
echo "  [JIT] Executing function #X with JIT-compiled code"
echo ""
echo "Actual output:"
./build/quartz --run-bc test_jit.qzb 2>&1 | tee /tmp/jit_test_output.txt
echo ""

# Check if JIT messages appeared
if grep -q "\[JIT\]" /tmp/jit_test_output.txt; then
    echo "✓ JIT messages detected - JIT is working!"
else
    echo "✗ No JIT messages detected - JIT may not be enabled"
    echo ""
    echo "Checking build configuration..."
    grep "JIT" build/CMakeCache.txt | grep -i enabled || echo "  JIT not found in CMake cache"
fi

echo ""
echo "=========================================="
echo "Test Complete"
echo "=========================================="

# Cleanup
rm -f test_jit.qzb /tmp/jit_test_output.txt
