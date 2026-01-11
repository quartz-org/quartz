# Bedrock Samples

Sample applications demonstrating the Bedrock web framework for Quartz.

## Samples

### hello_world.qz
The simplest Bedrock application. Shows basic route registration and response building.

```bash
quartz samples/bedrock/hello_world.qz
```

### rest_api.qz
A complete CRUD REST API with proper HTTP semantics, error handling, and logging middleware.

```bash
quartz samples/bedrock/rest_api.qz
```

### middleware.qz
Demonstrates middleware composition with authentication, logging, and path-scoped middleware.

```bash
quartz samples/bedrock/middleware.qz
```

### full_demo.qz
Comprehensive example showcasing all Bedrock features including multiple APIs, query parameters, custom error handlers, and server statistics.

```bash
quartz samples/bedrock/full_demo.qz
```

### nginx_uwsgi.qz
Production deployment example using nginx + uWSGI protocol for high-performance serving.

```bash
quartz samples/bedrock/nginx_uwsgi.qz
```

## Running the Samples

1. Build Quartz with the Bedrock extension:
```bash
./build.sh
```

2. Run any sample:
```bash
./build/quartz samples/bedrock/hello_world.qz
```

3. Test with curl:
```bash
curl http://localhost:8080/
curl http://localhost:8080/health
curl http://localhost:8080/api/products
```

## Performance Testing

```bash
# Simple benchmark
ab -n 10000 -c 100 http://localhost:8080/

# With wrk
wrk -t4 -c100 -d30s http://localhost:8080/api/products
```
