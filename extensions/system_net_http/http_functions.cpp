// ============================================================================
// system.net.http Extension - HTTP Functions
// Provides HTTP client functionality for Quartz
// ============================================================================

#include "function_registry.h"
#include "runtime.h"
#include "http_types.h"
#include <algorithm>
#include <sstream>

static inline Runtime* rt() {
    return global_runtime_ptr;
}

// ============================================================================
// Helper Functions
// ============================================================================

static inline bool isString(const Value& v) {
    return std::holds_alternative<std::string>(v);
}

static inline bool isInt(const Value& v) {
    return std::holds_alternative<int>(v);
}

static inline bool isBool(const Value& v) {
    return std::holds_alternative<bool>(v);
}

static inline bool isDictRef(const Value& v) {
    return std::holds_alternative<DictRef>(v);
}

// Convert a Quartz dict to Headers
static http::Headers dictToHeaders(const Value& v, Runtime* runtime) {
    http::Headers headers;
    if (!isDictRef(v) || !runtime) return headers;
    
    auto* dict = runtime->getDict(std::get<DictRef>(v));
    if (!dict) return headers;
    
    for (const auto& [key, value] : *dict) {
        if (isString(value)) {
            headers.set(key, std::get<std::string>(value));
        }
    }
    return headers;
}

// Convert Headers to a Quartz dict
static Value headersToDictValue(const http::Headers& headers, Runtime* runtime) {
    if (!runtime) return Value(0);
    
    std::unordered_map<std::string, Value> dict;
    for (const auto& [name, value] : headers.entries()) {
        dict[name] = Value(value);
    }
    return runtime->makeDict(std::move(dict));
}

// Build an HttpRequest object as a Quartz dict
static Value buildHttpRequestDict(const http::HttpRequest& req, Runtime* runtime) {
    if (!runtime) return Value(0);
    
    std::unordered_map<std::string, Value> dict;
    dict["method"] = Value(http::methodToString(req.method));
    dict["url"] = Value(req.url.toString());
    dict["version"] = Value(http::versionToString(req.version));
    dict["headers"] = headersToDictValue(req.headers, runtime);
    dict["body"] = Value(req.body);
    dict["timeoutMs"] = Value(req.timeoutMs);
    dict["followRedirects"] = Value(req.followRedirects);
    dict["maxRedirects"] = Value(req.maxRedirects);
    
    return runtime->makeDict(std::move(dict));
}

// Build an HttpResponse object as a Quartz dict
static Value buildHttpResponseDict(const http::HttpResponse& resp, Runtime* runtime) {
    if (!runtime) return Value(0);
    
    std::unordered_map<std::string, Value> dict;
    dict["version"] = Value(http::versionToString(resp.version));
    dict["statusCode"] = Value(resp.statusCode);
    dict["statusText"] = Value(resp.reasonPhrase);
    dict["headers"] = headersToDictValue(resp.headers, runtime);
    dict["body"] = Value(resp.body);
    dict["elapsedTimeMs"] = Value(resp.elapsedTimeMs);
    dict["finalUrl"] = Value(resp.finalUrl);
    dict["redirectCount"] = Value(resp.redirectCount);
    
    // Status helpers as boolean fields
    dict["ok"] = Value(resp.isSuccessful());
    dict["isInformational"] = Value(resp.isInformational());
    dict["isSuccessful"] = Value(resp.isSuccessful());
    dict["isRedirection"] = Value(resp.isRedirection());
    dict["isClientError"] = Value(resp.isClientError());
    dict["isServerError"] = Value(resp.isServerError());
    dict["isError"] = Value(resp.isError());
    
    return runtime->makeDict(std::move(dict));
}

// Build a URL object as a Quartz dict
static Value buildUrlDict(const http::URL& url, Runtime* runtime) {
    if (!runtime) return Value(0);
    
    std::unordered_map<std::string, Value> dict;
    dict["scheme"] = Value(url.scheme);
    dict["host"] = Value(url.host);
    dict["port"] = Value(url.port);
    dict["path"] = Value(url.path);
    dict["query"] = Value(url.query);
    dict["fragment"] = Value(url.fragment);
    dict["username"] = Value(url.username);
    dict["password"] = Value(url.password);
    dict["href"] = Value(url.toString());
    
    return runtime->makeDict(std::move(dict));
}

// Build a Cookie object as a Quartz dict
static Value buildCookieDict(const http::Cookie& cookie, Runtime* runtime) {
    if (!runtime) return Value(0);
    
    std::unordered_map<std::string, Value> dict;
    dict["name"] = Value(cookie.name);
    dict["value"] = Value(cookie.value);
    dict["domain"] = Value(cookie.domain);
    dict["path"] = Value(cookie.path);
    dict["expires"] = Value(cookie.expires);
    dict["maxAge"] = Value(cookie.maxAge);
    dict["secure"] = Value(cookie.secure);
    dict["httpOnly"] = Value(cookie.httpOnly);
    dict["sameSite"] = Value(cookie.sameSite);
    
    return runtime->makeDict(std::move(dict));
}

