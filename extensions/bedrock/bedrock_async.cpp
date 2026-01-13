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
    readBuffer_.reserve(8192);
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
    char buffer[8192];
    
#ifdef QZ_PLATFORM_WINDOWS
    int received = recv(fd_, buffer, sizeof(buffer), 0);
    if (received == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
            return 0;  // Would block
        }
        return -1;  // Error
    }
#else
    ssize_t received = recv(fd_, buffer, sizeof(buffer), 0);
    if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;  // Would block
        }
        return -1;  // Error
    }
#endif
    
    if (received == 0) {
        return -1;  // Connection closed
    }
    
    readBuffer_.append(buffer, received);
    touch();
    
    // Try to parse
    if (parseRequest()) {
        return static_cast<int>(received);
    }
    
    return static_cast<int>(received);
}

int AsyncConnection::writeNonBlocking() {
    if (writePos_ >= writeBuffer_.size()) {
        return 0;  // Nothing to write
    }
    
    size_t remaining = writeBuffer_.size() - writePos_;
    const char* data = writeBuffer_.c_str() + writePos_;
    
    int flags = 0;
#ifdef QZ_PLATFORM_LINUX
    flags = MSG_NOSIGNAL;
#endif
    
#ifdef QZ_PLATFORM_WINDOWS
    int sent = send(fd_, data, static_cast<int>(remaining), flags);
    if (sent == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
            return 0;  // Would block
        }
        return -1;  // Error
    }
#else
    ssize_t sent = send(fd_, data, remaining, flags);
    if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;  // Would block
        }
        return -1;  // Error
    }
#endif
    
    writePos_ += sent;
    touch();
    
    return static_cast<int>(sent);
}

bool AsyncConnection::hasCompleteRequest() const {
    return headersParsed_ && (bodyRead_ >= contentLength_);
}

void AsyncConnection::setResponse(const Response& response) {
    writeBuffer_ = response.build();
    writePos_ = 0;
}

void AsyncConnection::reset() {
    readBuffer_.clear();
    request_ = Request();
    contentLength_ = 0;
    bodyRead_ = 0;
    headersParsed_ = false;
    writeBuffer_.clear();
    writePos_ = 0;
    state_ = ConnectionState::READING_REQUEST;
    touch();
}

bool AsyncConnection::isTimedOut(int timeoutMs) const {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastActivity_).count();
    return elapsed > timeoutMs;
}

bool AsyncConnection::parseRequest() {
    if (headersParsed_) {
        // Reading body
        size_t headersEnd = readBuffer_.find("\r\n\r\n");
        if (headersEnd != std::string::npos) {
            size_t bodyStart = headersEnd + 4;
            size_t bodyAvailable = readBuffer_.size() - bodyStart;
            bodyRead_ = bodyAvailable;
            
            if (bodyRead_ >= contentLength_) {
                // Complete request
                request_.body = readBuffer_.substr(bodyStart, contentLength_);
                return true;
            }
        }
        return false;
    }
    
    // Look for end of headers
    size_t pos = readBuffer_.find("\r\n\r\n");
    if (pos == std::string::npos) {
        return false;  // Headers not complete yet
    }
    
    // Parse headers
    std::string headerSection = readBuffer_.substr(0, pos);
    
    // Parse request line
    size_t firstLine = headerSection.find("\r\n");
    if (firstLine == std::string::npos) {
        return false;
    }
    
    std::string requestLine = headerSection.substr(0, firstLine);
    size_t p1 = requestLine.find(' ');
    size_t p2 = requestLine.rfind(' ');
    
    if (p1 == std::string::npos || p2 == std::string::npos || p1 == p2) {
        return false;  // Invalid request line
    }
    
    request_.methodStr = requestLine.substr(0, p1);
    request_.method = stringToMethod(request_.methodStr);
    request_.rawPath = requestLine.substr(p1 + 1, p2 - p1 - 1);
    request_.protocol = requestLine.substr(p2 + 1);
    
    // Parse path and query string
    size_t qPos = request_.rawPath.find('?');
    if (qPos != std::string::npos) {
        request_.path = normalizePath(request_.rawPath.substr(0, qPos));
        request_.queryString = request_.rawPath.substr(qPos + 1);
        request_.query = parseQueryString(request_.queryString);
    } else {
        request_.path = normalizePath(request_.rawPath);
    }
    
    // Parse headers
    std::string headers = headerSection.substr(firstLine + 2);
    size_t lineStart = 0;
    while (lineStart < headers.size()) {
        size_t lineEnd = headers.find("\r\n", lineStart);
        if (lineEnd == std::string::npos) {
            lineEnd = headers.size();
        }
        
        std::string line = headers.substr(lineStart, lineEnd - lineStart);
        size_t colonPos = line.find(':');
        if (colonPos != std::string::npos) {
            std::string name = line.substr(0, colonPos);
            std::string value = line.substr(colonPos + 1);
            
            // Trim whitespace from value
            size_t start = value.find_first_not_of(" \t");
            if (start != std::string::npos) {
                value = value.substr(start);
            }
            
            // Store with lowercase key
            std::transform(name.begin(), name.end(), name.begin(), ::tolower);
            request_.headers[name] = value;
        }
        
        lineStart = lineEnd + 2;
    }
    
    // Extract common headers
    auto clIt = request_.headers.find("content-length");
    if (clIt != request_.headers.end()) {
        try {
            contentLength_ = std::stoull(clIt->second);
            request_.contentLength = contentLength_;
        } catch (...) {
            contentLength_ = 0;
        }
    }
    
    auto ctIt = request_.headers.find("content-type");
    if (ctIt != request_.headers.end()) {
        request_.contentType = ctIt->second;
    }
    
    auto hostIt = request_.headers.find("host");
    if (hostIt != request_.headers.end()) {
        request_.host = hostIt->second;
    }
    
    auto cookieIt = request_.headers.find("cookie");
    if (cookieIt != request_.headers.end()) {
        request_.cookies = parseCookies(cookieIt->second);
    }
    
    headersParsed_ = true;
    
    // Check if body is already available
    size_t bodyStart = pos + 4;
    if (bodyStart < readBuffer_.size()) {
        size_t bodyAvailable = readBuffer_.size() - bodyStart;
        bodyRead_ = bodyAvailable;
        
        if (bodyRead_ >= contentLength_) {
            request_.body = readBuffer_.substr(bodyStart, contentLength_);
            return true;
        }
    } else if (contentLength_ == 0) {
        return true;  // No body expected
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
        pendingCount_++;
    }
    queueCV_.notify_one();
    return true;
}

