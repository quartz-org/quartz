// ============================================================================
// Bedrock Async - High-Performance Event-Driven Server Implementation
// Cross-platform: Linux (epoll), macOS (kqueue), Windows (select)
// ============================================================================

#include "bedrock_async.h"
#include <algorithm>
#include <sstream>
#include <cstring>
#include <iostream>

#ifdef QZ_PLATFORM_LINUX
#include <signal.h>
#include <sys/eventfd.h>
#include <unistd.h>
#elif defined(QZ_PLATFORM_MACOS)
#include <unistd.h>
#include <fcntl.h>
#endif

namespace bedrock {

// ============================================================================
// AsyncConnection Implementation
// ============================================================================

AsyncConnection::AsyncConnection(socket_t fd, const std::string& remoteAddr, int remotePort)
    : fd_(fd)
    , remoteAddr_(remoteAddr)
    , remotePort_(remotePort)
    , lastActivity_(std::chrono::steady_clock::now())
{
    readBuffer_ = BufferPool::instance().acquire();
}

AsyncConnection::~AsyncConnection() {
    if (fd_ != INVALID_SOCKET_VALUE) {
        close_socket(fd_);
    }
}

bool AsyncConnection::setNonBlocking(bool nonBlocking) {
#ifdef QZ_PLATFORM_WINDOWS
    u_long mode = nonBlocking ? 1 : 0;
    return ioctlsocket(fd_, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(fd_, F_GETFL, 0);
    if (flags < 0) return false;
    if (nonBlocking) {
        flags |= O_NONBLOCK;
    } else {
        flags &= ~O_NONBLOCK;
    }
    return fcntl(fd_, F_SETFL, flags) == 0;
#endif
}

int AsyncConnection::readNonBlocking() {
    int totalRead = 0;
    
    while (true) {
        if (readBuffer_->remaining() < 4096) {
            readBuffer_->reserve(readBuffer_->capacity() + 8192);
        }
        
        char* ptr = readBuffer_->data() + readBuffer_->size();
        size_t space = readBuffer_->remaining();

#ifdef QZ_PLATFORM_WINDOWS
        int received = recv(fd_, ptr, static_cast<int>(space), 0);
        if (received == 0) return -1; // Closed
        if (received == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEWOULDBLOCK) break;
            return -1;
        }
#else
        ssize_t received = recv(fd_, ptr, space, 0);
        if (received == 0) return -1; // Closed
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return -1;
        }
#endif
        
        readBuffer_->advance(static_cast<size_t>(received)); // Just update size
        totalRead += static_cast<int>(received);
        
        if (readBuffer_->size() > 1024 * 1024) break; 
    }
    
    if (totalRead > 0) touch();
    
    // Try to parse
    parseRequest();
    
    return totalRead;
}

int AsyncConnection::writeNonBlocking() {
    int totalSent = 0;
    
    while (iovIndex_ < writeIov_.size()) {
        const auto& iov = writeIov_[iovIndex_];
        const char* data = iov.data + iovOffset_;
        size_t len = iov.len - iovOffset_;
        
        int flags = 0;
#ifdef QZ_PLATFORM_LINUX
        flags = MSG_NOSIGNAL;
#endif
        
#ifdef QZ_PLATFORM_WINDOWS
        int sent = send(fd_, data, static_cast<int>(len), flags);
        if (sent == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEWOULDBLOCK) break;
            return -1;
        }
#else
        ssize_t sent = send(fd_, data, len, flags);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return -1;
        }
#endif
        
        iovOffset_ += sent;
        totalSent += static_cast<int>(sent);
        
        if (iovOffset_ >= iov.len) {
            iovIndex_++;
            iovOffset_ = 0;
        }
        
        // Don't loop forever if we're sending a lot of small chunks
        if (totalSent > 65536) break; 
    }
    
    if (totalSent > 0) touch();
    return totalSent;
}

bool AsyncConnection::hasCompleteRequest() const {
    return headersParsed_ && (bodyRead_ >= contentLength_);
}

void AsyncConnection::setResponse(const Response& response) {
    response_ = response;
    writeIov_ = response_.buildIov();
    iovIndex_ = 0;
    iovOffset_ = 0;
}

void AsyncConnection::reset() {
    readBuffer_->clear();
    request_ = Request();
    contentLength_ = 0;
    bodyRead_ = 0;
    headersParsed_ = false;
    writeIov_.clear();
    iovIndex_ = 0;
    iovOffset_ = 0;
    state_ = ConnectionState::READING_REQUEST;
    touch();
}

