#include <bytecode/vm.h>

#include "function_registry.h"
#include "logger.h"
#include "vm_optimizations.h"

// JIT support (conditionally included)
#ifdef QZ_JIT_ENABLED
#include "jit.h"
#endif

#include <cstring>
#include <charconv>
#include <cmath>
#include <unordered_set>
#ifdef QZ_JIT_DEBUG
#include <iostream>
#endif

// ============================================================================
// VM Configuration Constants - Tunable for performance
// ============================================================================
namespace vm_config {
    // Default stack reservation capacity to avoid frequent reallocations
    constexpr size_t kDefaultStackReserve = 256;
    
    // Reserve capacity for call arguments vector
    constexpr size_t kDefaultArgsReserve = 16;
    
    // Reserve capacity for locals vector (adjusted per function)
    constexpr size_t kDefaultLocalsReserve = 64;
    
    // Try stack reserve for exception handling frames
    constexpr size_t kDefaultTryStackReserve = 8;
}
// ============================================================================

// Fast, predictable double formatting (no locale, no allocations, compact output)
static inline void appendDoubleFast(std::string& out, double v) {
    if (std::isnan(v)) { out += "nan"; return; }
    if (std::isinf(v)) { out += (v < 0) ? "-inf" : "inf"; return; }

    char buf[64];
    auto res = std::to_chars(std::begin(buf), std::end(buf), v, std::chars_format::general);
    if (res.ec != std::errc{}) {
        out += std::to_string(v);  // Fallback
        return;
    }

    char* begin = buf;
    char* end = res.ptr;
    char* ePos = static_cast<char*>(memchr(begin, 'e', end - begin));
    if (!ePos) ePos = static_cast<char*>(memchr(begin, 'E', end - begin));
    char* dotPos = static_cast<char*>(memchr(begin, '.', (ePos ? (ePos - begin) : (end - begin))));
    if (dotPos) {
        char* trimEnd = ePos ? ePos : end;
        while (trimEnd > dotPos + 1 && *(trimEnd - 1) == '0') --trimEnd;
        if (trimEnd > dotPos && *(trimEnd - 1) == '.') --trimEnd;
        out.append(begin, trimEnd - begin);
        if (ePos) out.append(ePos, end - ePos);
        return;
    }
    out.append(begin, end - begin);
}

static inline void appendValueRepr(std::string& out, const Value& val) {
    std::visit([&out](auto&& arg) {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, int>) {
            out += std::to_string(arg);
        } else if constexpr (std::is_same_v<T, double>) {
            appendDoubleFast(out, arg);
        } else if constexpr (std::is_same_v<T, std::string>) {
            out += '"';
            out += arg;
            out += '"';
        } else if constexpr (std::is_same_v<T, bool>) {
            out += arg ? "true" : "false";
        }
    }, val);
}

static inline void appendValueToStringVM(std::string& out, const Value& val, bool quoteStrings) {
    std::visit([&out, quoteStrings](auto&& arg) {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, int>) {
            out += std::to_string(arg);
        } else if constexpr (std::is_same_v<T, double>) {
            appendDoubleFast(out, arg);
        } else if constexpr (std::is_same_v<T, std::string>) {
            if (quoteStrings) {
                out += '"';
                out += arg;
                out += '"';
            } else {
                out += arg;
            }
        } else if constexpr (std::is_same_v<T, bool>) {
            out += arg ? "true" : "false";
        }
    }, val);
}

static inline bool isStringValue(const Value& v) { return std::holds_alternative<std::string>(v); }

// ===========================================================================
// BytecodeVM Constructor / Destructor
// ===========================================================================

BytecodeVM::BytecodeVM(Runtime& rt) : runtime(rt) {
#ifdef QZ_JIT_ENABLED
    jitEngine_ = std::make_unique<qz::jit::Engine>(rt);
    // Override compile-time threshold for aggressive JIT on hot loops
    jitThreshold_ = 10;
    jitEngine_->setThreshold(jitThreshold_);
    jitEnabled_ = true;  // Enable by default when built with JIT support
#endif
}

BytecodeVM::~BytecodeVM() = default;

void BytecodeVM::setJITEnabled(bool enabled) {
    jitEnabled_ = enabled;
#ifdef QZ_JIT_ENABLED
    if (jitEngine_) {
        jitEngine_->setEnabled(enabled);
    }
#endif
}

bool BytecodeVM::isJITEnabled() const {
    return jitEnabled_;
}

void BytecodeVM::setJITThreshold(uint32_t threshold) {
    jitThreshold_ = threshold;
#ifdef QZ_JIT_ENABLED
    if (jitEngine_) {
        jitEngine_->setThreshold(threshold);
    }
#endif
}

uint32_t BytecodeVM::jitThreshold() const {
    return jitThreshold_;
}

bool BytecodeVM::tryJITExecute(uint32_t functionIndex, const Value* args, size_t argCount,
                                Value& result, std::string* error) {
#ifdef QZ_JIT_ENABLED
    if (!jitEnabled_ || !jitEngine_ || !prog) {
        return false;
    }
    
    // Record the call for hotness tracking
    jitEngine_->recordCall(functionIndex);
    
    // Check if we have compiled code or should compile now
    qz::jit::CompiledFunction* compiled = jitEngine_->getCompiled(*prog, functionIndex);
    if (!compiled || !compiled->isValid) {
#ifdef QZ_JIT_DEBUG
        std::cerr << "[JIT] Function #" << functionIndex << " not compiled, falling back to interpreter" << std::endl;
#endif
        return false;  // Fall back to interpreter
    }
    
#ifdef QZ_JIT_DEBUG
    std::cerr << "[JIT] Executing function #" << functionIndex << " with JIT-compiled code" << std::endl;
#endif
    
    const bc::Function& fn = prog->functions[functionIndex];
    
    // Prepare JIT stack and locals
    std::vector<qz::jit::JITValue> jitStack(128);
    std::vector<qz::jit::JITValue> jitLocals(fn.localNameStrings.size());
    
    // Convert arguments to JIT values and place in locals (param slots)
    for (size_t i = 0; i < argCount && i < fn.paramNameStrings.size(); ++i) {
        jitLocals[i] = qz::jit::valueToJIT(args[i]);
    }
    
    // Execute compiled code
    qz::jit::JITFunction jitFn = compiled->getEntryPoint();
    jitFn(jitStack.data(), jitLocals.data(), &runtime);
    
    // Get return value from stack top
    // For now, assume result is at stack[0] after execution
    // This matches our stencil convention
    result = qz::jit::jitToValue(jitStack[0]);
    
    return true;
#else
    (void)functionIndex;
    (void)args;
    (void)argCount;
    (void)result;
    (void)error;
    return false;  // JIT not compiled in
#endif
}

// ===========================================================================

// Branch prediction hints for hot paths
#if defined(__GNUC__) || defined(__clang__)
#define VM_LIKELY(x)   __builtin_expect(!!(x), 1)
#define VM_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define VM_LIKELY(x)   (x)
#define VM_UNLIKELY(x) (x)
#endif

// Optimized byte reading - inline for hot path performance
uint8_t BytecodeVM::readU8(const std::vector<uint8_t>& code, size_t& ip, bool* ok) {
    if (VM_UNLIKELY(ip >= code.size())) { *ok = false; return 0; }
    return code[ip++];
}

uint16_t BytecodeVM::readU16(const std::vector<uint8_t>& code, size_t& ip, bool* ok) {
    if (VM_UNLIKELY(ip + 2 > code.size())) { *ok = false; return 0; }
    // Direct memory access for better optimization
    const uint8_t* ptr = code.data() + ip;
    uint16_t v = static_cast<uint16_t>(ptr[0]) | (static_cast<uint16_t>(ptr[1]) << 8);
    ip += 2;
    return v;
}

uint32_t BytecodeVM::readU32(const std::vector<uint8_t>& code, size_t& ip, bool* ok) {
    if (VM_UNLIKELY(ip + 4 > code.size())) { *ok = false; return 0; }
    // Direct memory access for better optimization
    const uint8_t* ptr = code.data() + ip;
    uint32_t v = static_cast<uint32_t>(ptr[0]) 
               | (static_cast<uint32_t>(ptr[1]) << 8) 
               | (static_cast<uint32_t>(ptr[2]) << 16) 
               | (static_cast<uint32_t>(ptr[3]) << 24);
    ip += 4;
    return v;
}

int32_t BytecodeVM::readI32(const std::vector<uint8_t>& code, size_t& ip, bool* ok) {
    return static_cast<int32_t>(readU32(code, ip, ok));
}

double BytecodeVM::readF64(const std::vector<uint8_t>& code, size_t& ip, bool* ok) {
    if (VM_UNLIKELY(ip + 8 > code.size())) { *ok = false; return 0.0; }
    // Use direct pointer access and unrolled byte reading
    const uint8_t* ptr = code.data() + ip;
    uint64_t bits = static_cast<uint64_t>(ptr[0])
                  | (static_cast<uint64_t>(ptr[1]) << 8)
                  | (static_cast<uint64_t>(ptr[2]) << 16)
                  | (static_cast<uint64_t>(ptr[3]) << 24)
                  | (static_cast<uint64_t>(ptr[4]) << 32)
                  | (static_cast<uint64_t>(ptr[5]) << 40)
                  | (static_cast<uint64_t>(ptr[6]) << 48)
                  | (static_cast<uint64_t>(ptr[7]) << 56);
    ip += 8;
    double d;
    std::memcpy(&d, &bits, sizeof(d));
    return d;
}

// Empty string singleton for invalid lookups
static const std::string kEmptyString;

const std::string& BytecodeVM::str(uint32_t stringIndex) const {
    if (VM_UNLIKELY(!prog || stringIndex == bc::kInvalidIndex || stringIndex >= prog->strings.size())) return kEmptyString;
    return prog->strings[stringIndex];
}