void WorkerPool::workerLoop() {
    while (running_.load()) {
        WorkItem item;
        
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            queueCV_.wait_for(lock, std::chrono::milliseconds(100), [this]() {
                return !workQueue_.empty() || !running_.load();
            });
            
            if (!running_.load() && workQueue_.empty()) {
                break;
            }
            
            if (workQueue_.empty()) {
                continue;
            }
            
            item = std::move(workQueue_.front());
            workQueue_.pop();
            pendingCount_--;
        }
        
        // Process the work item
        if (item.callback) {
            item.callback(Response());  // Placeholder - actual response comes from app
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
    return epollFd_ >= 0;
#elif defined(QZ_PLATFORM_MACOS)
    kqueueFd_ = kqueue();
    return kqueueFd_ >= 0;
#elif defined(QZ_PLATFORM_WINDOWS)
    FD_ZERO(&readSet_);
    FD_ZERO(&writeSet_);
    FD_ZERO(&exceptSet_);
    return true;
#else
    return false;
#endif
}

void IOMultiplexer::destroy() {
#ifdef QZ_PLATFORM_LINUX
    if (epollFd_ >= 0) {
        ::close(epollFd_);
        epollFd_ = -1;
    }
#elif defined(QZ_PLATFORM_MACOS)
    if (kqueueFd_ >= 0) {
        ::close(kqueueFd_);
        kqueueFd_ = -1;
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
        IOEvent event;
        event.fd = epollEvents_[i].data.fd;
        event.events = 0;
        
        if (epollEvents_[i].events & EPOLLIN)  event.events |= IO_READ;
        if (epollEvents_[i].events & EPOLLOUT) event.events |= IO_WRITE;
        if (epollEvents_[i].events & EPOLLERR) event.events |= IO_ERROR;
        if (epollEvents_[i].events & (EPOLLHUP | EPOLLRDHUP)) event.events |= IO_CLOSE;
        
        readyEvents_.push_back(event);
    }
    
    return numReady;
    
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
    
    return numReady;
    
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
        : std::max(1u, std::thread::hardware_concurrency());
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
    
    while (!shouldStop_.load()) {
        // Poll for events
        int numEvents = multiplexer_->poll(100);  // 100ms timeout
        
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
        
        // Process responses from workers
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
        
        // Set non-blocking
        conn->setNonBlocking(true);
        
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
        
        uint64_t requestId = nextRequestId_++;
        conn->setRequestId(requestId);
        
        // Fill in connection info
        Request& req = conn->getRequest();
        req.remoteAddr = conn->remoteAddr();
        req.remotePort = conn->remotePort();
        
        // IMPORTANT: Copy the request for the worker thread!
        // We need a copy because the connection might be reset or modified
        // before the worker processes the request
        Request requestCopy = req;
        
        // Capture fd, requestId, and the request copy for callback
        socket_t capturedFd = fd;
        
        // Process request on worker thread
        workerPool_->submit({
            fd,
            requestId,
            Request(),  // Empty placeholder, we use the captured copy instead
            [this, capturedFd, requestId, requestCopy = std::move(requestCopy)](Response) mutable {
                // This callback is called from worker thread
                // Process the captured request copy
                Response response = app_->handleRequest(requestCopy);
                
                std::lock_guard<std::mutex> lock(responseMutex_);
                responseQueue_.push({capturedFd, requestId, std::move(response)});
            }
        });
        
        totalRequests_++;
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
        std::string connHeader = conn->getRequest().getHeader("Connection");
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