bool AsyncConnection::isTimedOut(int timeoutMs) const {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastActivity_).count();
    return elapsed > timeoutMs;
}

bool AsyncConnection::parseRequest() {
    std::string_view buf = readBuffer_->fullView();
    
    if (headersParsed_) {
        // Reading body
        size_t headersEnd = buf.find("\r\n\r\n");
        if (headersEnd != std::string_view::npos) {
            size_t bodyStart = headersEnd + 4;
            size_t bodyAvailable = buf.size() - bodyStart;
            bodyRead_ = bodyAvailable;
            
            if (bodyRead_ >= contentLength_) {
                // Complete request
                request_.body = buf.substr(bodyStart, contentLength_);
                request_.buffer = readBuffer_; // Keep buffer alive
                return true;
            }
        }
        return false;
    }
    
    // Look for end of headers
    size_t pos = buf.find("\r\n\r\n");
    if (pos == std::string_view::npos) {
        return false;
    }
    
    // Parse request line
    std::string_view headerSection = buf.substr(0, pos);
    size_t firstLineEnd = headerSection.find("\r\n");
    if (firstLineEnd == std::string_view::npos) return false;
    
    std::string_view requestLine = headerSection.substr(0, firstLineEnd);
    size_t p1 = requestLine.find(' ');
    size_t p2 = requestLine.rfind(' ');
    
    if (p1 == std::string_view::npos || p2 == std::string_view::npos || p1 == p2) {
        return false;
    }
    
    request_.methodStr = requestLine.substr(0, p1);
    request_.method = stringToMethod(request_.methodStr);
    request_.rawPath = requestLine.substr(p1 + 1, p2 - p1 - 1);
    request_.protocol = requestLine.substr(p2 + 1);
    
    // Parse path and query string from rawPath
    size_t qPos = request_.rawPath.find('?');
    if (qPos != std::string_view::npos) {
        std::string_view pathPart = request_.rawPath.substr(0, qPos);
        request_.queryString = request_.rawPath.substr(qPos + 1);
        // Normalize and store path (storeString returns stable string_view)
        request_.path = request_.storeString(normalizePath(pathPart));
        // Parse query parameters
        request_.query = parseQueryString(request_.queryString);
    } else {
        // No query string, just normalize the path
        request_.path = request_.storeString(normalizePath(request_.rawPath));
    }
    
    // Parse headers - optimized vector-based parsing
    std::string_view headersPart = headerSection.substr(firstLineEnd + 2);
    size_t lineStart = 0;
    while (lineStart < headersPart.size()) {
        size_t lineEnd = headersPart.find("\r\n", lineStart);
        if (lineEnd == std::string_view::npos) lineEnd = headersPart.size();
        
        std::string_view line = headersPart.substr(lineStart, lineEnd - lineStart);
        size_t colonPos = line.find(':');
        if (colonPos != std::string_view::npos) {
            std::string_view name = line.substr(0, colonPos);
            std::string_view value = line.substr(colonPos + 1);
            
            // Trim whitespace
            size_t vstart = value.find_first_not_of(" \t");
            if (vstart != std::string_view::npos) {
                value = value.substr(vstart);
            }
            
            request_.headers.push_back({name, value});
            
            // Extract important headers quickly
            if (name.size() == 14) { // content-length
                bool match = true;
                const char* cl = "content-length";
                for(int i=0; i<14; ++i) if(std::tolower(name[i]) != cl[i]) { match=false; break; }
                if (match) {
                    contentLength_ = 0;
                    for (char c : value) if (c >= '0' && c <= '9') contentLength_ = contentLength_ * 10 + (c - '0');
                    request_.contentLength = contentLength_;
                }
            } else if (name.size() == 4) { // host
                bool match = true;
                const char* h = "host";
                for(int i=0; i<4; ++i) if(std::tolower(name[i]) != h[i]) { match=false; break; }
                if (match) request_.host = value;
            }
        }
        
        lineStart = lineEnd + 2;
        if (lineEnd == headersPart.size()) break;
    }
    
    headersParsed_ = true;
    
    // Check if body is already available
    size_t bodyStart = pos + 4;
    size_t bodyAvailable = buf.size() - bodyStart;
    bodyRead_ = bodyAvailable;
    
    if (bodyRead_ >= contentLength_) {
        request_.body = buf.substr(bodyStart, contentLength_);
        request_.buffer = readBuffer_;
        return true;
    }
    
    return false;
}

// ============================================================================
// WorkerPool Implementation
// ============================================================================

WorkerPool::WorkerPool(size_t numWorkers)
    : numWorkers_(numWorkers > 0 ? numWorkers : std::max(1u, std::thread::hardware_concurrency()))
{
}