// ============================================================================
// HTTP Request Builder Functions
// ============================================================================

void register_http_request_functions(FunctionRegistry& reg) {
    
    // ------------------------------------------------------------------------
    // system.net.http.request.create(method: string, url: string) -> HttpRequest
    // Creates a new HTTP request object
    // ------------------------------------------------------------------------
    reg.registerFunction("system.net.http.request.create", [](const std::vector<Value>& args) -> Value {
        if (!rt() || args.size() < 2) {
            throw LanguageException("ArgumentError", 
                "system.net.http.request.create requires (method, url) arguments");
        }
        if (!isString(args[0]) || !isString(args[1])) {
            throw LanguageException("TypeError", 
                "system.net.http.request.create: method and url must be strings");
        }
        
        http::HttpRequest req;
        req.method = http::stringToMethod(std::get<std::string>(args[0]));
        req.url = http::URL::parse(std::get<std::string>(args[1]));
        
        // Set default headers
        req.headers.set(http::header::UserAgent, "Quartz/1.0");
        req.headers.set(http::header::Accept, "*/*");
        
        return buildHttpRequestDict(req, rt());
    });
    
    // ------------------------------------------------------------------------
    // system.net.http.request.get(url: string) -> HttpRequest
    // Shorthand to create a GET request
    // ------------------------------------------------------------------------
    reg.registerFunction("system.net.http.request.get", [](const std::vector<Value>& args) -> Value {
        if (!rt() || args.empty() || !isString(args[0])) {
            throw LanguageException("ArgumentError", 
                "system.net.http.request.get requires a URL string");
        }
        
        http::HttpRequest req;
        req.method = http::Method::GET;
        req.url = http::URL::parse(std::get<std::string>(args[0]));
        req.headers.set(http::header::UserAgent, "Quartz/1.0");
        req.headers.set(http::header::Accept, "*/*");
        
        return buildHttpRequestDict(req, rt());
    });
    
    // ------------------------------------------------------------------------
    // system.net.http.request.post(url: string, body?: string) -> HttpRequest
    // Shorthand to create a POST request
    // ------------------------------------------------------------------------
    reg.registerFunction("system.net.http.request.post", [](const std::vector<Value>& args) -> Value {
        if (!rt() || args.empty() || !isString(args[0])) {
            throw LanguageException("ArgumentError", 
                "system.net.http.request.post requires a URL string");
        }
        
        http::HttpRequest req;
        req.method = http::Method::POST;
        req.url = http::URL::parse(std::get<std::string>(args[0]));
        req.headers.set(http::header::UserAgent, "Quartz/1.0");
        req.headers.set(http::header::Accept, "*/*");
        
        if (args.size() > 1 && isString(args[1])) {
            req.body = std::get<std::string>(args[1]);
            req.headers.set(http::header::ContentLength, std::to_string(req.body.size()));
        }
        
        return buildHttpRequestDict(req, rt());
    });
    
    // ------------------------------------------------------------------------
    // system.net.http.request.put(url: string, body?: string) -> HttpRequest
    // ------------------------------------------------------------------------
    reg.registerFunction("system.net.http.request.put", [](const std::vector<Value>& args) -> Value {
        if (!rt() || args.empty() || !isString(args[0])) {
            throw LanguageException("ArgumentError", 
                "system.net.http.request.put requires a URL string");
        }
        
        http::HttpRequest req;
        req.method = http::Method::PUT;
        req.url = http::URL::parse(std::get<std::string>(args[0]));
        req.headers.set(http::header::UserAgent, "Quartz/1.0");
        
        if (args.size() > 1 && isString(args[1])) {
            req.body = std::get<std::string>(args[1]);
            req.headers.set(http::header::ContentLength, std::to_string(req.body.size()));
        }
        
        return buildHttpRequestDict(req, rt());
    });
    
    // ------------------------------------------------------------------------
    // system.net.http.request.delete(url: string) -> HttpRequest
    // ------------------------------------------------------------------------
    reg.registerFunction("system.net.http.request.delete", [](const std::vector<Value>& args) -> Value {
        if (!rt() || args.empty() || !isString(args[0])) {
            throw LanguageException("ArgumentError", 
                "system.net.http.request.delete requires a URL string");
        }
        
        http::HttpRequest req;
        req.method = http::Method::DELETE_;
        req.url = http::URL::parse(std::get<std::string>(args[0]));
        req.headers.set(http::header::UserAgent, "Quartz/1.0");
        
        return buildHttpRequestDict(req, rt());
    });
    
    // ------------------------------------------------------------------------
    // system.net.http.request.patch(url: string, body?: string) -> HttpRequest
    // ------------------------------------------------------------------------
    reg.registerFunction("system.net.http.request.patch", [](const std::vector<Value>& args) -> Value {
        if (!rt() || args.empty() || !isString(args[0])) {
            throw LanguageException("ArgumentError", 
                "system.net.http.request.patch requires a URL string");
        }
        
        http::HttpRequest req;
        req.method = http::Method::PATCH;
        req.url = http::URL::parse(std::get<std::string>(args[0]));
        req.headers.set(http::header::UserAgent, "Quartz/1.0");
        
        if (args.size() > 1 && isString(args[1])) {
            req.body = std::get<std::string>(args[1]);
            req.headers.set(http::header::ContentLength, std::to_string(req.body.size()));
        }
        
        return buildHttpRequestDict(req, rt());
    });
    
    // ------------------------------------------------------------------------
    // system.net.http.request.head(url: string) -> HttpRequest
    // ------------------------------------------------------------------------
    reg.registerFunction("system.net.http.request.head", [](const std::vector<Value>& args) -> Value {
        if (!rt() || args.empty() || !isString(args[0])) {
            throw LanguageException("ArgumentError", 
                "system.net.http.request.head requires a URL string");
        }
        
        http::HttpRequest req;
        req.method = http::Method::HEAD;
        req.url = http::URL::parse(std::get<std::string>(args[0]));
        req.headers.set(http::header::UserAgent, "Quartz/1.0");
        
        return buildHttpRequestDict(req, rt());
    });
    
    // ------------------------------------------------------------------------
    // system.net.http.request.options(url: string) -> HttpRequest
    // ------------------------------------------------------------------------
    reg.registerFunction("system.net.http.request.options", [](const std::vector<Value>& args) -> Value {
        if (!rt() || args.empty() || !isString(args[0])) {
            throw LanguageException("ArgumentError", 
                "system.net.http.request.options requires a URL string");
        }
        
        http::HttpRequest req;
        req.method = http::Method::OPTIONS;
        req.url = http::URL::parse(std::get<std::string>(args[0]));
        req.headers.set(http::header::UserAgent, "Quartz/1.0");
        
        return buildHttpRequestDict(req, rt());
    });
    
    // ------------------------------------------------------------------------
    // system.net.http.request.serialize(request: HttpRequest) -> string
    // Serializes the request to HTTP wire format
    // ------------------------------------------------------------------------
    reg.registerFunction("system.net.http.request.serialize", [](const std::vector<Value>& args) -> Value {
        if (!rt() || args.empty() || !isDictRef(args[0])) {
            throw LanguageException("ArgumentError", 
                "system.net.http.request.serialize requires an HttpRequest dict");
        }
        
        auto* dict = rt()->getDict(std::get<DictRef>(args[0]));
        if (!dict) return Value(std::string(""));
        
        http::HttpRequest req;
        
        // Extract fields from dict
        auto it = dict->find("method");
        if (it != dict->end() && isString(it->second)) {
            req.method = http::stringToMethod(std::get<std::string>(it->second));
        }
        
        it = dict->find("url");
        if (it != dict->end() && isString(it->second)) {
            req.url = http::URL::parse(std::get<std::string>(it->second));
        }
        
        it = dict->find("version");
        if (it != dict->end() && isString(it->second)) {
            req.version = http::stringToVersion(std::get<std::string>(it->second));
        }
        
        it = dict->find("body");
        if (it != dict->end() && isString(it->second)) {
            req.body = std::get<std::string>(it->second);
        }
        
        it = dict->find("headers");
        if (it != dict->end() && isDictRef(it->second)) {
            req.headers = dictToHeaders(it->second, rt());
        }
        
        return Value(req.serialize());
    });
}

