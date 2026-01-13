// ============================================================================
// Bedrock - A Minimalist Web Framework for Quartz
// Types and Core Data Structures
// ============================================================================
// 
// Philosophy: No magic. No bloat. Just you and your code.
// - Explicit over implicit
// - Composition over inheritance
// - Performance by default
// - Zero hidden behavior
//
// ============================================================================

#ifndef BEDROCK_TYPES_H
#define BEDROCK_TYPES_H

#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <deque>
#include <memory>
#include <mutex>
#include <atomic>
#include <thread>
#include <functional>
#include <regex>
#include <chrono>
#include <optional>
#include <queue>
#include <condition_variable>
#include "bedrock_buffer.h"

// Platform detection
#if defined(_WIN32) || defined(_WIN64)
    #define QZ_PLATFORM_WINDOWS 1
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    using socket_t = SOCKET;
    #define INVALID_SOCKET_VALUE INVALID_SOCKET
    #define close_socket closesocket
#elif defined(__APPLE__) || defined(__MACH__)
    #define QZ_PLATFORM_MACOS 1
    #include <sys/socket.h>
    #include <sys/types.h>
    #include <sys/event.h>
    #include <sys/un.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
    using socket_t = int;
    #define INVALID_SOCKET_VALUE (-1)
    #define close_socket ::close
#elif defined(__linux__)
    #define QZ_PLATFORM_LINUX 1
    #include <sys/socket.h>
    #include <sys/types.h>
    #include <sys/epoll.h>
    #include <sys/un.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
    using socket_t = int;
    #define INVALID_SOCKET_VALUE (-1)
    #define close_socket ::close
#endif

namespace bedrock {

// ============================================================================
// HTTP Method Enumeration
// ============================================================================

enum class HttpMethod {
    GET,
    POST,
    PUT,
    DELETE_,
    PATCH,
    HEAD,
    OPTIONS,
    CONNECT,
    TRACE,
    ANY  // Matches any method
};

inline std::string methodToString(HttpMethod m) {
    switch (m) {
        case HttpMethod::GET:     return "GET";
        case HttpMethod::POST:    return "POST";
        case HttpMethod::PUT:     return "PUT";
        case HttpMethod::DELETE_: return "DELETE";
        case HttpMethod::PATCH:   return "PATCH";
        case HttpMethod::HEAD:    return "HEAD";
        case HttpMethod::OPTIONS: return "OPTIONS";
        case HttpMethod::CONNECT: return "CONNECT";
        case HttpMethod::TRACE:   return "TRACE";
        case HttpMethod::ANY:     return "*";
        default:                  return "GET";
    }
}

inline HttpMethod stringToMethod(std::string_view s) {
    if (s == "GET")     return HttpMethod::GET;
    if (s == "POST")    return HttpMethod::POST;
    if (s == "PUT")     return HttpMethod::PUT;
    if (s == "DELETE")  return HttpMethod::DELETE_;
    if (s == "PATCH")   return HttpMethod::PATCH;
    if (s == "HEAD")    return HttpMethod::HEAD;
    if (s == "OPTIONS") return HttpMethod::OPTIONS;
    if (s == "CONNECT") return HttpMethod::CONNECT;
    if (s == "TRACE")   return HttpMethod::TRACE;
    if (s == "*")       return HttpMethod::ANY;
    return HttpMethod::GET;
}

// ============================================================================
// Request Context - All request data in one place
// ============================================================================

struct Request {
    // Core HTTP fields (Zero-copy)
    HttpMethod method = HttpMethod::GET;
    std::string_view methodStr;
    std::string_view path;
    std::string_view rawPath;
    std::string_view queryString;
    std::string_view protocol = "HTTP/1.1";
    
    // Headers (Vector-based for zero allocations during parsing)
    struct Header {
        std::string_view name;
        std::string_view value;
    };
    std::vector<Header> headers;
    
    // Body (Zero-copy)
    std::string_view body;
    size_t contentLength = 0;
    std::string_view contentType;
    
    // Connection info (Persistent)
    std::string remoteAddr;
    int remotePort = 0;
    std::string_view host;
    