WorkerPool::~WorkerPool() {
    stop();
}

void WorkerPool::start() {
    if (running_.load()) return;
    
    running_ = true;
    
    for (size_t i = 0; i < numWorkers_; ++i) {
        workers_.emplace_back([this]() {
            workerLoop();
        });
    }
}

void WorkerPool::stop() {
    if (!running_.load()) return;
    
    running_ = false;
    queueCV_.notify_all();
    
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
}

bool WorkerPool::submit(WorkItem item) {
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        workQueue_.push(std::move(item));
        pendingCount_.fetch_add(1, std::memory_order_relaxed);
    }
    queueCV_.notify_one();
    return true;
}

void WorkerPool::workerLoop() {
    // Thread-local batch for reduced lock contention
    std::vector<WorkItem> localBatch;
    localBatch.reserve(16);
    
    while (running_.load(std::memory_order_relaxed)) {
        localBatch.clear();
        
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            
            // Wait with shorter timeout for better responsiveness
            queueCV_.wait_for(lock, std::chrono::microseconds(500), [this]() {
                return !workQueue_.empty() || !running_.load(std::memory_order_relaxed);
            });
            
            if (!running_.load(std::memory_order_relaxed) && workQueue_.empty()) {
                break;
            }
            
            // Batch dequeue - grab multiple items at once to reduce lock contention
            while (!workQueue_.empty() && localBatch.size() < 8) {
                localBatch.push_back(std::move(workQueue_.front()));
                workQueue_.pop();
                pendingCount_.fetch_sub(1, std::memory_order_relaxed);
            }
        }
        
        // Process batch outside the lock
        for (auto& item : localBatch) {
            if (!item.app) continue;
            
            // Process the request
            Response response = item.app->handleRequest(item.request);
            
            // Queue the response for the event loop
            auto* respQueue = static_cast<std::queue<AsyncServer::ResponseItem>*>(item.responseQueue);
            {
                std::lock_guard<std::mutex> lock(*item.responseMutex);
                respQueue->push({item.fd, item.requestId, std::move(response)});
            }
            
            // Wake up the event loop
            if (item.multiplexer) {
                item.multiplexer->wakeup();
            }
        }
    }
}

// ============================================================================
// IOMultiplexer Implementation
// ============================================================================

IOMultiplexer::IOMultiplexer(int maxConnections)
    : maxConnections_(maxConnections)
{
    readyEvents_.reserve(1024);
    
#ifdef QZ_PLATFORM_LINUX
    epollEvents_.resize(maxConnections);
#elif defined(QZ_PLATFORM_MACOS)
    kqueueEvents_.resize(maxConnections);
#endif
}

IOMultiplexer::~IOMultiplexer() {
    destroy();
}

bool IOMultiplexer::init() {
#ifdef QZ_PLATFORM_LINUX
    epollFd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epollFd_ < 0) return false;
    
    // Create wakeup eventfd
    wakeupFd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeupFd_ < 0) {
        close(epollFd_);
        epollFd_ = -1;
        return false;
    }
    
    // Add wakeupFd to epoll
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = wakeupFd_;
    if (epoll_ctl(epollFd_, EPOLL_CTL_ADD, wakeupFd_, &ev) < 0) {
        close(epollFd_);
        close(wakeupFd_);
        epollFd_ = -1;
        wakeupFd_ = -1;
        return false;
    }
    
    epollEvents_.resize(maxConnections_);
    return true;
#elif defined(QZ_PLATFORM_MACOS)
    kqueueFd_ = kqueue();
    if (kqueueFd_ < 0) return false;
    
    // Create wakeup pipe
    if (pipe(wakeupPipe_) < 0) {
        close(kqueueFd_);
        kqueueFd_ = -1;
        return false;
    }
    
    // Set non-blocking
    fcntl(wakeupPipe_[0], F_SETFL, O_NONBLOCK);
    fcntl(wakeupPipe_[1], F_SETFL, O_NONBLOCK);
    
    // Add to kqueue
    struct kevent ev;
    EV_SET(&ev, wakeupPipe_[0], EVFILT_READ, EV_ADD, 0, 0, nullptr);
    if (kevent(kqueueFd_, &ev, 1, nullptr, 0, nullptr) < 0) {
        close(kqueueFd_);
        close(wakeupPipe_[0]);
        close(wakeupPipe_[1]);
        kqueueFd_ = -1;
        wakeupPipe_[0] = -1;
        wakeupPipe_[1] = -1;
        return false;
    }
    
    kqueueEvents_.resize(maxConnections_);
    return true;
