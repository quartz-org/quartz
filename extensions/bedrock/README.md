# Bedrock

> A minimalist, high-performance web framework for Quartz.  
> **No magic. No bloat. Just you and your code.**

## Philosophy

Bedrock is built on four core principles:

1. **Explicit over Implicit** - Every behavior is clear and intentional
2. **Composition over Inheritance** - Build what you need with simple building blocks
3. **Performance by Default** - Zero-cost abstractions, optimized from the ground up
4. **Transparency** - You control everything, no hidden magic

## Features

- **High-Performance Async Server** - Event-driven with epoll (Linux), kqueue (macOS), and select (Windows)
- **Worker Thread Pool** - Parallel request processing for maximum throughput
- **Non-Blocking I/O** - Handles 10,000+ concurrent connections efficiently
- **nginx Integration** - Production-ready uWSGI protocol support
- **Flexible Routing** - Path parameters, wildcards, regex constraints
- **Middleware Chain** - Composable request/response processing
- **Zero Dependencies** - Pure C++ implementation, no external libraries
- **Cross-Platform** - Works on Linux, macOS, and Windows

## Quick Start

```quartz
import bedrock;
import system.json as json;

// Create application
let app = bedrock.create({
    "host": "0.0.0.0",
    "port": 8080
});

// Simple GET route
bedrock.get(app, "/", (req) => {
    return bedrock.response.html("<h1>Welcome to Bedrock!</h1>");
});

// JSON API endpoint
bedrock.get(app, "/api/users/:id", (req) => {
    let userId = req["params"]["id"];
    let user = {"id": userId, "name": "John Doe"};
    return bedrock.response.json(json.stringify(user));
});

// POST with body
bedrock.post(app, "/api/users", (req) => {
    let body = req["body"];
    let user = json.parse(body);
    // Save user...
    return bedrock.response.created(json.stringify(user), "/api/users/123");
});

// Start server
bedrock.start(app);
println("Server running on http://0.0.0.0:8080");
```

## API Reference

### Application Lifecycle

```quartz
// Create an application
let app = bedrock.create();                     // Default config
let app = bedrock.create("MyApp");              // Named app
let app = bedrock.create({"port": 3000});       // With config

// Configure application
bedrock.configure(app, {
    "host": "0.0.0.0",          // Bind address
    "port": 8080,               // Port number
    "backlog": 4096,            // Listen backlog
    "maxConnections": 10000,    // Max concurrent connections
    "timeout": 30000,           // Connection timeout (ms)
    "keepAlive": true,          // Enable HTTP keep-alive
    "keepAliveTimeout": 5000,   // Keep-alive timeout (ms)
    "tcpNoDelay": true,         // Disable Nagle's algorithm
    "reuseAddr": true,          // SO_REUSEADDR
    "reusePort": false,         // SO_REUSEPORT (Linux)
    "maxBodySize": 10485760,    // Max request body (10MB)
    "trustProxy": false,        // Trust X-Forwarded-* headers
    "verbose": false,           // Debug logging
    "accessLog": true           // Access logging
});

// Start/stop server
bedrock.start(app);         // Start listening
bedrock.stop(app);          // Stop server
bedrock.isRunning(app);     // Check if running

// Get statistics
let stats = bedrock.stats(app);
// {totalRequests, activeConnections, bytesReceived, bytesSent, ...}

// Cleanup
bedrock.destroy(app);       // Destroy application
```

### Route Registration

```quartz
// HTTP methods
bedrock.get(app, "/path", handler);
bedrock.post(app, "/path", handler);
bedrock.put(app, "/path", handler);
bedrock.delete(app, "/path", handler);
bedrock.patch(app, "/path", handler);
bedrock.any(app, "/path", handler);     // Any HTTP method

// Path parameters
bedrock.get(app, "/users/:id", (req) => {
    let id = req["params"]["id"];
    return bedrock.response.ok("User: " + id);
});

// Multiple parameters
bedrock.get(app, "/users/:userId/posts/:postId", (req) => {
    let userId = req["params"]["userId"];
    let postId = req["params"]["postId"];
    return bedrock.response.ok("User " + userId + ", Post " + postId);
});

// Wildcard routes
bedrock.get(app, "/files/*", handler);      // Single segment
bedrock.get(app, "/assets/**", handler);    // Multiple segments (catch-all)
```

### Request Object

The handler receives a request dictionary with:

