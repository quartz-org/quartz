#ifndef QZ_BYTECODE_VM_H
#define QZ_BYTECODE_VM_H

#include "bytecode.h"
#include "runtime.h"
#include "vm_optimizations.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <memory>

// Forward declaration for JIT
namespace qz::jit {
    class Engine;
}

class BytecodeVM {
public:
    explicit BytecodeVM(Runtime& runtime);
    ~BytecodeVM();
    
    // Enable/disable JIT compilation
    void setJITEnabled(bool enabled);
    bool isJITEnabled() const;
    
    // Set JIT compilation threshold (default: 100)
    void setJITThreshold(uint32_t threshold);
    uint32_t jitThreshold() const;

    bool run(const bc::Program& program, std::string* error);

    // Container cache for fast index operations (exposed to JIT)
    struct ContainerCacheEntry {
        enum Kind : uint8_t { None = 0, Array = 1, Dict = 2 } kind = None;
        size_t id = 0;
    };
    const std::unordered_map<uint32_t, ContainerCacheEntry>& getIndexContainerCache() const { return indexContainerCache; }
    void setIndexContainerCache(std::unordered_map<uint32_t, ContainerCacheEntry> cache) { indexContainerCache = std::move(cache); }

private:
    Runtime& runtime;

    struct BCLambda {
        uint32_t functionIndex = bc::kInvalidIndex;
        std::unordered_map<std::string, Value> captures;
    };

    struct BCMethod {
        uint32_t functionIndex = bc::kInvalidIndex;
        std::vector<std::string> paramNames;
        bool isStatic = false;
    };

    struct BCClass {
        std::string name;
        std::string parent;
        std::vector<std::string> fields;
        std::unordered_map<std::string, size_t> fieldToIndex; // field name -> index in fields vector
        std::unordered_map<std::string, BCMethod> methods;
        std::unordered_map<std::string, BCMethod> staticMethods;

        // constructor
        bool hasConstructor = false;
        uint32_t ctorFunctionIndex = bc::kInvalidIndex;
        std::vector<std::string> ctorParams;
        std::vector<std::pair<std::string, uint32_t>> ctorFieldInits; // fieldName -> expr function index

        // static fields
        std::vector<std::string> staticFieldNames;
        std::vector<std::pair<std::string, uint32_t>> staticFieldInitExprs; // fieldName -> expr function index

        // module association
        std::string modulePath;
    };

    const bc::Program* prog = nullptr;

    // Runtime tables for bytecode mode
    std::unordered_map<std::string, BCLambda> lambdas;
    std::unordered_map<std::string, BCClass> classes;
    std::unordered_map<std::string, std::string> classToModule;
    std::unordered_set<std::string> executedModules;
    
    // User-defined functions: name -> function index
    std::unordered_map<std::string, uint32_t> userFunctions;

    std::string currentLoadingModule;
    uint32_t nextLambdaId = 0;

    // ========================================================================
    // Container Cache for Fast Index Operations
    // ========================================================================
    // Caches resolved varName -> (kind, id) to avoid repeated string hashing
    // in tight loops. Cleared at function entry.
    std::unordered_map<uint32_t, ContainerCacheEntry> indexContainerCache;

    // ========================================================================
    // Property/Method Cache for Fast Member Lookup
    // ========================================================================
    // Caches resolved className.member -> (kind, offset/index) to avoid repeated
    // dictionary lookups. Cleared when new classes are defined.
    struct PropertyCacheEntry {
        enum Kind : uint8_t { None = 0, Field = 1, Method = 2 } kind = None;
        std::string className;          // for validation
        size_t fieldOffset = 0;         // index into instance's field vector (if Field)
        uint32_t methodIndex = bc::kInvalidIndex; // function index for method (if Method)
    };
    std::unordered_map<std::string, PropertyCacheEntry> propertyCache;

    // Execution
    Value runFunction(uint32_t functionIndex, const Value* args, size_t argCount,
                      const std::unordered_map<std::string, Value>* overrideVars,
                      const std::string* overrideThis,
                      std::string* error);

    bool evalCondition(const Value& v) const;
    Value applyBinary(const Value& left, const Value& right, bc::BinaryOp op) const;
    Value applyUnary(const Value& operand, bc::UnaryOp op) const;

    // Calls / objects
    Value callName(const std::string& name, const Value* args, size_t argCount, std::string* error);
    Value newObject(const std::string& fullClassName, const Value* args, size_t argCount, std::string* error);
    Value indexGet(const std::string& varName, const Value& indexValue);
    Value indexGet(uint32_t varNameStringIndex, const Value& indexValue);  // Fast path with caching

    // Import/module execution
    bool execImportString(const std::string& importStr, std::string* error);
    bool runModuleInit(const std::string& modulePath, std::string* error);

    // Definitions
    bool execDefClass(const std::vector<uint8_t>& code, size_t& ip, std::string* error);
    bool execDefInterface(const std::vector<uint8_t>& code, size_t& ip, std::string* error);

    // Byte reading helpers (advance ip)
    uint8_t readU8(const std::vector<uint8_t>& code, size_t& ip, bool* ok);
    uint16_t readU16(const std::vector<uint8_t>& code, size_t& ip, bool* ok);
    uint32_t readU32(const std::vector<uint8_t>& code, size_t& ip, bool* ok);
    int32_t readI32(const std::vector<uint8_t>& code, size_t& ip, bool* ok);
    double readF64(const std::vector<uint8_t>& code, size_t& ip, bool* ok);

    // Returns const ref to avoid string copy on every lookup
    const std::string& str(uint32_t stringIndex) const;
    
    // JIT support
#ifdef QZ_JIT_ENABLED
    std::unique_ptr<qz::jit::Engine> jitEngine_;
#endif
    bool jitEnabled_ = false;
    uint32_t jitThreshold_ = 100;
    
    // Try to execute function with JIT, returns false if should use interpreter
    bool tryJITExecute(uint32_t functionIndex, const Value* args, size_t argCount,
                       Value& result, std::string* error);
};

#endif // QZ_BYTECODE_VM_H