#else
    return true;
#endif
}

void IOMultiplexer::destroy() {
#ifdef QZ_PLATFORM_LINUX
    if (epollFd_ >= 0) {
        ::close(epollFd_);
        epollFd_ = -1;
    }
    if (wakeupFd_ >= 0) {
        ::close(wakeupFd_);
        wakeupFd_ = -1;
    }
#elif defined(QZ_PLATFORM_MACOS)
    if (kqueueFd_ >= 0) {
        ::close(kqueueFd_);
        kqueueFd_ = -1;
    }
    if (wakeupPipe_[0] >= 0) {
        ::close(wakeupPipe_[0]);
        wakeupPipe_[0] = -1;
    }
    if (wakeupPipe_[1] >= 0) {
        ::close(wakeupPipe_[1]);
        wakeupPipe_[1] = -1;
    }
    fdEvents_.clear();
#elif defined(QZ_PLATFORM_WINDOWS)
    fdEvents_.clear();
#endif
}

bool IOMultiplexer::addSocket(socket_t fd, uint32_t events) {
#ifdef QZ_PLATFORM_LINUX
    if (epollFd_ < 0 || fd < 0) return false;
    
    struct epoll_event ev;
    ev.events = EPOLLET;  // Edge-triggered
    if (events & IO_READ)  ev.events |= EPOLLIN;
    if (events & IO_WRITE) ev.events |= EPOLLOUT;
    ev.data.fd = fd;
    
    return epoll_ctl(epollFd_, EPOLL_CTL_ADD, fd, &ev) == 0;
    
#elif defined(QZ_PLATFORM_MACOS)
    if (kqueueFd_ < 0 || fd < 0) return false;
    
    struct kevent ev[2];
    int n = 0;
    
    if (events & IO_READ) {
        EV_SET(&ev[n++], fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    }
    if (events & IO_WRITE) {
        EV_SET(&ev[n++], fd, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    }
    
    if (n > 0 && kevent(kqueueFd_, ev, n, nullptr, 0, nullptr) < 0) {
        return false;
    }
    
    fdEvents_[fd] = events;
    return true;
    
#elif defined(QZ_PLATFORM_WINDOWS)
    fdEvents_[fd] = events;
    return true;
#else
    return false;
#endif
}

bool IOMultiplexer::modifySocket(socket_t fd, uint32_t events) {
#ifdef QZ_PLATFORM_LINUX
    if (epollFd_ < 0 || fd < 0) return false;
    
    struct epoll_event ev;
    ev.events = EPOLLET;
    if (events & IO_READ)  ev.events |= EPOLLIN;
    if (events & IO_WRITE) ev.events |= EPOLLOUT;
    ev.data.fd = fd;
    
    return epoll_ctl(epollFd_, EPOLL_CTL_MOD, fd, &ev) == 0;
    
#elif defined(QZ_PLATFORM_MACOS)
    if (kqueueFd_ < 0 || fd < 0) return false;
    
    auto it = fdEvents_.find(fd);
    uint32_t oldEvents = (it != fdEvents_.end()) ? it->second : 0;
    
    struct kevent ev[4];
    int n = 0;
    
    // Remove old events
    if ((oldEvents & IO_READ) && !(events & IO_READ)) {
        EV_SET(&ev[n++], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    }
    if ((oldEvents & IO_WRITE) && !(events & IO_WRITE)) {
        EV_SET(&ev[n++], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    }
    
    // Add new events
    if (!(oldEvents & IO_READ) && (events & IO_READ)) {
        EV_SET(&ev[n++], fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    }
    if (!(oldEvents & IO_WRITE) && (events & IO_WRITE)) {
        EV_SET(&ev[n++], fd, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    }
    
    if (n > 0) {
        kevent(kqueueFd_, ev, n, nullptr, 0, nullptr);
    }
    
    fdEvents_[fd] = events;
    return true;
    
#elif defined(QZ_PLATFORM_WINDOWS)
    fdEvents_[fd] = events;
    return true;
#else
    return false;
#endif
}

bool IOMultiplexer::removeSocket(socket_t fd) {
#ifdef QZ_PLATFORM_LINUX
    if (epollFd_ < 0 || fd < 0) return false;
    return epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, nullptr) == 0;
    
#elif defined(QZ_PLATFORM_MACOS)
    if (kqueueFd_ < 0 || fd < 0) return false;
    
    auto it = fdEvents_.find(fd);
    if (it == fdEvents_.end()) return false;
    
    struct kevent ev[2];
    int n = 0;
    
    if (it->second & IO_READ) {
        EV_SET(&ev[n++], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    }
    if (it->second & IO_WRITE) {
        EV_SET(&ev[n++], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    }
    
    kevent(kqueueFd_, ev, n, nullptr, 0, nullptr);
    fdEvents_.erase(it);
    return true;
    
#elif defined(QZ_PLATFORM_WINDOWS)
    fdEvents_.erase(fd);
    return true;
#else
    return false;
#endif
}

int IOMultiplexer::poll(int timeoutMs) {
    readyEvents_.clear();
    
#ifdef QZ_PLATFORM_LINUX
    if (epollFd_ < 0) return -1;
    
    int numReady = epoll_wait(epollFd_, epollEvents_.data(), 
                               static_cast<int>(epollEvents_.size()), timeoutMs);
    
    for (int i = 0; i < numReady; i++) {
        if (epollEvents_[i].data.fd == wakeupFd_) {
            // Wakeup signal received, consume it
            uint64_t val;
            read(wakeupFd_, &val, sizeof(val));
            continue;
        }
        
        IOEvent event;
        event.fd = epollEvents_[i].data.fd;
        event.events = 0;
        
        if (epollEvents_[i].events & EPOLLIN)  event.events |= IO_READ;
        if (epollEvents_[i].events & EPOLLOUT) event.events |= IO_WRITE;
        if (epollEvents_[i].events & EPOLLERR) event.events |= IO_ERROR;
        if (epollEvents_[i].events & EPOLLHUP) event.events |= IO_CLOSE;
        
        readyEvents_.push_back(event);
    }
    
    return static_cast<int>(readyEvents_.size());
    
#elif defined(QZ_PLATFORM_MACOS)
    if (kqueueFd_ < 0) return -1;
    
    struct timespec ts;
    struct timespec* tsPtr = nullptr;
    
    if (timeoutMs >= 0) {
        ts.tv_sec = timeoutMs / 1000;
        ts.tv_nsec = (timeoutMs % 1000) * 1000000;
        tsPtr = &ts;
    }
    
    int numReady = kevent(kqueueFd_, nullptr, 0, kqueueEvents_.data(), 
                          static_cast<int>(kqueueEvents_.size()), tsPtr);
    
    for (int i = 0; i < numReady; i++) {
        if (kqueueEvents_[i].ident == (uintptr_t)wakeupPipe_[0]) {
            // Wakeup signal received, consume it
            char buf[64];
            read(wakeupPipe_[0], buf, sizeof(buf));
            continue;
        }
        
        IOEvent event;
        event.fd = static_cast<socket_t>(kqueueEvents_[i].ident);
        event.events = 0;
        
        if (kqueueEvents_[i].flags & EV_ERROR) {
            event.events |= IO_ERROR;
        } else if (kqueueEvents_[i].flags & EV_EOF) {
            event.events |= IO_CLOSE;
        } else if (kqueueEvents_[i].filter == EVFILT_READ) {
            event.events |= IO_READ;
        } else if (kqueueEvents_[i].filter == EVFILT_WRITE) {
            event.events |= IO_WRITE;
        }
        
        readyEvents_.push_back(event);
    }
    
    return static_cast<int>(readyEvents_.size());
    
#elif defined(QZ_PLATFORM_WINDOWS)
    // Windows select-based implementation
    FD_ZERO(&readSet_);
    FD_ZERO(&writeSet_);
    FD_ZERO(&exceptSet_);
    
    socket_t maxFd = 0;
    for (const auto& [fd, events] : fdEvents_) {
        if (events & IO_READ)  FD_SET(fd, &readSet_);
        if (events & IO_WRITE) FD_SET(fd, &writeSet_);
        FD_SET(fd, &exceptSet_);
        if (fd > maxFd) maxFd = fd;
    }
    
    if (fdEvents_.empty()) {
        if (timeoutMs > 0) {
            Sleep(timeoutMs);
        }
        return 0;
    }
    
    struct timeval tv;
    struct timeval* tvPtr = nullptr;
    
    if (timeoutMs >= 0) {
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        tvPtr = &tv;
    }
    
    int numReady = select(static_cast<int>(maxFd + 1), &readSet_, &writeSet_, &exceptSet_, tvPtr);
    
    if (numReady > 0) {
        for (const auto& [fd, events] : fdEvents_) {
            uint32_t readyEvents = 0;
            if (FD_ISSET(fd, &readSet_))   readyEvents |= IO_READ;
            if (FD_ISSET(fd, &writeSet_))  readyEvents |= IO_WRITE;
            if (FD_ISSET(fd, &exceptSet_)) readyEvents |= IO_ERROR;
            
            if (readyEvents) {
                IOEvent event;
                event.fd = fd;
                event.events = readyEvents;
                readyEvents_.push_back(event);
            }
        }
    }
    
    return numReady;
#else
    return -1;
#endif
}

void IOMultiplexer::wakeup() {
#ifdef QZ_PLATFORM_LINUX
    if (wakeupFd_ >= 0) {
        uint64_t val = 1;
        write(wakeupFd_, &val, sizeof(val));
    }
#elif defined(QZ_PLATFORM_MACOS)
    if (wakeupPipe_[1] >= 0) {
        char c = '1';
        write(wakeupPipe_[1], &c, 1);
    }
#elif defined(QZ_PLATFORM_WINDOWS)
    // Not needed for select, as it's always checking all FDs
#endif
}

// ============================================================================
// ConnectionManager Implementation
// ============================================================================

ConnectionManager::ConnectionManager(size_t maxConnections)
    : maxConnections_(maxConnections)
{
}

ConnectionManager::~ConnectionManager() {
    connections_.clear();
}

AsyncConnection* ConnectionManager::addConnection(socket_t fd, const std::string& remoteAddr, int remotePort) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (connections_.size() >= maxConnections_) {
        return nullptr;
    }
    
    auto conn = std::make_unique<AsyncConnection>(fd, remoteAddr, remotePort);
    auto* ptr = conn.get();
    connections_[fd] = std::move(conn);
    return ptr;
}

AsyncConnection* ConnectionManager::getConnection(socket_t fd) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = connections_.find(fd);
    return it != connections_.end() ? it->second.get() : nullptr;
}

void ConnectionManager::removeConnection(socket_t fd) {
    std::lock_guard<std::mutex> lock(mutex_);
    connections_.erase(fd);
}

std::vector<socket_t> ConnectionManager::getTimedOutConnections(int timeoutMs) {
    std::vector<socket_t> timedOut;
    
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [fd, conn] : connections_) {
        if (conn->isTimedOut(timeoutMs)) {
            timedOut.push_back(fd);
        }
    }
    
    return timedOut;
}

// ============================================================================
// AsyncServer Implementation
// ============================================================================

AsyncServer::AsyncServer(Application* app)
    : app_(app)
{
}

AsyncServer::~AsyncServer() {
    stop();
}

bool AsyncServer::createServerSocket() {
#ifdef QZ_PLATFORM_WINDOWS
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

#ifdef QZ_PLATFORM_LINUX
    signal(SIGPIPE, SIG_IGN);
#endif
    
    const AppConfig& config = app_->config();
    
    serverSocket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (serverSocket_ == INVALID_SOCKET_VALUE) {
        std::cerr << "[bedrock-async] socket() failed: " << strerror(errno) << std::endl;
        return false;
    }
    
    // Set socket options
    if (config.reuseAddr) {
        int opt = 1;
        setsockopt(serverSocket_, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&opt), sizeof(opt));
    }
    
#ifdef SO_REUSEPORT
    if (config.reusePort) {
        int opt = 1;
        setsockopt(serverSocket_, SOL_SOCKET, SO_REUSEPORT,
                   reinterpret_cast<const char*>(&opt), sizeof(opt));
    }
#endif
    
    if (config.tcpNoDelay) {
        int opt = 1;
        setsockopt(serverSocket_, IPPROTO_TCP, TCP_NODELAY,
                   reinterpret_cast<const char*>(&opt), sizeof(opt));
    }
    
    // Set non-blocking
#ifdef QZ_PLATFORM_WINDOWS
    u_long mode = 1;
    ioctlsocket(serverSocket_, FIONBIO, &mode);
#else
    int flags = fcntl(serverSocket_, F_GETFL, 0);
    fcntl(serverSocket_, F_SETFL, flags | O_NONBLOCK);
#endif
    
    // Bind
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(config.port));
    
    if (config.host == "0.0.0.0" || config.host.empty()) {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        inet_pton(AF_INET, config.host.c_str(), &addr.sin_addr);
    }
    
    if (bind(serverSocket_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "[bedrock-async] bind() failed: " << strerror(errno) << std::endl;
        close_socket(serverSocket_);
        serverSocket_ = INVALID_SOCKET_VALUE;
        return false;
    }
    
    if (listen(serverSocket_, config.backlog) < 0) {
        std::cerr << "[bedrock-async] listen() failed: " << strerror(errno) << std::endl;
        close_socket(serverSocket_);
        serverSocket_ = INVALID_SOCKET_VALUE;
        return false;
    }
    
    return true;
}

bool AsyncServer::start() {
    if (running_.load()) {
        return false;
    }
    
    const AppConfig& config = app_->config();
    
    // Create server socket
    if (!createServerSocket()) {
        return false;
    }
    
    // Initialize I/O multiplexer
    multiplexer_ = std::make_unique<IOMultiplexer>(config.maxConnections);
    if (!multiplexer_->init()) {
        std::cerr << "[bedrock-async] Failed to initialize I/O multiplexer" << std::endl;
        close_socket(serverSocket_);
        serverSocket_ = INVALID_SOCKET_VALUE;
        return false;
    }
    
    // Register server socket for read events (accept)
    multiplexer_->addSocket(serverSocket_, IO_READ);
    
    // Create connection manager
    connManager_ = std::make_unique<ConnectionManager>(config.maxConnections);
    
    // Create and start worker pool
    size_t numWorkers = config.workerThreads > 0 
        ? config.workerThreads 
        : std::max(1u, std::min(static_cast<uint32_t>(std::thread::hardware_concurrency()), 4u));
    workerPool_ = std::make_unique<WorkerPool>(numWorkers);
    workerPool_->start();
    
    running_ = true;
    shouldStop_ = false;
    
    // Start event loop thread
    eventLoopThread_ = std::thread([this]() {
        eventLoop();
    });
    
    std::cerr << "[bedrock-async] Server started on " << config.host << ":" << config.port 
              << " with " << numWorkers << " workers" << std::endl;
    
    return true;
}

void AsyncServer::stop() {
    if (!running_.load()) {
        return;
    }
    
    shouldStop_ = true;
    
    // Stop worker pool first
    if (workerPool_) {
        workerPool_->stop();
    }
    
    // Close server socket to unblock multiplexer
    if (serverSocket_ != INVALID_SOCKET_VALUE) {
        multiplexer_->removeSocket(serverSocket_);
        close_socket(serverSocket_);
        serverSocket_ = INVALID_SOCKET_VALUE;
    }
    
    // Wait for event loop thread
    if (eventLoopThread_.joinable()) {
        eventLoopThread_.join();
    }
    
    // Cleanup
    if (multiplexer_) {
        multiplexer_->destroy();
        multiplexer_.reset();
    }
    
    connManager_.reset();
    workerPool_.reset();
    
    running_ = false;
    
    std::cerr << "[bedrock-async] Server stopped" << std::endl;
}

void AsyncServer::eventLoop() {
    const AppConfig& config = app_->config();
    auto lastCleanup = std::chrono::steady_clock::now();
    
    while (!shouldStop_.load(std::memory_order_relaxed)) {
        // Poll for events - shorter timeout for better responsiveness
        int numEvents = multiplexer_->poll(10);  // 10ms timeout (was 100ms)
        
        // Process responses from workers FIRST - prioritize completing requests
        processResponses();
        
        // Process ready events
        for (const auto& event : multiplexer_->events()) {
            if (event.fd == serverSocket_) {
                // Accept new connection
                if (event.events & IO_READ) {
                    handleAccept();
                }
            } else {
                // Handle client connection events
                if (event.events & IO_ERROR) {
                    handleError(event.fd);
                } else if (event.events & IO_CLOSE) {
                    handleError(event.fd);
                } else {
                    if (event.events & IO_READ) {
                        handleRead(event.fd);
                    }
                    if (event.events & IO_WRITE) {
                        handleWrite(event.fd);
                    }
                }
            }
        }
        
        // Process responses again in case workers finished during event processing
        processResponses();
        
        // Periodic cleanup of timed-out connections
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastCleanup).count() >= 5) {
            cleanupTimeouts();
            lastCleanup = now;
        }
    }
}

void AsyncServer::handleAccept() {
    while (true) {
        struct sockaddr_in clientAddr;
        socklen_t clientLen = sizeof(clientAddr);
        
        socket_t clientFd = accept(serverSocket_,
                                   reinterpret_cast<struct sockaddr*>(&clientAddr),
                                   &clientLen);
        
        if (clientFd == INVALID_SOCKET_VALUE) {
#ifdef QZ_PLATFORM_WINDOWS
            if (WSAGetLastError() == WSAEWOULDBLOCK) break;
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
#endif
            break;
        }
        
        // Get client info
        char ipStr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, ipStr, sizeof(ipStr));
        int port = ntohs(clientAddr.sin_port);
        
        // Add connection
        AsyncConnection* conn = connManager_->addConnection(clientFd, ipStr, port);
        if (!conn) {
            close_socket(clientFd);
            continue;
        }
        
        // Set non-blocking and TCP_NODELAY
        conn->setNonBlocking(true);
        
        const AppConfig& config = app_->config();
        if (config.tcpNoDelay) {
            int opt = 1;
#ifdef QZ_PLATFORM_WINDOWS
            setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&opt), sizeof(opt));
#else
            setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
#endif
        }
        
        // Register for read events
        multiplexer_->addSocket(clientFd, IO_READ);
        
        totalConnections_++;
    }
}