    // Parsed data (Allocations only when needed)
    std::unordered_map<std::string, std::string> params;      // Route parameters (:id, :name)
    std::unordered_map<std::string, std::string> query;       // Query string parameters
    std::unordered_map<std::string, std::string> cookies;     // Parsed cookies
    
    // Request-scoped storage
    std::unordered_map<std::string, std::string> locals;
    
    // The underlying buffer (kept alive until request is handled)
    std::shared_ptr<IOBuffer> buffer;
    
    // Stable storage for strings when not using buffer
    std::shared_ptr<std::deque<std::string>> stringStorage;
    
    // Helper to store string and get view
    std::string_view storeString(const std::string& s) {
        if (!stringStorage) stringStorage = std::make_shared<std::deque<std::string>>();
        stringStorage->push_back(s);
        return stringStorage->back();
    }
    
    // Timing
    std::chrono::steady_clock::time_point startTime;
    
    Request() : startTime(std::chrono::steady_clock::now()) {}
    
    // Get header (case-insensitive)
    std::string_view getHeader(std::string_view name) const;
    
    // Get parameter (route param, then query param)
    std::string getParam(const std::string& name) const;
    
    // Get query parameter
    std::string getQuery(const std::string& name) const;
    
    // Get cookie
    std::string getCookie(const std::string& name) const;
    
    // Get local (middleware storage)
    std::string getLocal(const std::string& name) const;
    void setLocal(const std::string& name, const std::string& value);
    
    // Elapsed time in milliseconds
    int64_t elapsedMs() const;
    
    // Check content type
    bool isJson() const;
    bool isForm() const;
    bool isMultipart() const;
};

// ============================================================================
// Response Builder - Explicit response construction
// ============================================================================

struct Response {
    int statusCode = 200;
    std::string statusText = "OK";
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    bool sent = false;
    bool chunked = false;
    
    // Status setters (chainable pattern)
    Response& status(int code);
    Response& status(int code, const std::string& text);
    
    // Header setters (chainable)
    Response& header(const std::string& name, const std::string& value);
    Response& contentType(const std::string& type);
    Response& contentLength(size_t len);
    
    // Cookie setter
    Response& cookie(const std::string& name, const std::string& value, 
                     int maxAge = -1, const std::string& path = "/",
                     bool httpOnly = true, bool secure = false,
                     const std::string& sameSite = "Lax");
    
    // Body setters (each marks response as ready to send)
    Response& text(const std::string& content);
    Response& html(const std::string& content);
    Response& json(const std::string& jsonContent);
    Response& send(const std::string& content, const std::string& contentType = "text/plain");
    
    // Special responses
    Response& redirect(const std::string& location, int code = 302);
    Response& notFound(const std::string& message = "Not Found");
    Response& badRequest(const std::string& message = "Bad Request");
    Response& serverError(const std::string& message = "Internal Server Error");
    Response& noContent();
    
    // Build HTTP response string (DEPRECATED: Use buildIov for writev)
    std::string build() const;

    // For Scatter/Gather I/O (writev)
    struct BufferView {
        const char* data;
        size_t len;
    };
    std::vector<BufferView> buildIov() const;
    
    // Factory methods for quick responses
    static Response ok(const std::string& body = "", const std::string& contentType = "text/plain");
    static Response created(const std::string& body = "", const std::string& location = "");
    static Response accepted();
    static Response jsonResponse(const std::string& json);
    static Response htmlResponse(const std::string& html);
    static Response errorResponse(int code, const std::string& message);
};

// ============================================================================
// Route Matching
// ============================================================================

// Route parameter extracted from path
struct RouteParam {
    std::string name;
    size_t position;  // Position in path segments
    bool isWildcard;  // true for * or **
};

// Compiled route pattern
struct RoutePattern {
    std::string pattern;           // Original pattern string
    std::regex regex;              // Compiled regex for matching
    std::vector<RouteParam> params;// Parameters in order
    bool isStatic;                 // True if no dynamic segments
    int priority;                  // Higher = matched first (static > specific > wildcard)
    