bool BytecodeVM::evalCondition(const Value& v) const {
    // Fast-path: direct type checks for common types to avoid std::visit overhead
    if (const bool* b = std::get_if<bool>(&v)) {
        return *b;
    }
    if (const int* i = std::get_if<int>(&v)) {
        return *i != 0;
    }
    if (const double* d = std::get_if<double>(&v)) {
        return *d != 0.0;
    }
    if (const std::string* s = std::get_if<std::string>(&v)) {
        return !s->empty();
    }
    // Fallback for less common types (TaskRef, BufferRef, etc.)
    return std::visit([](auto&& arg) -> bool {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, TaskRef>) return true;
        else if constexpr (std::is_same_v<T, BufferRef>) return true;
        else if constexpr (std::is_same_v<T, ArrayRef>) return true;
        else if constexpr (std::is_same_v<T, DictRef>) return true;
        else return false;
    }, v);
}

// Fast-path helper for int-int binary operations (most common case)
static inline Value applyBinaryIntInt(int l, int r, bc::BinaryOp op) {
    switch (op) {
    case bc::BinaryOp::ADD: return Value(l + r);
    case bc::BinaryOp::SUB: return Value(l - r);
    case bc::BinaryOp::MUL: return Value(l * r);
    case bc::BinaryOp::DIV:
        if (r == 0) throw LanguageException("ArithmeticError", "Division by zero");
        return Value(l / r);
    case bc::BinaryOp::EQ: return Value(l == r);
    case bc::BinaryOp::NE: return Value(l != r);
    case bc::BinaryOp::LT: return Value(l < r);
    case bc::BinaryOp::GT: return Value(l > r);
    case bc::BinaryOp::LE: return Value(l <= r);
    case bc::BinaryOp::GE: return Value(l >= r);
    }
    return Value{};
}

// Fast-path helper for double-double binary operations
static inline Value applyBinaryDoubleDouble(double l, double r, bc::BinaryOp op) {
    switch (op) {
    case bc::BinaryOp::ADD: return Value(l + r);
    case bc::BinaryOp::SUB: return Value(l - r);
    case bc::BinaryOp::MUL: return Value(l * r);
    case bc::BinaryOp::DIV:
        if (r == 0.0) throw LanguageException("ArithmeticError", "Division by zero");
        return Value(l / r);
    case bc::BinaryOp::EQ: return Value(l == r);
    case bc::BinaryOp::NE: return Value(l != r);
    case bc::BinaryOp::LT: return Value(l < r);
    case bc::BinaryOp::GT: return Value(l > r);
    case bc::BinaryOp::LE: return Value(l <= r);
    case bc::BinaryOp::GE: return Value(l >= r);
    }
    return Value{};
}

Value BytecodeVM::applyBinary(const Value& left, const Value& right, bc::BinaryOp op) const {
    // Fast-path: int-int operations (most common in loops, counters, etc.)
    if (const int* li = std::get_if<int>(&left)) {
        if (const int* ri = std::get_if<int>(&right)) {
            return applyBinaryIntInt(*li, *ri, op);
        }
        // int-double promotion
        if (const double* rd = std::get_if<double>(&right)) {
            return applyBinaryDoubleDouble(static_cast<double>(*li), *rd, op);
        }
    }
    
    // Fast-path: double-double operations
    if (const double* ld = std::get_if<double>(&left)) {
        if (const double* rd = std::get_if<double>(&right)) {
            return applyBinaryDoubleDouble(*ld, *rd, op);
        }
        // double-int promotion
        if (const int* ri = std::get_if<int>(&right)) {
            return applyBinaryDoubleDouble(*ld, static_cast<double>(*ri), op);
        }
    }
    
    // Fast-path: string concatenation
    if (op == bc::BinaryOp::ADD) {
        if (std::holds_alternative<std::string>(left) || std::holds_alternative<std::string>(right)) {
            return Value(to_string(left) + to_string(right));
        }
    }
    
    // Fallback to generic visitor for remaining cases
    return std::visit([&](auto&& l, auto&& r) -> Value {
        using L = std::decay_t<decltype(l)>;
        using R = std::decay_t<decltype(r)>;

        // string concat support for interpolated strings
        if constexpr (std::is_same_v<L, std::string> || std::is_same_v<R, std::string>) {
            if (op == bc::BinaryOp::ADD) {
                return Value(to_string(left) + to_string(right));
            }
            return Value{};
        }

        if constexpr (std::is_arithmetic_v<L> && std::is_arithmetic_v<R>) {
            switch (op) {
            case bc::BinaryOp::ADD: return Value(l + r);
            case bc::BinaryOp::SUB: return Value(l - r);
            case bc::BinaryOp::MUL: return Value(l * r);
            case bc::BinaryOp::DIV:
                // Check for division by zero
                if constexpr (std::is_integral_v<R>) {
                    if (r == 0) {
                        throw LanguageException("ArithmeticError", "Division by zero");
                    }
                } else if constexpr (std::is_floating_point_v<R>) {
                    if (r == 0.0) {
                        throw LanguageException("ArithmeticError", "Division by zero");
                    }
                }
                return Value(l / r);
            case bc::BinaryOp::EQ: return Value(l == r);
            case bc::BinaryOp::NE: return Value(l != r);
            case bc::BinaryOp::LT: return Value(l < r);
            case bc::BinaryOp::GT: return Value(l > r);
            case bc::BinaryOp::LE: return Value(l <= r);
            case bc::BinaryOp::GE: return Value(l >= r);
            }
        }
        return Value{};
    }, left, right);
}

Value BytecodeVM::applyUnary(const Value& operand, bc::UnaryOp op) const {
    // Fast-path for common unary operations
    if (op == bc::UnaryOp::NEG) {
        if (const int* i = std::get_if<int>(&operand)) return Value(-*i);
        if (const double* d = std::get_if<double>(&operand)) return Value(-*d);
        return operand;
    }
    // NOT
    return Value(!evalCondition(operand));
}

Value BytecodeVM::indexGet(const std::string& varName, const Value& indexValue) {
    // Optimized array access path
    if (std::holds_alternative<int>(indexValue)) {
        auto aIt = runtime.varToArrayId.find(varName);
        if (aIt != runtime.varToArrayId.end()) {
            const int idx = std::get<int>(indexValue);
            if (idx < 0) {
                throw LanguageException("IndexError", "Array index cannot be negative: " + std::to_string(idx));
            }
            const Value* val = runtime.arrayAt(aIt->second, static_cast<size_t>(idx));
            if (val) return *val;
            // Check if it's a bounds error vs invalid array
            size_t sz = runtime.arraySize(aIt->second);
            if (sz > 0) {
                throw LanguageException("IndexError", "Array index out of bounds: " + std::to_string(idx) + " (size: " + std::to_string(sz) + ")");
            }
        }
    }
    // Optimized dict access path
    else if (std::holds_alternative<std::string>(indexValue)) {
        auto dIt = runtime.varToDictId.find(varName);
        if (dIt != runtime.varToDictId.end()) {
            const std::string& key = std::get<std::string>(indexValue);
            const Value* val = runtime.dictAt(dIt->second, key);
            if (val) return *val;
            // Check if it's a key error vs invalid dict
            if (runtime.dictSize(dIt->second) > 0 || runtime.dictData(dIt->second)) {
                throw LanguageException("KeyError", "Dictionary key not found: '" + key + "'");
            }
        }
    }

    return Value{};
}

// Fast-path indexGet using string index for cache lookup (avoids varName hashing per access)
Value BytecodeVM::indexGet(uint32_t varNameStringIndex, const Value& indexValue) {
    // Look up in container cache first (fast path)
    auto cacheIt = indexContainerCache.find(varNameStringIndex);
    
    if (cacheIt == indexContainerCache.end()) {
        // Cache miss - resolve container and cache it
        ContainerCacheEntry entry;
        const std::string& varName = str(varNameStringIndex);
        
        auto aIt = runtime.varToArrayId.find(varName);
        if (aIt != runtime.varToArrayId.end()) {
            entry.kind = ContainerCacheEntry::Array;
            entry.id = aIt->second;
        } else {
            auto dIt = runtime.varToDictId.find(varName);
            if (dIt != runtime.varToDictId.end()) {
                entry.kind = ContainerCacheEntry::Dict;
                entry.id = dIt->second;
            }
        }
        cacheIt = indexContainerCache.emplace(varNameStringIndex, entry).first;
    }
    
    const ContainerCacheEntry& entry = cacheIt->second;
    
    // Fast array access
    if (entry.kind == ContainerCacheEntry::Array) {
        if (const int* idx = std::get_if<int>(&indexValue)) {
            if (*idx < 0) {
                throw LanguageException("IndexError", "Array index cannot be negative: " + std::to_string(*idx));
            }
            const Value* val = runtime.arrayAt(entry.id, static_cast<size_t>(*idx));
            if (val) return *val;
            size_t sz = runtime.arraySize(entry.id);
            if (sz > 0) {
                throw LanguageException("IndexError", "Array index out of bounds: " + std::to_string(*idx) + " (size: " + std::to_string(sz) + ")");
            }
        }
        return Value{};
    }
    
    // Fast dict access
    if (entry.kind == ContainerCacheEntry::Dict) {
        if (const std::string* key = std::get_if<std::string>(&indexValue)) {
            const Value* val = runtime.dictAt(entry.id, *key);
            if (val) return *val;
            if (runtime.dictData(entry.id)) {
                throw LanguageException("KeyError", "Dictionary key not found: '" + *key + "'");
            }
        }
        return Value{};
    }
    
    return Value{};
}


bool BytecodeVM::execDefInterface(const std::vector<uint8_t>& code, size_t& ip, std::string* error) {
    bool ok = true;
    uint32_t nameIdx = readU32(code, ip, &ok);
    uint32_t extendsCount = readU32(code, ip, &ok);
    for (uint32_t i = 0; i < extendsCount; ++i) (void)readU32(code, ip, &ok);
    uint32_t genericCount = readU32(code, ip, &ok);
    for (uint32_t i = 0; i < genericCount; ++i) (void)readU32(code, ip, &ok);
    uint32_t methodCount = readU32(code, ip, &ok);
    for (uint32_t i = 0; i < methodCount; ++i) {
        (void)readU32(code, ip, &ok);
        (void)readU8(code, ip, &ok);
    }
    if (!ok) {
        if (error) *error = "Corrupt DEF_INTERFACE payload";
        return false;
    }

    // Keep current interpreter behavior: interfaces are registered but not enforced.
    InterfaceDef idef;
    idef.name = str(nameIdx);
    runtime.interfaceRegistry[idef.name] = idef;
    return true;
}

