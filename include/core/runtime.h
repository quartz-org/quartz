#ifndef RUNTIME_H
#define RUNTIME_H

#include "types.h"
#include "function_registry.h"
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <stdexcept>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <thread>
#include <queue>
#include <vector>
#include "qz_export.h"

// Forward declaration
class ObjectInstance;
using ObjectInstancePtr = std::shared_ptr<ObjectInstance>;

namespace qz { namespace jit { class Compiler; } }

// ============================================================================
// Runtime Exceptions
// ============================================================================

class RuntimeException : public std::runtime_error {
public:
    RuntimeException(const std::string& message, int line = -1, int column = -1)
        : std::runtime_error(formatMessage(message, line, column))
        , line(line), column(column) {}
    
    int getLine() const { return line; }
    int getColumn() const { return column; }
    
private:
    int line;
    int column;
    
    static std::string formatMessage(const std::string& msg, int line, int col) {
        if (line >= 0) {
            return msg + " [line " + std::to_string(line) + ", col " + std::to_string(col) + "]";
        }
        return msg;
    }
};

class UndefinedVariableException : public RuntimeException {
public:
    UndefinedVariableException(const std::string& varName, int line = -1, int col = -1)
        : RuntimeException("Undefined variable: '" + varName + "'", line, col) {}
};

class UndefinedClassException : public RuntimeException {
public:
    UndefinedClassException(const std::string& className, int line = -1, int col = -1)
        : RuntimeException("Undefined class: '" + className + "'", line, col) {}
};

class UndefinedMethodException : public RuntimeException {
public:
    UndefinedMethodException(const std::string& methodName, const std::string& className, int line = -1, int col = -1)
        : RuntimeException("Method '" + methodName + "' not found in class '" + className + "'", line, col) {}
};

class UndefinedFunctionException : public RuntimeException {
public:
    UndefinedFunctionException(const std::string& funcName, int line = -1, int col = -1)
        : RuntimeException("Undefined function: '" + funcName + "'", line, col) {}
};

class ImmutableVariableException : public RuntimeException {
public:
    ImmutableVariableException(const std::string& varName, int line = -1, int col = -1)
        : RuntimeException("Cannot reassign immutable variable: '" + varName + "'", line, col) {}
};

class ModuleNotFoundException : public RuntimeException {
public:
    ModuleNotFoundException(const std::string& moduleName, int line = -1, int col = -1)
        : RuntimeException("Module not found: '" + moduleName + "'", line, col) {}
};

class TypeError : public RuntimeException {
public:
    TypeError(const std::string& message, int line = -1, int col = -1)
        : RuntimeException("Type error: " + message, line, col) {}
};

// ============================================================================
// Language-Level Exception (for throw/catch in user code)
// ============================================================================

class LanguageException : public std::exception {
public:
    LanguageException(const std::string& type, const std::string& message, int line = -1, int col = -1)
        : exceptionType(type), message(message), line(line), column(col) {}
    
    const char* what() const noexcept override { return message.c_str(); }
    std::string getType() const { return exceptionType; }
    std::string getMessage() const { return message; }
    int getLine() const { return line; }
    int getColumn() const { return column; }
    
    // For exception object fields
    void setField(const std::string& name, const Value& val) { fields[name] = val; }
    Value getField(const std::string& name) const { 
        auto it = fields.find(name);
        return it != fields.end() ? it->second : Value{std::string("")};
    }
    bool hasField(const std::string& name) const { return fields.find(name) != fields.end(); }
    const std::unordered_map<std::string, Value>& getFields() const { return fields; }

private:
    std::string exceptionType;
    std::string message;
    int line;
    int column;
    std::unordered_map<std::string, Value> fields;
};

// ============================================================================
// Object Instance
// ============================================================================

// Runtime representation of a class instance
class ObjectInstance {
public:
    ObjectInstance(const std::string& className) : className(className) {}
    
    std::string getClassName() const { return className; }
    
    void setField(const std::string& name, const Value& value) {
        fields[name] = value;
    }
    
    Value getField(const std::string& name) {
        if (fields.find(name) != fields.end()) {
            return fields[name];
        }
        return Value{};
    }
    
    bool hasField(const std::string& name) const {
        return fields.find(name) != fields.end();
    }
    
private:
    std::string className;
    std::unordered_map<std::string, Value> fields;
};

enum class RuntimeState { IDLE, EXECUTING, HALTED, COMPLETED };

class Runtime {
    friend class BytecodeVM;
    friend class qz::jit::Compiler;
public:
    Runtime() : state(RuntimeState::IDLE), threadPoolShutdown(false) {}
    ~Runtime();  // Destructor to clean up thread pool