```quartz
let handler = (req) => {
    // HTTP info
    req["method"];           // "GET", "POST", etc.
    req["path"];             // "/users/123"
    req["rawPath"];          // Original path before normalization
    req["queryString"];      // "foo=bar&baz=qux"
    req["protocol"];         // "HTTP/1.1"
    
    // Connection info
    req["host"];             // "example.com"
    req["remoteAddr"];       // "192.168.1.1"
    req["remotePort"];       // 54321
    
    // Body
    req["body"];             // Request body string
    req["contentType"];      // "application/json"
    req["contentLength"];    // 1234
    
    // Parsed data
    req["headers"];          // {"content-type": "...", ...}
    req["query"];            // {"foo": "bar", "baz": "qux"}
    req["params"];           // Route parameters {"id": "123"}
    req["cookies"];          // {"session": "abc123"}
    
    // Timing
    req["elapsedMs"];        // Time since request started
    
    // ...return response
};
```

### Response Helpers

```quartz
// Success responses
bedrock.response.ok();                              // 200, empty body
bedrock.response.ok("Hello");                       // 200 with text body
bedrock.response.ok("<html>...", "text/html");     // 200 with custom content type

// JSON responses
bedrock.response.json('{"key": "value"}');          // 200 application/json

// HTML responses
bedrock.response.html("<h1>Hello</h1>");            // 200 text/html

// Created (201)
bedrock.response.created();                         // 201
bedrock.response.created('{"id": 1}', "/api/resource/1");  // With body and location

// No Content (204)
bedrock.response.noContent();

// Redirects
bedrock.response.redirect("/new-location");         // 302
bedrock.response.redirect("/new-location", 301);    // Custom code

// Errors
bedrock.response.error(400, "Bad Request");
bedrock.response.error(500, "Internal Server Error");
bedrock.response.notFound();                        // 404
bedrock.response.notFound("Resource not found");    // 404 with message

// Custom response (full control)
return {
    "status": 200,
    "statusText": "OK",
    "body": "Hello, World!",
    "contentType": "text/plain",
    "headers": {
        "X-Custom-Header": "value",
        "Cache-Control": "no-cache"
    }
};
```

### Middleware

```quartz
// Global middleware (runs for all routes)
bedrock.use(app, (req) => {
    // Logging middleware
    println(req["method"] + " " + req["path"]);
    return nil;  // Continue to next handler
});

// Path-scoped middleware
bedrock.use(app, "/api", (req) => {
    // Only runs for /api/* routes
    let token = req["headers"]["authorization"];
    if (token == "") {
        return bedrock.response.error(401, "Unauthorized");
    }
    return nil;  // Continue
});

// Multiple middleware compose naturally
bedrock.use(app, loggerMiddleware);
bedrock.use(app, authMiddleware);
bedrock.use(app, "/admin", adminOnlyMiddleware);
```

### Error Handling

```quartz
// Custom error handler
bedrock.onError(app, (req, error) => {
    println("Error: " + error);
    return {
        "status": 500,
        "body": json.stringify({"error": error}),
        "contentType": "application/json"
    };
});

// Custom 404 handler
bedrock.onNotFound(app, (req) => {
    return {
        "status": 404,
        "body": json.stringify({
            "error": "Not Found",
            "path": req["path"]
        }),
        "contentType": "application/json"
    };
});
```

## Production Deployment

### Standalone Mode

```quartz
let app = bedrock.create({
    "host": "0.0.0.0",
    "port": 8080,
    "backlog": 4096,
    "maxConnections": 10000,
    "keepAlive": true,
    "tcpNoDelay": true,
    "reuseAddr": true
});

// Register routes...

bedrock.start(app);
println("Bedrock server running on port 8080");
```

### nginx + uWSGI Mode

For production with nginx as reverse proxy, use the uWSGI protocol:

**nginx.conf:**
```nginx
upstream bedrock {
    server unix:/tmp/bedrock.sock;
}

server {
    listen 80;
    server_name example.com;
    
    location / {
        uwsgi_pass bedrock;
        include uwsgi_params;
    }
}
```

**Quartz application:**
```quartz
import system.net.uwsgi as uwsgi;

// Create uWSGI server
let server = uwsgi.create({
    "socket": "/tmp/bedrock.sock",
    "backlog": 4096
});

// Register handler
uwsgi.onRequest(server, (req) => {
    // Handle request using Bedrock-style routing
    // This integrates with your Bedrock app
    return {
        "status": 200,
        "headers": {"Content-Type": "application/json"},
        "body": '{"message": "Hello from Bedrock!"}'
    };
});

uwsgi.start(server);
```

