// ============================================================================
// Runtime - Core Implementation
// Handles core runtime initialization, module loading, and variable management
// ============================================================================

#include "runtime.h"
#include "types.h"
#include "parser.h"
#include "lexer.h"
#include "syntax.h"
#include "logger.h"
#include "function_registry.h"

#include <fstream>
#include <filesystem>
#include <algorithm>
#include <dlfcn.h>
#include <cctype>
#include <thread>
#include <chrono>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace fs = std::filesystem;

Runtime* global_runtime_ptr __attribute__((visibility("default"))) = nullptr;

static fs::path getExecutableDir() {
#if defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) == 0) {
        std::error_code ec;
        fs::path p = fs::weakly_canonical(fs::path(buf.c_str()), ec);
        if (!ec) return p.parent_path();
        return fs::path(buf.c_str()).parent_path();
    }
    return fs::current_path();
#elif defined(__linux__)
    std::string buf(4096, '\0');
    ssize_t n = ::readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (n > 0) {
        buf[(size_t)n] = '\0';
        std::error_code ec;
        fs::path p = fs::weakly_canonical(fs::path(buf.c_str()), ec);
        if (!ec) return p.parent_path();
        return fs::path(buf.c_str()).parent_path();
    }
    return fs::current_path();
#else
    return fs::current_path();
#endif
}

static fs::path findExtensionsDir() {
    // Preferred: next to the executable (./build/quartz -> ./build/extensions)
    {
        fs::path candidate = getExecutableDir() / "extensions";
        if (fs::exists(candidate)) return candidate;
    }

    // Fallback: cwd-relative repo layout
    {
        fs::path candidate = fs::path("build") / "extensions";
        if (fs::exists(candidate)) return candidate;
    }

    return {};
}

// ============================================================================
// Core Runtime Methods
// ============================================================================

void Runtime::setSourcePath(const std::string& path) {
    sourceDirectory = fs::path(path).parent_path().string();
}

void Runtime::setSourceDirectory(const std::string& dir) {
    sourceDirectory = dir;
}

void Runtime::execute(const AST& ast) {
    executionMode = "interp";
    state = RuntimeState::EXECUTING;
    emitHook("start", {Value(executionMode)});

    for (const auto& node : ast.nodes) {
        if (state == RuntimeState::HALTED) break;
        executeNode(node);
    }

    if (state == RuntimeState::HALTED) {
        emitHook("halt", {Value(executionMode), Value(std::string("halted"))});
        emitHook("end", {Value(executionMode), Value(std::string("halted"))});
        return;
    }

    state = RuntimeState::COMPLETED;
    emitHook("end", {Value(executionMode), Value(std::string("completed"))});
}

void Runtime::initialize() {
    global_runtime_ptr = this;
    
    // Pre-reserve storage vectors to reduce allocation pressure
    // These are tuned for typical program sizes; they grow automatically if needed
    constexpr size_t kInitialArrayCapacity = 64;
    constexpr size_t kInitialDictCapacity = 32;
    constexpr size_t kInitialFreeListCapacity = 16;
    
    arrayStorage.reserve(kInitialArrayCapacity);
    dictStorage.reserve(kInitialDictCapacity);
    arrayFreeList.reserve(kInitialFreeListCapacity);
    dictFreeList.reserve(kInitialFreeListCapacity);
    
    initThreadPool();
    initStandardLibrary();
}

// ============================================================================
// Thread Pool Implementation
// ============================================================================

void Runtime::initThreadPool() {
    threadPoolShutdown = false;
    threadPoolWorkers.reserve(kThreadPoolSize);
    for (size_t i = 0; i < kThreadPoolSize; ++i) {
        threadPoolWorkers.emplace_back(&Runtime::threadPoolWorkerLoop, this);
    }
}

void Runtime::threadPoolWorkerLoop() {
    while (true) {
        WorkItem item;
        {
            std::unique_lock<std::mutex> lock(workQueueMtx);
            workQueueCv.wait(lock, [this]() {
                return threadPoolShutdown || !workQueue.empty();
            });
            
            if (threadPoolShutdown && workQueue.empty()) {
                return;  // Exit thread
            }
            
            item = std::move(workQueue.front());
            workQueue.pop();
        }
        
        // Execute the work item
        try {
            Value result = item.work();
            {
                std::lock_guard<std::mutex> lock(item.task->mtx);
                item.task->result = result;
                item.task->state = TaskState::Fulfilled;
            }
            item.task->cv.notify_all();
        } catch (const std::exception& e) {
            {
                std::lock_guard<std::mutex> lock(item.task->mtx);
                item.task->error = e.what();
                item.task->state = TaskState::Rejected;
            }
            item.task->cv.notify_all();
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(item.task->mtx);
                item.task->error = "Unknown error in task";
                item.task->state = TaskState::Rejected;
            }
            item.task->cv.notify_all();
        }
    }
}