    // Initialize standard library + extensions (should be called before execution)
    void initialize();
    
    // Execution control
    void execute(const AST& ast);
    void halt();
    bool isHalted() const { return state == RuntimeState::HALTED; }
    RuntimeState getState() const { return state; }

    // ------------------------------------------------------------------------
    // Runtime lifecycle hooks + global error callbacks
    // ------------------------------------------------------------------------
    // Hooks are registered with an event name and a lambda value.
    // Supported events (convention): "start", "end", "halt", "error".
    // - start(mode)
    // - end(mode, status)
    // - halt(mode, reason)
    // - error(mode, context, message, line, col)
    bool addHook(const std::string& event, const Value& callback);
    bool clearHooks(const std::string& event);
    void clearAllHooks();
    size_t hookCount(const std::string& event) const;

    // Report an error to hooks + logger. If halt==true, runtime moves to HALTED.
    void notifyError(const std::string& context, const std::string& message,
                     int line = -1, int column = -1, bool halt = false);
    
    // Source file management (for module resolution)
    void setSourcePath(const std::string& path);
    void setSourceDirectory(const std::string& dir);
    std::string getSourceDirectory() const { return sourceDirectory; }
    
    // Variable management
    void setVariable(const std::string& name, const Value& value);
    Value getVariable(const std::string& name);
    bool hasVariable(const std::string& name) const;
    void clearVariables();
    
    // Object management
    ObjectInstancePtr createObject(const std::string& className);
    ObjectInstancePtr getObject(const std::string& name);
    bool isObjectVariable(const std::string& name) const;
    
    // Namespace/import management
    void registerImport(const std::string& ns);
    std::string resolveFunctionName(const std::string& name);
    bool hasImport(const std::string& alias) const;
    
    // Diagnostics
    size_t getVariableCount() const;
    std::vector<std::string> listVariables() const;

    // ------------------------------------------------------------------------
    // Container helpers (arrays/dicts)
    // ------------------------------------------------------------------------
    std::vector<Value>* getArray(const ArrayRef& ref);
    const std::vector<Value>* getArray(const ArrayRef& ref) const;
    std::unordered_map<std::string, Value>* getDict(const DictRef& ref);
    const std::unordered_map<std::string, Value>* getDict(const DictRef& ref) const;

    Value makeArray(std::vector<Value> elements);
    Value makeDict(std::unordered_map<std::string, Value> entries);

    // JIT helper for fast array/dict access
    Value* indexGetForJIT(const std::string& varName, int64_t index);
    static Value* indexGetForJITStub(Runtime* runtime, const std::string* varName, int64_t index);
    static Value* arrayAtForJITStub(Runtime* runtime, size_t arrayId, int64_t index);
    static size_t indexGetResolveAndPatch(Runtime* runtime, const std::string* varName, uint64_t* cacheSlot);

    // Fast path helpers for JIT inline caching
    size_t getArrayIdForVarName(const std::string& varName) const;
    size_t getDictIdForVarName(const std::string& varName) const;
    inline Value* arrayAtForJIT(size_t arrayId, size_t index) noexcept { return arrayAt(arrayId, index); }
    inline const Value* arrayAtForJIT(size_t arrayId, size_t index) const noexcept { return arrayAt(arrayId, index); }

    // ------------------------------------------------------------------------
    // Task helpers (async background work)
    // ------------------------------------------------------------------------
    enum class TaskState { Pending, Fulfilled, Rejected };
    
    // Submit work that will run on a background thread. Returns a TaskRef immediately.
    // The work function should NOT call back into the interpreter/VM.
    Value submitTask(std::function<Value()> work);
    
    // Create a delayed task that resolves to `value` after `delayMs` milliseconds.
    Value submitDelayedTask(int delayMs, const Value& value);
    
    // Check if a task has completed (fulfilled or rejected).
    bool taskReady(const TaskRef& ref) const;
    
    // Get the result of a completed task. Blocks until done.
    // Throws LanguageException if the task was rejected.
    Value taskGet(const TaskRef& ref);
    
    // Get task status as a string: "pending", "ok", or "error"
    std::string taskStatus(const TaskRef& ref) const;
    
    // Get error message if task was rejected; empty string otherwise.
    std::string taskError(const TaskRef& ref) const;

    // ------------------------------------------------------------------------
    // Buffer helpers (binary data for I/O operations)
    // ------------------------------------------------------------------------
    
    // Create a new empty buffer with optional initial capacity
    Value makeBuffer(size_t initialCapacity = 0);
    
    // Create a buffer from a string (copies bytes)
    Value makeBufferFromString(const std::string& str);
    