// ============================================================================
// HTTP Response Functions
// ============================================================================

void register_http_response_functions(FunctionRegistry& reg) {
    
    // ------------------------------------------------------------------------
    // system.net.http.response.create(statusCode: int, body?: string) -> HttpResponse
    // Creates a new HTTP response object (useful for testing/mocking)
    // ------------------------------------------------------------------------
    reg.registerFunction("system.net.http.response.create", [](const std::vector<Value>& args) -> Value {
        if (!rt() || args.empty() || !isInt(args[0])) {
            throw LanguageException("ArgumentError", 
                "system.net.http.response.create requires a statusCode");
        }
        
        http::HttpResponse resp;
        resp.statusCode = std::get<int>(args[0]);
        resp.reasonPhrase = http::statusCodeToReason(static_cast<http::StatusCode>(resp.statusCode));
        
        if (args.size() > 1 && isString(args[1])) {
            resp.body = std::get<std::string>(args[1]);
        }
        
        return buildHttpResponseDict(resp, rt());
    });
    
    // ------------------------------------------------------------------------
    // system.net.http.response.isOk(response: HttpResponse) -> bool
    // ------------------------------------------------------------------------
    reg.registerFunction("system.net.http.response.isOk", [](const std::vector<Value>& args) -> Value {
        if (!rt() || args.empty() || !isDictRef(args[0])) {
            return Value(false);
        }
        
        auto* dict = rt()->getDict(std::get<DictRef>(args[0]));
        if (!dict) return Value(false);
        
        auto it = dict->find("statusCode");
        if (it == dict->end() || !isInt(it->second)) return Value(false);
        
        int code = std::get<int>(it->second);
        return Value(http::isSuccessful(code));
    });
    
    // ------------------------------------------------------------------------
    // system.net.http.response.getHeader(response: HttpResponse, name: string) -> string?
    // ------------------------------------------------------------------------
    reg.registerFunction("system.net.http.response.getHeader", [](const std::vector<Value>& args) -> Value {
        if (!rt() || args.size() < 2) {
            return Value(std::string(""));
        }
        if (!isDictRef(args[0]) || !isString(args[1])) {
            return Value(std::string(""));
        }
        
        auto* dict = rt()->getDict(std::get<DictRef>(args[0]));
        if (!dict) return Value(std::string(""));
        
        auto it = dict->find("headers");
        if (it == dict->end() || !isDictRef(it->second)) {
            return Value(std::string(""));
        }
        
        auto* headers = rt()->getDict(std::get<DictRef>(it->second));
        if (!headers) return Value(std::string(""));
        
        const std::string& name = std::get<std::string>(args[1]);
        
        // Case-insensitive header lookup
        std::string lowerName = name;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
        
        for (const auto& [key, value] : *headers) {
            std::string lowerKey = key;
            std::transform(lowerKey.begin(), lowerKey.end(), lowerKey.begin(), ::tolower);
            if (lowerKey == lowerName && isString(value)) {
                return value;
            }
        }
        
        return Value(std::string(""));
    });
}