void AsyncServer::handleRead(socket_t fd) {
    AsyncConnection* conn = connManager_->getConnection(fd);
    if (!conn) return;
    
    if (conn->state() != ConnectionState::READING_REQUEST &&
        conn->state() != ConnectionState::IDLE) {
        return;  // Not in reading state
    }
    
    conn->setState(ConnectionState::READING_REQUEST);
    
    int result = conn->readNonBlocking();
    
    if (result < 0) {
        // Error or connection closed
        multiplexer_->removeSocket(fd);
        connManager_->removeConnection(fd);
        return;
    }
    
    if (conn->hasCompleteRequest()) {
        // Request complete, submit to worker pool
        conn->setState(ConnectionState::PROCESSING);
        
        uint64_t requestId = nextRequestId_.fetch_add(1, std::memory_order_relaxed);
        conn->setRequestId(requestId);
        
        // Fill in connection info
        Request& req = conn->getRequest();
        req.remoteAddr = conn->remoteAddr();
        req.remotePort = conn->remotePort();
        
        // Submit work item with all context needed for processing
        WorkItem item;
        item.fd = fd;
        item.requestId = requestId;
        item.request = std::move(req);
        item.app = app_;
        item.responseMutex = &responseMutex_;
        item.responseQueue = &responseQueue_;
        item.multiplexer = multiplexer_.get();
        
        workerPool_->submit(std::move(item));
        
        totalRequests_.fetch_add(1, std::memory_order_relaxed);
    }
}

