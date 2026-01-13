// ============================================================================
// Bedrock - Application and Server Implementation
// High-performance HTTP server with standalone and uWSGI modes
// ============================================================================

#include "bedrock_types.h"
#include "bedrock_async.h"
#include <algorithm>
#include <sstream>
#include <cstring>
#include <thread>
#include <atomic>
#include <iostream>

#ifdef QZ_PLATFORM_LINUX
#include <signal.h>
#endif

namespace bedrock {

// ============================================================================
// Forward Declaration
// ============================================================================

class Application;

// ============================================================================
// HTTP Request Parser
// ============================================================================

class HttpParser {
public:
    enum class State {
        REQUEST_LINE,
        HEADERS,
        BODY,
        COMPLETE,
        ERROR
    };
    
    HttpParser() : state_(State::REQUEST_LINE), contentLength_(0), bodyRead_(0) {}
    
    // Feed data to parser, returns bytes consumed
    size_t feed(const char* data, size_t length);
    
    // Check if parsing is complete
    bool isComplete() const { return state_ == State::COMPLETE; }
    bool hasError() const { return state_ == State::ERROR; }
    
    // Get parsed request (valid when isComplete())
    Request& getRequest() { return request_; }
    
    // Reset parser for next request
    void reset();
    
private:
    State state_;
    Request request_;
    std::string buffer_;
    size_t contentLength_;
    size_t bodyRead_;
    
    bool parseRequestLine();
    bool parseHeaders();
};

void HttpParser::reset() {
    state_ = State::REQUEST_LINE;
    request_ = Request();
    buffer_.clear();
    contentLength_ = 0;
    bodyRead_ = 0;
}

size_t HttpParser::feed(const char* data, size_t length) {
    if (state_ == State::COMPLETE || state_ == State::ERROR) {
        return 0;
    }
    
    size_t consumed = 0;
    
    while (consumed < length && state_ != State::COMPLETE && state_ != State::ERROR) {
        if (state_ == State::BODY) {
            // Read body data directly
            size_t remaining = contentLength_ - bodyRead_;
            size_t toRead = std::min(remaining, length - consumed);
            request_.body.append(data + consumed, toRead);
            bodyRead_ += toRead;
            consumed += toRead;
            
            if (bodyRead_ >= contentLength_) {
                state_ = State::COMPLETE;
            }
            continue;
        }
        
        // Buffer data for line-based parsing
        buffer_ += data[consumed++];
        
        // Look for CRLF
        if (buffer_.size() >= 2 && 
            buffer_[buffer_.size() - 2] == '\r' && 
            buffer_[buffer_.size() - 1] == '\n') {
            
            std::string line = buffer_.substr(0, buffer_.size() - 2);
            buffer_.clear();
            
            switch (state_) {
                case State::REQUEST_LINE:
                    if (!line.empty()) {
                        // Parse: METHOD PATH PROTOCOL
                        size_t pos1 = line.find(' ');
                        size_t pos2 = line.rfind(' ');
                        
                        if (pos1 != std::string::npos && pos2 != std::string::npos && pos1 != pos2) {
                            request_.methodStr = line.substr(0, pos1);
                            request_.method = stringToMethod(request_.methodStr);
                            request_.rawPath = line.substr(pos1 + 1, pos2 - pos1 - 1);
                            request_.protocol = line.substr(pos2 + 1);
                            
                            // Parse path and query string
                            size_t qPos = request_.rawPath.find('?');
                            if (qPos != std::string::npos) {
                                request_.path = normalizePath(request_.rawPath.substr(0, qPos));
                                request_.queryString = request_.rawPath.substr(qPos + 1);
                                request_.query = parseQueryString(request_.queryString);
                            } else {
                                request_.path = normalizePath(request_.rawPath);
                            }
                            
                            state_ = State::HEADERS;
                        } else {
                            state_ = State::ERROR;
                        }
                    }
                    break;
                    
                case State::HEADERS:
                    if (line.empty()) {
                        // End of headers
                        // Parse Content-Length
                        auto clIt = request_.headers.find("content-length");
                        if (clIt != request_.headers.end()) {
                            try {
                                contentLength_ = std::stoull(clIt->second);
                                request_.contentLength = contentLength_;
                            } catch (...) {
                                contentLength_ = 0;
                            }
                        }
                        
                        // Parse Content-Type
                        auto ctIt = request_.headers.find("content-type");
                        if (ctIt != request_.headers.end()) {
                            request_.contentType = ctIt->second;
                        }
                        
                        // Parse Host
                        auto hostIt = request_.headers.find("host");
                        if (hostIt != request_.headers.end()) {
                            request_.host = hostIt->second;
                        }
                        
                        // Parse Cookies
                        auto cookieIt = request_.headers.find("cookie");
                        if (cookieIt != request_.headers.end()) {
                            request_.cookies = parseCookies(cookieIt->second);
                        }
                        
                        if (contentLength_ > 0) {
                            request_.body.reserve(contentLength_);
                            state_ = State::BODY;
                        } else {
                            state_ = State::COMPLETE;
                        }
                    } else {
                        // Parse header: Name: Value
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
                    }
                    break;
                    
                default:
                    break;
            }
        }
    }
    
    return consumed;
}

// ============================================================================
// Connection Handler
// ============================================================================

class Connection {
public:
    Connection(socket_t fd, const std::string& remoteAddr, int remotePort);
    ~Connection();
    
