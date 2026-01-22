#!/bin/bash
# Performance comparison test: JIT vs Interpreter
# This demonstrates the performance improvement from JIT compilation

set -e

echo "=========================================="
echo "JIT Performance Comparison Test"
echo "=========================================="
echo ""

# Check if quartz is built
if [ ! -f "./build/quartz" ]; then
    echo "Error: quartz binary not found. Please build first."
    exit 1
fi

# Create a test file with a tight loop
cat > tight_loop_test.qz << 'EOF'
// Tight loop test for performance comparison
let sum = 0;
let i = 0;
while (i < 500000) {
    sum = sum + i;
    i = i + 1;
}
sum;
EOF

# Compile to bytecode
echo "Compiling test to bytecode..."
./build/quartz --compile tight_loop_test.qz -o tight_loop_test.qzb > /dev/null 2>&1

# Test with JIT enabled (default)
echo ""
echo "Running with JIT enabled..."
echo "----------------------------------------"
export QZ_JIT_DEBUG=1
JIT_TIME=$(python3 -c "
import time, subprocess
start = time.perf_counter()
subprocess.run(['./build/quartz', '--run-bc', 'tight_loop_test.qzb'], capture_output=True)
end = time.perf_counter()
print(f'{(end - start) * 1000:.2f}')
" 2>&1 | tee /tmp/jit_output.txt | grep -v "^\[JIT\]" | tail -1)

# Extract JIT compilation info
echo ""
echo "JIT Compilation Info:"
grep "Successfully compiled" /tmp/jit_output.txt || echo "  No JIT compilation occurred"

# Test with JIT disabled
echo ""
echo "Running with JIT disabled..."
echo "----------------------------------------"
unset QZ_JIT_DEBUG
INTERP_TIME=$(python3 -c "
import time, subprocess
import os
os.environ['QZ_JIT_ENABLED'] = '0'
start = time.perf_counter()
subprocess.run(['./build/quartz', '--run-bc', 'tight_loop_test.qzb'], capture_output=True)
end = time.perf_counter()
print(f'{(end - start) * 1000:.2f}')
" 2>&1 | tail -1)

# Calculate speedup
echo ""
echo "=========================================="
echo "Performance Results"
echo "=========================================="
echo "JIT enabled:  ${JIT_TIME} ms"
echo "JIT disabled: ${INTERP_TIME} ms"
echo ""

# Calculate speedup
SPEEDUP=$(python3 -c "print(f'{float($INTERP_TIME) / float($JIT_TIME):.2f}x')")
echo "Speedup: ${SPEEDUP}x faster with JIT"
echo ""

# Expected results
echo "Expected Results:"
echo "  - JIT: ~1-5 ms (native code execution)"
echo "  - Interpreter: ~30-40 ms (bytecode interpretation)"
echo "  - Speedup: 10-30x improvement"
echo ""

# Cleanup
rm -f tight_loop_test.qz tight_loop_test.qzb /tmp/jit_output.txt

echo "=========================================="
echo "Test Complete"
echo "=========================================="
