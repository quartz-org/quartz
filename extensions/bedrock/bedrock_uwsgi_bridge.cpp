// ============================================================================
// Bedrock - uWSGI Bridge for nginx Backend Mode
// Allows Bedrock apps to run behind nginx via uWSGI protocol
// ============================================================================

#include "bedrock_types.h"
#include "function_registry.h"
#include "runtime.h"
#include <sstream>

// This file provides integration between Bedrock and system.net.uwsgi
// allowing Bedrock apps to be served via nginx

namespace bedrock {

// Convert uWSGI request format to Bedrock request
Request convertUwsgiRequest(
    const std::string& method,
    const std::string& uri,
    const std::string& path,
    const std::string& queryString,
    const std::string& protocol,
    const std::string& host,
    const std::string& remoteAddr,
    int remotePort,
    const std::string& contentType,
    size_t contentLength,
    const std::string& body,
    const std::unordered_map<std::string, std::string>& headers
) {
    Request req;
    
    req.method = stringToMethod(method);
    req.methodStr = method;
    req.rawPath = uri;
    req.path = normalizePath(path);
    req.queryString = queryString;
    req.protocol = protocol;
    req.host = host;
    req.remoteAddr = remoteAddr;
    req.remotePort = remotePort;
    req.contentType = contentType;
    req.contentLength = contentLength;
    req.body = body;
    req.headers = headers;
    
    // Parse query string
    if (!queryString.empty()) {
        req.query = parseQueryString(queryString);
    }
    
    // Parse cookies if present
    auto cookieIt = headers.find("cookie");
    if (cookieIt != headers.end()) {
        req.cookies = parseCookies(cookieIt->second);
    }
    
    return req;
}

// Convert Bedrock response to uWSGI response format
std::unordered_map<std::string, std::string> convertToUwsgiResponse(const Response& resp) {
    std::unordered_map<std::string, std::string> result;
    
    result["status"] = std::to_string(resp.statusCode);
    result["statusText"] = resp.statusText;
    result["body"] = resp.body;
    
    // Build headers string
    std::ostringstream headersStr;
    for (const auto& [name, value] : resp.headers) {
        headersStr << name << ": " << value << "\r\n";
    }
    result["headers"] = headersStr.str();
    
    return result;
}

} // namespace bedrock

// ============================================================================
// uWSGI Bridge Function Registration
// ============================================================================

void register_bedrock_uwsgi_bridge(FunctionRegistry& reg);

void register_bedrock_uwsgi_bridge(FunctionRegistry& reg) {
    using namespace bedrock;
    
    // ------------------------------------------------------------------------
    // bedrock.handleUwsgiRequest(app: app, request: dict) -> dict
    // Process a uWSGI request through the Bedrock application
    // This allows using Bedrock routing/middleware with uWSGI transport
    // ------------------------------------------------------------------------
    reg.registerFunction("bedrock.handleUwsgiRequest", [](const std::vector<Value>& args) -> Value {
        if (args.size() < 2) {
            throw LanguageException("TypeError", 
                "bedrock.handleUwsgiRequest: requires (app, request)");
        }
        
        // For now, return a placeholder
        // Full implementation would:
        // 1. Extract app reference
        // 2. Convert request dict to Request object
        // 3. Call app->handleRequest()
        // 4. Convert Response back to dict
        
        Runtime* runtime = global_runtime_ptr;
        if (!runtime) {
            throw LanguageException("BedrockError", "Runtime not available");
        }
        
        std::unordered_map<std::string, Value> response;
        response["status"] = Value(200);
        response["body"] = Value(std::string("Bedrock uWSGI Bridge"));
        
        std::unordered_map<std::string, Value> headers;
        headers["Content-Type"] = Value(std::string("text/plain"));
        response["headers"] = runtime->makeDict(std::move(headers));
        
        return runtime->makeDict(std::move(response));
    });
}