    socket_t fd() const { return fd_; }
    const std::string& remoteAddr() const { return remoteAddr_; }
    int remotePort() const { return remotePort_; }
    
    // Read data into parser
    int readData();
    
    // Send response
    int sendResponse(const Response& response);
    int sendData(const char* data, size_t length);
    
    // Check if request is complete
    bool requestReady() const { return parser_.isComplete(); }
    bool hasError() const { return parser_.hasError(); }
    
    // Get request
    Request& getRequest() { return parser_.getRequest(); }
    
    // Reset for keep-alive
    void reset() { parser_.reset(); }
    
    // Set non-blocking mode
    bool setNonBlocking(bool nonBlocking);
    
private:
    socket_t fd_;
    std::string remoteAddr_;
    int remotePort_;
    HttpParser parser_;
};

Connection::Connection(socket_t fd, const std::string& remoteAddr, int remotePort)
    : fd_(fd), remoteAddr_(remoteAddr), remotePort_(remotePort) {
}

Connection::~Connection() {
    if (fd_ != INVALID_SOCKET_VALUE) {
        close_socket(fd_);
    }
}

bool Connection::setNonBlocking(bool nonBlocking) {
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

int Connection::readData() {
    char buffer[8192];
    int received = recv(fd_, buffer, sizeof(buffer), 0);
    
    if (received > 0) {
        parser_.feed(buffer, received);
    }
    
    return received;
}

int Connection::sendData(const char* data, size_t length) {
    size_t sent = 0;
    while (sent < length) {
        int flags = 0;
#ifdef QZ_PLATFORM_LINUX
        flags = MSG_NOSIGNAL;
#endif
        int n = ::send(fd_, data + sent, length - sent, flags);
        if (n <= 0) {
            return n;
        }
        sent += n;
    }
    return static_cast<int>(sent);
}

int Connection::sendResponse(const Response& response) {
    std::string data = response.build();
    return sendData(data.c_str(), data.size());
}

// ============================================================================
// Application Implementation
// ============================================================================

std::atomic<uint64_t> Application::nextId_{0};

Application::Application(const std::string& name) 
    : name_(name) {
    id_ = "bedrock_app_" + std::to_string(nextId_++);
    
    // Default not found handler
    notFoundHandler_ = [](Context& ctx, NextFn next) {
        ctx.res.notFound("Not Found: " + ctx.req.path);
    };
    
    // Default error handler
    errorHandler_ = [](Context& ctx, const std::string& error) {
        ctx.res.serverError("Internal Server Error: " + error);
    };
}

Application::~Application() {
    stop();
}

Application& Application::configure(const AppConfig& cfg) {
    config_ = cfg;
    return *this;
}

Application& Application::get(const std::string& path, Handler handler) {
    router_.get(path, handler);
    return *this;
}

Application& Application::post(const std::string& path, Handler handler) {
    router_.post(path, handler);
    return *this;
}

Application& Application::put(const std::string& path, Handler handler) {
    router_.put(path, handler);
    return *this;
}

Application& Application::del(const std::string& path, Handler handler) {
    router_.del(path, handler);
    return *this;
}

Application& Application::patch(const std::string& path, Handler handler) {
    router_.patch(path, handler);
    return *this;
}

Application& Application::any(const std::string& path, Handler handler) {
    router_.any(path, handler);
    return *this;
}

Application& Application::use(Handler middleware) {
    globalMiddleware_.push_back(middleware);
    return *this;
}

Application& Application::use(const std::string& path, Handler middleware) {
    router_.use(path, middleware);
    return *this;
}

Application& Application::mount(const std::string& path, std::shared_ptr<Router> subRouter) {
    router_.mount(path, subRouter);
    return *this;
}

Application& Application::onError(ErrorHandler handler) {
    errorHandler_ = handler;
    return *this;
}

Application& Application::onNotFound(Handler handler) {
    notFoundHandler_ = handler;
    return *this;
}

bool Application::createSocket() {
#ifdef QZ_PLATFORM_WINDOWS
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
    
#ifdef QZ_PLATFORM_LINUX
    signal(SIGPIPE, SIG_IGN);
#endif
    
    serverSocket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (serverSocket_ == INVALID_SOCKET_VALUE) {
        std::cerr << "[bedrock] socket() failed: " << strerror(errno) << std::endl;
        return false;
    }
    std::cerr << "[bedrock] socket() created fd=" << serverSocket_ << std::endl;
    
    // Set socket options
    if (config_.reuseAddr) {
        int opt = 1;
        if (setsockopt(serverSocket_, SOL_SOCKET, SO_REUSEADDR, 
                   reinterpret_cast<const char*>(&opt), sizeof(opt)) < 0) {
            std::cerr << "[bedrock] setsockopt SO_REUSEADDR failed: " << strerror(errno) << std::endl;
        }
    }
    
#ifdef SO_REUSEPORT
    if (config_.reusePort) {
        int opt = 1;
        setsockopt(serverSocket_, SOL_SOCKET, SO_REUSEPORT,
                   reinterpret_cast<const char*>(&opt), sizeof(opt));
    }
#endif
    
    if (config_.tcpNoDelay) {
        int opt = 1;
        setsockopt(serverSocket_, IPPROTO_TCP, TCP_NODELAY,
                   reinterpret_cast<const char*>(&opt), sizeof(opt));
    }
    
    // Bind
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(config_.port));
    
    if (config_.host == "0.0.0.0" || config_.host.empty()) {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        inet_pton(AF_INET, config_.host.c_str(), &addr.sin_addr);
    }
    
    std::cerr << "[bedrock] binding to port " << config_.port << std::endl;
    if (bind(serverSocket_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "[bedrock] bind() failed: " << strerror(errno) << std::endl;
        close_socket(serverSocket_);
        serverSocket_ = INVALID_SOCKET_VALUE;
        return false;
    }
    std::cerr << "[bedrock] bind() succeeded" << std::endl;
    
    // Listen
    std::cerr << "[bedrock] calling listen() with backlog=" << config_.backlog << std::endl;
    if (listen(serverSocket_, config_.backlog) < 0) {
        std::cerr << "[bedrock] listen() failed: " << strerror(errno) << std::endl;
        close_socket(serverSocket_);
        serverSocket_ = INVALID_SOCKET_VALUE;
        return false;
    }
    std::cerr << "[bedrock] listen() succeeded" << std::endl;
    
    return true;
}

void Application::acceptLoop() {
    while (!shouldStop_.load()) {
        struct sockaddr_in clientAddr;
        socklen_t clientLen = sizeof(clientAddr);
        
        socket_t clientFd = accept(serverSocket_, 
                                   reinterpret_cast<struct sockaddr*>(&clientAddr), 
                                   &clientLen);
        
        if (clientFd == INVALID_SOCKET_VALUE) {
            if (shouldStop_.load()) break;
            continue;
        }
        
        // Get client info
        char ipStr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, ipStr, sizeof(ipStr));
        int port = ntohs(clientAddr.sin_port);
        
        stats_.totalConnections++;
        stats_.activeConnections++;
        
        // Handle connection
        auto conn = std::make_unique<Connection>(clientFd, ipStr, port);
        handleConnection(std::move(conn));
        
        stats_.activeConnections--;
    }
}

void Application::handleConnection(std::unique_ptr<Connection> conn) {
    conn->setNonBlocking(false);  // Blocking mode for simplicity
    
    // Set receive timeout
    struct timeval tv;
    tv.tv_sec = config_.connectionTimeoutMs / 1000;
    tv.tv_usec = (config_.connectionTimeoutMs % 1000) * 1000;
    setsockopt(conn->fd(), SOL_SOCKET, SO_RCVTIMEO, 
               reinterpret_cast<const char*>(&tv), sizeof(tv));
    
    int requestCount = 0;
    bool keepAlive = config_.enableKeepAlive;
    
    do {
        // Read request
        while (!conn->requestReady() && !conn->hasError()) {
            int n = conn->readData();
            if (n <= 0) break;
        }
        
        if (!conn->requestReady()) break;
        
        // Fill in connection info
        Request& req = conn->getRequest();
        req.remoteAddr = conn->remoteAddr();
        req.remotePort = conn->remotePort();
        
        // Handle request
        Response response = handleRequest(req);
        
        // Update stats
        stats_.totalRequests++;
        if (response.statusCode >= 200 && response.statusCode < 300) {
            stats_.errors2xx++;
        } else if (response.statusCode >= 400 && response.statusCode < 500) {
            stats_.errors4xx++;
        } else if (response.statusCode >= 500) {
            stats_.errors5xx++;
        }
        
        // Check keep-alive
        std::string connHeader = req.getHeader("Connection");
        std::transform(connHeader.begin(), connHeader.end(), connHeader.begin(), ::tolower);
        
        if (connHeader == "close" || !config_.enableKeepAlive) {
            keepAlive = false;
            response.header("Connection", "close");
        } else if (connHeader == "keep-alive" || req.protocol == "HTTP/1.1") {
            keepAlive = true;
            response.header("Connection", "keep-alive");
        } else {
            keepAlive = false;
        }
        
        // Send response
        int sent = conn->sendResponse(response);
        if (sent > 0) {
            stats_.bytesSent += sent;
        }
        
        // Reset for next request
        conn->reset();
        requestCount++;
        
    } while (keepAlive && requestCount < config_.maxRequestsPerConnection && !shouldStop_.load());
}

void Application::executeHandlers(Context& ctx, const std::vector<Handler>& handlers, size_t index) {
    if (index >= handlers.size() || ctx.isSent()) {
        return;
    }
    
    try {
        handlers[index](ctx, [this, &ctx, &handlers, index]() {
            executeHandlers(ctx, handlers, index + 1);
        });
    } catch (const std::exception& e) {
        ctx.hasError = true;
        ctx.errorMessage = e.what();
        if (errorHandler_) {
            errorHandler_(ctx, e.what());
        }
    }
}

Response Application::handleRequest(Request& req) {
    std::lock_guard<std::mutex> lock(executeMutex_);
    
    Context ctx;
    ctx.req = std::move(req);
    
    // Build handler chain
    std::vector<Handler> handlers;
    
    // 1. Global middleware
    for (const auto& mw : globalMiddleware_) {
        handlers.push_back(mw);
    }
    
    // 2. Router middleware matching path
    for (const auto& [prefix, mw] : router_.getMiddleware()) {
        if (prefix.empty() || ctx.req.path.find(prefix) == 0) {
            handlers.push_back(mw);
        }
    }
    
    // 3. Find matching route
    Route* route = nullptr;
    std::unordered_map<std::string, std::string> params;
    
    if (router_.findRoute(ctx.req.method, ctx.req.path, route, params)) {
        ctx.req.params = params;
        ctx.matchedRoute = route->pattern.pattern;
        
        for (const auto& handler : route->handlers) {
            handlers.push_back(handler);
        }
    } else {
        // No route found - use not found handler
        handlers.push_back(notFoundHandler_);
    }
    
    // Execute handler chain
    executeHandlers(ctx, handlers, 0);
    
    // If response wasn't sent, send default
    if (!ctx.isSent()) {
        if (ctx.hasError) {
            ctx.res.serverError(ctx.errorMessage);
        } else {
            ctx.res.notFound("No response generated");
        }
    }
    
    return std::move(ctx.res);
}

bool Application::start() {
    if (running_.load()) {
        std::cerr << "[bedrock] Already running" << std::endl;
        return false;
    }
    
    stats_.reset();
    
    // Use high-performance async mode by default
    if (useAsyncMode_) {
        std::cerr << "[bedrock] Starting in HIGH-PERFORMANCE async mode on " 
                  << config_.host << ":" << config_.port << std::endl;
        
        asyncServer_ = std::make_unique<AsyncServer>(this);
        if (asyncServer_->start()) {
            running_ = true;
            shouldStop_ = false;
            std::cerr << "[bedrock] Async server started successfully" << std::endl;
            return true;
        } else {
            std::cerr << "[bedrock] Async mode failed, falling back to blocking mode" << std::endl;
            asyncServer_.reset();
            useAsyncMode_ = false;
        }
    }
    
    // Fallback to blocking mode (legacy)
    std::cerr << "[bedrock] Starting in blocking mode on " 
              << config_.host << ":" << config_.port << std::endl;
    
    if (!createSocket()) {
        std::cerr << "[bedrock] Failed to create socket: " << strerror(errno) << std::endl;
        return false;
    }
    
    std::cerr << "[bedrock] Socket created successfully" << std::endl;
    
    shouldStop_ = false;
    running_ = true;
    
    // Start accept thread (blocking mode)
    acceptThread_ = std::thread(&Application::acceptLoop, this);
    
    std::cerr << "[bedrock] Accept thread started (blocking mode)" << std::endl;
    
    return true;
}

void Application::stop() {
    if (!running_.load()) return;
    
    shouldStop_ = true;
    
    // Stop async server if running
    if (asyncServer_) {
        asyncServer_->stop();
        asyncServer_.reset();
    }
    
    // Close server socket to unblock accept (blocking mode)
    if (serverSocket_ != INVALID_SOCKET_VALUE) {
        close_socket(serverSocket_);
        serverSocket_ = INVALID_SOCKET_VALUE;
    }
    
    // Wait for accept thread (blocking mode)
    if (acceptThread_.joinable()) {
        acceptThread_.join();
    }
    
    running_ = false;
}

void Application::wait() {
    // In async mode, just block until stop is called
    if (asyncServer_) {
        while (running_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return;
    }
    
    // In blocking mode, wait for accept thread
    if (acceptThread_.joinable()) {
        acceptThread_.join();
    }
}

// ============================================================================
// Application Manager Implementation
// ============================================================================

ApplicationManager& ApplicationManager::instance() {
    static ApplicationManager manager;
    return manager;
}

ApplicationManager::~ApplicationManager() {
    shutdown();
}

Application* ApplicationManager::createApp(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto app = std::make_unique<Application>(name);
    std::string id = app->id();
    apps_[id] = std::move(app);
    
    return apps_[id].get();
}

Application* ApplicationManager::getApp(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = apps_.find(id);
    return it != apps_.end() ? it->second.get() : nullptr;
}

bool ApplicationManager::removeApp(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = apps_.find(id);
    if (it == apps_.end()) return false;
    
    it->second->stop();
    apps_.erase(it);
    return true;
}

void ApplicationManager::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    for (auto& [id, app] : apps_) {
        app->stop();
    }
    apps_.clear();
}

std::vector<std::string> ApplicationManager::listApps() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<std::string> result;
    result.reserve(apps_.size());
    for (const auto& [id, app] : apps_) {
        result.push_back(id);
    }
    return result;
}

} // namespace bedrock