bool BytecodeVM::execDefClass(const std::vector<uint8_t>& code, size_t& ip, std::string* error) {
    bool ok = true;
    uint32_t nameIdx = readU32(code, ip, &ok);
    uint32_t parentIdx = readU32(code, ip, &ok);

    std::string className = str(nameIdx);
    std::string parentName = (parentIdx == bc::kInvalidIndex) ? "" : str(parentIdx);

    uint32_t ifaceCount = readU32(code, ip, &ok);
    for (uint32_t i = 0; i < ifaceCount; ++i) (void)readU32(code, ip, &ok);

    uint32_t genericCount = readU32(code, ip, &ok);
    for (uint32_t i = 0; i < genericCount; ++i) (void)readU32(code, ip, &ok);

    uint32_t fieldCount = readU32(code, ip, &ok);
    std::vector<std::string> fields;
    fields.reserve(fieldCount);
    for (uint32_t i = 0; i < fieldCount; ++i) fields.push_back(str(readU32(code, ip, &ok)));

    uint32_t staticFieldCount = readU32(code, ip, &ok);
    struct StaticInit { std::string name; bool hasInit=false; uint32_t exprFn=bc::kInvalidIndex; };
    std::vector<StaticInit> staticFields;
    for (uint32_t i = 0; i < staticFieldCount; ++i) {
        StaticInit si;
        si.name = str(readU32(code, ip, &ok));
        si.hasInit = readU8(code, ip, &ok) != 0;
        si.exprFn = readU32(code, ip, &ok);
        staticFields.push_back(si);
    }

    uint32_t methodCount = readU32(code, ip, &ok);
    std::vector<std::tuple<std::string,bool,std::vector<std::string>,uint32_t>> methods;
    methods.reserve(methodCount);
    for (uint32_t i = 0; i < methodCount; ++i) {
        std::string mname = str(readU32(code, ip, &ok));
        bool isStatic = readU8(code, ip, &ok) != 0;
        uint8_t pc = readU8(code, ip, &ok);
        std::vector<std::string> params;
        params.reserve(pc);
        for (uint8_t j = 0; j < pc; ++j) params.push_back(str(readU32(code, ip, &ok)));
        uint32_t fnIdx = readU32(code, ip, &ok);
        methods.push_back({mname, isStatic, std::move(params), fnIdx});
    }

    bool hasCtor = readU8(code, ip, &ok) != 0;
    uint32_t ctorFn = bc::kInvalidIndex;
    std::vector<std::string> ctorParams;
    std::vector<std::pair<std::string, uint32_t>> ctorFieldInits;
    if (hasCtor) {
        uint8_t pc = readU8(code, ip, &ok);
        ctorParams.reserve(pc);
        for (uint8_t i = 0; i < pc; ++i) ctorParams.push_back(str(readU32(code, ip, &ok)));
        ctorFn = readU32(code, ip, &ok);
        uint32_t initCount = readU32(code, ip, &ok);
        ctorFieldInits.reserve(initCount);
        for (uint32_t i = 0; i < initCount; ++i) {
            std::string fname = str(readU32(code, ip, &ok));
            uint32_t efn = readU32(code, ip, &ok);
            ctorFieldInits.push_back({fname, efn});
        }
    }

    if (!ok) {
        if (error) *error = "Corrupt DEF_CLASS payload";
        return false;
    }

    BCClass bcClass;
    bcClass.name = className;
    bcClass.parent = parentName;
    bcClass.fields = fields;
    // Build field name -> index map
    for (size_t i = 0; i < fields.size(); ++i) {
        bcClass.fieldToIndex[fields[i]] = i;
    }
    bcClass.hasConstructor = hasCtor;
    bcClass.ctorFunctionIndex = ctorFn;
    bcClass.ctorParams = ctorParams;
    bcClass.ctorFieldInits = ctorFieldInits;
    bcClass.modulePath = currentLoadingModule;

    for (const auto& si : staticFields) {
        bcClass.staticFieldNames.push_back(si.name);
        if (si.hasInit && si.exprFn != bc::kInvalidIndex) {
            bcClass.staticFieldInitExprs.push_back({si.name, si.exprFn});
        }
    }

    for (auto& tup : methods) {
        const std::string& mname = std::get<0>(tup);
        bool isStatic = std::get<1>(tup);
        const std::vector<std::string>& params = std::get<2>(tup);
        uint32_t fnIdx = std::get<3>(tup);

        BCMethod m;
        m.functionIndex = fnIdx;
        m.paramNames = params;
        m.isStatic = isStatic;

        if (isStatic) bcClass.staticMethods[mname] = m;
        else bcClass.methods[mname] = m;
    }

    classes[className] = std::move(bcClass);
    if (!currentLoadingModule.empty()) classToModule[className] = currentLoadingModule;
    // Invalidate property cache because class definitions changed
    propertyCache.clear();

    // Also mirror interpreter classRegistry enough for object creation / dot access naming.
    ClassDef def;
    def.name = className;
    def.parentClass = parentName;
    for (const auto& f : fields) def.fields.push_back({f, TypeAnnotation()});
    runtime.classRegistry[className] = def;

    // Initialize static fields
    for (const auto& sf : staticFields) {
        std::string key = className + "::" + sf.name;
        if (sf.hasInit && sf.exprFn != bc::kInvalidIndex) {
            Value v = runFunction(sf.exprFn, nullptr, 0, nullptr, nullptr, error);
            runtime.staticFields[key] = v;
        } else {
            runtime.staticFields[key] = Value{};
        }
    }

    return true;
}

bool BytecodeVM::runModuleInit(const std::string& modulePath, std::string* error) {
    if (executedModules.find(modulePath) != executedModules.end()) return true;
    auto it = prog->modules.find(modulePath);
    if (it == prog->modules.end()) {
        if (error) *error = "Module not in bytecode program: " + modulePath;
        return false;
    }
    executedModules.insert(modulePath);
    runFunction(it->second, nullptr, 0, nullptr, nullptr, error);
    return error == nullptr || error->empty();
}

bool BytecodeVM::execImportString(const std::string& importStr, std::string* error) {
    // Mirror Runtime::executeNode Import parsing, but for file modules execute embedded init.
    std::string alias;
    std::string modulePath;
    bool isWildcard = false;
    std::vector<std::string> specificItems;

    std::string s = importStr;
    size_t bracePos = s.find(":{");
    if (bracePos != std::string::npos) {
        std::string beforeBrace = s.substr(0, bracePos);
        size_t colonPos = beforeBrace.find(':');
        if (colonPos != std::string::npos) {
            alias = beforeBrace.substr(0, colonPos);
            modulePath = beforeBrace.substr(colonPos + 1);
        } else {
            modulePath = beforeBrace;
        }
        size_t endBrace = s.find('}', bracePos);
        if (endBrace != std::string::npos) {
            std::string itemsStr = s.substr(bracePos + 2, endBrace - bracePos - 2);
            size_t pos = 0;
            while (pos < itemsStr.length()) {
                size_t commaPos = itemsStr.find(',', pos);
                if (commaPos == std::string::npos) commaPos = itemsStr.length();
                specificItems.push_back(itemsStr.substr(pos, commaPos - pos));
                pos = commaPos + 1;
            }
        }
    } else {
        size_t colonPos = s.find(':');
        if (colonPos != std::string::npos) {
            alias = s.substr(0, colonPos);
            s = s.substr(colonPos + 1);
        }
        if (s.size() >= 2 && s.substr(s.size() - 2) == ".*") {
            modulePath = s.substr(0, s.size() - 2);
            isWildcard = true;
        } else {
            size_t lastDot = s.rfind('.');
            if (lastDot != std::string::npos) {
                modulePath = s.substr(0, lastDot);
                specificItems.push_back(s.substr(lastDot + 1));
            } else {
                modulePath = s;
                isWildcard = true;
            }
        }
    }

    if (FunctionRegistry::instance().hasNamespace(modulePath)) {
        if (!alias.empty()) {
            runtime.imports[alias] = modulePath;
        } else if (isWildcard) {
            runtime.imports[modulePath] = modulePath;
        } else {
            for (const auto& item : specificItems) runtime.imports[item] = modulePath;
        }
        return true;
    }

    if (!runModuleInit(modulePath, error)) return false;

    if (!alias.empty()) runtime.imports[alias] = modulePath;
    else runtime.imports[modulePath] = modulePath;

    return true;
}

