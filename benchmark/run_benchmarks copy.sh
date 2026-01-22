#!/bin/bash
# =============================================================================
# Quartz Performance Benchmark Runner
# =============================================================================
# Comprehensive benchmark suite for evaluating Quartz VM performance
#
# Usage:
#   ./run_benchmarks.sh                    # Run all benchmarks
#   ./run_benchmarks.sh --suite micro      # Run specific suite
#   ./run_benchmarks.sh --quick            # Quick smoke test
#   ./run_benchmarks.sh --verbose          # Detailed output
#   ./run_benchmarks.sh --output results/  # Custom output directory
# =============================================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
QUARTZ_ROOT="$(dirname "$SCRIPT_DIR")"
QUARTZ="$QUARTZ_ROOT/build/quartz"
RESULTS_DIR="$SCRIPT_DIR/results"
TIMESTAMP=$(date +%Y-%m-%d_%H-%M-%S)

# Default settings
SUITE="all"
WARMUP_RUNS=2
BENCHMARK_RUNS=5
VERBOSE=false
QUICK=false

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --suite)
            SUITE="$2"
            shift 2
            ;;
        --runs)
            BENCHMARK_RUNS="$2"
            shift 2
            ;;
        --warmup)
            WARMUP_RUNS="$2"
            shift 2
            ;;
        --verbose|-v)
            VERBOSE=true
            shift
            ;;
        --quick|-q)
            QUICK=true
            WARMUP_RUNS=1
            BENCHMARK_RUNS=3
            shift
            ;;
        --output|-o)
            RESULTS_DIR="$2"
            shift 2
            ;;
        --help|-h)
            echo "Usage: $0 [OPTIONS]"
            echo ""
            echo "Options:"
            echo "  --suite SUITE   Run specific suite (micro, compute, memory, realworld, all)"
            echo "  --runs N        Number of benchmark runs (default: 5)"
            echo "  --warmup N      Number of warmup runs (default: 2)"
            echo "  --verbose, -v   Verbose output"
            echo "  --quick, -q     Quick mode (fewer runs)"
            echo "  --output, -o    Output directory for results"
            echo "  --help, -h      Show this help"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

# Ensure quartz is built
if [[ ! -x "$QUARTZ" ]]; then
    echo -e "${RED}Error: Quartz binary not found at $QUARTZ${NC}"
    echo "Please build Quartz first: cd $QUARTZ_ROOT && mkdir -p build && cd build && cmake .. && make"
    exit 1
fi

# Create results directory
RUN_RESULTS_DIR="$RESULTS_DIR/$TIMESTAMP"
mkdir -p "$RUN_RESULTS_DIR"

# Update latest symlink
rm -f "$RESULTS_DIR/latest"
ln -sf "$TIMESTAMP" "$RESULTS_DIR/latest"

echo -e "${CYAN}=============================================="
echo "   Quartz Performance Benchmark Suite"
echo "=============================================="
echo -e "${NC}"
echo "Quartz:    $QUARTZ"
echo "Suite:     $SUITE"
echo "Runs:      $BENCHMARK_RUNS (warmup: $WARMUP_RUNS)"
echo "Output:    $RUN_RESULTS_DIR"
echo ""