void AsyncServer::handleWrite(socket_t fd) {
    AsyncConnection* conn = connManager_->getConnection(fd);
    if (!conn) return;
    
    if (conn->state() != ConnectionState::WRITING_RESPONSE) {
        return;
    }
    
    int result = conn->writeNonBlocking();
    
    if (result < 0) {
        // Error
        multiplexer_->removeSocket(fd);
        connManager_->removeConnection(fd);
        return;
    }
    
    if (!conn->hasDataToWrite()) {
        // Response sent completely
        const AppConfig& config = app_->config();
        
        // Check for keep-alive
        std::string connHeader(conn->getRequest().getHeader("Connection"));
        std::transform(connHeader.begin(), connHeader.end(), connHeader.begin(), ::tolower);
        
        if (connHeader == "keep-alive" || 
            (conn->getRequest().protocol == "HTTP/1.1" && connHeader != "close")) {
            if (config.enableKeepAlive) {
                // Reset for next request
                conn->reset();
                conn->setState(ConnectionState::IDLE);
                multiplexer_->modifySocket(fd, IO_READ);
                return;
            }
        }
        
        // Close connection
        multiplexer_->removeSocket(fd);
        connManager_->removeConnection(fd);
    }
}

void AsyncServer::handleError(socket_t fd) {
    multiplexer_->removeSocket(fd);
    connManager_->removeConnection(fd);
}

void AsyncServer::processResponses() {
    std::queue<ResponseItem> responses;
    
    {
        std::lock_guard<std::mutex> lock(responseMutex_);
        std::swap(responses, responseQueue_);
    }
    
    while (!responses.empty()) {
        ResponseItem item = std::move(responses.front());
        responses.pop();
        
        AsyncConnection* conn = connManager_->getConnection(item.fd);
        if (!conn || conn->requestId() != item.requestId) {
            continue;  // Connection closed or request changed
        }
        
        // Set response and switch to writing state
        conn->setResponse(item.response);
        conn->setState(ConnectionState::WRITING_RESPONSE);
        
        // Register for write events
        multiplexer_->modifySocket(item.fd, IO_WRITE);
        
        // Try to write immediately
        handleWrite(item.fd);
    }
}

void AsyncServer::cleanupTimeouts() {
    const AppConfig& config = app_->config();
    
    auto timedOut = connManager_->getTimedOutConnections(config.connectionTimeoutMs);
    
    for (socket_t fd : timedOut) {
        multiplexer_->removeSocket(fd);
        connManager_->removeConnection(fd);
    }
}

} // namespace bedrock