Runtime::~Runtime() {
    // Signal shutdown and wake all workers
    {
        std::lock_guard<std::mutex> lock(workQueueMtx);
        threadPoolShutdown = true;
    }
    workQueueCv.notify_all();
    
    // Wait for all workers to finish
    for (auto& worker : threadPoolWorkers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    threadPoolWorkers.clear();
    
    // Clear global pointer if it points to us
    if (global_runtime_ptr == this) {
        global_runtime_ptr = nullptr;
    }
}

// ============================================================================
// ARC-Managed Array Accessors
// ============================================================================

std::vector<Value>* Runtime::getArray(const ArrayRef& ref) {
    if (ref.id >= arrayStorage.size()) return nullptr;
    auto& slot = arrayStorage[ref.id];
    if (slot.refcount == 0) return nullptr;  // Slot is free
    return &slot.data;
}

const std::vector<Value>* Runtime::getArray(const ArrayRef& ref) const {
    if (ref.id >= arrayStorage.size()) return nullptr;
    const auto& slot = arrayStorage[ref.id];
    if (slot.refcount == 0) return nullptr;
    return &slot.data;
}

std::unordered_map<std::string, Value>* Runtime::getDict(const DictRef& ref) {
    if (ref.id >= dictStorage.size()) return nullptr;
    auto& slot = dictStorage[ref.id];
    if (slot.refcount == 0) return nullptr;
    return &slot.data;
}

const std::unordered_map<std::string, Value>* Runtime::getDict(const DictRef& ref) const {
    if (ref.id >= dictStorage.size()) return nullptr;
    const auto& slot = dictStorage[ref.id];
    if (slot.refcount == 0) return nullptr;
    return &slot.data;
}

// ============================================================================
// ARC Reference Counting Implementation
// ============================================================================

void Runtime::retainArray(size_t id) {
    if (id < arrayStorage.size() && arrayStorage[id].refcount > 0) {
        ++arrayStorage[id].refcount;
    }
}

void Runtime::releaseArray(size_t id) {
    if (id >= arrayStorage.size()) return;
    auto& slot = arrayStorage[id];
    if (slot.refcount == 0) return;  // Already free
    
    if (--slot.refcount == 0) {
        // Return slot to free list; clear data but KEEP capacity for reuse (fast path).
        // Only shrink pathologically large arrays to prevent memory bloat.
        slot.data.clear();
        if (slot.data.capacity() > 1024) {
            std::vector<Value>().swap(slot.data);  // Force deallocation
        }
        arrayFreeList.push_back(id);
    }
}

void Runtime::retainDict(size_t id) {
    if (id < dictStorage.size() && dictStorage[id].refcount > 0) {
        ++dictStorage[id].refcount;
    }
}

void Runtime::releaseDict(size_t id) {
    if (id >= dictStorage.size()) return;
    auto& slot = dictStorage[id];
    if (slot.refcount == 0) return;
    
    if (--slot.refcount == 0) {
        // Clear data but keep bucket structure for reuse (avoids rehashing on next use).
        // Only deallocate pathologically large dicts.
        slot.data.clear();
        if (slot.data.bucket_count() > 256) {
            std::unordered_map<std::string, Value>().swap(slot.data);
        }
        dictFreeList.push_back(id);
    }
}

uint32_t Runtime::arrayRefCount(size_t id) const {
    if (id >= arrayStorage.size()) return 0;
    return arrayStorage[id].refcount;
}

uint32_t Runtime::dictRefCount(size_t id) const {
    if (id >= dictStorage.size()) return 0;
    return dictStorage[id].refcount;
}

// ============================================================================
// ARC Scope Management
// ============================================================================
// These helpers properly manage reference counts when switching variable scopes
// (e.g., during lambda invocation or method calls)

void Runtime::retainScope(const std::unordered_map<std::string, Value>& scope) {
    for (const auto& pair : scope) {
        retainValue(pair.second);
    }
}

void Runtime::releaseScope(const std::unordered_map<std::string, Value>& scope) {
    for (const auto& pair : scope) {
        releaseValue(pair.second);
    }
}

void Runtime::pushScope(const std::unordered_map<std::string, Value>& newVars) {
    // Release current scope, then set new scope and retain it
    releaseScope(variables);
    variables = newVars;
    retainScope(variables);
}

void Runtime::popScope(const std::unordered_map<std::string, Value>& savedVars) {
    // Release current scope, then restore saved scope (already retained)
    releaseScope(variables);
    variables = savedVars;
}

// ============================================================================
// ARC-Managed Allocation (with free-list reuse)
// ============================================================================

Value Runtime::makeArray(std::vector<Value> elements) {
    size_t arrayId;
    if (!arrayFreeList.empty()) {
        // Reuse a free slot (ARC optimization)
        arrayId = arrayFreeList.back();
        arrayFreeList.pop_back();
        arrayStorage[arrayId].data = std::move(elements);
        arrayStorage[arrayId].refcount = 1;
    } else {
        // Allocate new slot
        arrayId = arrayStorage.size();
        arrayStorage.push_back(ArraySlot{std::move(elements), 1});
    }
    return Value(ArrayRef{arrayId});
}

Value Runtime::makeDict(std::unordered_map<std::string, Value> entries) {
    size_t dictId;
    if (!dictFreeList.empty()) {
        dictId = dictFreeList.back();
        dictFreeList.pop_back();
        auto& slot = dictStorage[dictId];
        // Reuse slot - if entries is larger, rehash will happen anyway
        // If slot has adequate buckets, swap is faster than move
        slot.data = std::move(entries);
        slot.refcount = 1;
    } else {
        dictId = dictStorage.size();
        // For new dicts, reserve minimum buckets to reduce rehashing
        if (entries.empty()) {
            entries.reserve(8);  // Reasonable default for small dicts
        }
        dictStorage.push_back(DictSlot{std::move(entries), 1});
    }
    return Value(DictRef{dictId});
}

size_t Runtime::getArrayIdForVarName(const std::string& varName) const {
    auto it = varToArrayId.find(varName);
    if (it != varToArrayId.end()) {
        return it->second;
    }
    return std::numeric_limits<size_t>::max();
}

size_t Runtime::getDictIdForVarName(const std::string& varName) const {
    auto it = varToDictId.find(varName);
    if (it != varToDictId.end()) {
        return it->second;
    }
    return std::numeric_limits<size_t>::max();
}



Value* Runtime::indexGetForJIT(const std::string& varName, int64_t index) {
    auto it = varToArrayId.find(varName);
    if (it != varToArrayId.end()) {
        return arrayAt(it->second, static_cast<size_t>(index));
    }
    auto dictIt = varToDictId.find(varName);
    if (dictIt != varToDictId.end()) {
        // dict access not implemented yet
        return nullptr;
    }
    return nullptr;
}

Value* Runtime::indexGetForJITStub(Runtime* runtime, const std::string* varName, int64_t index) {
    if (varName) {
        return runtime->indexGetForJIT(*varName, index);
    }
    return nullptr;
}

Value* Runtime::arrayAtForJITStub(Runtime* runtime, size_t arrayId, int64_t index) {
    return runtime->arrayAtForJIT(arrayId, static_cast<size_t>(index));
}

size_t Runtime::indexGetResolveAndPatch(Runtime* runtime, const std::string* varName, uint64_t* cacheSlot) {
    if (!varName || !cacheSlot) return std::numeric_limits<size_t>::max();
    size_t arrayId = runtime->getArrayIdForVarName(*varName);
    if (arrayId != std::numeric_limits<size_t>::max()) {
        *cacheSlot = arrayId;
    }
    return arrayId;
}

// ============================================================================
// Task/Async Helpers
// ============================================================================

Value Runtime::submitTask(std::function<Value()> work) {
    std::string taskId = "__task_" + std::to_string(nextTaskId++);
    auto task = std::make_shared<StoredTask>();
    {
        std::lock_guard<std::mutex> lock(taskStorageMtx);
        taskStorage[taskId] = task;
    }
    
    // Queue work for thread pool execution
    {
        std::lock_guard<std::mutex> lock(workQueueMtx);
        workQueue.push(WorkItem{task, std::move(work)});
    }
    workQueueCv.notify_one();
    
    return Value(TaskRef{taskId});
}

Value Runtime::submitDelayedTask(int delayMs, const Value& value) {
    // Copy value to capture in lambda (since it may be moved)
    Value capturedValue = value;
    return submitTask([delayMs, capturedValue]() -> Value {
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        return capturedValue;
    });
}

bool Runtime::taskReady(const TaskRef& ref) const {
    std::shared_ptr<StoredTask> task;
    {
        std::lock_guard<std::mutex> lock(taskStorageMtx);
        auto it = taskStorage.find(ref.id);
        if (it == taskStorage.end()) return true;  // Unknown task treated as done
        task = it->second;
    }
    std::lock_guard<std::mutex> lock(task->mtx);
    return task->state != TaskState::Pending;
}

Value Runtime::taskGet(const TaskRef& ref) {
    std::shared_ptr<StoredTask> task;
    {
        std::lock_guard<std::mutex> lock(taskStorageMtx);
        auto it = taskStorage.find(ref.id);
        if (it == taskStorage.end()) {
            throw LanguageException("RuntimeError", "Unknown task: " + ref.id);
        }
        task = it->second;
    }
    
    // Wait for completion
    {
        std::unique_lock<std::mutex> lock(task->mtx);
        task->cv.wait(lock, [&task]() { return task->state != TaskState::Pending; });
        
        if (task->state == TaskState::Rejected) {
            throw LanguageException("RuntimeError", task->error);
        }
        return task->result;
    }
}

std::string Runtime::taskStatus(const TaskRef& ref) const {
    std::shared_ptr<StoredTask> task;
    {
        std::lock_guard<std::mutex> lock(taskStorageMtx);
        auto it = taskStorage.find(ref.id);
        if (it == taskStorage.end()) return "error";
        task = it->second;
    }
    std::lock_guard<std::mutex> lock(task->mtx);
    switch (task->state) {
        case TaskState::Pending: return "pending";
        case TaskState::Fulfilled: return "ok";
        case TaskState::Rejected: return "error";
    }
    return "unknown";
}

std::string Runtime::taskError(const TaskRef& ref) const {
    std::shared_ptr<StoredTask> task;
    {
        std::lock_guard<std::mutex> lock(taskStorageMtx);
        auto it = taskStorage.find(ref.id);
        if (it == taskStorage.end()) return "Unknown task";
        task = it->second;
    }
    std::lock_guard<std::mutex> lock(task->mtx);
    return task->error;
}

// ============================================================================
// Buffer Helpers
// ============================================================================

Value Runtime::makeBuffer(size_t initialCapacity) {
    std::lock_guard<std::mutex> lock(bufferStorageMtx);
    std::string bufferId = "__buffer_" + std::to_string(nextBufferId++);
    bufferStorage[bufferId] = std::vector<uint8_t>();
    if (initialCapacity > 0) {
        bufferStorage[bufferId].reserve(initialCapacity);
    }
    return Value(BufferRef{bufferId});
}

Value Runtime::makeBufferFromString(const std::string& str) {
    std::lock_guard<std::mutex> lock(bufferStorageMtx);
    std::string bufferId = "__buffer_" + std::to_string(nextBufferId++);
    bufferStorage[bufferId] = std::vector<uint8_t>(str.begin(), str.end());
    return Value(BufferRef{bufferId});
}

std::vector<uint8_t>* Runtime::getBuffer(const BufferRef& ref) {
    std::lock_guard<std::mutex> lock(bufferStorageMtx);
    auto it = bufferStorage.find(ref.id);
    if (it == bufferStorage.end()) return nullptr;
    return &it->second;
}

const std::vector<uint8_t>* Runtime::getBuffer(const BufferRef& ref) const {
    std::lock_guard<std::mutex> lock(bufferStorageMtx);
    auto it = bufferStorage.find(ref.id);
    if (it == bufferStorage.end()) return nullptr;
    return &it->second;
}

// Fast, predictable double formatting.
// - Uses std::to_chars (no locale, no allocations)
// - Emits a compact representation (no trailing zeros)
// - Handles NaN/Inf explicitly
static inline void appendDoubleFast(std::string& out, double v) {
    if (std::isnan(v)) {
        out += "nan";
        return;
    }
    if (std::isinf(v)) {
        out += (v < 0) ? "-inf" : "inf";
        return;
    }

    // Fits typical double text forms, including scientific.
    // 64 is conservative; avoids overflow for extreme exponents.
    char buf[64];
    auto res = std::to_chars(std::begin(buf), std::end(buf), v, std::chars_format::general);
    if (res.ec != std::errc{}) {
        // Fallback: should be rare. Keep behavior correct over fast.
        out += std::to_string(v);
        return;
    }

    // Trim trailing zeros in the fractional part when not using scientific notation.
    // std::to_chars(general) may still emit trailing zeros depending on lib.
    char* begin = buf;
    char* end = res.ptr;
    char* ePos = static_cast<char*>(memchr(begin, 'e', end - begin));
    if (!ePos) ePos = static_cast<char*>(memchr(begin, 'E', end - begin));
    char* dotPos = static_cast<char*>(memchr(begin, '.', (ePos ? (ePos - begin) : (end - begin))));
    if (dotPos) {
        char* trimEnd = ePos ? ePos : end;
        while (trimEnd > dotPos + 1 && *(trimEnd - 1) == '0') {
            --trimEnd;
        }
        if (trimEnd > dotPos && *(trimEnd - 1) == '.') {
            --trimEnd;
        }

        out.append(begin, trimEnd - begin);
        if (ePos) {
            out.append(ePos, end - ePos);
        }
        return;
    }

    out.append(begin, end - begin);
}

size_t Runtime::bufferSize(const BufferRef& ref) const {
    std::lock_guard<std::mutex> lock(bufferStorageMtx);
    auto it = bufferStorage.find(ref.id);
    if (it == bufferStorage.end()) return 0;
    return it->second.size();
}

size_t Runtime::bufferCapacity(const BufferRef& ref) const {
    std::lock_guard<std::mutex> lock(bufferStorageMtx);
    auto it = bufferStorage.find(ref.id);
    if (it == bufferStorage.end()) return 0;
    return it->second.capacity();
}

std::string Runtime::bufferToString(const BufferRef& ref) const {
    std::lock_guard<std::mutex> lock(bufferStorageMtx);
    auto it = bufferStorage.find(ref.id);
    if (it == bufferStorage.end()) return "";
    return std::string(it->second.begin(), it->second.end());
}

bool Runtime::bufferAppendString(const BufferRef& ref, const std::string& data) {
    std::lock_guard<std::mutex> lock(bufferStorageMtx);
    auto it = bufferStorage.find(ref.id);
    if (it == bufferStorage.end()) return false;
    it->second.insert(it->second.end(), data.begin(), data.end());
    return true;
}

bool Runtime::bufferAppendBuffer(const BufferRef& ref, const BufferRef& other) {
    std::lock_guard<std::mutex> lock(bufferStorageMtx);
    auto it = bufferStorage.find(ref.id);
    auto otherIt = bufferStorage.find(other.id);
    if (it == bufferStorage.end() || otherIt == bufferStorage.end()) return false;
    it->second.insert(it->second.end(), otherIt->second.begin(), otherIt->second.end());
    return true;
}

Value Runtime::bufferSlice(const BufferRef& ref, size_t start, size_t end) {
    std::lock_guard<std::mutex> lock(bufferStorageMtx);
    auto it = bufferStorage.find(ref.id);
    if (it == bufferStorage.end()) {
        std::string bufferId = "__buffer_" + std::to_string(nextBufferId++);
        bufferStorage[bufferId] = std::vector<uint8_t>();
        return Value(BufferRef{bufferId});
    }
    
    auto& buf = it->second;
    if (start >= buf.size()) start = buf.size();
    if (end > buf.size()) end = buf.size();
    if (start > end) start = end;
    
    std::string newBufferId = "__buffer_" + std::to_string(nextBufferId++);
    bufferStorage[newBufferId] = std::vector<uint8_t>(buf.begin() + start, buf.begin() + end);
    return Value(BufferRef{newBufferId});
}

bool Runtime::bufferClear(const BufferRef& ref) {
    std::lock_guard<std::mutex> lock(bufferStorageMtx);
    auto it = bufferStorage.find(ref.id);
    if (it == bufferStorage.end()) return false;
    it->second.clear();
    return true;
}

Value Runtime::bufferCopy(const BufferRef& ref) {
    std::lock_guard<std::mutex> lock(bufferStorageMtx);
    auto it = bufferStorage.find(ref.id);
    if (it == bufferStorage.end()) {
        std::string bufferId = "__buffer_" + std::to_string(nextBufferId++);
        bufferStorage[bufferId] = std::vector<uint8_t>();
        return Value(BufferRef{bufferId});
    }
    
    std::string newBufferId = "__buffer_" + std::to_string(nextBufferId++);
    bufferStorage[newBufferId] = it->second;
    return Value(BufferRef{newBufferId});
}

int Runtime::bufferGetByte(const BufferRef& ref, size_t index) const {
    std::lock_guard<std::mutex> lock(bufferStorageMtx);
    auto it = bufferStorage.find(ref.id);
    if (it == bufferStorage.end() || index >= it->second.size()) return -1;
    return static_cast<int>(it->second[index]);
}

bool Runtime::bufferSetByte(const BufferRef& ref, size_t index, uint8_t value) {
    std::lock_guard<std::mutex> lock(bufferStorageMtx);
    auto it = bufferStorage.find(ref.id);
    if (it == bufferStorage.end() || index >= it->second.size()) return false;
    it->second[index] = value;
    return true;
}

static inline void appendFormatted(std::string& out, const Runtime* rt, const Value& val, bool quoteStrings) {
    std::visit([&](auto&& arg) {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, int>) {
            out += std::to_string(arg);
        } else if constexpr (std::is_same_v<T, double>) {
            appendDoubleFast(out, arg);
        } else if constexpr (std::is_same_v<T, bool>) {
            out += arg ? "true" : "false";
        } else if constexpr (std::is_same_v<T, std::string>) {
            if (quoteStrings) {
                out += '"';
                out += arg;
                out += '"';
            } else {
                out += arg;
            }
        } else if constexpr (std::is_same_v<T, ArrayRef>) {
            if (rt) out += rt->formatArrayById(arg.id, quoteStrings);
            else out += "<array>";
        } else if constexpr (std::is_same_v<T, DictRef>) {
            if (rt) out += rt->formatDictById(arg.id, quoteStrings);
            else out += "<dict>";
        } else if constexpr (std::is_same_v<T, TaskRef>) {
            out += "<task:";
            out += arg.id;
            out += ">";
        } else if constexpr (std::is_same_v<T, BufferRef>) {
            out += "<buffer:";
            out += arg.id;
            if (rt) {
                out += ",size=";
                out += std::to_string(rt->bufferSize(arg));
            }
            out += ">";
        }
    }, val);
}

