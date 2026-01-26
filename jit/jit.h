/**
 * =============================================================================
 * Quartz Copy-and-Patch JIT Compiler
 * =============================================================================
 *
 * A lightweight, portable JIT compiler using the copy-and-patch technique.
 * 
 * Features:
 * - No external dependencies (no LLVM, no Cranelift)
 * - Portable across architectures (stencils compiled per-arch at build time)
 * - Fast compilation (just memcpy + patch)
 * - Good runtime performance (compiler-optimized stencils)
 * 
 * How it works:
 * 1. Pre-compiled code templates ("stencils") are embedded in the binary
 * 2. At runtime, stencils are copied to executable memory
 * 3. "Holes" in the stencils are patched with concrete values
 * 4. The patched code is executed directly
 */

#ifndef QZ_JIT_H
#define QZ_JIT_H

#include <cstdint>
#include <cstddef>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <variant>  // For Value type

// Include types.h for Value definition
#include "core/types.h"

// Forward declarations
namespace bc {
    struct Function;
    struct Program;
    enum class OpCode : uint8_t;
    enum class BinaryOp : uint8_t;
    enum class UnaryOp : uint8_t;
}

class Runtime;

namespace qz::jit {

// =============================================================================
// Configuration
// =============================================================================

// Default JIT threshold (calls before compilation)
constexpr uint32_t kDefaultJITThreshold = 1;

// Maximum compiled function size (bytes)
constexpr size_t kMaxCompiledSize = 1024 * 1024;  // 1 MB

// Code alignment for cache efficiency
constexpr size_t kCodeAlignment = 16;

// =============================================================================
// JIT Value Type (matches stencil layout)
// =============================================================================

struct alignas(16) JITValue {
    uint64_t bits;      // Raw bits (int64, double bits, or pointer)
    uint8_t  tag;       // Type tag
    uint8_t  _pad[7];   // Padding to 16 bytes
    
    // Type tags
    static constexpr uint8_t TAG_INT    = 0;
    static constexpr uint8_t TAG_DOUBLE = 1;
    static constexpr uint8_t TAG_BOOL   = 2;
    static constexpr uint8_t TAG_STRING = 3;
    static constexpr uint8_t TAG_ARRAY  = 4;
    static constexpr uint8_t TAG_DICT   = 5;
    
    // Constructors
    static JITValue makeInt(int64_t v) {
        JITValue val;
        val.bits = static_cast<uint64_t>(v);
        val.tag = TAG_INT;
        return val;
    }
    
    static JITValue makeDouble(double v) {
        JITValue val;
        val.tag = TAG_DOUBLE;
        *reinterpret_cast<double*>(&val.bits) = v;
        return val;
    }
    
    static JITValue makeBool(bool v) {
        JITValue val;
        val.bits = v ? 1 : 0;
        val.tag = TAG_BOOL;
        return val;
    }
    
    // Accessors
    int64_t asInt() const { return static_cast<int64_t>(bits); }
    double asDouble() const { return *reinterpret_cast<const double*>(&bits); }
    bool asBool() const { return bits != 0; }
};

static_assert(sizeof(JITValue) == 16, "JITValue must be 16 bytes");

// =============================================================================
// Executable Memory Region
// =============================================================================

class CodeRegion {
public:
    CodeRegion();
    ~CodeRegion();
    
    // Non-copyable
    CodeRegion(const CodeRegion&) = delete;
    CodeRegion& operator=(const CodeRegion&) = delete;
    
    // Movable
    CodeRegion(CodeRegion&& other) noexcept;
    CodeRegion& operator=(CodeRegion&& other) noexcept;
    
    // Allocate executable memory for code
    bool allocate(size_t size);
    
    // Deallocate memory
    void deallocate();
    
    // Get code pointer
    uint8_t* data() { return code_; }
    const uint8_t* data() const { return code_; }
    
    // Get capacity
    size_t capacity() const { return capacity_; }
    
    // Check if allocated
    bool isAllocated() const { return code_ != nullptr; }
    
    // Make memory executable (after writing code)
    bool makeExecutable();
    
    // Make memory writable (for patching)
    bool makeWritable();

private:
    uint8_t* code_ = nullptr;
    size_t capacity_ = 0;
};

// =============================================================================
// Compiled Function
// =============================================================================

// Function signature: void fn(JITValue* stack, JITValue* locals, void* runtime)
using JITFunction = void (*)(JITValue* stack, JITValue* locals, void* runtime);

struct CompiledFunction {
    CodeRegion code;
    size_t codeSize = 0;
    uint32_t functionIndex = 0;
    bool isValid = false;
    std::vector<uint64_t> cacheSlots; // persistent cache slots for IC
    
    JITFunction getEntryPoint() const {
        return reinterpret_cast<JITFunction>(code.data());
    }
};

// =============================================================================
// JIT Compiler
// =============================================================================

class Compiler {
public:
    explicit Compiler(Runtime& runtime);
    ~Compiler();
    
    // Compile a bytecode function to native code
    // Returns nullptr if compilation fails or function is not suitable
    CompiledFunction* compile(const bc::Program& program, uint32_t functionIndex);
    
    // Check if a function is JIT-compilable
    bool canCompile(const bc::Function& fn);
    
    // Get compilation statistics
    struct Stats {
        uint64_t functionsCompiled = 0;
        uint64_t bytecodesCompiled = 0;
        uint64_t nativeCodeBytes = 0;
        uint64_t compilationTimeNs = 0;
    };
    const Stats& stats() const { return stats_; }

private:
    Runtime& runtime_;
    Stats stats_;
    
    // Code emission buffer
    std::vector<uint8_t> emitBuffer_;
    size_t emitOffset_ = 0;
    
