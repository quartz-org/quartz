# Quartz High-Performance HTTP Server

A uWSGI-like async HTTP server framework for Quartz, providing high-performance non-blocking I/O using platform-native event loops (epoll on Linux, kqueue on macOS, IOCP on Windows).

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                         nginx (reverse proxy)                    │
│                    Load balancing, SSL termination              │
└─────────────────────────┬───────────────────────────────────────┘
                          │
         ┌────────────────┼────────────────┐
         │                │                │
         ▼                ▼                ▼
┌─────────────┐  ┌─────────────┐  ┌─────────────┐
│ Quartz:8080 │  │ Quartz:8081 │  │ Quartz:8082 │
│             │  │             │  │             │
│ Event Loop  │  │ Event Loop  │  │ Event Loop  │
│  (kqueue/   │  │  (kqueue/   │  │  (kqueue/   │
│   epoll)    │  │   epoll)    │  │   epoll)    │
└─────────────┘  └─────────────┘  └─────────────┘
```

## Components

### 1. `system.net.socket` Extension
Cross-platform TCP/UDP socket library with:
- Non-blocking I/O
- Socket options (SO_REUSEADDR, TCP_NODELAY, etc.)
- High-performance buffer management
- Connection statistics

### 2. `system.async.eventloop` Extension
Platform-native event loop using:
- **Linux**: epoll with edge-triggered events
- **macOS**: kqueue
- **Windows**: IOCP (I/O Completion Ports)

Features:
- Timer management (setTimeout, setInterval)
- Immediate and next-tick callbacks
- Idle callbacks for background work
- Statistics and monitoring

## Quick Start

### 1. Build Quartz with new extensions
```bash
cd /path/to/quartz
bash rebuild_core.sh
```

### 2. Run the HTTP server
```bash
./build/quartz samples/demos/http_server.qz
```

### 3. Test it
```bash
curl http://localhost:8080/api/health
curl http://localhost:8080/api/users
curl http://localhost:8080/api/benchmark
```

## Benchmarking

### Using wrk (recommended)
```bash
# Install wrk: brew install wrk (macOS) or apt install wrk (Linux)

# Health check (minimal overhead)
wrk -t4 -c100 -d30s http://localhost:8080/api/health

# JSON response
wrk -t4 -c100 -d30s http://localhost:8080/api/users

# CPU-bound
wrk -t4 -c100 -d30s http://localhost:8080/api/benchmark
```

### Using the benchmark script
```bash
chmod +x samples/demos/benchmark.sh
./samples/demos/benchmark.sh
```

## Production Deployment with nginx

### 1. Start multiple Quartz instances
```bash
chmod +x samples/demos/start_cluster.sh
NUM_INSTANCES=4 ./samples/demos/start_cluster.sh start
```

### 2. Configure nginx
```bash
# Copy nginx config
sudo cp samples/demos/nginx/quartz_server.conf /etc/nginx/sites-available/
sudo ln -s /etc/nginx/sites-available/quartz_server.conf /etc/nginx/sites-enabled/
sudo nginx -t && sudo systemctl reload nginx
```

### 3. Test through nginx
```bash
curl http://localhost/api/health
```

## API Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | Welcome HTML page |
| `/api/health` | GET | Health check (minimal response) |
| `/api/users` | GET | List all users |
| `/api/users` | POST | Create new user |
| `/api/benchmark` | GET | Light CPU benchmark (10k iterations) |
| `/api/benchmark/heavy` | GET | Heavy CPU benchmark (100k iterations) |
| `/api/stats` | GET | Server statistics |
| `/api/echo` | GET | Echo request info |

## Performance Tuning

### Kernel Parameters (Linux)
```bash
# Increase connection backlog
echo 65535 > /proc/sys/net/core/somaxconn

# Increase file descriptor limit
ulimit -n 65535

# Enable TCP reuse
echo 1 > /proc/sys/net/ipv4/tcp_tw_reuse
```

### nginx Tuning
```nginx
worker_processes auto;
worker_connections 10000;
multi_accept on;
use epoll;  # Linux
# use kqueue;  # macOS
```

## Extension API Reference

### system.net.socket

```quartz
// Create sockets
let server = socket.tcp();
let client = socket.udp();

// High-level helpers
let server = socket.createServer("0.0.0.0", 8080);
let client = socket.createClient("example.com", 80);

// Operations
socket.bind(sock, "0.0.0.0", 8080);
socket.listen(sock, 1024);
let conn = socket.accept(sock);
socket.connect(sock, "host", 80);

// I/O
socket.send(sock, "data");
let data = socket.recv(sock, 4096);
socket.sendAll(sock, "all data");

// Options
socket.setNonBlocking(sock, true);
socket.setNoDelay(sock, true);
socket.setReuseAddr(sock, true);

// Info
let info = socket.info(sock);
socket.close(sock);
```

### system.async.eventloop

```quartz
// Lifecycle
eventloop.init();
eventloop.run();
eventloop.runOnce(100);  // timeout ms
eventloop.runFor(5000);  // duration ms
eventloop.stop();

// I/O Watchers
eventloop.watchRead(socketId, fd, fn(event) { ... });
eventloop.watchWrite(socketId, fd, fn(event) { ... });
eventloop.watchAccept(socketId, fd, fn(event) { ... });
eventloop.unwatch(socketId);

// Timers
let id = eventloop.setTimeout(1000, fn(event) { ... });
let id = eventloop.setInterval(100, fn(event) { ... });
eventloop.clearTimeout(id);
eventloop.clearInterval(id);

// Scheduling
eventloop.nextTick(fn(event) { ... });
eventloop.setImmediate(fn(event) { ... });
eventloop.onIdle(fn(event) { ... });

// Stats
let stats = eventloop.stats();
eventloop.resetStats();
```

## Comparison with Other Solutions

| Feature | Quartz HTTP | uWSGI | Node.js | Go net/http |
|---------|-------------|-------|---------|-------------|
| Event Loop | Native (epoll/kqueue) | Native | libuv | goroutines |
| Language | Quartz | Python/C | JavaScript | Go |
| Concurrency Model | Event-driven | Process/Thread | Event-driven | Goroutines |
| Memory per connection | Low | Medium | Low | Low |
| Startup Time | Fast | Medium | Medium | Fast |

## Future Improvements

- [ ] HTTP/2 support
- [ ] WebSocket support
- [ ] TLS/SSL native support
- [x] Request routing with path parameters (via Bedrock)
- [x] Middleware system (via Bedrock)
- [x] Request body parsing (JSON, form data) (via Bedrock)
- [ ] Response streaming
- [x] Worker thread pool for CPU-bound tasks (via Bedrock async server)

## Bedrock Integration

The Bedrock framework now uses a **high-performance async architecture** by default:
- Event-driven I/O with epoll (Linux), kqueue (macOS), or select (Windows)
- Worker thread pool for parallel request processing
- Non-blocking connections supporting 10,000+ concurrent clients
- Automatic fallback to blocking mode if needed