std::string Runtime::formatArrayById(size_t arrayId, bool quoteStrings) const {
    if (arrayId >= arrayStorage.size()) return "[]";
    const auto& slot = arrayStorage[arrayId];
    if (slot.refcount == 0) return "[]";  // Slot is free
    const auto& vec = slot.data;
    std::string out;
    out.reserve(2 + vec.size() * 8);
    out += "[";
    for (size_t i = 0; i < vec.size(); ++i) {
        if (i) out += ", ";
        appendFormatted(out, this, vec[i], true /*quote strings inside containers*/);
    }
    out += "]";
    return out;
}

std::string Runtime::formatDictById(size_t dictId, bool quoteStrings) const {
    if (dictId >= dictStorage.size()) return "{}";
    const auto& slot = dictStorage[dictId];
    if (slot.refcount == 0) return "{}";
    const auto& dict = slot.data;
    std::string out;
    out.reserve(2 + dict.size() * 16);
    out += "{";
    bool first = true;
    for (const auto& kv : dict) {
        if (!first) out += ", ";
        first = false;
        out += '"';
        out += kv.first;
        out += "\": ";
        appendFormatted(out, this, kv.second, true /*quote strings inside containers*/);
    }
    out += "}";
    return out;
}

std::string Runtime::formatValue(const Value& v, bool quoteStrings) const {
    std::string out;
    out.reserve(32);
    appendFormatted(out, this, v, quoteStrings);
    return out;
}