    bool matches(const std::string& path, std::unordered_map<std::string, std::string>& outParams) const;
    static RoutePattern compile(const std::string& pattern);
};

// ============================================================================
// Handler Types
// ============================================================================

// Forward declarations
struct Request;
struct Response;
class Context;

// Next function for middleware chaining
using NextFn = std::function<void()>;

// Handler receives context and next function
using Handler = std::function<void(Context&, NextFn)>;

// Simple handler (no next, for endpoints)
using SimpleHandler = std::function<void(Context&)>;

// Error handler receives context and exception info
using ErrorHandler = std::function<void(Context&, const std::string& error)>;

// ============================================================================
// Context - Request/Response pair with utilities
// ============================================================================

class Context {
public:
    Request req;
    Response res;
    
    // The matched route (if any)
    std::string matchedRoute;
    
    // Error state
    bool hasError = false;
    std::string errorMessage;
    std::string errorType;
    
    // Check if response was already sent
    bool isSent() const { return res.sent; }
    
    // Mark response as sent
    void markSent() { res.sent = true; }
    
    // Get/set locals (shorthand)
    std::string local(const std::string& name) const { return req.getLocal(name); }
    void local(const std::string& name, const std::string& value) { req.setLocal(name, value); }
    
    // Get param (shorthand)
    std::string param(const std::string& name) const { return req.getParam(name); }
    std::string query(const std::string& name) const { return req.getQuery(name); }
    std::string_view header(std::string_view name) const { return req.getHeader(name); }
};

// ============================================================================
// Route Definition
// ============================================================================

struct Route {
    HttpMethod method;
    RoutePattern pattern;
    std::vector<Handler> handlers;  // Middleware + final handler
    std::string name;               // Optional route name for reverse lookup
    
    bool matches(HttpMethod m, const std::string& path, 
                 std::unordered_map<std::string, std::string>& outParams) const;
};

// ============================================================================
// Router - Handles routing logic
// ============================================================================

class Router {
public:
    Router(const std::string& prefix = "");
    
    // Route registration
    Router& get(const std::string& path, Handler handler);
    Router& post(const std::string& path, Handler handler);
    Router& put(const std::string& path, Handler handler);
    Router& del(const std::string& path, Handler handler);  // "delete" is reserved
    Router& patch(const std::string& path, Handler handler);
    Router& head(const std::string& path, Handler handler);
    Router& options(const std::string& path, Handler handler);
    Router& any(const std::string& path, Handler handler);  // Matches any method
    
    // Route with multiple methods
    Router& route(const std::vector<HttpMethod>& methods, const std::string& path, Handler handler);
    
    // Named routes
    Router& get(const std::string& path, Handler handler, const std::string& name);
    
    // Middleware (runs before all routes in this router)
    Router& use(Handler middleware);
    Router& use(const std::string& path, Handler middleware);  // Path-scoped middleware
    
    // Sub-router (mount another router)
    Router& mount(const std::string& path, std::shared_ptr<Router> subRouter);
    
    // Get prefix
    const std::string& getPrefix() const { return prefix_; }
    
    // Find matching route
    bool findRoute(HttpMethod method, std::string_view path,
                   Route*& outRoute, std::unordered_map<std::string, std::string>& outParams);
    
    // Get all routes (for debugging/documentation)
    const std::vector<Route>& getRoutes() const { return routes_; }
    
    // Get middleware
    const std::vector<std::pair<std::string, Handler>>& getMiddleware() const { return middleware_; }
    
    // Get sub-routers
    const std::vector<std::pair<std::string, std::shared_ptr<Router>>>& getSubRouters() const { return subRouters_; }
    
private:
    std::string prefix_;
    std::vector<Route> routes_;
    std::vector<std::pair<std::string, Handler>> middleware_;  // (path prefix, handler)
    std::vector<std::pair<std::string, std::shared_ptr<Router>>> subRouters_;
    
    // Low-level Radix Tree for high-performance matching
    struct RadixNode {
        std::string segment;
        std::unordered_map<std::string, std::unique_ptr<RadixNode>> staticChildren;
        std::unique_ptr<RadixNode> paramChild;
        std::string paramName;
        std::unique_ptr<RadixNode> wildcardChild;
        Route* route = nullptr;
        
