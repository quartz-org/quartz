// ============================================================================
// Bedrock Async - High-Performance Event-Driven Server
// Cross-platform: Linux (epoll), macOS (kqueue), Windows (select)
// ============================================================================

#ifndef BEDROCK_ASYNC_H
#define BEDROCK_ASYNC_H

#include "bedrock_types.h"
#include <thread>
#include <atomic>
#include <queue>
#include <condition_variable>
#include <vector>
#include <memory>
#include <functional>

namespace bedrock {

// ============================================================================
// Forward Declarations
// ============================================================================
class AsyncConnection;
class ConnectionPool;
class WorkerPool;
class IOMultiplexer;

// ============================================================================
// Connection State Machine
// ============================================================================
enum class ConnectionState {
    READING_REQUEST,    // Reading HTTP request headers/body
    PROCESSING,         // Request is being handled by worker
    WRITING_RESPONSE,   // Writing response back to client
    CLOSING,            // Connection is being closed
    IDLE                // Connection is idle (keep-alive)
};

// ============================================================================
// Async Connection - Non-blocking connection with state
// ============================================================================
class AsyncConnection {
public:
    AsyncConnection(socket_t fd, const std::string& remoteAddr, int remotePort);
    ~AsyncConnection();
    
    // Socket info
    socket_t fd() const { return fd_; }
    const std::string& remoteAddr() const { return remoteAddr_; }
    int remotePort() const { return remotePort_; }
    
    // Non-blocking I/O
    bool setNonBlocking(bool nonBlocking);
    
    // Read available data (returns bytes read, 0 for would-block, -1 for error)
    int readNonBlocking();
    
    // Write pending data (returns bytes written, 0 for would-block, -1 for error)
    int writeNonBlocking();
    
    // Buffer management
    bool hasCompleteRequest() const;
    bool hasDataToWrite() const { return writePos_ < writeBuffer_.size(); }
    Request& getRequest() { return request_; }
    void setResponse(const Response& response);
    void reset();  // Reset for keep-alive
    
    // State
    ConnectionState state() const { return state_; }
    void setState(ConnectionState s) { state_ = s; }
    
    // Timing
    std::chrono::steady_clock::time_point lastActivity() const { return lastActivity_; }
    void touch() { lastActivity_ = std::chrono::steady_clock::now(); }
    bool isTimedOut(int timeoutMs) const;
    
    // Request ID for cross-thread tracking
    uint64_t requestId() const { return requestId_; }
    void setRequestId(uint64_t id) { requestId_ = id; }
    
private:
    socket_t fd_;
    std::string remoteAddr_;
    int remotePort_;
    ConnectionState state_ = ConnectionState::READING_REQUEST;
    
    // Read buffer and parser state
    std::string readBuffer_;
    Request request_;
    size_t contentLength_ = 0;
    size_t bodyRead_ = 0;
    bool headersParsed_ = false;
    
    // Write buffer
    std::string writeBuffer_;
    size_t writePos_ = 0;
    
    // Timing
    std::chrono::steady_clock::time_point lastActivity_;
    
    // Request tracking
    uint64_t requestId_ = 0;
    
    // Internal parsing
    bool parseRequest();
    bool parseRequestLine();
    bool parseHeaders();
};

// ============================================================================
// Work Item - Request to be processed by worker
// ============================================================================
struct WorkItem {
    socket_t fd;
    uint64_t requestId;
    Request request;
    std::function<void(Response)> callback;
};

// ============================================================================
// Worker Pool - Processes requests on multiple threads
// ============================================================================
class WorkerPool {
public:
    WorkerPool(size_t numWorkers);
    ~WorkerPool();
    
    void start();
    void stop();
    bool submit(WorkItem item);
    
    size_t pendingItems() const { return pendingCount_.load(); }
    size_t activeWorkers() const { return numWorkers_; }
    
private:
    void workerLoop();
    
    size_t numWorkers_;
    std::atomic<bool> running_{false};
    std::vector<std::thread> workers_;
    
