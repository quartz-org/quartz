#ifndef QZ_VM_OPTIMIZATIONS_H
#define QZ_VM_OPTIMIZATIONS_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <array>
#include <type_traits>
#include <vector>
#include <unordered_set>
#include <string>

// ============================================================================
// VM Optimization Utilities
// ============================================================================

// ============================================================================
// Computed Goto Support (GCC/Clang only)
// ============================================================================
// Uses labels-as-values extension for direct threading dispatch.
// Falls back to switch dispatch on unsupported compilers.

// Computed gotos are enabled by default on GCC/Clang, but can be disabled
// via QZ_VM_COMPUTED_GOTOS=0 build flag (set in build.conf or CMake)
#if (defined(__GNUC__) || defined(__clang__)) && (!defined(QZ_VM_COMPUTED_GOTOS) || QZ_VM_COMPUTED_GOTOS)
    #define VM_USE_COMPUTED_GOTO 1
    #define VM_DISPATCH_LABEL(name) label_##name
    #define VM_DISPATCH_DECL(name) VM_DISPATCH_LABEL(name):
    #define VM_DISPATCH_ADDR(name) &&VM_DISPATCH_LABEL(name)
#else
    #define VM_USE_COMPUTED_GOTO 0
#endif

// ============================================================================
// Small Vector - Stack-allocated vector with heap fallback
// ============================================================================
// Avoids heap allocation for small collections (common case).
// Template parameters:
//   T - element type
//   N - inline capacity (number of elements stored on stack)

template<typename T, size_t N>
class SmallVector {
public:
    SmallVector() noexcept : size_(0), capacity_(N), data_(inline_storage()) {}
    
    ~SmallVector() {
        clear();
        if (data_ != inline_storage()) {
            delete[] data_;
        }
    }
    
    // Move constructor
    SmallVector(SmallVector&& other) noexcept : size_(0), capacity_(N), data_(inline_storage()) {
        if (other.data_ == other.inline_storage()) {
            // Other is using inline storage - copy elements
            for (size_t i = 0; i < other.size_; ++i) {
                new (&data_[i]) T(std::move(other.data_[i]));
            }
            size_ = other.size_;
        } else {
            // Other is using heap - steal pointer
            data_ = other.data_;
            size_ = other.size_;
            capacity_ = other.capacity_;
            other.data_ = other.inline_storage();
            other.size_ = 0;
            other.capacity_ = N;
        }
    }
    
    // Move assignment
    SmallVector& operator=(SmallVector&& other) noexcept {
        if (this != &other) {
            clear();
            if (data_ != inline_storage()) {
                delete[] data_;
            }
            
            if (other.data_ == other.inline_storage()) {
                data_ = inline_storage();
                capacity_ = N;
                for (size_t i = 0; i < other.size_; ++i) {
                    new (&data_[i]) T(std::move(other.data_[i]));
                }
                size_ = other.size_;
            } else {
                data_ = other.data_;
                size_ = other.size_;
                capacity_ = other.capacity_;
                other.data_ = other.inline_storage();
                other.size_ = 0;
                other.capacity_ = N;
            }
        }
        return *this;
    }
    
    // Disable copy (expensive)
    SmallVector(const SmallVector&) = delete;
    SmallVector& operator=(const SmallVector&) = delete;
    
    void push_back(const T& value) {
        ensure_capacity(size_ + 1);
        new (&data_[size_]) T(value);
        ++size_;
    }
    
    void push_back(T&& value) {
        ensure_capacity(size_ + 1);
        new (&data_[size_]) T(std::move(value));
        ++size_;
    }
    
    template<typename... Args>
    T& emplace_back(Args&&... args) {
        ensure_capacity(size_ + 1);
        new (&data_[size_]) T(std::forward<Args>(args)...);
        return data_[size_++];
    }
    
    void pop_back() {
        if (size_ > 0) {
            --size_;
            data_[size_].~T();
        }
    }
    
    void clear() {
        for (size_t i = 0; i < size_; ++i) {
            data_[i].~T();
        }
        size_ = 0;
    }
    
    void reserve(size_t new_cap) {
        if (new_cap > capacity_) {
            grow(new_cap);
        }
    }
    
    void resize(size_t new_size) {
        if (new_size > size_) {
            ensure_capacity(new_size);
            for (size_t i = size_; i < new_size; ++i) {
                new (&data_[i]) T();
            }
        } else {
            for (size_t i = new_size; i < size_; ++i) {
                data_[i].~T();
            }
        }
        size_ = new_size;
    }
    