// ============================================================================
// HTTP Status Code Functions
// ============================================================================

void register_http_status_functions(FunctionRegistry& reg) {
    
    // ------------------------------------------------------------------------
    // system.net.http.status.text(code: int) -> string
    // Get the reason phrase for a status code
    // ------------------------------------------------------------------------
    reg.registerFunction("system.net.http.status.text", [](const std::vector<Value>& args) -> Value {
        if (args.empty() || !isInt(args[0])) {
            return Value(std::string("Unknown"));
        }
        return Value(http::statusCodeToReason(static_cast<http::StatusCode>(std::get<int>(args[0]))));
    });
    
    // ------------------------------------------------------------------------
    // system.net.http.status.isInformational(code: int) -> bool
    // ------------------------------------------------------------------------
    reg.registerFunction("system.net.http.status.isInformational", [](const std::vector<Value>& args) -> Value {
        if (args.empty() || !isInt(args[0])) return Value(false);
        return Value(http::isInformational(std::get<int>(args[0])));
    });
    
    // ------------------------------------------------------------------------
    // system.net.http.status.isSuccessful(code: int) -> bool
    // ------------------------------------------------------------------------
    reg.registerFunction("system.net.http.status.isSuccessful", [](const std::vector<Value>& args) -> Value {
        if (args.empty() || !isInt(args[0])) return Value(false);
        return Value(http::isSuccessful(std::get<int>(args[0])));
    });
    
    // ------------------------------------------------------------------------
    // system.net.http.status.isRedirection(code: int) -> bool
    // ------------------------------------------------------------------------
    reg.registerFunction("system.net.http.status.isRedirection", [](const std::vector<Value>& args) -> Value {
        if (args.empty() || !isInt(args[0])) return Value(false);
        return Value(http::isRedirection(std::get<int>(args[0])));
    });
    
    // ------------------------------------------------------------------------
    // system.net.http.status.isClientError(code: int) -> bool
    // ------------------------------------------------------------------------
    reg.registerFunction("system.net.http.status.isClientError", [](const std::vector<Value>& args) -> Value {
        if (args.empty() || !isInt(args[0])) return Value(false);
        return Value(http::isClientError(std::get<int>(args[0])));
    });
    
    // ------------------------------------------------------------------------
    // system.net.http.status.isServerError(code: int) -> bool
    // ------------------------------------------------------------------------
    reg.registerFunction("system.net.http.status.isServerError", [](const std::vector<Value>& args) -> Value {
        if (args.empty() || !isInt(args[0])) return Value(false);
        return Value(http::isServerError(std::get<int>(args[0])));
    });
    
    // ------------------------------------------------------------------------
    // system.net.http.status.isError(code: int) -> bool
    // ------------------------------------------------------------------------
    reg.registerFunction("system.net.http.status.isError", [](const std::vector<Value>& args) -> Value {
        if (args.empty() || !isInt(args[0])) return Value(false);
        int code = std::get<int>(args[0]);
        return Value(http::isClientError(code) || http::isServerError(code));
    });
}

// ============================================================================
// URL Functions
// ============================================================================