    std::queue<WorkItem> workQueue_;
    std::mutex queueMutex_;
    std::condition_variable queueCV_;
    std::atomic<size_t> pendingCount_{0};
};

// ============================================================================
// I/O Multiplexer - Platform-specific event polling
// ============================================================================

// Event flags
constexpr uint32_t IO_READ  = 0x01;
constexpr uint32_t IO_WRITE = 0x02;
constexpr uint32_t IO_ERROR = 0x04;
constexpr uint32_t IO_CLOSE = 0x08;

// Event structure
struct IOEvent {
    socket_t fd;
    uint32_t events;
};

class IOMultiplexer {
public:
    IOMultiplexer(int maxConnections = 10000);
    ~IOMultiplexer();
    
    bool init();
    void destroy();
    
    bool addSocket(socket_t fd, uint32_t events);
    bool modifySocket(socket_t fd, uint32_t events);
    bool removeSocket(socket_t fd);
    
    // Poll for events, returns number of ready sockets
    int poll(int timeoutMs);
    
    // Wake up the poll() call
    void wakeup();
    
    // Get events after poll
    const std::vector<IOEvent>& events() const { return readyEvents_; }
    
private:
    int maxConnections_;
    std::vector<IOEvent> readyEvents_;
    
#ifdef QZ_PLATFORM_LINUX
    int epollFd_ = -1;
    int wakeupFd_ = -1;
    std::vector<struct epoll_event> epollEvents_;
#elif defined(QZ_PLATFORM_MACOS)
    int kqueueFd_ = -1;
    int wakeupPipe_[2] = {-1, -1};
    std::vector<struct kevent> kqueueEvents_;
    std::unordered_map<socket_t, uint32_t> fdEvents_;
#elif defined(QZ_PLATFORM_WINDOWS)
    fd_set readSet_, writeSet_, exceptSet_;
    std::unordered_map<socket_t, uint32_t> fdEvents_;
#endif
};

// ============================================================================
// Connection Manager - Manages all active connections
// ============================================================================
class ConnectionManager {
public:
    ConnectionManager(size_t maxConnections = 10000);
    ~ConnectionManager();
    
    // Add new connection
    AsyncConnection* addConnection(socket_t fd, const std::string& remoteAddr, int remotePort);
    
    // Get connection by fd
    AsyncConnection* getConnection(socket_t fd);
    
    // Remove connection
    void removeConnection(socket_t fd);
    
    // Get all connections for iteration
    const std::unordered_map<socket_t, std::unique_ptr<AsyncConnection>>& connections() const {
        return connections_;
    }
    
    // Cleanup timed-out connections
    std::vector<socket_t> getTimedOutConnections(int timeoutMs);
    
    // Stats
    size_t activeConnections() const { return connections_.size(); }
    
private:
    size_t maxConnections_;
    std::unordered_map<socket_t, std::unique_ptr<AsyncConnection>> connections_;
    mutable std::mutex mutex_;
};

// ============================================================================
// Async Server - Main event-driven server
// ============================================================================
class AsyncServer {
public:
    AsyncServer(Application* app);
    ~AsyncServer();
    
    bool start();
    void stop();
    bool isRunning() const { return running_.load(); }
    
    // Stats
    uint64_t totalConnections() const { return totalConnections_.load(); }
    uint64_t totalRequests() const { return totalRequests_.load(); }
    
private:
    Application* app_;
    std::atomic<bool> running_{false};
    std::atomic<bool> shouldStop_{false};
    
    socket_t serverSocket_ = INVALID_SOCKET_VALUE;
    std::unique_ptr<IOMultiplexer> multiplexer_;
    std::unique_ptr<ConnectionManager> connManager_;
    std::unique_ptr<WorkerPool> workerPool_;
    
    std::thread eventLoopThread_;
    
    // Stats
    std::atomic<uint64_t> totalConnections_{0};
    std::atomic<uint64_t> totalRequests_{0};
    std::atomic<uint64_t> nextRequestId_{0};
    
    // Response queue (from workers back to event loop)
    struct ResponseItem {
        socket_t fd;
        uint64_t requestId;
        Response response;
    };
    std::queue<ResponseItem> responseQueue_;
    std::mutex responseMutex_;
    
    // Internal methods
    bool createServerSocket();
    void eventLoop();
    void handleAccept();
    void handleRead(socket_t fd);
    void handleWrite(socket_t fd);
    void handleError(socket_t fd);
    void processResponses();
    void cleanupTimeouts();
};

} // namespace bedrock

#endif // BEDROCK_ASYNC_H