Value Runtime::invokeLambdaValue(const Value& lambdaVal, const std::vector<Value>& args) {
    if (!std::holds_alternative<std::string>(lambdaVal)) return Value{};
    const std::string& id = std::get<std::string>(lambdaVal);
    if (id.rfind("__lambda_", 0) != 0) return Value{};

    if (externalLambdaInvoker) {
        return externalLambdaInvoker(id, args);
    }

    return invokeLambda(id, args);
}

void Runtime::setExternalLambdaInvoker(
    std::function<Value(const std::string& lambdaId, const std::vector<Value>& args)> invoker) {
    externalLambdaInvoker = std::move(invoker);
}

void Runtime::setVariable(const std::string& name, const Value& val) {
    // ARC: release old value if it exists, retain new value
    auto it = variables.find(name);
    if (it != variables.end()) {
        releaseValue(it->second);  // Release old
    }
    retainValue(val);  // Retain new
    variables[name] = val;
}

void Runtime::registerImport(const std::string& ns) {
    imports[ns] = ns;
}

std::string Runtime::resolveFunctionName(const std::string& name) {
    size_t dotPos = name.find('.');
    if (dotPos == std::string::npos) {
        return name;
    }
    std::string prefix = name.substr(0, dotPos);
    std::string rest = name.substr(dotPos + 1);
    auto it = imports.find(prefix);
    if (it != imports.end()) {
        return it->second + "." + rest;
    }
    return name;
}