        RadixNode* getOrCreateStatic(std::string_view seg) {
            std::string s(seg);
            if (staticChildren.find(s) == staticChildren.end()) {
                staticChildren[s] = std::make_unique<RadixNode>();
                staticChildren[s]->segment = s;
            }
            return staticChildren[s].get();
        }
    };
    
    RadixNode radixRoot_;
    void addToRadix(Route& route);
    
    void addRoute(HttpMethod method, const std::string& path, Handler handler, const std::string& name = "");
};

// ============================================================================
// Application Configuration
// ============================================================================

struct AppConfig {
    // Server settings
    std::string host = "127.0.0.1";
    int port = 8080;
    std::string unixSocket;           // Unix socket path (alternative to TCP)
    bool useUnixSocket = false;
    
    // Performance settings
    int backlog = 4096;
    int maxConnections = 10000;
    int workerThreads = 0;            // 0 = auto (CPU cores)
    int connectionTimeoutMs = 30000;
    int keepAliveTimeoutMs = 5000;
    bool tcpNoDelay = true;
    bool reuseAddr = true;
    bool reusePort = false;
    
    // Request limits
    size_t maxBodySize = 10 * 1024 * 1024;  // 10MB
    size_t maxHeaderSize = 8192;
    int maxRequestsPerConnection = 1000;
    
    // Features
    bool enableKeepAlive = true;
    bool enableCompression = false;
    bool trustProxy = false;          // Trust X-Forwarded-* headers
    
    // Logging
    bool verbose = false;
    bool accessLog = true;
    
    // Mode
    bool standaloneMode = true;       // true = standalone, false = uWSGI mode
    std::string uwsgiSocket;          // uWSGI socket for nginx integration
};

// ============================================================================
// Server Statistics
// ============================================================================

struct ServerStats {
    std::atomic<uint64_t> totalRequests{0};
    std::atomic<uint64_t> activeConnections{0};
    std::atomic<uint64_t> totalConnections{0};
    std::atomic<uint64_t> bytesReceived{0};
    std::atomic<uint64_t> bytesSent{0};
    std::atomic<uint64_t> errors2xx{0};
    std::atomic<uint64_t> errors4xx{0};
    std::atomic<uint64_t> errors5xx{0};
    std::chrono::steady_clock::time_point startTime;
    
    ServerStats() : startTime(std::chrono::steady_clock::now()) {}
    
    uint64_t uptimeMs() const {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count();
    }
    
    double requestsPerSecond() const {
        uint64_t ms = uptimeMs();
        return ms > 0 ? (double)totalRequests.load() * 1000.0 / (double)ms : 0.0;
    }
    
    void reset() {
        totalRequests = 0;
        activeConnections = 0;
        totalConnections = 0;
        bytesReceived = 0;
        bytesSent = 0;
        errors2xx = 0;
        errors4xx = 0;
        errors5xx = 0;
        startTime = std::chrono::steady_clock::now();
    }
};

// ============================================================================
// Static File Options
// ============================================================================

struct StaticOptions {
    std::string root;                    // Root directory
    std::string index = "index.html";    // Default index file
    bool dotfiles = false;               // Serve dot files
    bool etag = true;                    // Enable ETag headers
    bool lastModified = true;            // Enable Last-Modified headers
    int maxAge = 0;                      // Cache-Control max-age (seconds)
    std::vector<std::string> extensions; // Extensions to try (e.g., [".html", ".htm"])
    