Value BytecodeVM::callName(const std::string& name, const Value* args, size_t argCount, std::string* error) {
    if (name == "__bc_import") {
        if (argCount != 1 || !std::holds_alternative<std::string>(args[0])) {
            if (error) *error = "__bc_import expects 1 string arg";
            return Value{};
        }
        std::string importStr = std::get<std::string>(args[0]);
        if (!execImportString(importStr, error)) {
            return Value{};
        }
        return Value(true);
    }

    // Lambda var name mapping (mirror evaluate.cpp)
    auto lIt = runtime.varToLambdaId.find(name);
    if (lIt != runtime.varToLambdaId.end()) {
        auto lit = lambdas.find(lIt->second);
        if (lit != lambdas.end()) {
            return runFunction(lit->second.functionIndex, args, argCount, &lit->second.captures, nullptr, error);
        }
    }

    if (runtime.hasVariable(name)) {
        Value vv = runtime.getVariable(name);
        if (std::holds_alternative<std::string>(vv)) {
            std::string possible = std::get<std::string>(vv);
            if (possible.rfind("__lambda_", 0) == 0) {
                auto it = lambdas.find(possible);
                if (it != lambdas.end()) {
                    return runFunction(it->second.functionIndex, args, argCount, &it->second.captures, nullptr, error);
                }
            }
        }
    }

    // method call on object: prefix.member
    size_t dotPos = name.find('.');
    if (dotPos != std::string::npos) {
        std::string prefix = name.substr(0, dotPos);
        std::string member = name.substr(dotPos + 1);

        auto objIt = runtime.objects.find(prefix);
        if (objIt == runtime.objects.end()) {
            if (runtime.hasVariable(prefix)) {
                Value varVal = runtime.getVariable(prefix);
                if (std::holds_alternative<std::string>(varVal)) {
                    std::string objId = std::get<std::string>(varVal);
                    objIt = runtime.objects.find(objId);
                }
            }
        }

        if (objIt != runtime.objects.end()) {
            auto obj = objIt->second;
            std::string className = obj->getClassName();

            // Property/method cache lookup
            std::string cacheKey = className + "." + member;
            auto cacheIt = propertyCache.find(cacheKey);
            if (cacheIt != propertyCache.end()) {
                const PropertyCacheEntry& entry = cacheIt->second;
                if (entry.kind == PropertyCacheEntry::Method && entry.methodIndex != bc::kInvalidIndex) {
                    std::string thisObj = objIt->first;
                    return runFunction(entry.methodIndex, args, argCount, nullptr, &thisObj, error);
                }
                if (entry.kind == PropertyCacheEntry::Field) {
                    // field offset caching not yet implemented, fall through
                }
            }

            if (argCount == 0 && obj->hasField(member)) {
                // Update cache
                PropertyCacheEntry entry;
                entry.kind = PropertyCacheEntry::Field;
                entry.className = className;
                entry.fieldOffset = 0; // placeholder
                propertyCache[cacheKey] = entry;
                return obj->getField(member);
            }

            auto cIt = classes.find(className);
            if (cIt != classes.end()) {
                auto mIt = cIt->second.methods.find(member);
                if (mIt != cIt->second.methods.end() && mIt->second.functionIndex != bc::kInvalidIndex) {
                    // Update cache
                    PropertyCacheEntry entry;
                    entry.kind = PropertyCacheEntry::Method;
                    entry.className = className;
                    entry.methodIndex = mIt->second.functionIndex;
                    propertyCache[cacheKey] = entry;
                    // Set this
                    std::string thisObj = objIt->first;
                    return runFunction(mIt->second.functionIndex, args, argCount, nullptr, &thisObj, error);
                }
            }

            // fallback field
            if (obj->hasField(member)) {
                // Update cache
                PropertyCacheEntry entry;
                entry.kind = PropertyCacheEntry::Field;
                entry.className = className;
                entry.fieldOffset = 0; // placeholder
                propertyCache[cacheKey] = entry;
                return obj->getField(member);
            }
        }
    }

    // Check for user-defined functions (unqualified names only)
    if (name.find('.') == std::string::npos) {
        auto userFuncIt = userFunctions.find(name);
        if (userFuncIt != userFunctions.end()) {
            return runFunction(userFuncIt->second, args, argCount, nullptr, nullptr, error);
        }
    }

    // Built-in function registry
    if (name.find('.') == std::string::npos) {
        for (const auto& pair : runtime.imports) {
            std::string full = pair.second + "." + name;
            if (FunctionRegistry::instance().exists(full)) {
                std::vector<Value> argVec(args, args + argCount);
                return FunctionRegistry::instance().call(full, argVec);
            }
        }
        runtime.notifyError("call", "Unknown function: " + name, -1, -1, false);
        return Value{};
    }

    std::string resolved = runtime.resolveFunctionName(name);
    if (FunctionRegistry::instance().exists(resolved)) {
        std::vector<Value> argVec(args, args + argCount);
        return FunctionRegistry::instance().call(resolved, argVec);
    }

    runtime.notifyError("call", "Unknown function: " + resolved, -1, -1, false);
    return Value{};
}

Value BytecodeVM::newObject(const std::string& fullClassName, const Value* args, size_t argCount, std::string* error) {
    std::string actualClassName = fullClassName;

    // qualified new: module.Class
    size_t dotPos = fullClassName.rfind('.');
    if (dotPos != std::string::npos) {
        std::string moduleRef = fullClassName.substr(0, dotPos);
        std::string className = fullClassName.substr(dotPos + 1);

        std::string modulePath;
        auto aliasIt = runtime.imports.find(moduleRef);
        if (aliasIt != runtime.imports.end()) modulePath = aliasIt->second;
        else modulePath = moduleRef;

        bool found = false;
        for (const auto& kv : classToModule) {
            if (kv.first == className && kv.second == modulePath) { found = true; break; }
        }
        if (!found) {
            Logger::instance().log(LogLevel::ERROR, "Class '" + className + "' not found in module '" + modulePath + "'");
            return Value(fullClassName + "_null");
        }
        actualClassName = className;
    }

    auto cIt = classes.find(actualClassName);
    if (cIt == classes.end()) {
        Logger::instance().log(LogLevel::ERROR, "Class '" + actualClassName + "' not found");
        return Value(fullClassName + "_null");
    }

    // Create instance
    auto instance = runtime.createObject(actualClassName);

    // Initialize fields (parent then own)
    if (!cIt->second.parent.empty()) {
        auto pIt = classes.find(cIt->second.parent);
        if (pIt != classes.end()) {
            for (const auto& f : pIt->second.fields) instance->setField(f, Value{});
        }
    }
    for (const auto& f : cIt->second.fields) instance->setField(f, Value{});

    static int objectCounter = 0;
    std::string objId = actualClassName + "_" + std::to_string(objectCounter++);
    runtime.objects[objId] = instance;

    // constructor
    if (cIt->second.hasConstructor && cIt->second.ctorFunctionIndex != bc::kInvalidIndex) {
        // ARC: Save current scope and retain
        auto savedVars = runtime.variables;
        runtime.retainScope(savedVars);
        auto savedThis = runtime.currentThisObject;
        runtime.currentThisObject = objId;

        // ARC: Push constructor scope
        runtime.pushScope(std::unordered_map<std::string, Value>{});

        // bind params (ARC-aware via setVariable)
        for (size_t i = 0; i < cIt->second.ctorParams.size() && i < argCount; ++i) {
            runtime.setVariable(cIt->second.ctorParams[i], args[i]);
        }

        // field initializers
        for (const auto& init : cIt->second.ctorFieldInits) {
            Value v = runFunction(init.second, nullptr, 0, nullptr, &runtime.currentThisObject, error);
            instance->setField(init.first, v);
        }

        runFunction(cIt->second.ctorFunctionIndex, args, argCount, nullptr, &runtime.currentThisObject, error);

        // ARC: Restore scope
        runtime.popScope(savedVars);
        runtime.currentThisObject = savedThis;
    }

    return Value(objId);
}