    T& operator[](size_t idx) { return data_[idx]; }
    const T& operator[](size_t idx) const { return data_[idx]; }
    
    T& back() { return data_[size_ - 1]; }
    const T& back() const { return data_[size_ - 1]; }
    
    T* data() { return data_; }
    const T* data() const { return data_; }
    
    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    size_t capacity() const { return capacity_; }
    
    T* begin() { return data_; }
    T* end() { return data_ + size_; }
    const T* begin() const { return data_; }
    const T* end() const { return data_ + size_; }
    
private:
    T* inline_storage() { return reinterpret_cast<T*>(&inline_buffer_); }
    const T* inline_storage() const { return reinterpret_cast<const T*>(&inline_buffer_); }
    
    void ensure_capacity(size_t required) {
        if (required > capacity_) {
            grow(std::max(required, capacity_ * 2));
        }
    }
    
    void grow(size_t new_cap) {
        T* new_data = new T[new_cap];
        for (size_t i = 0; i < size_; ++i) {
            new (&new_data[i]) T(std::move(data_[i]));
            data_[i].~T();
        }
        if (data_ != inline_storage()) {
            delete[] data_;
        }
        data_ = new_data;
        capacity_ = new_cap;
    }
    
    size_t size_;
    size_t capacity_;
    T* data_;
    typename std::aligned_storage<sizeof(T) * N, alignof(T)>::type inline_buffer_;
};

// ============================================================================
// String Pool for Interning
// ============================================================================
// Maintains a pool of unique strings to avoid repeated allocations.
// Thread-safe for read operations, requires external sync for writes.

class StringPool {
public:
    // Get or create an interned string, returns pointer to pooled string
    const std::string* intern(const std::string& s) {
        auto it = pool_.find(s);
        if (it != pool_.end()) {
            return &(*it);
        }
        auto result = pool_.insert(s);
        return &(*result.first);
    }
    
    // Check if string is already interned
    bool contains(const std::string& s) const {
        return pool_.find(s) != pool_.end();
    }
    
    size_t size() const { return pool_.size(); }
    
    void clear() { pool_.clear(); }
    
private:
    std::unordered_set<std::string> pool_;
};

// ============================================================================
// Deferred Release Queue for Batch ARC
// ============================================================================
// Collects values to be released at scope exit instead of immediately.
// This reduces ARC overhead in tight loops by batching releases.

template<size_t InlineCapacity = 32>
class DeferredReleaseQueue {
public:
    // Add a value to be released later (only containers need tracking)
    void defer(size_t id, bool isArray) {
        if (count_ < InlineCapacity) {
            entries_[count_++] = {id, isArray};
        } else {
            overflow_.push_back({id, isArray});
        }
    }
    
    // Process all deferred releases
    template<typename ReleaseFn>
    void flush(ReleaseFn&& release) {
        for (size_t i = 0; i < count_; ++i) {
            if (entries_[i].isArray) {
                release(entries_[i].id, true);
            } else {
                release(entries_[i].id, false);
            }
        }
        for (const auto& e : overflow_) {
            release(e.id, e.isArray);
        }
        count_ = 0;
        overflow_.clear();
    }
    
    bool empty() const { return count_ == 0 && overflow_.empty(); }
    size_t size() const { return count_ + overflow_.size(); }
    
private:
    struct Entry {
        size_t id;
        bool isArray;
    };
    
    std::array<Entry, InlineCapacity> entries_;
    size_t count_ = 0;
    std::vector<Entry> overflow_;
};

// ============================================================================
// Fast Integer to String Conversion
// ============================================================================
// Avoids std::to_string overhead for common integer values

inline const char* fastIntToStr(int v, char* buf, size_t bufSize) {
    // Fast path for common small values
    static const char* smallInts[] = {
        "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
        "10", "11", "12", "13", "14", "15", "16", "17", "18", "19",
        "20", "21", "22", "23", "24", "25", "26", "27", "28", "29",
        "30", "31", "32"
    };
    
    if (v >= 0 && v <= 32) {
        return smallInts[v];
    }
    
    // General case
    char* end = buf + bufSize - 1;
    char* ptr = end;
    *ptr = '\0';
    
    bool neg = v < 0;
    if (neg) v = -v;
    
    do {
        *--ptr = '0' + (v % 10);
        v /= 10;
    } while (v > 0);
    
    if (neg) *--ptr = '-';
    
    return ptr;
}

#endif // QZ_VM_OPTIMIZATIONS_H
