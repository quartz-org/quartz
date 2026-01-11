// ============================================================================
// Bedrock - Main Extension Entry Point
// A minimalist, high-performance web framework for Quartz
// ============================================================================
//
// Philosophy: No magic. No bloat. Just you and your code.
//
// Features:
// - High-performance HTTP server (standalone mode)
// - nginx integration via uWSGI protocol
// - Flexible routing with path parameters
// - Composable middleware pipeline
// - Explicit response building
// - Zero hidden behavior
//
// ============================================================================

#include "function_registry.h"

// Forward declarations for function registration
extern void register_bedrock_app_functions(FunctionRegistry& reg);
extern void register_bedrock_route_functions(FunctionRegistry& reg);
extern void register_bedrock_middleware_functions(FunctionRegistry& reg);
extern void register_bedrock_response_functions(FunctionRegistry& reg);
extern void register_bedrock_uwsgi_bridge(FunctionRegistry& reg);

extern "C" __attribute__((visibility("default"))) void init_extension(FunctionRegistry& reg) {
    // Core application lifecycle
    register_bedrock_app_functions(reg);
    
    // Route registration
    register_bedrock_route_functions(reg);
    
    // Middleware support
    register_bedrock_middleware_functions(reg);
    
    // Response helpers
    register_bedrock_response_functions(reg);
    
    // uWSGI bridge for nginx integration
    register_bedrock_uwsgi_bridge(reg);
}