# Function to run a single benchmark
run_benchmark() {
    local name="$1"
    local file="$2"
    local category="$3"
    
    if [[ ! -f "$file" ]]; then
        echo -e "${YELLOW}  [SKIP] $name - file not found${NC}"
        return
    fi
    
    # Compile first
    local bytecode="${file%.qz}.qzb"
    if ! "$QUARTZ" --compile "$file" -o "$bytecode" > /dev/null 2>&1; then
        echo -e "${RED}  [FAIL] $name - compilation failed${NC}"
        return
    fi
    
    # Warmup runs
    for ((i=1; i<=WARMUP_RUNS; i++)); do
        "$QUARTZ" --run-bc "$bytecode" > /dev/null 2>&1 || true
    done
    
    # Timed runs using python for accurate timing (avoids bash integer overflow)
    local total_time=0
    local times=()
    
    for ((i=1; i<=BENCHMARK_RUNS; i++)); do
        local elapsed_ms
        elapsed_ms=$(python3 -c "
import time, subprocess
start = time.perf_counter()
subprocess.run(['$QUARTZ', '--run-bc', '$bytecode'], capture_output=True)
end = time.perf_counter()
print(max(0, int((end - start) * 1000)))
" 2>/dev/null)
        # Validate that we got a valid number
        if [[ ! "$elapsed_ms" =~ ^[0-9]+$ ]]; then
            elapsed_ms=0
        fi
        times+=($elapsed_ms)
        total_time=$((total_time + elapsed_ms))
    done
    
    # Calculate statistics
    local avg_time=$((total_time / BENCHMARK_RUNS))
    
    # Calculate standard deviation
    local sum_sq=0
    for t in "${times[@]}"; do
        local diff=$((t - avg_time))
        sum_sq=$((sum_sq + diff * diff))
    done
    local variance=$((sum_sq / BENCHMARK_RUNS))
    local std_dev=$(echo "scale=2; sqrt($variance)" | bc 2>/dev/null || echo "0")
    
    # Find min/max (use arithmetic comparison with default values)
    local min_time=${times[0]:-0}
    local max_time=${times[0]:-0}
    for t in "${times[@]}"; do
        if [[ -n "$t" && "$t" =~ ^[0-9]+$ ]]; then
            (( t < min_time )) && min_time=$t
            (( t > max_time )) && max_time=$t
        fi
    done
    
    # Output result
    printf "  %-45s %6d ms (±%s, min:%d, max:%d)\n" "$name" "$avg_time" "$std_dev" "$min_time" "$max_time"
    
    # Save to CSV
    echo "$category,$name,$avg_time,$std_dev,$min_time,$max_time,$BENCHMARK_RUNS" >> "$RUN_RESULTS_DIR/results.csv"
    
    # Cleanup bytecode
    rm -f "$bytecode"
}

# Function to run a benchmark suite
run_suite() {
    local suite_name="$1"
    local suite_dir="$SCRIPT_DIR/$suite_name"
    
    if [[ ! -d "$suite_dir" ]]; then
        echo -e "${YELLOW}Suite directory not found: $suite_dir${NC}"
        return
    fi
    
    echo -e "${BLUE}--- $suite_name Benchmarks ---${NC}"
    echo ""
    
    for file in "$suite_dir"/*.qz; do
        [[ -f "$file" ]] || continue
        local name=$(basename "$file" .qz)
        run_benchmark "$name" "$file" "$suite_name"
    done
    
    echo ""
}

# Initialize results CSV
echo "category,name,avg_ms,std_dev,min_ms,max_ms,runs" > "$RUN_RESULTS_DIR/results.csv"

# Record system info
{
    echo "Benchmark Run: $TIMESTAMP"
    echo "System: $(uname -a)"
    echo "CPU: $(grep 'model name' /proc/cpuinfo 2>/dev/null | head -1 | cut -d: -f2 | xargs || echo 'Unknown')"
    echo "Memory: $(free -h 2>/dev/null | grep Mem | awk '{print $2}' || echo 'Unknown')"
    echo "Quartz: $($QUARTZ --version 2>/dev/null || echo 'Unknown version')"
    echo ""
} > "$RUN_RESULTS_DIR/system_info.txt"

# Run benchmarks based on suite selection
case $SUITE in
    all)
        run_suite "micro"
        run_suite "compute"
        run_suite "memory"
        run_suite "realworld"
        ;;
    micro|compute|memory|realworld)
        run_suite "$SUITE"
        ;;
    *)
        echo -e "${RED}Unknown suite: $SUITE${NC}"
        exit 1
        ;;
esac

# Generate summary
echo -e "${CYAN}=============================================="
echo "   Summary"
echo "=============================================="
echo -e "${NC}"

if [[ -f "$RUN_RESULTS_DIR/results.csv" ]]; then
    echo "Results saved to: $RUN_RESULTS_DIR"
    echo ""
    
    # Show top 5 slowest benchmarks
    echo "Slowest benchmarks:"
    tail -n +2 "$RUN_RESULTS_DIR/results.csv" | sort -t, -k3 -rn | head -5 | while IFS=, read -r cat name avg std min max runs; do
        printf "  %-45s %6d ms\n" "$name" "$avg"
    done
    
    echo ""
    
    # Show category averages
    echo "Category averages:"
    for cat in micro compute memory realworld; do
        avg=$(grep "^$cat," "$RUN_RESULTS_DIR/results.csv" 2>/dev/null | awk -F, '{sum+=$3; count++} END {if(count>0) printf "%d", sum/count; else print "N/A"}')
        [[ "$avg" != "N/A" && -n "$avg" ]] && printf "  %-15s %6d ms\n" "$cat" "$avg"
    done
fi

echo ""
echo -e "${GREEN}Benchmark complete!${NC}"