    // Get raw pointer to buffer data (for extensions)
    std::vector<uint8_t>* getBuffer(const BufferRef& ref);
    const std::vector<uint8_t>* getBuffer(const BufferRef& ref) const;
    
    // Get buffer size
    size_t bufferSize(const BufferRef& ref) const;
    
    // Get buffer capacity
    size_t bufferCapacity(const BufferRef& ref) const;
    
    // Convert buffer to string
    std::string bufferToString(const BufferRef& ref) const;
    
    // Append data to buffer (from string or another buffer)
    bool bufferAppendString(const BufferRef& ref, const std::string& data);
    bool bufferAppendBuffer(const BufferRef& ref, const BufferRef& other);
    
    // Slice buffer (returns new buffer with copy of data)
    Value bufferSlice(const BufferRef& ref, size_t start, size_t end);
    
    // Clear buffer contents
    bool bufferClear(const BufferRef& ref);
    
    // Copy buffer (returns new buffer)
    Value bufferCopy(const BufferRef& ref);
    
    // Byte-level access
    int bufferGetByte(const BufferRef& ref, size_t index) const;
    bool bufferSetByte(const BufferRef& ref, size_t index, uint8_t value);

    // Formatting (used by to_string(Value) and I/O)
    std::string formatValue(const Value& v, bool quoteStrings = false) const;
    std::string formatArrayById(size_t arrayId, bool quoteStrings = false) const;
    std::string formatDictById(size_t dictId, bool quoteStrings = false) const;

    // Lambda invocation helper for extensions
    Value invokeLambdaValue(const Value& lambdaVal, const std::vector<Value>& args);

    // Set by the bytecode VM during execution so extensions can invoke bytecode lambdas.
    void setExternalLambdaInvoker(std::function<Value(const std::string& lambdaId, const std::vector<Value>& args)> invoker);

    // ========================================================================
    // ARC-Style Reference Counting API
    // ========================================================================
    // Increment/decrement reference counts for heap objects (arrays, dicts).
    // These are called automatically by setVariable/clearVariables.
    // When refcount drops to zero, the slot is recycled to a free list.
    
    void retainArray(size_t id);
    void releaseArray(size_t id);
    void retainDict(size_t id);
    void releaseDict(size_t id);
    
    // Automatic ARC: retain/release any ArrayRef/DictRef inside a Value
    // These are the core building blocks for automatic memory management
    // Optimized: check variant index directly to skip type checks for primitives
    // Value = variant<int(0), double(1), string(2), bool(3), ArrayRef(4), DictRef(5), TaskRef(6), BufferRef(7)>
    inline void retainValue(const Value& v) {
        const auto idx = v.index();
        if (idx < 4) return;  // int, double, string, bool - no ARC needed
        if (idx == 4) {
            retainArray(std::get<ArrayRef>(v).id);
        } else if (idx == 5) {
            retainDict(std::get<DictRef>(v).id);
        }
        // TaskRef(6), BufferRef(7) - not managed by this ARC system
    }
    
    inline void releaseValue(const Value& v) {
        const auto idx = v.index();
        if (idx < 4) return;  // Fast path for primitives
        if (idx == 4) {
            releaseArray(std::get<ArrayRef>(v).id);
        } else if (idx == 5) {
            releaseDict(std::get<DictRef>(v).id);
        }
    }
    
    // Get current refcount (for debugging/diagnostics)
    uint32_t arrayRefCount(size_t id) const;
    uint32_t dictRefCount(size_t id) const;
    
    // Pool statistics (for diagnostics)
    size_t arrayPoolSize() const { return arrayStorage.size(); }
    size_t arrayFreeCount() const { return arrayFreeList.size(); }
    size_t dictPoolSize() const { return dictStorage.size(); }
    size_t dictFreeCount() const { return dictFreeList.size(); }

private:
    std::unordered_map<std::string, Value> variables;
    std::unordered_map<std::string, ObjectInstancePtr> objects;  // Store object instances
    
    // ========================================================================
    // ARC-Managed Array Storage
    // ========================================================================
    // Each slot has: data vector + refcount. When refcount == 0, slot is free.
    struct ArraySlot {
        std::vector<Value> data;
        uint32_t refcount = 0;  // 0 means slot is free/unallocated
    };
    std::vector<ArraySlot> arrayStorage;
    std::vector<size_t> arrayFreeList;  // Stack of free slot IDs for reuse
    
    // ========================================================================
    // ARC-Managed Dict Storage
    // ========================================================================
    struct DictSlot {
        std::unordered_map<std::string, Value> data;
        uint32_t refcount = 0;
    };
    std::vector<DictSlot> dictStorage;
    std::vector<size_t> dictFreeList;
    