Value BytecodeVM::runFunction(uint32_t functionIndex, const Value* args, size_t argCount,
                              const std::unordered_map<std::string, Value>* overrideVars,
                              const std::string* overrideThis,
                              std::string* error) {
    if (!prog || functionIndex >= prog->functions.size()) {
        if (error) *error = "Invalid function index";
        return Value{};
    }

    // =========================================================================
    // JIT Execution Path
    // =========================================================================
    // Try JIT execution for simple functions without overrides
    // JIT is only used for "clean" function calls - no variable overrides
    // or 'this' context changes that would require interpreter semantics
#ifdef QZ_JIT_ENABLED
    if (jitEnabled_ && !overrideVars && !overrideThis) {
        Value jitResult;
        if (tryJITExecute(functionIndex, args, argCount, jitResult, error)) {
            return jitResult;
        }
        // Fall through to interpreter if JIT didn't handle it
    }
#endif
    // =========================================================================

    const bc::Function& fn = prog->functions[functionIndex];
    const std::vector<uint8_t>& code = fn.code;
    
    // Check if we have pre-decoded metadata available
    const bool useCachedMetadata = fn.hasCachedMetadata && !fn.instructionCache.empty();

    // Build direct-indexed metadata lookup table for O(1) access
    // Instead of linear search per instruction, we index by IP directly
    std::vector<const bc::InstructionMeta*> metaByIp;
    if (useCachedMetadata) {
        metaByIp.resize(code.size(), nullptr);
        for (const auto& m : fn.instructionCache) {
            if (m.ip < metaByIp.size()) {
                metaByIp[m.ip] = &m;
            }
        }
    }

    // Clear container cache at function entry (containers may have changed)
    indexContainerCache.clear();
    propertyCache.clear();

    // ARC: Save/override variable scope for calls (mirrors interpreter behavior)
    auto savedVars = runtime.variables;
    runtime.retainScope(savedVars);
    auto savedThis = runtime.currentThisObject;

    if (overrideVars) {
        runtime.pushScope(*overrideVars);
    } else {
        runtime.pushScope(std::unordered_map<std::string, Value>{});
    }
    if (overrideThis) runtime.currentThisObject = *overrideThis;

    // Bind parameters into runtime.variables (ARC-aware via setVariable)
    for (size_t i = 0; i < fn.paramNameStrings.size() && i < argCount; ++i) {
        runtime.setVariable(str(fn.paramNameStrings[i]), args[i]);
    }

    // Slot locals (fast-path) - use SmallVector to avoid heap for small functions
    // Most functions have < 64 locals, so inline storage avoids allocation
    SmallVector<Value, 64> locals;
    const size_t localsSize = fn.localNameStrings.size();
    locals.resize(localsSize);
    // Initialize param slots if present
    for (size_t i = 0; i < fn.paramNameStrings.size() && i < argCount; ++i) {
        if (i < locals.size()) locals[i] = args[i];
    }

    // Stack - use SmallVector for small stack depths (most operations)
    SmallVector<Value, 128> stack;

    // Dirty flag: tracks if locals have been modified since last sync
    // This allows lazy sync only when needed (e.g., before lambda capture)
    bool localsDirty = false;

    // Sync locals to runtime.variables - only called before lambda capture
    // This is lazy: we only sync when actually needed instead of every STORE_SLOT
    auto syncLocalsToVariables = [&]() {
        if (!localsDirty) return;
        for (size_t i = 0; i < locals.size() && i < fn.localNameStrings.size(); ++i) {
            runtime.setVariable(str(fn.localNameStrings[i]), locals[i]);
        }
        localsDirty = false;
    };

    struct TryFrame {
        size_t catchIp = 0;
        size_t finallyIp = 0;
        bool hasFinally = false;
        std::string catchVarName;
        std::string catchType;
    };
    std::vector<TryFrame> tryStack;
    tryStack.reserve(vm_config::kDefaultTryStackReserve);

    // Deferred release queue for batch ARC operations
    // Releases are batched and processed periodically or at scope exit
    DeferredReleaseQueue<64> deferredReleases;
    size_t releaseCounter = 0;
    constexpr size_t kReleaseBatchSize = 256; // Flush every N store operations
    
    auto flushDeferredReleases = [&]() {
        deferredReleases.flush([&](size_t id, bool isArray) {
            if (isArray) {
                runtime.releaseArray(id);
            } else {
                runtime.releaseDict(id);
            }
        });
    };

    bool pendingRethrow = false;
    LanguageException pendingExc("Exception", "");

    size_t ip = 0;

    // Optimized pop: use move semantics to avoid copies
    auto pop = [&]() -> Value {
        if (stack.empty()) {
            throw LanguageException("RuntimeError", "VM stack underflow");
        }
        Value v = std::move(stack.back());
        stack.pop_back();
        return v;
    };

    auto push = [&](Value v) { stack.push_back(std::move(v)); };

    // ARC: Restore scope helper for exceptions
    auto restoreAndThrow = [&](const LanguageException& ex) -> void {
        flushDeferredReleases();
        runtime.popScope(savedVars);
        runtime.currentThisObject = savedThis;
        throw ex;
    };

    auto bindCatchObject = [&](const std::string& varName, const LanguageException& ex) {
        auto excObj = std::make_shared<ObjectInstance>(ex.getType());
        excObj->setField("message", Value(ex.getMessage()));
        excObj->setField("type", Value(ex.getType()));
        for (const auto& kv : ex.getFields()) excObj->setField(kv.first, kv.second);
        runtime.objects[varName] = excObj;
    };

    auto handleException = [&](const LanguageException& ex) -> bool {
        // Walk try stack from innermost to outermost.
        while (!tryStack.empty()) {
            TryFrame& tf = tryStack.back();

            // Type match => jump to catch (leave TRY frame for catch's TRY_POP).
            if (tf.catchType == "Exception" || ex.getType() == tf.catchType) {
                bindCatchObject(tf.catchVarName, ex);
                ip = tf.catchIp;
                pendingRethrow = false;
                return true;
            }

            // Type mismatch: if finally exists, pop this TRY and jump to finally, then rethrow.
            if (tf.hasFinally) {
                pendingRethrow = true;
                pendingExc = ex;
                size_t finallyIp = tf.finallyIp;
                tryStack.pop_back();
                ip = finallyIp;
                return true;
            }

            // No finally, pop and continue searching outer TRY.
            tryStack.pop_back();
        }
        return false;
    };

    auto raise = [&](const LanguageException& ex) {
        if (!handleException(ex)) {
            restoreAndThrow(ex);
        }
    };

    // Direct pointer access for code - faster than vector bounds checking
    const uint8_t* codeData = code.data();
    const size_t codeSize = code.size();

    // Used for fallback bytecode reading when metadata not available
    bool ok = true;

    while (ip < codeSize) {
        const size_t instructionStart = ip;
        bc::OpCode op = static_cast<bc::OpCode>(codeData[ip++]);
        
        // ====================================================================
        // Optimized Instruction Decoding Path
        // ====================================================================
        // Direct-indexed metadata lookup: O(1) instead of O(n) linear search
        // The metaByIp table is built at function entry for fast access.
        // ====================================================================
        const bc::InstructionMeta* meta = nullptr;
        if (useCachedMetadata && instructionStart < metaByIp.size()) {
            meta = metaByIp[instructionStart];
        }

        try {
#if VM_USE_COMPUTED_GOTO
    static const void* dispatch_table[] = {
        &&label_NOP,
        &&label_PUSH_INT32,
        &&label_PUSH_DOUBLE64,
        &&label_PUSH_BOOL,
        &&label_PUSH_STRING,
        &&label_POP,
        &&label_LOAD_VAR,
        &&label_STORE_VAR,
        &&label_DECLARE_ARRAY,
        &&label_DECLARE_DICT,
        &&label_DECLARE_LAMBDA,
        &&label_BINARY_OP,
        &&label_UNARY_OP,
        &&label_INDEX_GET,
        &&label_JUMP,
        &&label_JUMP_IF_FALSE,
        &&label_JUMP_IF_TRUE,
        &&label_CALL_NAME,
        &&label_NEW_OBJECT,
        &&label_MAKE_LAMBDA,
        &&label_DEF_FUNCTION,
        &&label_TRY_PUSH,
        &&label_TRY_POP,
        &&label_CATCH_CLEAR,
        &&label_THROW_VALUE,
        &&label_THROW_NEW,
        &&label_FINALLY_END,
        &&label_DEF_CLASS,
        &&label_DEF_INTERFACE,
        &&label_SET_CURRENT_MODULE,
        &&label_CLEAR_CURRENT_MODULE,
        &&label_RETURN_VALUE,
        &&label_RETURN_VOID,
        &&label_LOAD_SLOT,
        &&label_STORE_SLOT,
        &&label_MAKE_ARRAY_EXPR,
        &&label_MAKE_DICT_EXPR,
        &&label_PUSH_INT32_0,
        &&label_PUSH_INT32_1,
        &&label_PUSH_INT32_NEG1,
        &&label_PUSH_TRUE,
        &&label_PUSH_FALSE,
        &&label_PUSH_NULL,
        &&label_LOAD_SLOT_0,
        &&label_STORE_SLOT_0,
        &&label_CALL_NAME_0,
        &&label_CALL_NAME_1,
        &&label_CALL_NAME_2,
        &&label_INCREMENT_SLOT,
        &&label_DECREMENT_SLOT,
        &&label_LOAD_SLOT_PUSH_INT32,
        &&label_BINARY_OP_STORE_SLOT,
        &&label_LOOP_COND_SLOT_LT_INT32,
    };
    goto *dispatch_table[static_cast<uint8_t>(op)];
#endif
            switch (op) {
            label_NOP:
            case bc::OpCode::NOP:
                break;

            label_PUSH_INT32:
            case bc::OpCode::PUSH_INT32: {
                int32_t v;
                if (VM_LIKELY(meta != nullptr)) {
                    // Use pre-decoded value (still need to advance IP manually)
                    v = static_cast<int32_t>(meta->imm0);
                    ip += 4; // Skip the 4-byte immediate
                } else {
                    v = readI32(code, ip, &ok);
                    if (!ok) throw std::runtime_error("Bytecode decode error");
                }
                push(Value((int)v));
                break;
            }

            label_PUSH_DOUBLE64:
            case bc::OpCode::PUSH_DOUBLE64: {
                double v = readF64(code, ip, &ok);
                if (!ok) throw std::runtime_error("Bytecode decode error");
                push(Value(v));
                break;
            }

            label_PUSH_BOOL:
            case bc::OpCode::PUSH_BOOL: {
                uint8_t b;
                if (VM_LIKELY(meta != nullptr)) {
                    b = static_cast<uint8_t>(meta->imm0);
                    ip += 1; // Skip the 1-byte immediate
                } else {
                    b = readU8(code, ip, &ok);
                    if (!ok) throw std::runtime_error("Bytecode decode error");
                }
                push(Value(b != 0));
                break;
            }

            label_PUSH_STRING:
            case bc::OpCode::PUSH_STRING: {
                uint32_t sidx;
                if (VM_LIKELY(meta != nullptr)) {
                    sidx = meta->imm0;
                    ip += 4; // Skip the 4-byte immediate
                } else {
                    sidx = readU32(code, ip, &ok);
                    if (!ok) throw std::runtime_error("Bytecode decode error");
                }
                push(Value(str(sidx)));
                break;
            }

            label_POP:
            case bc::OpCode::POP:
                (void)pop();
                break;

            label_LOAD_VAR:
            case bc::OpCode::LOAD_VAR: {
                uint32_t nidx;
                if (VM_LIKELY(meta != nullptr)) {
                    nidx = meta->imm0;
                    ip += 4; // Skip the 4-byte immediate
                } else {
                    nidx = readU32(code, ip, &ok);
                }
                std::string name = str(nidx);

                // Mirror Runtime::evaluate Identifier dot access
                size_t dotPos = name.find('.');
                if (dotPos != std::string::npos) {
                    std::string objName = name.substr(0, dotPos);
                    std::string fieldName = name.substr(dotPos + 1);

                    auto objIt = runtime.objects.find(objName);
                    if (objIt != runtime.objects.end()) {
                        push(objIt->second->getField(fieldName));
                        break;
                    }
                    if (runtime.hasVariable(objName)) {
                        Value vv = runtime.getVariable(objName);
                        if (std::holds_alternative<std::string>(vv)) {
                            std::string objId = std::get<std::string>(vv);
                            auto idIt = runtime.objects.find(objId);
                            if (idIt != runtime.objects.end()) {
                                push(idIt->second->getField(fieldName));
                                break;
                            }
                        }
                    }
                }

                push(runtime.getVariable(name));
                break;
            }

            label_LOAD_SLOT:
            case bc::OpCode::LOAD_SLOT: {
                uint16_t slot;
                if (VM_LIKELY(meta != nullptr)) {
                    slot = static_cast<uint16_t>(meta->imm0);
                    ip += 2; // Skip the 2-byte immediate
                } else {
                    slot = readU16(code, ip, &ok);
                    if (!ok) throw std::runtime_error("Bytecode decode error");
                }
                if (slot >= locals.size()) {
                    throw LanguageException("RuntimeError", "Local slot index out of bounds: " + std::to_string(slot));
                }
                push(locals[slot]);
                break;
            }

            label_STORE_VAR:
            case bc::OpCode::STORE_VAR: {
                uint32_t nidx;
                if (VM_LIKELY(meta != nullptr)) {
                    nidx = meta->imm0;
                    ip += 4; // Skip the 4-byte immediate
                } else {
                    nidx = readU32(code, ip, &ok);
                }
                std::string name = str(nidx);
                Value v = pop();
                runtime.setVariable(name, v);
                break;
            }

            label_STORE_SLOT:
            case bc::OpCode::STORE_SLOT: {
                uint16_t slot;
                if (VM_LIKELY(meta != nullptr)) {
                    slot = static_cast<uint16_t>(meta->imm0);
                    ip += 2; // Skip the 2-byte immediate
                } else {
                    slot = readU16(code, ip, &ok);
                    if (!ok) throw std::runtime_error("Bytecode decode error");
                }
                Value v = pop();
                if (VM_UNLIKELY(slot >= locals.size())) {
                    throw LanguageException("RuntimeError", "Local slot index out of bounds: " + std::to_string(slot));
                }
                // Batch ARC: defer old value release, retain new (only for containers)
                Value& oldVal = locals[slot];
                const auto oldIdx = oldVal.index();
                if (oldIdx == 4) { // ArrayRef
                    deferredReleases.defer(std::get<ArrayRef>(oldVal).id, true);
                } else if (oldIdx == 5) { // DictRef
                    deferredReleases.defer(std::get<DictRef>(oldVal).id, false);
                }
                runtime.retainValue(v);
                oldVal = std::move(v);
                localsDirty = true;  // Mark for lazy sync before lambda capture
                
                // Periodically flush deferred releases to prevent unbounded growth
                if (++releaseCounter >= kReleaseBatchSize) {
                    flushDeferredReleases();
                    releaseCounter = 0;
                }
                break;
            }

            label_DECLARE_ARRAY:
            case bc::OpCode::DECLARE_ARRAY: {
                uint32_t nidx = readU32(code, ip, &ok);
                uint16_t count = readU16(code, ip, &ok);
                if (!ok) throw std::runtime_error("Bytecode decode error");
                std::string varName = str(nidx);

                std::vector<Value> arrayVec;
                arrayVec.resize(count);
                for (int i = (int)count - 1; i >= 0; --i) arrayVec[(size_t)i] = pop();

                // Use ARC-managed allocation
                Value arrVal = runtime.makeArray(std::move(arrayVec));
                size_t arrayId = std::get<ArrayRef>(arrVal).id;
                runtime.varToArrayId[varName] = arrayId;
                runtime.setVariable(varName, arrVal);
                break;
            }

            label_DECLARE_DICT:
            case bc::OpCode::DECLARE_DICT: {
                uint32_t nidx = readU32(code, ip, &ok);
                uint16_t count = readU16(code, ip, &ok);
                std::string varName = str(nidx);

                std::vector<std::string> keys;
                keys.reserve(count);
                for (uint16_t i = 0; i < count; ++i) keys.push_back(str(readU32(code, ip, &ok)));
                if (!ok) throw std::runtime_error("Bytecode decode error");

                std::unordered_map<std::string, Value> dictMap;
                dictMap.reserve(count);
                std::vector<Value> values;
                values.resize(count);
                for (int i = (int)count - 1; i >= 0; --i) values[(size_t)i] = pop();
                for (size_t i = 0; i < count; ++i) dictMap[keys[i]] = values[i];

                // Use ARC-managed allocation
                Value dictVal = runtime.makeDict(std::move(dictMap));
                size_t dictId = std::get<DictRef>(dictVal).id;
                runtime.varToDictId[varName] = dictId;
                runtime.setVariable(varName, dictVal);
                break;
            }

            label_MAKE_ARRAY_EXPR:
            case bc::OpCode::MAKE_ARRAY_EXPR: {
                uint16_t count = readU16(code, ip, &ok);
                if (!ok) throw std::runtime_error("Bytecode decode error");

                std::vector<Value> arrayVec;
                arrayVec.resize(count);
                for (int i = (int)count - 1; i >= 0; --i) arrayVec[(size_t)i] = pop();

                // Use ARC-managed allocation
                push(runtime.makeArray(std::move(arrayVec)));
                break;
            }

            label_MAKE_DICT_EXPR:
            case bc::OpCode::MAKE_DICT_EXPR: {
                uint16_t count = readU16(code, ip, &ok);
                std::vector<std::string> keys;
                keys.reserve(count);
                for (uint16_t i = 0; i < count; ++i) keys.push_back(str(readU32(code, ip, &ok)));
                if (!ok) throw std::runtime_error("Bytecode decode error");

                std::vector<Value> values;
                values.resize(count);
                for (int i = (int)count - 1; i >= 0; --i) values[(size_t)i] = pop();

                std::unordered_map<std::string, Value> dictMap;
                dictMap.reserve(count);
                for (size_t i = 0; i < (size_t)count; ++i) {
                    dictMap[keys[i]] = values[i];
                }

                // Use ARC-managed allocation
                push(runtime.makeDict(std::move(dictMap)));
                break;
            }

            label_DECLARE_LAMBDA:
            case bc::OpCode::DECLARE_LAMBDA: {
                uint32_t nidx = readU32(code, ip, &ok);
                uint32_t fidx = readU32(code, ip, &ok);
                if (!ok) throw std::runtime_error("Bytecode decode error");

                // Sync locals to runtime.variables before capturing
                syncLocalsToVariables();

                std::string lambdaId = "__lambda_" + std::to_string(nextLambdaId++);
                BCLambda l;
                l.functionIndex = fidx;
                l.captures = runtime.variables;
                lambdas[lambdaId] = std::move(l);

                std::string varName = str(nidx);
                runtime.varToLambdaId[varName] = lambdaId;
                runtime.setVariable(varName, Value(lambdaId));
                break;
            }

            label_MAKE_LAMBDA:
            case bc::OpCode::MAKE_LAMBDA: {
                uint32_t fidx = readU32(code, ip, &ok);
                if (!ok) throw std::runtime_error("Bytecode decode error");
                
                // Sync locals to runtime.variables before capturing
                syncLocalsToVariables();
                
                std::string lambdaId = "__lambda_" + std::to_string(nextLambdaId++);
                BCLambda l;
                l.functionIndex = fidx;
                l.captures = runtime.variables;
                lambdas[lambdaId] = std::move(l);
                push(Value(lambdaId));
                break;
            }

            label_BINARY_OP:
            case bc::OpCode::BINARY_OP: {
                bc::BinaryOp bop = (bc::BinaryOp)readU8(code, ip, &ok);
                if (!ok) throw std::runtime_error("Bytecode decode error");
                Value right = pop();
                Value left = pop();
                
                // Ultra-fast inline path for int-int operations (most common case)
                // This avoids function call overhead for tight loops
                if (const int* li = std::get_if<int>(&left)) {
                    if (const int* ri = std::get_if<int>(&right)) {
                        switch (bop) {
                        case bc::BinaryOp::ADD: push(Value(*li + *ri)); break;
                        case bc::BinaryOp::SUB: push(Value(*li - *ri)); break;
                        case bc::BinaryOp::MUL: push(Value(*li * *ri)); break;
                        case bc::BinaryOp::DIV:
                            if (*ri == 0) throw LanguageException("ArithmeticError", "Division by zero");
                            push(Value(*li / *ri)); break;
                        case bc::BinaryOp::LT: push(Value(*li < *ri)); break;
                        case bc::BinaryOp::GT: push(Value(*li > *ri)); break;
                        case bc::BinaryOp::LE: push(Value(*li <= *ri)); break;
                        case bc::BinaryOp::GE: push(Value(*li >= *ri)); break;
                        case bc::BinaryOp::EQ: push(Value(*li == *ri)); break;
                        case bc::BinaryOp::NE: push(Value(*li != *ri)); break;
                        }
                        break;  // exit BINARY_OP case
                    }
                }
                // Fallback to general path
                push(applyBinary(left, right, bop));
                break;
            }

            label_UNARY_OP:
            case bc::OpCode::UNARY_OP: {
                bc::UnaryOp uop = (bc::UnaryOp)readU8(code, ip, &ok);
                if (!ok) throw std::runtime_error("Bytecode decode error");
                Value v = pop();
                push(applyUnary(v, uop));
                break;
            }

            label_INDEX_GET:
            case bc::OpCode::INDEX_GET: {
                uint32_t nidx = readU32(code, ip, &ok);
                if (!ok) throw std::runtime_error("Bytecode decode error");
                Value idxV = pop();
                // Use cached indexGet for fast repeated access (avoids string hashing)
                push(indexGet(nidx, idxV));
                break;
            }

            label_JUMP:
            case bc::OpCode::JUMP: {
                size_t target;
                if (VM_LIKELY(meta != nullptr)) {
                    // Pre-computed absolute target (validated at load time)
                    target = meta->imm0;
                    ip += 4; // Skip the 4-byte relative offset
                } else {
                    int32_t rel = readI32(code, ip, &ok);
                    if (!ok) throw std::runtime_error("Bytecode decode error");
                    int64_t newIp = (int64_t)ip + rel;
                    if (newIp < 0 || (size_t)newIp > code.size()) {
                        throw LanguageException("RuntimeError", "Invalid jump target: out of bounds");
                    }
                    target = (size_t)newIp;
                }
                ip = target;
                break;
            }

            label_JUMP_IF_FALSE:
            case bc::OpCode::JUMP_IF_FALSE: {
                size_t target;
                if (VM_LIKELY(meta != nullptr)) {
                    target = meta->imm0;
                    ip += 4; // Skip the 4-byte relative offset
                } else {
                    int32_t rel = readI32(code, ip, &ok);
                    if (!ok) throw std::runtime_error("Bytecode decode error");
                    int64_t newIp = (int64_t)ip + rel;
                    if (newIp < 0 || (size_t)newIp > code.size()) {
                        throw LanguageException("RuntimeError", "Invalid jump target: out of bounds");
                    }
                    target = (size_t)newIp;
                }
                Value cond = pop();
                // Inline fast-path for bool (most common after comparisons)
                if (const bool* b = std::get_if<bool>(&cond)) {
                    if (!*b) ip = target;
                } else if (!evalCondition(cond)) {
                    ip = target;
                }
                break;
            }

            label_JUMP_IF_TRUE:
            case bc::OpCode::JUMP_IF_TRUE: {
                size_t target;
                if (VM_LIKELY(meta != nullptr)) {
                    target = meta->imm0;
                    ip += 4; // Skip the 4-byte relative offset
                } else {
                    int32_t rel = readI32(code, ip, &ok);
                    if (!ok) throw std::runtime_error("Bytecode decode error");
                    int64_t newIp = (int64_t)ip + rel;
                    if (newIp < 0 || (size_t)newIp > code.size()) {
                        throw LanguageException("RuntimeError", "Invalid jump target: out of bounds");
                    }
                    target = (size_t)newIp;
                }
                Value cond = pop();
                // Inline fast-path for bool
                if (const bool* b = std::get_if<bool>(&cond)) {
                    if (*b) ip = target;
                } else if (evalCondition(cond)) {
                    ip = target;
                }
                break;
            }

            label_CALL_NAME:
            case bc::OpCode::CALL_NAME: {
                uint32_t nidx;
                uint8_t argc;
                if (VM_LIKELY(meta != nullptr)) {
                    nidx = meta->imm0;
                    argc = static_cast<uint8_t>(meta->imm1);
                    ip += 5; // Skip u32 + u8
                } else {
                    nidx = readU32(code, ip, &ok);
                    argc = readU8(code, ip, &ok);
                    if (!ok) throw std::runtime_error("Bytecode decode error");
                }

                std::vector<Value> callArgs;
                callArgs.reserve(vm_config::kDefaultArgsReserve);
                callArgs.resize(argc);
                for (int i = (int)argc - 1; i >= 0; --i) callArgs[(size_t)i] = std::move(pop());

                Value rv = callName(str(nidx), callArgs.data(), callArgs.size(), error);
                push(std::move(rv));
                break;
            }

            label_NEW_OBJECT:
            case bc::OpCode::NEW_OBJECT: {
                uint32_t nidx;
                uint8_t argc;
                if (VM_LIKELY(meta != nullptr)) {
                    nidx = meta->imm0;
                    argc = static_cast<uint8_t>(meta->imm1);
                    ip += 5; // Skip u32 + u8
                } else {
                    nidx = readU32(code, ip, &ok);
                    argc = readU8(code, ip, &ok);
                    if (!ok) throw std::runtime_error("Bytecode decode error");
                }
                std::vector<Value> ctorArgs;
                ctorArgs.reserve(vm_config::kDefaultArgsReserve);
                ctorArgs.resize(argc);
                for (int i = (int)argc - 1; i >= 0; --i) ctorArgs[(size_t)i] = std::move(pop());

                Value obj = newObject(str(nidx), ctorArgs.data(), ctorArgs.size(), error);
                push(std::move(obj));
                break;
            }

            label_TRY_PUSH:
            case bc::OpCode::TRY_PUSH: {
                uint32_t catchIpAbs = readU32(code, ip, &ok);
                uint32_t finallyIpAbs = readU32(code, ip, &ok);
                bool hasFinally = readU8(code, ip, &ok) != 0;
                std::string catchVar = str(readU32(code, ip, &ok));
                std::string catchType = str(readU32(code, ip, &ok));
                if (!ok) throw std::runtime_error("Bytecode decode error");

                // Validate exception handler addresses
                if (catchIpAbs > code.size()) {
                    throw LanguageException("RuntimeError", "Invalid catch handler address: out of bounds");
                }
                if (hasFinally && finallyIpAbs > code.size()) {
                    throw LanguageException("RuntimeError", "Invalid finally handler address: out of bounds");
                }

                TryFrame tf;
                tf.catchIp = (size_t)catchIpAbs;
                tf.finallyIp = (size_t)finallyIpAbs;
                tf.hasFinally = hasFinally;
                tf.catchVarName = catchVar;
                tf.catchType = catchType;
                tryStack.push_back(std::move(tf));
                break;
            }

            label_TRY_POP:
            case bc::OpCode::TRY_POP:
                if (!tryStack.empty()) tryStack.pop_back();
                break;

            label_CATCH_CLEAR:
            case bc::OpCode::CATCH_CLEAR: {
                std::string varName = str(readU32(code, ip, &ok));
                if (!ok) throw std::runtime_error("Bytecode decode error");
                runtime.objects.erase(varName);
                break;
            }

            label_THROW_VALUE:
            case bc::OpCode::THROW_VALUE: {
                Value v = pop();
                std::string msg = to_string(v);
                raise(LanguageException("Exception", msg));
                break;
            }

            label_THROW_NEW:
            case bc::OpCode::THROW_NEW: {
                uint32_t tIdx = readU32(code, ip, &ok);
                if (!ok) throw std::runtime_error("Bytecode decode error");
                Value v = pop();
                std::string msg = to_string(v);
                raise(LanguageException(str(tIdx), msg));
                break;
            }

            label_FINALLY_END:
            case bc::OpCode::FINALLY_END: {
                if (pendingRethrow) {
                    pendingRethrow = false;
                    raise(pendingExc);
                }
                break;
            }

            label_DEF_CLASS:
            case bc::OpCode::DEF_CLASS: {
                if (!execDefClass(code, ip, error)) return Value{};
                break;
            }

            label_DEF_INTERFACE:
            case bc::OpCode::DEF_INTERFACE: {
                if (!execDefInterface(code, ip, error)) return Value{};
                break;
            }

            label_DEF_FUNCTION:
            case bc::OpCode::DEF_FUNCTION: {
                // Register a user-defined function
                uint32_t nameIdx = readU32(code, ip, &ok);
                uint32_t funcIdx = readU32(code, ip, &ok);
                if (!ok) throw std::runtime_error("Bytecode decode error in DEF_FUNCTION");
                
                std::string funcName = str(nameIdx);
                userFunctions[funcName] = funcIdx;
                Logger::instance().log(LogLevel::DEBUG, "DEF_FUNCTION: registered '" + funcName + "' as function index " + std::to_string(funcIdx));
                break;
            }

            label_SET_CURRENT_MODULE:
            case bc::OpCode::SET_CURRENT_MODULE: {
                uint32_t sidx = readU32(code, ip, &ok);
                if (!ok) throw std::runtime_error("Bytecode decode error");
                currentLoadingModule = str(sidx);
                break;
            }

            label_CLEAR_CURRENT_MODULE:
            case bc::OpCode::CLEAR_CURRENT_MODULE:
                currentLoadingModule.clear();
                break;

            label_RETURN_VALUE:
            case bc::OpCode::RETURN_VALUE: {
                Value rv = pop();
                // ARC: Retain return value before scope switch
                runtime.retainValue(rv);
                // Flush deferred releases before returning
                flushDeferredReleases();
                // ARC: Restore scope
                runtime.popScope(savedVars);
                runtime.currentThisObject = savedThis;
                return rv;
            }

            label_RETURN_VOID:
            case bc::OpCode::RETURN_VOID:
                // Flush deferred releases before returning
                flushDeferredReleases();
                // ARC: Restore scope
                runtime.popScope(savedVars);
                runtime.currentThisObject = savedThis;
                return Value{};

            // ====================================================================
            // Specialized opcodes for common patterns (performance optimization)
            // ====================================================================
            
            label_PUSH_INT32_0:
            case bc::OpCode::PUSH_INT32_0:
                push(Value(0));
                break;
                
            label_PUSH_INT32_1:
            case bc::OpCode::PUSH_INT32_1:
                push(Value(1));
                break;
                
            label_PUSH_INT32_NEG1:
            case bc::OpCode::PUSH_INT32_NEG1:
                push(Value(-1));
                break;
                
            label_PUSH_TRUE:
            case bc::OpCode::PUSH_TRUE:
                push(Value(true));
                break;
                
            label_PUSH_FALSE:
            case bc::OpCode::PUSH_FALSE:
                push(Value(false));
                break;
                
            label_PUSH_NULL:
            case bc::OpCode::PUSH_NULL:
                push(Value(std::string("")));
                break;
                
            label_LOAD_SLOT_0:
            case bc::OpCode::LOAD_SLOT_0:
                if (VM_UNLIKELY(locals.empty())) {
                    throw LanguageException("RuntimeError", "Local slot 0 out of bounds");
                }
                push(locals[0]);
                break;
                
            label_STORE_SLOT_0:
            case bc::OpCode::STORE_SLOT_0: {
                if (VM_UNLIKELY(locals.empty())) {
                    throw LanguageException("RuntimeError", "Local slot 0 out of bounds");
                }
                Value v = pop();
                // Batch ARC: defer old value release
                Value& oldVal = locals[0];
                const auto oldIdx = oldVal.index();
                if (oldIdx == 4) { // ArrayRef
                    deferredReleases.defer(std::get<ArrayRef>(oldVal).id, true);
                } else if (oldIdx == 5) { // DictRef
                    deferredReleases.defer(std::get<DictRef>(oldVal).id, false);
                }
                runtime.retainValue(v);
                oldVal = std::move(v);
                localsDirty = true;
                break;
            }
            
            label_CALL_NAME_0:
            case bc::OpCode::CALL_NAME_0: {
                uint32_t nidx;
                if (VM_LIKELY(meta != nullptr)) {
                    nidx = meta->imm0;
                    ip += 4;
                } else {
                    nidx = readU32(code, ip, &ok);
                    if (!ok) throw std::runtime_error("Bytecode decode error");
                }
                
                std::vector<Value> callArgs;  // Empty for 0-arg call
                Value rv = callName(str(nidx), callArgs.data(), callArgs.size(), error);
                push(std::move(rv));
                break;
            }
            
            label_CALL_NAME_1:
            case bc::OpCode::CALL_NAME_1: {
                uint32_t nidx;
                if (VM_LIKELY(meta != nullptr)) {
                    nidx = meta->imm0;
                    ip += 4;
                } else {
                    nidx = readU32(code, ip, &ok);
                    if (!ok) throw std::runtime_error("Bytecode decode error");
                }
                
                std::vector<Value> callArgs;
                callArgs.reserve(1);
                callArgs.push_back(std::move(pop()));
                Value rv = callName(str(nidx), callArgs.data(), callArgs.size(), error);
                push(std::move(rv));
                break;
            }
            
            label_CALL_NAME_2:
            case bc::OpCode::CALL_NAME_2: {
                uint32_t nidx;
                if (VM_LIKELY(meta != nullptr)) {
                    nidx = meta->imm0;
                    ip += 4;
                } else {
                    nidx = readU32(code, ip, &ok);
                    if (!ok) throw std::runtime_error("Bytecode decode error");
                }
                
                std::vector<Value> callArgs;
                callArgs.reserve(2);
                callArgs.resize(2);
                callArgs[1] = std::move(pop());
                callArgs[0] = std::move(pop());
                Value rv = callName(str(nidx), callArgs.data(), callArgs.size(), error);
                push(std::move(rv));
                break;
            }
            
            label_INCREMENT_SLOT:
            case bc::OpCode::INCREMENT_SLOT: {
                uint16_t slot;
                if (VM_LIKELY(meta != nullptr)) {
                    slot = static_cast<uint16_t>(meta->imm0);
                    ip += 2;
                } else {
                    slot = readU16(code, ip, &ok);
                    if (!ok) throw std::runtime_error("Bytecode decode error");
                }
                
                if (slot >= locals.size()) {
                    throw LanguageException("RuntimeError", "Local slot out of bounds: " + std::to_string(slot));
                }
                
                // Only works for integers
                if (const int* i = std::get_if<int>(&locals[slot])) {
                    locals[slot] = Value(*i + 1);
                    // Sync to variables
                    if (slot < fn.localNameStrings.size()) {
                        runtime.setVariable(str(fn.localNameStrings[slot]), locals[slot]);
                    }
                } else {
                    throw LanguageException("RuntimeError", "INCREMENT_SLOT requires integer value");
                }
                break;
            }
            
            label_DECREMENT_SLOT:
            case bc::OpCode::DECREMENT_SLOT: {
                uint16_t slot;
                if (VM_LIKELY(meta != nullptr)) {
                    slot = static_cast<uint16_t>(meta->imm0);
                    ip += 2;
                } else {
                    slot = readU16(code, ip, &ok);
                    if (!ok) throw std::runtime_error("Bytecode decode error");
                }
                
                if (slot >= locals.size()) {
                    throw LanguageException("RuntimeError", "Local slot out of bounds: " + std::to_string(slot));
                }
                
                // Only works for integers
                if (const int* i = std::get_if<int>(&locals[slot])) {
                    locals[slot] = Value(*i - 1);
                    // Sync to variables
                    if (slot < fn.localNameStrings.size()) {
                        runtime.setVariable(str(fn.localNameStrings[slot]), locals[slot]);
                    }
                } else {
                    throw LanguageException("RuntimeError", "DECREMENT_SLOT requires integer value");
                }
                break;
            }
            
            label_LOAD_SLOT_PUSH_INT32:
            case bc::OpCode::LOAD_SLOT_PUSH_INT32: {
                uint16_t slot;
                int32_t value;
                if (VM_LIKELY(meta != nullptr)) {
                    slot = meta->imm1;
                    value = static_cast<int32_t>(meta->imm0);
                    ip += 6; // u16 + i32
                } else {
                    slot = readU16(code, ip, &ok);
                    value = readI32(code, ip, &ok);
                    if (!ok) throw std::runtime_error("Bytecode decode error");
                }
                
                if (slot >= locals.size()) {
                    throw LanguageException("RuntimeError", "Local slot out of bounds: " + std::to_string(slot));
                }
                push(locals[slot]);
                push(Value((int)value));
                break;
            }
            
            label_BINARY_OP_STORE_SLOT:
            case bc::OpCode::BINARY_OP_STORE_SLOT: {
                uint8_t opByte;
                uint16_t slot;
                if (VM_LIKELY(meta != nullptr)) {
                    opByte = static_cast<uint8_t>(meta->imm0);
                    slot = meta->imm1;
                    ip += 3; // u8 + u16
                } else {
                    opByte = readU8(code, ip, &ok);
                    slot = readU16(code, ip, &ok);
                    if (!ok) throw std::runtime_error("Bytecode decode error");
                }
                
                if (slot >= locals.size()) {
                    throw LanguageException("RuntimeError", "Local slot out of bounds: " + std::to_string(slot));
                }
                
                bc::BinaryOp bop = static_cast<bc::BinaryOp>(opByte);
                Value right = pop();
                Value left = pop();
                
                // Fast-path for int-int operations
                if (const int* li = std::get_if<int>(&left)) {
                    if (const int* ri = std::get_if<int>(&right)) {
                        int result;
                        switch (bop) {
                        case bc::BinaryOp::ADD: result = *li + *ri; break;
                        case bc::BinaryOp::SUB: result = *li - *ri; break;
                        case bc::BinaryOp::MUL: result = *li * *ri; break;
                        case bc::BinaryOp::DIV:
                            if (*ri == 0) throw LanguageException("ArithmeticError", "Division by zero");
                            result = *li / *ri; break;
                        default: goto binary_op_store_fallback;
                        }
                        locals[slot] = Value(result);
                        localsDirty = true;
                        break;
                    }
                }
                
                binary_op_store_fallback:
                {
                    Value result = applyBinary(left, right, bop);
                    
                    // Defer release of old container value
                    Value& oldVal = locals[slot];
                    const auto oldIdx = oldVal.index();
                    if (oldIdx == 4) { // ArrayRef
                        deferredReleases.defer(std::get<ArrayRef>(oldVal).id, true);
                    } else if (oldIdx == 5) { // DictRef
                        deferredReleases.defer(std::get<DictRef>(oldVal).id, false);
                    }
                    
                    locals[slot] = result;
                    localsDirty = true;
                }
                break;
            }
            
            label_LOOP_COND_SLOT_LT_INT32:
            case bc::OpCode::LOOP_COND_SLOT_LT_INT32: {
                // Super-instruction: slot < constant ? continue : jump
                // Fuses: LOAD_SLOT + PUSH_INT32 + BINARY_OP(LT) + JUMP_IF_FALSE
                uint16_t slot;
                int32_t limit;
                size_t target;
                if (VM_LIKELY(meta != nullptr)) {
                    // Pre-decoded: imm0 = absolute target, imm1 = limit, imm2 = slot
                    target = meta->imm0;
                    limit = static_cast<int32_t>(meta->imm1);
                    slot = meta->imm2;
                    ip += 10; // u16 + i32 + i32
                } else {
                    slot = readU16(code, ip, &ok);
                    limit = readI32(code, ip, &ok);
                    int32_t relJump = readI32(code, ip, &ok);
                    if (!ok) throw std::runtime_error("Bytecode decode error");
                    int64_t newIp = static_cast<int64_t>(ip) + relJump;
                    if (newIp < 0 || static_cast<size_t>(newIp) > code.size()) {
                        throw LanguageException("RuntimeError", "Invalid jump target");
                    }
                    target = static_cast<size_t>(newIp);
                }
                
                if (slot >= locals.size()) {
                    throw LanguageException("RuntimeError", "Local slot out of bounds: " + std::to_string(slot));
                }
                
                // Fast-path: expect slot to be int (loop counter)
                const Value& slotVal = locals[slot];
                if (const int* iv = std::get_if<int>(&slotVal)) {
                    if (*iv >= limit) {
                        ip = target;  // Jump out of loop
                    }
                    // else: condition true, continue loop body
                } else {
                    // Fallback: compare non-int values
                    Value cmp = applyBinary(slotVal, Value(limit), bc::BinaryOp::LT);
                    if (!evalCondition(cmp)) {
                        ip = target;
                    }
                }
                break;
            }
            }
        } catch (const LanguageException& ex) {
            // Flush deferred releases before exception handling
            flushDeferredReleases();
            // Uncaught from a callee (lambda/method/constructor): try to handle here.
            raise(ex);
        }
    }

    // Flush remaining deferred releases
    flushDeferredReleases();
    // ARC: Restore scope at end of function
    runtime.popScope(savedVars);
    runtime.currentThisObject = savedThis;
    return Value{};
}