    std::unordered_map<std::string, std::string> mimeTypes;  // Custom mime types
};

// ============================================================================
// CORS Options
// ============================================================================

struct CorsOptions {
    std::vector<std::string> origins;    // Allowed origins ("*" for all)
    std::vector<std::string> methods;    // Allowed methods
    std::vector<std::string> headers;    // Allowed headers
    std::vector<std::string> exposeHeaders;  // Headers to expose
    bool credentials = false;            // Allow credentials
    int maxAge = 86400;                  // Preflight cache (seconds)
};

// ============================================================================
// Helper functions
// ============================================================================

// URL decode
std::string urlDecode(std::string_view encoded);

// URL encode
std::string urlEncode(std::string_view str);

// Parse query string
std::unordered_map<std::string, std::string> parseQueryString(std::string_view query);

// Parse cookies
std::unordered_map<std::string, std::string> parseCookies(std::string_view cookieHeader);

// Get MIME type for file extension
std::string getMimeType(const std::string& extension);

// Get HTTP status text
std::string getStatusText(int code);

// Normalize path (remove .., resolve /)
std::string normalizePath(std::string_view path);

// Path join
std::string pathJoin(std::string_view base, std::string_view path);

// File exists
bool fileExists(const std::string& path);

// Read file contents
std::string readFile(const std::string& path);

// Get file extension
std::string getExtension(const std::string& path);

// ============================================================================
// Application Class Declaration
// ============================================================================

class Connection;     // Forward declaration
class AsyncServer;    // Forward declaration for high-performance async server

class Application {
public:
    Application(const std::string& name = "");
    ~Application();
    
    // Configuration
    Application& configure(const AppConfig& config);
    AppConfig& config() { return config_; }
    const AppConfig& config() const { return config_; }
    
    // Router access
    Router& router() { return router_; }
    
    // Route registration shortcuts
    Application& get(const std::string& path, Handler handler);
    Application& post(const std::string& path, Handler handler);
    Application& put(const std::string& path, Handler handler);
    Application& del(const std::string& path, Handler handler);
    Application& patch(const std::string& path, Handler handler);
    Application& any(const std::string& path, Handler handler);
    
    // Middleware
    Application& use(Handler middleware);
    Application& use(const std::string& path, Handler middleware);
    
    // Mount sub-router
    Application& mount(const std::string& path, std::shared_ptr<Router> subRouter);
    
    // Error handling
    Application& onError(ErrorHandler handler);
    Application& onNotFound(Handler handler);
    
    // Lifecycle
    bool start();
    void stop();
    bool isRunning() const { return running_.load(); }
    
    // Wait for server to stop
    void wait();
    
    // Get stats
    const ServerStats& stats() const { return stats_; }
    
    // Get ID
    const std::string& id() const { return id_; }
    const std::string& name() const { return name_; }
    
    // Handle request (used internally and for uWSGI integration)
    Response handleRequest(Request& req);
    
private:
    std::string id_;
    std::string name_;
    AppConfig config_;
    Router router_;
    ServerStats stats_;
    
    std::atomic<bool> running_{false};
    std::atomic<bool> shouldStop_{false};
    
    socket_t serverSocket_ = INVALID_SOCKET_VALUE;
    std::thread acceptThread_;
    std::vector<std::thread> workerThreads_;
    
    // Async server for high-performance mode (uses event loop + worker pool)
    std::unique_ptr<AsyncServer> asyncServer_;
    bool useAsyncMode_ = true;  // Default to async mode for best performance
    
    // Handlers
    ErrorHandler errorHandler_;
    Handler notFoundHandler_;
    std::vector<Handler> globalMiddleware_;
    
    // Internal methods
    bool createSocket();
    void acceptLoop();
    void handleConnection(std::unique_ptr<Connection> conn);
    void executeHandlers(Context& ctx, const std::vector<Handler>& handlers, size_t index);
    
    // Mutex to protect Quartz VM execution (as it's not thread-safe)
    std::recursive_mutex executeMutex_;
    
    static std::atomic<uint64_t> nextId_;
};

// ============================================================================
// Application Manager (singleton for managing multiple apps)
// ============================================================================

class ApplicationManager {
public:
    static ApplicationManager& instance();
    
    Application* createApp(const std::string& name = "");
    Application* getApp(const std::string& id);
    bool removeApp(const std::string& id);
    void shutdown();
    
    std::vector<std::string> listApps() const;
    
private:
    ApplicationManager() = default;
    ~ApplicationManager();
    
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<Application>> apps_;
    std::atomic<uint64_t> nextId_{0};
};

} // namespace bedrock

#endif // BEDROCK_TYPES_H