    // ========================================================================
    // Fast-Path Inline Accessors (zero-overhead hot path)
    // ========================================================================
    // These bypass function call overhead for critical index operations.
    // PRECONDITION: caller must ensure id < storage.size() for safety.
    
    // Fast array element access - returns nullptr if invalid
    inline Value* arrayAt(size_t id, size_t index) noexcept {
        if (id >= arrayStorage.size()) return nullptr;
        auto& slot = arrayStorage[id];
        if (slot.refcount == 0 || index >= slot.data.size()) return nullptr;
        return &slot.data[index];
    }
    
    inline const Value* arrayAt(size_t id, size_t index) const noexcept {
        if (id >= arrayStorage.size()) return nullptr;
        const auto& slot = arrayStorage[id];
        if (slot.refcount == 0 || index >= slot.data.size()) return nullptr;
        return &slot.data[index];
    }
    
    // Fast array data pointer - returns nullptr if invalid
    inline std::vector<Value>* arrayData(size_t id) noexcept {
        if (id >= arrayStorage.size()) return nullptr;
        auto& slot = arrayStorage[id];
        return slot.refcount > 0 ? &slot.data : nullptr;
    }
    
    inline const std::vector<Value>* arrayData(size_t id) const noexcept {
        if (id >= arrayStorage.size()) return nullptr;
        const auto& slot = arrayStorage[id];
        return slot.refcount > 0 ? &slot.data : nullptr;
    }
    
    // Fast array size - returns 0 if invalid
    inline size_t arraySize(size_t id) const noexcept {
        if (id >= arrayStorage.size()) return 0;
        const auto& slot = arrayStorage[id];
        return slot.refcount > 0 ? slot.data.size() : 0;
    }
    
    // Fast dict lookup - returns nullptr if key not found or invalid
    inline Value* dictAt(size_t id, const std::string& key) noexcept {
        if (id >= dictStorage.size()) return nullptr;
        auto& slot = dictStorage[id];
        if (slot.refcount == 0) return nullptr;
        auto it = slot.data.find(key);
        return it != slot.data.end() ? &it->second : nullptr;
    }
    
    inline const Value* dictAt(size_t id, const std::string& key) const noexcept {
        if (id >= dictStorage.size()) return nullptr;
        const auto& slot = dictStorage[id];
        if (slot.refcount == 0) return nullptr;
        auto it = slot.data.find(key);
        return it != slot.data.end() ? &it->second : nullptr;
    }
    
    // Fast dict data pointer - returns nullptr if invalid
    inline std::unordered_map<std::string, Value>* dictData(size_t id) noexcept {
        if (id >= dictStorage.size()) return nullptr;
        auto& slot = dictStorage[id];
        return slot.refcount > 0 ? &slot.data : nullptr;
    }
    
    inline const std::unordered_map<std::string, Value>* dictData(size_t id) const noexcept {
        if (id >= dictStorage.size()) return nullptr;
        const auto& slot = dictStorage[id];
        return slot.refcount > 0 ? &slot.data : nullptr;
    }
    
    // Fast dict size - returns 0 if invalid
    inline size_t dictSize(size_t id) const noexcept {
        if (id >= dictStorage.size()) return 0;
        const auto& slot = dictStorage[id];
        return slot.refcount > 0 ? slot.data.size() : 0;
    }
    
    // Unchecked accessors (caller guarantees validity) - maximum performance
    inline std::vector<Value>& arrayDataUnchecked(size_t id) noexcept {
        return arrayStorage[id].data;
    }
    inline const std::vector<Value>& arrayDataUnchecked(size_t id) const noexcept {
        return arrayStorage[id].data;
    }
    inline std::unordered_map<std::string, Value>& dictDataUnchecked(size_t id) noexcept {
        return dictStorage[id].data;
    }
    inline const std::unordered_map<std::string, Value>& dictDataUnchecked(size_t id) const noexcept {
        return dictStorage[id].data;
    }

    std::unordered_map<std::string, std::vector<uint8_t>> bufferStorage;  // Store byte buffers
    mutable std::mutex bufferStorageMtx;  // Protects bufferStorage map and nextBufferId
    std::unordered_map<std::string, size_t> varToArrayId;  // Map variable name to array ID
    std::unordered_map<std::string, size_t> varToDictId;   // Map variable name to dict ID
    RuntimeState state;
    std::string executionMode = "interp"; // "interp" or "bytecode" (best-effort)
    std::unordered_map<std::string, std::string> imports;  // alias -> full namespace
    bool shouldBreak = false;     // For break statement
    bool shouldContinue = false;  // For continue statement
    bool shouldReturn = false;    // For return statement inside nested blocks
    Value pendingReturnValue;     // Value to return when shouldReturn is set
    size_t nextLambdaId = 0; // Counter for unique lambda IDs
    std::atomic<size_t> nextBufferId{0}; // Counter for unique buffer IDs (atomic for thread safety)
    