void register_http_url_functions(FunctionRegistry& reg) {
    
    // ------------------------------------------------------------------------
    // system.net.http.url.parse(urlString: string) -> URL
    // Parse a URL string into components
    // ------------------------------------------------------------------------
    reg.registerFunction("system.net.http.url.parse", [](const std::vector<Value>& args) -> Value {
        if (!rt() || args.empty() || !isString(args[0])) {
            throw LanguageException("ArgumentError", 
                "system.net.http.url.parse requires a URL string");
        }
        
        http::URL url = http::URL::parse(std::get<std::string>(args[0]));
        return buildUrlDict(url, rt());
    });
    
    // ------------------------------------------------------------------------
    // system.net.http.url.build(components: dict) -> string
    // Build a URL string from components
    // ------------------------------------------------------------------------
    reg.registerFunction("system.net.http.url.build", [](const std::vector<Value>& args) -> Value {
        if (!rt() || args.empty() || !isDictRef(args[0])) {
            throw LanguageException("ArgumentError", 
                "system.net.http.url.build requires a dict of URL components");
        }
        
        auto* dict = rt()->getDict(std::get<DictRef>(args[0]));
        if (!dict) return Value(std::string(""));
        
        http::URL url;
        
        auto it = dict->find("scheme");
        if (it != dict->end() && isString(it->second)) {
            url.scheme = std::get<std::string>(it->second);
        }
        
        it = dict->find("host");
        if (it != dict->end() && isString(it->second)) {
            url.host = std::get<std::string>(it->second);
        }
        
        it = dict->find("port");
        if (it != dict->end() && isInt(it->second)) {
            url.port = std::get<int>(it->second);
        }
        
        it = dict->find("path");
        if (it != dict->end() && isString(it->second)) {
            url.path = std::get<std::string>(it->second);
        }
        
        it = dict->find("query");
        if (it != dict->end() && isString(it->second)) {
            url.query = std::get<std::string>(it->second);
        }
        
        it = dict->find("fragment");
        if (it != dict->end() && isString(it->second)) {
            url.fragment = std::get<std::string>(it->second);
        }
        
        it = dict->find("username");
        if (it != dict->end() && isString(it->second)) {
            url.username = std::get<std::string>(it->second);
        }
        
        it = dict->find("password");
        if (it != dict->end() && isString(it->second)) {
            url.password = std::get<std::string>(it->second);
        }
        
        return Value(url.toString());
    });
    
    // ------------------------------------------------------------------------
    // system.net.http.url.encode(str: string) -> string
    // URL-encode a string
    // ------------------------------------------------------------------------
    reg.registerFunction("system.net.http.url.encode", [](const std::vector<Value>& args) -> Value {
        if (args.empty() || !isString(args[0])) {
            return Value(std::string(""));
        }
        
        const std::string& input = std::get<std::string>(args[0]);
        std::ostringstream oss;
        
        for (unsigned char c : input) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                oss << c;
            } else {
                oss << '%' << std::hex << std::uppercase 
                    << static_cast<int>(c >> 4) << static_cast<int>(c & 0x0F);
            }
        }
        
        return Value(oss.str());
    });
    
    // ------------------------------------------------------------------------
    // system.net.http.url.decode(str: string) -> string
    // URL-decode a string
    // ------------------------------------------------------------------------
    reg.registerFunction("system.net.http.url.decode", [](const std::vector<Value>& args) -> Value {
        if (args.empty() || !isString(args[0])) {
            return Value(std::string(""));
        }
        
        const std::string& input = std::get<std::string>(args[0]);
        std::ostringstream oss;
        
        for (size_t i = 0; i < input.size(); ++i) {
            if (input[i] == '%' && i + 2 < input.size()) {
                std::string hex = input.substr(i + 1, 2);
                try {
                    char c = static_cast<char>(std::stoi(hex, nullptr, 16));
                    oss << c;
                    i += 2;
                } catch (...) {
                    oss << input[i];
                }
            } else if (input[i] == '+') {
                oss << ' ';
            } else {
                oss << input[i];
            }
        }
        
        return Value(oss.str());
    });
}

// ============================================================================
// Headers Functions
// ============================================================================

