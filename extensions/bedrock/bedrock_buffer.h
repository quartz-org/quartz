#ifndef BEDROCK_BUFFER_H
#define BEDROCK_BUFFER_H

#include <vector>
#include <memory>
#include <cstring>
#include <string_view>

namespace bedrock {

/**
 * A reusable, fixed-capacity buffer for high-performance I/O.
 * Avoids repeated allocations by staying alive across multiple requests.
 */
class IOBuffer {
public:
    static constexpr size_t kDefaultCapacity = 16384; // 16 KB

    IOBuffer(size_t capacity = kDefaultCapacity) 
        : data_(new char[capacity])
        , capacity_(capacity)
        , size_(0) 
    {}

    // Non-copyable
    IOBuffer(const IOBuffer&) = delete;
    IOBuffer& operator=(const IOBuffer&) = delete;

    // Movable
    IOBuffer(IOBuffer&& other) noexcept
        : data_(other.data_)
        , capacity_(other.capacity_)
        , size_(other.size_) {
        other.data_ = nullptr;
        other.size_ = 0;
    }

    ~IOBuffer() {
        delete[] data_;
    }

    char* data() { return data_; }
    const char* data() const { return data_; }
    size_t size() const { return size_; }
    size_t capacity() const { return capacity_; }
    size_t remaining() const { return capacity_ - size_; }

    void append(const char* buf, size_t len) {
        if (size_ + len > capacity_) resize(size_ + len);
        if (buf) std::memcpy(data_ + size_, buf, len);
        size_ += len;
    }

    void advance(size_t len) {
        if (size_ + len > capacity_) resize(size_ + len);
        size_ += len;
    }

    void clear() {
        size_ = 0;
    }

    std::string_view view(size_t offset, size_t len) const {
        if (offset + len > size_) return "";
        return std::string_view(data_ + offset, len);
    }

    std::string_view fullView() const {
        return std::string_view(data_, size_);
    }

    void reserve(size_t newCapacity) {
        resize(newCapacity);
    }

private:
    void resize(size_t newCapacity) {
        if (newCapacity <= capacity_) return;
        char* newData = new char[newCapacity];
        std::memcpy(newData, data_, size_);
        delete[] data_;
        data_ = newData;
        capacity_ = newCapacity;
    }

    char* data_;
    size_t capacity_;
    size_t size_;
};

/**
 * A simple pool of IO buffers to avoid constant construction/destruction.
 */
class BufferPool {
public:
    static BufferPool& instance() {
        static BufferPool pool;
        return pool;
    }

    std::unique_ptr<IOBuffer> acquire() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pool_.empty()) {
            return std::make_unique<IOBuffer>();
        }
        auto buf = std::move(pool_.back());
        pool_.pop_back();
        buf->clear();
        return buf;
    }

    void release(std::unique_ptr<IOBuffer> buf) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pool_.size() < kMaxPoolSize) {
            pool_.push_back(std::move(buf));
        }
    }

private:
    BufferPool() = default;
    static constexpr size_t kMaxPoolSize = 1024;
    std::vector<std::unique_ptr<IOBuffer>> pool_;
    std::mutex mutex_;
};

} // namespace bedrock

#endif // BEDROCK_BUFFER_H