## Architecture

Bedrock uses a **high-performance async architecture** by default:

```
┌─────────────────────────────────────────────────────────┐
│                    Event Loop                           │
│  Platform-specific: epoll (Linux), kqueue (macOS),      │
│                     select (Windows)                    │
│                                                         │
│  ┌─────────────┐                                        │
│  │ Server FD   │◄─── Accept new connections             │
│  └─────────────┘                                        │
│                                                         │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐     │
│  │ Client FD 1 │  │ Client FD 2 │  │ Client FD N │ ... │
│  └─────────────┘  └─────────────┘  └─────────────┘     │
│         │                │                │             │
│         └────────────────┴────────────────┘             │
│                          │                              │
└──────────────────────────┼──────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────┐
│                   Worker Thread Pool                    │
│                                                         │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐              │
│  │ Worker 1 │  │ Worker 2 │  │ Worker N │  (N = CPUs)  │
│  └──────────┘  └──────────┘  └──────────┘              │
│                                                         │
│  - Parse requests in parallel                           │
│  - Execute route handlers                               │
│  - Build responses                                      │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

### Key Benefits

| Feature | Benefit |
|---------|---------|
| **Non-blocking I/O** | Single thread handles 10,000+ connections |
| **Edge-triggered polling** | Minimal syscall overhead |
| **Worker thread pool** | CPU cores utilized for request processing |
| **Lock-free queues** | Minimal contention between threads |

## Performance Tips

1. **Enable TCP_NODELAY** for low-latency responses
2. **Use SO_REUSEPORT** on Linux for multi-process scaling
3. **Set appropriate timeouts** to prevent connection leaks
4. **Use JSON for APIs** - Bedrock's JSON handling is optimized
5. **Keep middleware minimal** - Each middleware adds latency
6. **Configure worker threads** - Set `workerThreads` to match your CPU cores

## Example: REST API

```quartz
import bedrock;
import system.json as json;

let app = bedrock.create({"port": 8080});

// In-memory store
let users = {};
let nextId = 1;

// List users
bedrock.get(app, "/api/users", (req) => {
    let userList = [];
    // Convert dict to list...
    return bedrock.response.json(json.stringify(userList));
});

// Get user
bedrock.get(app, "/api/users/:id", (req) => {
    let id = req["params"]["id"];
    if (users[id] == nil) {
        return bedrock.response.notFound("User not found");
    }
    return bedrock.response.json(json.stringify(users[id]));
});

// Create user
bedrock.post(app, "/api/users", (req) => {
    let data = json.parse(req["body"]);
    let id = nextId;
    nextId = nextId + 1;
    data["id"] = id;
    users[id] = data;
    return bedrock.response.created(json.stringify(data), "/api/users/" + id);
});

// Update user
bedrock.put(app, "/api/users/:id", (req) => {
    let id = req["params"]["id"];
    if (users[id] == nil) {
        return bedrock.response.notFound("User not found");
    }
    let data = json.parse(req["body"]);
    data["id"] = id;
    users[id] = data;
    return bedrock.response.json(json.stringify(data));
});

// Delete user
bedrock.delete(app, "/api/users/:id", (req) => {
    let id = req["params"]["id"];
    if (users[id] == nil) {
        return bedrock.response.notFound("User not found");
    }
    users[id] = nil;
    return bedrock.response.noContent();
});

bedrock.start(app);
println("REST API running on http://localhost:8080");
```

## Why Bedrock?

| Feature | Bedrock | Spring Boot | Express.js |
|---------|---------|-------------|------------|
| Magic/Annotations | ❌ None | ✅ Heavy | 🔶 Some |
| Configuration | Explicit | Convention | Mixed |
| Learning Curve | Low | High | Medium |
| Startup Time | ~1ms | ~5-10s | ~100ms |
| Memory Usage | ~10MB | ~200MB+ | ~50MB |
| Dependencies | 0 | 100+ | 20+ |

Bedrock gives you:
- **Full control** over your application behavior
- **Predictable performance** with no hidden overhead
- **Simple debugging** - what you write is what runs
- **Easy testing** - no framework magic to work around

---

*Bedrock: Build on solid ground.*