void register_http_headers_functions(FunctionRegistry& reg) {
    
    // ------------------------------------------------------------------------
    // system.net.http.headers.create() -> Headers (dict)
    // Create a new headers object
    // ------------------------------------------------------------------------
    reg.registerFunction("system.net.http.headers.create", [](const std::vector<Value>& args) -> Value {
        if (!rt()) return Value(0);
        std::unordered_map<std::string, Value> dict;
        return rt()->makeDict(std::move(dict));
    });
    
    // ------------------------------------------------------------------------
    // system.net.http.headers.set(headers: dict, name: string, value: string) -> dict
    // Set a header value
    // ------------------------------------------------------------------------
    reg.registerFunction("system.net.http.headers.set", [](const std::vector<Value>& args) -> Value {
        if (!rt() || args.size() < 3) {
            throw LanguageException("ArgumentError", 
                "system.net.http.headers.set requires (headers, name, value)");
        }
        if (!isDictRef(args[0]) || !isString(args[1]) || !isString(args[2])) {
            throw LanguageException("TypeError", 
                "system.net.http.headers.set: invalid argument types");
        }
        
        auto* dict = rt()->getDict(std::get<DictRef>(args[0]));
        if (!dict) return args[0];
        
        (*dict)[std::get<std::string>(args[1])] = args[2];
        return args[0];
    });
    
    // ------------------------------------------------------------------------
    // system.net.http.headers.get(headers: dict, name: string) -> string?
    // Get a header value (case-insensitive)
    // ------------------------------------------------------------------------
    reg.registerFunction("system.net.http.headers.get", [](const std::vector<Value>& args) -> Value {
        if (!rt() || args.size() < 2) {
            return Value(std::string(""));
        }
        if (!isDictRef(args[0]) || !isString(args[1])) {
            return Value(std::string(""));
        }
        
        auto* dict = rt()->getDict(std::get<DictRef>(args[0]));
        if (!dict) return Value(std::string(""));
        
        const std::string& name = std::get<std::string>(args[1]);
        std::string lowerName = name;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
        
        for (const auto& [key, value] : *dict) {
            std::string lowerKey = key;
            std::transform(lowerKey.begin(), lowerKey.end(), lowerKey.begin(), ::tolower);
            if (lowerKey == lowerName && isString(value)) {
                return value;
            }
        }
        
        return Value(std::string(""));
    });
    
    // ------------------------------------------------------------------------
    // system.net.http.headers.has(headers: dict, name: string) -> bool
    // Check if a header exists (case-insensitive)
    // ------------------------------------------------------------------------
    reg.registerFunction("system.net.http.headers.has", [](const std::vector<Value>& args) -> Value {
        if (!rt() || args.size() < 2) {
            return Value(false);
        }
        if (!isDictRef(args[0]) || !isString(args[1])) {
            return Value(false);
        }
        
        auto* dict = rt()->getDict(std::get<DictRef>(args[0]));
        if (!dict) return Value(false);
        
        const std::string& name = std::get<std::string>(args[1]);
        std::string lowerName = name;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
        
        for (const auto& [key, value] : *dict) {
            std::string lowerKey = key;
            std::transform(lowerKey.begin(), lowerKey.end(), lowerKey.begin(), ::tolower);
            if (lowerKey == lowerName) {
                return Value(true);
            }
        }
        
        return Value(false);
    });
}

// ============================================================================
// Cookie Functions
// ============================================================================

void register_http_cookie_functions(FunctionRegistry& reg) {
    
    // ------------------------------------------------------------------------
    // system.net.http.cookie.parse(setCookieHeader: string) -> Cookie
    // Parse a Set-Cookie header
    // ------------------------------------------------------------------------
    reg.registerFunction("system.net.http.cookie.parse", [](const std::vector<Value>& args) -> Value {
        if (!rt() || args.empty() || !isString(args[0])) {
            throw LanguageException("ArgumentError", 
                "system.net.http.cookie.parse requires a Set-Cookie header string");
        }
        
        http::Cookie cookie = http::Cookie::parse(std::get<std::string>(args[0]));
        return buildCookieDict(cookie, rt());
    });
    
    // ------------------------------------------------------------------------
    // system.net.http.cookie.create(name: string, value: string) -> Cookie
    // Create a new cookie
    // ------------------------------------------------------------------------
    reg.registerFunction("system.net.http.cookie.create", [](const std::vector<Value>& args) -> Value {
        if (!rt() || args.size() < 2) {
            throw LanguageException("ArgumentError", 
                "system.net.http.cookie.create requires (name, value)");
        }
        if (!isString(args[0]) || !isString(args[1])) {
            throw LanguageException("TypeError", 
                "system.net.http.cookie.create: name and value must be strings");
        }
        
        http::Cookie cookie;
        cookie.name = std::get<std::string>(args[0]);
        cookie.value = std::get<std::string>(args[1]);
        
        return buildCookieDict(cookie, rt());
    });
    
    // ------------------------------------------------------------------------
    // system.net.http.cookie.toString(cookie: dict) -> string
    // Serialize a cookie to a Cookie header value
    // ------------------------------------------------------------------------
    reg.registerFunction("system.net.http.cookie.toString", [](const std::vector<Value>& args) -> Value {
        if (!rt() || args.empty() || !isDictRef(args[0])) {
            return Value(std::string(""));
        }
        
        auto* dict = rt()->getDict(std::get<DictRef>(args[0]));
        if (!dict) return Value(std::string(""));
        
        http::Cookie cookie;
        
        auto it = dict->find("name");
        if (it != dict->end() && isString(it->second)) {
            cookie.name = std::get<std::string>(it->second);
        }
        
        it = dict->find("value");
        if (it != dict->end() && isString(it->second)) {
            cookie.value = std::get<std::string>(it->second);
        }
        
        it = dict->find("domain");
        if (it != dict->end() && isString(it->second)) {
            cookie.domain = std::get<std::string>(it->second);
        }
        
        it = dict->find("path");
        if (it != dict->end() && isString(it->second)) {
            cookie.path = std::get<std::string>(it->second);
        }
        
        it = dict->find("expires");
        if (it != dict->end() && isString(it->second)) {
            cookie.expires = std::get<std::string>(it->second);
        }
        
        it = dict->find("maxAge");
        if (it != dict->end() && isInt(it->second)) {
            cookie.maxAge = std::get<int>(it->second);
        }
        
        it = dict->find("secure");
        if (it != dict->end() && isBool(it->second)) {
            cookie.secure = std::get<bool>(it->second);
        }
        
        it = dict->find("httpOnly");
        if (it != dict->end() && isBool(it->second)) {
            cookie.httpOnly = std::get<bool>(it->second);
        }
        
        it = dict->find("sameSite");
        if (it != dict->end() && isString(it->second)) {
            cookie.sameSite = std::get<std::string>(it->second);
        }
        
        return Value(cookie.toString());
    });
}