bool BytecodeVM::run(const bc::Program& program, std::string* error) {
    prog = &program;

    runtime.executionMode = "bytecode";
    runtime.state = RuntimeState::EXECUTING;
    // Note: program-level hooks will register during execution; start is mainly for host-registered hooks.
    runtime.emitHook("start", {Value(runtime.executionMode)});

    // Allow extensions to invoke bytecode lambdas by id (e.g. system.collection.map).
    runtime.setExternalLambdaInvoker([this](const std::string& lambdaId, const std::vector<Value>& args) -> Value {
        auto it = lambdas.find(lambdaId);
        if (it == lambdas.end()) return Value{};
        std::string err;
        return runFunction(it->second.functionIndex, args.data(), args.size(), &it->second.captures, nullptr, &err);
    });

    // Initialize runtime state similarly to interpreter main
    // (extensions are already available via FunctionRegistry setup in main)

    try {
        (void)runFunction(program.entryFunction, nullptr, 0, nullptr, nullptr, error);
    } catch (const std::exception& ex) {
        if (error) *error = std::string("VM fatal: ") + ex.what();
        runtime.notifyError("vm", error ? *error : std::string("VM fatal"), -1, -1, true);
        runtime.emitHook("end", {Value(runtime.executionMode), Value(std::string("error"))});
        runtime.setExternalLambdaInvoker(nullptr);
        return false;
    }

    if (error && !error->empty()) {
        runtime.notifyError("vm", *error, -1, -1, true);
        runtime.emitHook("end", {Value(runtime.executionMode), Value(std::string("error"))});
        runtime.setExternalLambdaInvoker(nullptr);
        return false;
    }

    runtime.state = RuntimeState::COMPLETED;
    runtime.emitHook("end", {Value(runtime.executionMode), Value(std::string("completed"))});

    runtime.setExternalLambdaInvoker(nullptr);
    return true;
}