// ============================================================================
// Standard Library and Extensions
// ============================================================================

void Runtime::initStandardLibrary() {
    loadExtensions();
    registerGlobalExceptionClass();
}

void Runtime::registerGlobalExceptionClass() {
    // Register base Exception class - available globally without import
    ClassDef exceptionClass;
    exceptionClass.name = "Exception";
    exceptionClass.fields.push_back({"message", TypeAnnotation("string")});
    exceptionClass.fields.push_back({"type", TypeAnnotation("string")});
    
    // Add getMessage method
    MethodDef getMessage;
    getMessage.name = "getMessage";
    getMessage.visibility = "public";
    getMessage.returnType = TypeAnnotation("string");
    getMessage.body = nullptr;  // Built-in implementation
    exceptionClass.methods.push_back(getMessage);
    
    // Add getType method
    MethodDef getType;
    getType.name = "getType";
    getType.visibility = "public";
    getType.returnType = TypeAnnotation("string");
    getType.body = nullptr;  // Built-in implementation
    exceptionClass.methods.push_back(getType);
    
    classRegistry["Exception"] = exceptionClass;
    
    // Register derived exception types
    ClassDef runtimeError = exceptionClass;
    runtimeError.name = "RuntimeError";
    runtimeError.parentClass = "Exception";
    classRegistry["RuntimeError"] = runtimeError;
    
    ClassDef valueError = exceptionClass;
    valueError.name = "ValueError";
    valueError.parentClass = "Exception";
    classRegistry["ValueError"] = valueError;
    
    ClassDef typeError = exceptionClass;
    typeError.name = "TypeError";
    typeError.parentClass = "Exception";
    classRegistry["TypeError"] = typeError;
    
    ClassDef indexError = exceptionClass;
    indexError.name = "IndexError";
    indexError.parentClass = "Exception";
    classRegistry["IndexError"] = indexError;
    
    ClassDef nullError = exceptionClass;
    nullError.name = "NullError";
    classRegistry["NullError"] = nullError;

    ClassDef arithmeticError = exceptionClass;
    arithmeticError.name = "ArithmeticError";
    arithmeticError.parentClass = "Exception";
    classRegistry["ArithmeticError"] = arithmeticError;

    ClassDef keyError = exceptionClass;
    keyError.name = "KeyError";
    keyError.parentClass = "Exception";
    classRegistry["KeyError"] = keyError;
}