// ============================================================================
// HTTP Method Enum Functions
// ============================================================================

void register_http_method_functions(FunctionRegistry& reg) {
    
    // ------------------------------------------------------------------------
    // system.net.http.method.GET -> string
    // system.net.http.method.POST -> string, etc.
    // These functions return the method strings for use with request creation
    // ------------------------------------------------------------------------
    reg.registerFunction("system.net.http.method.GET", [](const std::vector<Value>&) -> Value {
        return Value(std::string("GET"));
    });
    
    reg.registerFunction("system.net.http.method.POST", [](const std::vector<Value>&) -> Value {
        return Value(std::string("POST"));
    });
    
    reg.registerFunction("system.net.http.method.PUT", [](const std::vector<Value>&) -> Value {
        return Value(std::string("PUT"));
    });
    
    reg.registerFunction("system.net.http.method.DELETE", [](const std::vector<Value>&) -> Value {
        return Value(std::string("DELETE"));
    });
    
    reg.registerFunction("system.net.http.method.PATCH", [](const std::vector<Value>&) -> Value {
        return Value(std::string("PATCH"));
    });
    
    reg.registerFunction("system.net.http.method.HEAD", [](const std::vector<Value>&) -> Value {
        return Value(std::string("HEAD"));
    });
    
    reg.registerFunction("system.net.http.method.OPTIONS", [](const std::vector<Value>&) -> Value {
        return Value(std::string("OPTIONS"));
    });
    
    reg.registerFunction("system.net.http.method.TRACE", [](const std::vector<Value>&) -> Value {
        return Value(std::string("TRACE"));
    });
    
    reg.registerFunction("system.net.http.method.CONNECT", [](const std::vector<Value>&) -> Value {
        return Value(std::string("CONNECT"));
    });
}

// ============================================================================
// Content Type Constants
// ============================================================================

void register_http_mime_functions(FunctionRegistry& reg) {
    
    reg.registerFunction("system.net.http.mime.JSON", [](const std::vector<Value>&) -> Value {
        return Value(std::string("application/json"));
    });
    
    reg.registerFunction("system.net.http.mime.XML", [](const std::vector<Value>&) -> Value {
        return Value(std::string("application/xml"));
    });
    
    reg.registerFunction("system.net.http.mime.FORM", [](const std::vector<Value>&) -> Value {
        return Value(std::string("application/x-www-form-urlencoded"));
    });
    
    reg.registerFunction("system.net.http.mime.MULTIPART", [](const std::vector<Value>&) -> Value {
        return Value(std::string("multipart/form-data"));
    });
    
    reg.registerFunction("system.net.http.mime.TEXT", [](const std::vector<Value>&) -> Value {
        return Value(std::string("text/plain"));
    });
    
    reg.registerFunction("system.net.http.mime.HTML", [](const std::vector<Value>&) -> Value {
        return Value(std::string("text/html"));
    });
    
    reg.registerFunction("system.net.http.mime.CSS", [](const std::vector<Value>&) -> Value {
        return Value(std::string("text/css"));
    });
    
    reg.registerFunction("system.net.http.mime.JS", [](const std::vector<Value>&) -> Value {
        return Value(std::string("text/javascript"));
    });
    
    reg.registerFunction("system.net.http.mime.PNG", [](const std::vector<Value>&) -> Value {
        return Value(std::string("image/png"));
    });
    
    reg.registerFunction("system.net.http.mime.JPEG", [](const std::vector<Value>&) -> Value {
        return Value(std::string("image/jpeg"));
    });
    
    reg.registerFunction("system.net.http.mime.GIF", [](const std::vector<Value>&) -> Value {
        return Value(std::string("image/gif"));
    });
    
    reg.registerFunction("system.net.http.mime.OCTET_STREAM", [](const std::vector<Value>&) -> Value {
        return Value(std::string("application/octet-stream"));
    });
}

// ============================================================================
// Authentication Helpers
// ============================================================================