    // Lambda/Closure storage
    struct StoredLambda {
        ASTNodePtr node;  // The lambda AST node
        std::unordered_map<std::string, Value> captures;  // Captured variables
    };
    std::unordered_map<std::string, StoredLambda> lambdaStorage;  // Store lambda closures
    std::unordered_map<std::string, std::string> varToLambdaId;  // Map variable name to lambda ID

    // User-defined function storage
    struct StoredFunction {
        ASTNodePtr node;  // The FunctionDef AST node (contains params and body)
        std::vector<std::string> params;  // Parameter names for quick access
    };
    std::unordered_map<std::string, StoredFunction> userFunctions;  // name -> function definition

    // Task/async storage
    struct StoredTask {
        TaskState state = TaskState::Pending;
        Value result;
        std::string error;
        mutable std::mutex mtx;
        mutable std::condition_variable cv;
    };
    std::unordered_map<std::string, std::shared_ptr<StoredTask>> taskStorage;
    mutable std::mutex taskStorageMtx;  // Protects taskStorage map itself
    std::atomic<size_t> nextTaskId{0};

    // Thread pool for async task execution
    struct WorkItem {
        std::shared_ptr<StoredTask> task;
        std::function<Value()> work;
    };
    std::vector<std::thread> threadPoolWorkers;
    std::queue<WorkItem> workQueue;
    std::mutex workQueueMtx;
    std::condition_variable workQueueCv;
    std::atomic<bool> threadPoolShutdown;
    static constexpr size_t kThreadPoolSize = 4;  // Configurable pool size
    
    void initThreadPool();
    void threadPoolWorkerLoop();

    // If set, used to invoke lambdas by id from outside the interpreter (e.g. bytecode VM).
    std::function<Value(const std::string&, const std::vector<Value>&)> externalLambdaInvoker;
    
    // Class registry
    std::map<std::string, ClassDef> classRegistry;  // Registered class definitions
    std::map<std::string, InterfaceDef> interfaceRegistry;  // Registered interface definitions
    std::unordered_map<std::string, std::string> classToModule;  // className -> modulePath
    std::string currentThisObject;  // Current 'this' context
    std::string currentLoadingModule;  // Module currently being loaded (for class association)
    std::string sourceDirectory;    // Directory of the main source file
    std::unordered_set<std::string> loadedModules;  // Track already loaded modules
    
    // Static field storage: className::fieldName -> value
    std::unordered_map<std::string, Value> staticFields;

    // Hook storage: event -> ordered lambda IDs
    std::unordered_map<std::string, std::vector<std::string>> hooks;
    bool inErrorCallback = false;
    
    // Internal methods
    void initStandardLibrary();
    void loadExtensions();
    void registerGlobalExceptionClass();  // Register built-in Exception class
    void executeNode(const ASTNodePtr& node);
    Value evaluate(const ASTNodePtr& node);
    Value applyBinaryOp(const Value& left, const Value& right, const std::string& op);
    Value executeMethodBody(const ASTNodePtr& body);  // Execute method body and return value
    Value invokeLambda(const std::string& lambdaId, const std::vector<Value>& args);  // Call a lambda
    
    // ARC-aware scope management (used internally for lambda/method calls)
    // These properly retain/release values when switching variable scopes
    void pushScope(const std::unordered_map<std::string, Value>& newVars);
    void popScope(const std::unordered_map<std::string, Value>& savedVars);
    void retainScope(const std::unordered_map<std::string, Value>& scope);
    void releaseScope(const std::unordered_map<std::string, Value>& scope);
    
    // Module loading
    bool loadModule(const std::string& modulePath);  // Load a file-based module
    bool isExtensionLoaded(const std::string& ns) const;  // Check if namespace is from extension
    
    // Helper for error reporting
    std::string getNodeTypeString(const ASTNodePtr& node) const;
    void reportError(const std::string& context, const std::string& message);

    // Hook emission helper
    void emitHook(const std::string& event, const std::vector<Value>& args);
};

// Exported pointer to the most recently-initialized Runtime instance.
// This is used by stdlib/extension helpers that need container storage.
extern QZ_CORE_API Runtime* global_runtime_ptr;

// Quartz naming convention (Qz prefix)
using QzRuntime = Runtime;

#endif // RUNTIME_H