void Runtime::loadExtensions() {
    // Look for extensions in the build directory
    fs::path base_ext_dir = findExtensionsDir();
    if (base_ext_dir.empty() || !fs::exists(base_ext_dir)) {
        return;
    }
    
    try {
        // Iterate through each extension subdirectory
        for (auto& ext_dir : fs::directory_iterator(base_ext_dir)) {
            if (!fs::is_directory(ext_dir)) continue;
            
            // Look for shared library files in each extension directory
            // .dylib on macOS, .so on Linux
            for (auto& p : fs::recursive_directory_iterator(ext_dir)) {
                std::string ext = p.path().extension().string();
                if (ext == ".dylib" || ext == ".so" || ext == ".dll") {
                    void* handle = dlopen(p.path().c_str(), RTLD_LAZY | RTLD_GLOBAL);
                    if (handle) {
                        // Try with underscore first (macOS convention)
                        auto init = (void (*)(FunctionRegistry&)) dlsym(handle, "_init_extension");
                        if (!init) {
                            // Try without underscore
                            init = (void (*)(FunctionRegistry&)) dlsym(handle, "init_extension");
                        }
                        if (init) {
                            init(FunctionRegistry::instance());
                        }
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        Logger::instance().log(LogLevel::DEBUG, std::string("Exception in loadExtensions: ") + e.what());
    }
}

bool Runtime::isExtensionLoaded(const std::string& ns) const {
    return FunctionRegistry::instance().hasNamespace(ns);
}

// ============================================================================
// Module Loading
// ============================================================================

bool Runtime::loadModule(const std::string& modulePath) {
    // Convert module path (e.g., "mymodule.utils") to directory path (e.g., "mymodule/utils")
    std::string dirPath = modulePath;
    std::replace(dirPath.begin(), dirPath.end(), '.', '/');
    
    // Build full path relative to source file directory
    fs::path fullPath = fs::path(sourceDirectory) / dirPath;
    
    // Check if directory exists
    if (!fs::exists(fullPath) || !fs::is_directory(fullPath)) {
        return false;
    }
    
    // Track loaded modules to prevent circular imports
    if (loadedModules.find(modulePath) != loadedModules.end()) {
        return true;  // Already loaded
    }
    loadedModules.insert(modulePath);
    
    // Set current loading module for class association
    std::string previousModule = currentLoadingModule;
    currentLoadingModule = modulePath;
    
    Logger::instance().log(LogLevel::DEBUG, "Loading module from: " + fullPath.string());
    
    // Load all .qz files in the directory
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(fullPath)) {
        if (entry.path().extension() == ".qz") {
            files.push_back(entry.path());
        }
    }
    
    // Sort files for deterministic loading order
    std::sort(files.begin(), files.end());
    
    // Parse and execute each file
    for (const auto& file : files) {
        std::ifstream sourceFile(file);
        if (!sourceFile.is_open()) {
            Logger::instance().log(LogLevel::ERROR, "Could not open module file: " + file.string());
            continue;
        }
        
        std::string sourceCode((std::istreambuf_iterator<char>(sourceFile)), std::istreambuf_iterator<char>());
        sourceFile.close();
        
        Logger::instance().log(LogLevel::DEBUG, "Parsing module file: " + file.filename().string());
        
        // Tokenize and parse
        SyntaxConfig config;
        Lexer lexer(sourceCode, config);
        std::vector<Token> tokens = lexer.tokenize();
        
        Parser parser(tokens, sourceCode);
        
        try {
            AST ast = parser.parse();
            
            // Execute the module's AST (this will register classes, functions, etc.)
            for (const auto& node : ast.nodes) {
                if (state == RuntimeState::HALTED) break;
                executeNode(node);
            }
        } catch (const std::exception& ex) {
            Logger::instance().log(LogLevel::ERROR, "Error loading module file " + file.string() + ": " + ex.what());
            currentLoadingModule = previousModule;
            return false;
        }
    }
    
    // Restore previous module context
    currentLoadingModule = previousModule;
    
    return true;
}

// ============================================================================
// Runtime Control
// ============================================================================

void Runtime::halt() {
    if (state == RuntimeState::EXECUTING) {
        state = RuntimeState::HALTED;
        emitHook("halt", {Value(executionMode), Value(std::string("halt()"))});
    }
}

// ============================================================================
// Hooks + Global Error Callbacks
// ============================================================================

static inline std::string normalizeEvent(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

static inline bool isLambdaIdValue(const Value& v) {
    if (!std::holds_alternative<std::string>(v)) return false;
    const std::string& s = std::get<std::string>(v);
    return s.rfind("__lambda_", 0) == 0;
}

bool Runtime::addHook(const std::string& event, const Value& callback) {
    if (!isLambdaIdValue(callback)) return false;
    std::string ev = normalizeEvent(event);
    hooks[ev].push_back(std::get<std::string>(callback));
    return true;
}

bool Runtime::clearHooks(const std::string& event) {
    std::string ev = normalizeEvent(event);
    auto it = hooks.find(ev);
    if (it == hooks.end()) return false;
    hooks.erase(it);
    return true;
}

void Runtime::clearAllHooks() {
    hooks.clear();
}

size_t Runtime::hookCount(const std::string& event) const {
    std::string ev = normalizeEvent(event);
    auto it = hooks.find(ev);
    if (it == hooks.end()) return 0;
    return it->second.size();
}

void Runtime::emitHook(const std::string& event, const std::vector<Value>& args) {
    std::string ev = normalizeEvent(event);
    auto it = hooks.find(ev);
    if (it == hooks.end()) return;
    for (const auto& lambdaId : it->second) {
        try {
            (void)invokeLambdaValue(Value(lambdaId), args);
        } catch (const std::exception& e) {
            // Avoid hook failures crashing the runtime; log and continue.
            Logger::instance().log(LogLevel::ERROR, std::string("Hook '") + ev + "' failed: " + e.what());
        } catch (...) {
            Logger::instance().log(LogLevel::ERROR, std::string("Hook '") + ev + "' failed");
        }
    }
}

void Runtime::notifyError(const std::string& context, const std::string& message,
                          int line, int column, bool haltNow) {
    // Log the error (keeps existing behavior where errors are visible by default).
    std::string msg = context + ": " + message;
    if (line >= 0) {
        msg += " [line " + std::to_string(line) + ", col " + std::to_string(column) + "]";
    }
    Logger::instance().log(LogLevel::ERROR, msg);

    // Invoke error callbacks (guard against recursive errors).
    if (!inErrorCallback) {
        inErrorCallback = true;
        emitHook("error", {Value(executionMode), Value(context), Value(message), Value(line), Value(column)});
        inErrorCallback = false;
    }

    if (haltNow) {
        state = RuntimeState::HALTED;
    }
}

// ============================================================================
// Object Management
// ============================================================================

ObjectInstancePtr Runtime::createObject(const std::string& className) {
    auto instance = std::make_shared<ObjectInstance>(className);
    return instance;
}

ObjectInstancePtr Runtime::getObject(const std::string& name) {
    auto it = objects.find(name);
    if (it != objects.end()) {
        return it->second;
    }
    return nullptr;
}

bool Runtime::isObjectVariable(const std::string& name) const {
    return objects.find(name) != objects.end();
}

// ============================================================================
// Variable Management
// ============================================================================

bool Runtime::hasVariable(const std::string& name) const {
    return variables.find(name) != variables.end();
}

void Runtime::clearVariables() {
    // ARC: release all values before clearing
    for (const auto& pair : variables) {
        releaseValue(pair.second);
    }
    variables.clear();
    objects.clear();
}

size_t Runtime::getVariableCount() const {
    return variables.size() + objects.size();
}

std::vector<std::string> Runtime::listVariables() const {
    std::vector<std::string> result;
    for (const auto& pair : variables) {
        result.push_back(pair.first);
    }
    for (const auto& pair : objects) {
        result.push_back(pair.first + " (object)");
    }
    return result;
}

Value Runtime::getVariable(const std::string& name) {
    auto it = variables.find(name);
    if (it != variables.end()) {
        return it->second;
    }
    return Value{}; // Default
}