    // Label/jump management
    struct Label {
        size_t targetOffset = SIZE_MAX;  // Offset in emitBuffer when resolved
        std::vector<size_t> patchSites;  // Offsets that need patching
    };
    std::unordered_map<size_t, Label> labels_;  // bytecode IP -> label
    // Register allocation for local slots
    std::unordered_map<uint16_t, uint8_t> slotToReg_;   // slot -> register index (0=R14,1=R15)
    std::unordered_map<uint8_t, uint16_t> regToSlot_;   // register index -> slot
    std::vector<uint16_t> usedSlots_;                   // slots used in function
    std::unordered_set<uint16_t> dirtySlots_;           // slots whose register value differs from memory
    std::unordered_map<size_t, size_t> cacheSlotMap_; // IP -> cache slot index
    std::vector<uint64_t> cacheSlots_;                  // storage for cache slots
    std::unordered_map<size_t, size_t> cacheSlotPlaceholders_; // slot index -> placeholder offset in emitted code (first mov rax)
    std::unordered_map<size_t, size_t> cacheSlotAddressPlaceholders_; // slot index -> placeholder offset for slot address (second mov rdx)
    // Runtime layout offsets (computed once)
    size_t offsetArrayStorage_;
    size_t arraySlotSize_;
    size_t arraySlotShift_;       // log2(arraySlotSize_) if power of two, else 0
    size_t arraySlotRefcountOffset_;
    size_t arraySlotDataOffset_;

    
    // Internal compilation methods
    void analyzeSlotUsage(const bc::Function& fn);
    void flushDirtySlots();
    size_t allocateCacheSlot(size_t ip);
    void emitCacheSlotPlaceholder(size_t slotIndex);
    void resetEmitState();
    void emitByte(uint8_t b);
    void emitBytes(const uint8_t* data, size_t len);
    void emitU64(uint64_t v);
    size_t emitImm64Placeholder(); // emits mov rax, 0x0 and returns offset of immediate
    void emitPrologue();
    void emitEpilogue();
    
    // Stencil emission with patching
    void emitStencil(size_t stencilIndex);
    void patchHole(size_t codeOffset, size_t holeOffset, uint64_t value);
    
    // Direct code emission helpers for x86-64
    void emitMovMemImm32(int8_t offset, int32_t imm);
    void emitMovMemImm8(int8_t offset, uint8_t imm);
    void emitAddRbxImm8(int8_t imm);
    void emitSubRbxImm8(int8_t imm);
    void emitSubRbxImm32(int32_t imm);
    void emitLoadLocal(uint16_t slot);
    void emitStoreLocal(uint16_t slot);
    void emitLoadSlotToXmm(uint8_t xmmReg, uint16_t slot);
    void emitStoreSlotFromXmm(uint8_t xmmReg, uint16_t slot);
    void emitLoadHotSlots();
    void emitStoreXmmToStack(uint8_t xmmReg);
    void emitLoadXmmFromStack(uint8_t xmmReg);
    
    // Array fast path helpers
    void emitArrayGetFastPath(size_t arrayId);
    void emitArrayGetFastPathFromReg(uint8_t reg);
    
    // Opcode compilation
    bool compileOpcode(const bc::Function& fn, const bc::Program& program,
                       size_t& ip, int& stackDelta);
    
    // Jump handling
    void createLabel(size_t bytecodeIP);
    void emitJumpToLabel(size_t bytecodeIP);
    void resolveLabel(size_t bytecodeIP);
    void patchJumps();
};

// =============================================================================
// JIT Engine (Main Interface)
// =============================================================================

class Engine {
public:
    explicit Engine(Runtime& runtime);
    ~Engine();
    
    // Enable/disable JIT
    void setEnabled(bool enabled) { enabled_ = enabled; }
    bool isEnabled() const { return enabled_; }
    
    // Set compilation threshold
    void setThreshold(uint32_t threshold) { threshold_ = threshold; }
    uint32_t threshold() const { return threshold_; }
    
    // Record a function call (for hotness tracking)
    void recordCall(uint32_t functionIndex);
    
    // Check if function is hot enough to compile
    bool shouldCompile(uint32_t functionIndex) const;
    
    // Get or compile a function
    // Returns nullptr if not compiled yet or compilation failed
    CompiledFunction* getCompiled(const bc::Program& program, uint32_t functionIndex);
    
    // Try to execute a function with JIT
    // Returns true if executed, false if should fall back to interpreter
    bool tryExecute(const bc::Program& program, uint32_t functionIndex,
                   JITValue* stack, JITValue* locals);
    
    // Get statistics
    struct Stats {
        uint64_t totalCalls = 0;
        uint64_t jitExecutions = 0;
        uint64_t compilations = 0;
        uint64_t compilationFailures = 0;
    };
    const Stats& stats() const { return stats_; }
    
    // Clear all compiled code
    void clear();

private:
    Runtime& runtime_;
    std::unique_ptr<Compiler> compiler_;
    bool enabled_ = true;
    uint32_t threshold_ = kDefaultJITThreshold;
    
    // Call counts per function
    std::unordered_map<uint32_t, uint32_t> callCounts_;
    
    // Compiled function cache
    std::unordered_map<uint32_t, std::unique_ptr<CompiledFunction>> compiledCache_;
    
    Stats stats_;
};

// =============================================================================
// Utility Functions
// =============================================================================

// Convert Quartz Value to JITValue
JITValue valueToJIT(const Value& v);

// Convert JITValue to Quartz Value
Value jitToValue(const JITValue& v);

} // namespace qz::jit

#endif // QZ_JIT_H