void register_http_auth_functions(FunctionRegistry& reg) {
    
    // ------------------------------------------------------------------------
    // system.net.http.auth.basic(username: string, password: string) -> string
    // Generate a Basic Authorization header value
    // ------------------------------------------------------------------------
    reg.registerFunction("system.net.http.auth.basic", [](const std::vector<Value>& args) -> Value {
        if (args.size() < 2 || !isString(args[0]) || !isString(args[1])) {
            throw LanguageException("ArgumentError", 
                "system.net.http.auth.basic requires (username, password)");
        }
        
        http::BasicAuth auth;
        auth.username = std::get<std::string>(args[0]);
        auth.password = std::get<std::string>(args[1]);
        
        return Value(auth.toHeader());
    });
    
    // ------------------------------------------------------------------------
    // system.net.http.auth.bearer(token: string) -> string
    // Generate a Bearer Authorization header value
    // ------------------------------------------------------------------------
    reg.registerFunction("system.net.http.auth.bearer", [](const std::vector<Value>& args) -> Value {
        if (args.empty() || !isString(args[0])) {
            throw LanguageException("ArgumentError", 
                "system.net.http.auth.bearer requires a token string");
        }
        
        http::BearerAuth auth;
        auth.token = std::get<std::string>(args[0]);
        
        return Value(auth.toHeader());
    });
}

// ============================================================================
// Header Name Constants
// ============================================================================

void register_http_header_constants(FunctionRegistry& reg) {
    // Request headers
    reg.registerFunction("system.net.http.header.ACCEPT", [](const std::vector<Value>&) -> Value {
        return Value(std::string("Accept"));
    });
    reg.registerFunction("system.net.http.header.ACCEPT_ENCODING", [](const std::vector<Value>&) -> Value {
        return Value(std::string("Accept-Encoding"));
    });
    reg.registerFunction("system.net.http.header.ACCEPT_LANGUAGE", [](const std::vector<Value>&) -> Value {
        return Value(std::string("Accept-Language"));
    });
    reg.registerFunction("system.net.http.header.AUTHORIZATION", [](const std::vector<Value>&) -> Value {
        return Value(std::string("Authorization"));
    });
    reg.registerFunction("system.net.http.header.CACHE_CONTROL", [](const std::vector<Value>&) -> Value {
        return Value(std::string("Cache-Control"));
    });
    reg.registerFunction("system.net.http.header.CONNECTION", [](const std::vector<Value>&) -> Value {
        return Value(std::string("Connection"));
    });
    reg.registerFunction("system.net.http.header.CONTENT_LENGTH", [](const std::vector<Value>&) -> Value {
        return Value(std::string("Content-Length"));
    });
    reg.registerFunction("system.net.http.header.CONTENT_TYPE", [](const std::vector<Value>&) -> Value {
        return Value(std::string("Content-Type"));
    });
    reg.registerFunction("system.net.http.header.COOKIE", [](const std::vector<Value>&) -> Value {
        return Value(std::string("Cookie"));
    });
    reg.registerFunction("system.net.http.header.HOST", [](const std::vector<Value>&) -> Value {
        return Value(std::string("Host"));
    });
    reg.registerFunction("system.net.http.header.ORIGIN", [](const std::vector<Value>&) -> Value {
        return Value(std::string("Origin"));
    });
    reg.registerFunction("system.net.http.header.REFERER", [](const std::vector<Value>&) -> Value {
        return Value(std::string("Referer"));
    });
    reg.registerFunction("system.net.http.header.USER_AGENT", [](const std::vector<Value>&) -> Value {
        return Value(std::string("User-Agent"));
    });
    
    // Response headers
    reg.registerFunction("system.net.http.header.ETAG", [](const std::vector<Value>&) -> Value {
        return Value(std::string("ETag"));
    });
    reg.registerFunction("system.net.http.header.LOCATION", [](const std::vector<Value>&) -> Value {
        return Value(std::string("Location"));
    });
    reg.registerFunction("system.net.http.header.SERVER", [](const std::vector<Value>&) -> Value {
        return Value(std::string("Server"));
    });
    reg.registerFunction("system.net.http.header.SET_COOKIE", [](const std::vector<Value>&) -> Value {
        return Value(std::string("Set-Cookie"));
    });
    reg.registerFunction("system.net.http.header.WWW_AUTHENTICATE", [](const std::vector<Value>&) -> Value {
        return Value(std::string("WWW-Authenticate"));
    });
    
    // CORS headers
    reg.registerFunction("system.net.http.header.ACCESS_CONTROL_ALLOW_ORIGIN", [](const std::vector<Value>&) -> Value {
        return Value(std::string("Access-Control-Allow-Origin"));
    });
    reg.registerFunction("system.net.http.header.ACCESS_CONTROL_ALLOW_METHODS", [](const std::vector<Value>&) -> Value {
        return Value(std::string("Access-Control-Allow-Methods"));
    });
    reg.registerFunction("system.net.http.header.ACCESS_CONTROL_ALLOW_HEADERS", [](const std::vector<Value>&) -> Value {
        return Value(std::string("Access-Control-Allow-Headers"));
    });
}
