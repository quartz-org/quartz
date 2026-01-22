# Computed Gotos Implementation for Quartz VM

## Overview

Computed gotos is an optimization technique that eliminates branch prediction overhead in VM instruction dispatch by using a jump table instead of a switch statement. This is particularly effective for tight loops and hot code paths.

## Implementation Status

The computed gotos feature has been added to the Quartz VM with the following changes:

### 1. CMake Configuration (`CMakeLists.txt`)

Added a new build option `QZ_VM_COMPUTED_GOTOS` that:
- Is enabled by default for GCC/Clang compilers
- Is automatically disabled for MSVC (not supported)
- Adds `-DQZ_VM_COMPUTED_GOTOS=1` preprocessor definition when enabled

```cmake
option(QZ_VM_COMPUTED_GOTOS "Enable computed gotos for instruction dispatch (GCC/Clang only)" ON)
```

### 2. Build System Integration

The build system now:
- Detects MSVC and automatically disables computed gotos
- Shows "Computed gotos: ENABLED/DISABLED" in build summary
- Only compiles computed goto code when supported

## Benefits

1. **Performance**: Eliminates branch prediction overhead in instruction dispatch loop
2. **Predictable Dispatch**: Direct jump to instruction handler instead of cascading conditionals
3. **Hot Path Optimization**: Particularly effective for tight loops and frequently executed code

## Usage

### Enable Computed Gotos (Default)

```bash
./build.sh
```

Computed gotos are enabled by default on GCC/Clang systems.

### Disable Computed Gotos

```bash
./build.sh -DQZ_VM_COMPUTED_GOTOS=OFF
```

Or modify `build.conf`:

```conf
QZ_VM_COMPUTED_GOTOS=0
```

## Technical Details

### How It Works

1. **Jump Table**: A static array of label addresses for each opcode
2. **Direct Dispatch**: `goto *dispatch_table[op_code]` jumps directly to handler
3. **No Branch Prediction**: CPU doesn't need to predict branches

### Implementation Approach

The implementation uses a macro-based approach that:
- Expands to computed goto dispatch when `QZ_VM_COMPUTED_GOTOS` is defined
- Falls back to traditional switch statement for MSVC or when disabled
- Maintains compatibility with existing code structure

## Compatibility

- **GCC/Clang**: Full support with computed gotos enabled
- **MSVC**: Automatically falls back to switch statement
- **Performance**: Expected 10-30% improvement in tight loops

## Testing

To benchmark the performance difference:

```bash
# Run with computed gotos (default)
./build.sh
./benchmark/run_benchmarks.sh

# Run with traditional switch
./build.sh -DQZ_VM_COMPUTED_GOTOS=OFF
./benchmark/run_benchmarks.sh

# Compare results
./benchmark/compare_results.sh
```

## Future Enhancements

Potential improvements for computed gotos:
1. **Inline Caching**: Cache frequently used instruction handlers
2. **Superinstructions**: Fuse common instruction sequences
3. **Thread-local Dispatch**: Per-thread jump tables for parallel execution
4. **Profile-guided Optimization**: Use runtime profiling data to optimize hot paths

## References

- GCC Computed Gotos: https://gcc.gnu.org/onlinedocs/gcc/Labels-as-Values.html
- LuaJIT Dispatch: https://luajit.org/luajit.html
- Python Bytecode: https://docs.python.org/3/library/dis.html
