// ============================================================================
// Bedrock - Quartz Function Bindings
// Exposes Bedrock web framework to Quartz language
// ============================================================================

#include "function_registry.h"
#include "runtime.h"
#include "bedrock_types.h"
#include <sstream>

using namespace bedrock;

static inline Runtime* rt() {
    return global_runtime_ptr;
}

// ============================================================================
// Type Checking Helpers
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

static inline bool isLambda(const Value& v) {
    if (!std::holds_alternative<std::string>(v)) return false;
    const std::string& s = std::get<std::string>(v);
    return s.rfind("__lambda_", 0) == 0;
}

// ============================================================================
// App Reference Helpers
// ============================================================================

static std::string makeAppRef(const std::string& id) {
    return "bedrock:" + id;
}

static std::string extractAppId(const std::string& ref) {
    if (ref.substr(0, 8) == "bedrock:") {
        return ref.substr(8);
    }
    return ref;
}

static bool isAppRef(const Value& v) {
    if (!isString(v)) return false;
    const std::string& s = std::get<std::string>(v);
    return s.substr(0, 8) == "bedrock:";
}

// ============================================================================
// Handler Storage
// ============================================================================

static std::unordered_map<std::string, std::vector<Value>> g_routeHandlers;
static std::unordered_map<std::string, Value> g_errorHandlers;
static std::unordered_map<std::string, Value> g_notFoundHandlers;
static std::mutex g_handlerMutex;

static std::string makeRouteKey(const std::string& appId, const std::string& method, const std::string& path) {
    return appId + ":" + method + ":" + path;
}

static void storeRouteHandler(const std::string& key, const Value& handler) {
    std::lock_guard<std::mutex> lock(g_handlerMutex);
    g_routeHandlers[key].push_back(handler);
}

static void storeErrorHandler(const std::string& appId, const Value& handler) {
    std::lock_guard<std::mutex> lock(g_handlerMutex);
    g_errorHandlers[appId] = handler;
}

static void storeNotFoundHandler(const std::string& appId, const Value& handler) {
    std::lock_guard<std::mutex> lock(g_handlerMutex);
    g_notFoundHandlers[appId] = handler;
}

// ============================================================================
// Dict Conversion Helpers
// ============================================================================

static Value buildRequestDict(const Request& req, Runtime* runtime) {
    if (!runtime) return Value(0);
    
    std::unordered_map<std::string, Value> dict;
    
    dict["method"] = Value(methodToString(req.method));
    dict["path"] = Value(req.path);
    dict["rawPath"] = Value(req.rawPath);
    dict["queryString"] = Value(req.queryString);
    dict["protocol"] = Value(req.protocol);
    dict["host"] = Value(req.host);
    dict["remoteAddr"] = Value(req.remoteAddr);
    dict["remotePort"] = Value(req.remotePort);
    dict["contentType"] = Value(req.contentType);
    dict["contentLength"] = Value(static_cast<int>(req.contentLength));
    dict["body"] = Value(req.body);
    dict["elapsedMs"] = Value(static_cast<int>(req.elapsedMs()));
    
    // Headers as nested dict
    std::unordered_map<std::string, Value> headersDict;
    for (const auto& [key, value] : req.headers) {
        headersDict[key] = Value(value);
    }
    dict["headers"] = runtime->makeDict(std::move(headersDict));
    
    // Query params
    std::unordered_map<std::string, Value> queryDict;
    for (const auto& [key, value] : req.query) {
        queryDict[key] = Value(value);
    }
    dict["query"] = runtime->makeDict(std::move(queryDict));
    
    // Route params
    std::unordered_map<std::string, Value> paramsDict;
    for (const auto& [key, value] : req.params) {
        paramsDict[key] = Value(value);
    }
    dict["params"] = runtime->makeDict(std::move(paramsDict));
    
    // Cookies
    std::unordered_map<std::string, Value> cookiesDict;
    for (const auto& [key, value] : req.cookies) {
        cookiesDict[key] = Value(value);
    }
    dict["cookies"] = runtime->makeDict(std::move(cookiesDict));
    
    // Locals
    std::unordered_map<std::string, Value> localsDict;
    for (const auto& [key, value] : req.locals) {
        localsDict[key] = Value(value);
    }
    dict["locals"] = runtime->makeDict(std::move(localsDict));
    
    return runtime->makeDict(std::move(dict));
}

static Response dictToResponse(const Value& v, Runtime* runtime) {
    Response resp;
    
    if (!isDictRef(v) || !runtime) {
        return resp;
    }
    
    auto* dict = runtime->getDict(std::get<DictRef>(v));
    if (!dict) return resp;
    
    // Status code
    if (dict->count("status") && isInt((*dict)["status"])) {
        resp.statusCode = std::get<int>((*dict)["status"]);
        resp.statusText = getStatusText(resp.statusCode);
    }
    
    // Status text override
    if (dict->count("statusText") && isString((*dict)["statusText"])) {
        resp.statusText = std::get<std::string>((*dict)["statusText"]);
    }
    
    // Body
    if (dict->count("body") && isString((*dict)["body"])) {
        resp.body = std::get<std::string>((*dict)["body"]);
    }
    
    // Content type shorthand
    if (dict->count("contentType") && isString((*dict)["contentType"])) {
        resp.headers["Content-Type"] = std::get<std::string>((*dict)["contentType"]);
    }
    
    // Headers
    if (dict->count("headers") && isDictRef((*dict)["headers"])) {
        auto* headersDict = runtime->getDict(std::get<DictRef>((*dict)["headers"]));
        if (headersDict) {
            for (const auto& [key, value] : *headersDict) {
                if (isString(value)) {
                    resp.headers[key] = std::get<std::string>(value);
                }
            }
        }
    }
    
    resp.sent = true;
    return resp;
}

static AppConfig dictToConfig(const Value& v, Runtime* runtime) {
    AppConfig config;
    
    if (!isDictRef(v) || !runtime) return config;
    
    auto* dict = runtime->getDict(std::get<DictRef>(v));
    if (!dict) return config;
    
    // Network settings
    if (dict->count("host") && isString((*dict)["host"])) {
        config.host = std::get<std::string>((*dict)["host"]);
    }
    if (dict->count("port") && isInt((*dict)["port"])) {
        config.port = std::get<int>((*dict)["port"]);
    }
    if (dict->count("backlog") && isInt((*dict)["backlog"])) {
        config.backlog = std::get<int>((*dict)["backlog"]);
    }
    
    // Performance settings
    if (dict->count("maxConnections") && isInt((*dict)["maxConnections"])) {
        config.maxConnections = std::get<int>((*dict)["maxConnections"]);
    }
    if (dict->count("timeout") && isInt((*dict)["timeout"])) {
        config.connectionTimeoutMs = std::get<int>((*dict)["timeout"]);
    }
    if (dict->count("keepAliveTimeout") && isInt((*dict)["keepAliveTimeout"])) {
        config.keepAliveTimeoutMs = std::get<int>((*dict)["keepAliveTimeout"]);
    }
    if (dict->count("tcpNoDelay")) {
        if (isBool((*dict)["tcpNoDelay"])) {
            config.tcpNoDelay = std::get<bool>((*dict)["tcpNoDelay"]);
        } else if (isInt((*dict)["tcpNoDelay"])) {
            config.tcpNoDelay = std::get<int>((*dict)["tcpNoDelay"]) != 0;
        }
    }
    if (dict->count("reuseAddr")) {
        if (isBool((*dict)["reuseAddr"])) {
            config.reuseAddr = std::get<bool>((*dict)["reuseAddr"]);
        } else if (isInt((*dict)["reuseAddr"])) {
            config.reuseAddr = std::get<int>((*dict)["reuseAddr"]) != 0;
        }
    }
    if (dict->count("reusePort")) {
        if (isBool((*dict)["reusePort"])) {
            config.reusePort = std::get<bool>((*dict)["reusePort"]);
        } else if (isInt((*dict)["reusePort"])) {
            config.reusePort = std::get<int>((*dict)["reusePort"]) != 0;
        }
    }
    
    // Request limits
    if (dict->count("maxBodySize") && isInt((*dict)["maxBodySize"])) {
        config.maxBodySize = std::get<int>((*dict)["maxBodySize"]);
    }
    
    // Features
    if (dict->count("keepAlive")) {
        if (isBool((*dict)["keepAlive"])) {
            config.enableKeepAlive = std::get<bool>((*dict)["keepAlive"]);
        } else if (isInt((*dict)["keepAlive"])) {
            config.enableKeepAlive = std::get<int>((*dict)["keepAlive"]) != 0;
        }
    }
    if (dict->count("trustProxy")) {
        if (isBool((*dict)["trustProxy"])) {
            config.trustProxy = std::get<bool>((*dict)["trustProxy"]);
        } else if (isInt((*dict)["trustProxy"])) {
            config.trustProxy = std::get<int>((*dict)["trustProxy"]) != 0;
        }
    }
    
    // Logging
    if (dict->count("verbose")) {
        if (isBool((*dict)["verbose"])) {
            config.verbose = std::get<bool>((*dict)["verbose"]);
        } else if (isInt((*dict)["verbose"])) {
            config.verbose = std::get<int>((*dict)["verbose"]) != 0;
        }
    }
    if (dict->count("accessLog")) {
        if (isBool((*dict)["accessLog"])) {
            config.accessLog = std::get<bool>((*dict)["accessLog"]);
        } else if (isInt((*dict)["accessLog"])) {
            config.accessLog = std::get<int>((*dict)["accessLog"]) != 0;
        }
    }
    
    return config;
}

static Value buildStatsDict(const ServerStats& stats, Runtime* runtime) {
    if (!runtime) return Value(0);
    
    std::unordered_map<std::string, Value> dict;
    
    dict["totalRequests"] = Value(static_cast<int>(stats.totalRequests.load()));
    dict["activeConnections"] = Value(static_cast<int>(stats.activeConnections.load()));
    dict["totalConnections"] = Value(static_cast<int>(stats.totalConnections.load()));
    dict["bytesReceived"] = Value(static_cast<int>(stats.bytesReceived.load()));
    dict["bytesSent"] = Value(static_cast<int>(stats.bytesSent.load()));
    dict["responses2xx"] = Value(static_cast<int>(stats.errors2xx.load()));
    dict["responses4xx"] = Value(static_cast<int>(stats.errors4xx.load()));
    dict["responses5xx"] = Value(static_cast<int>(stats.errors5xx.load()));
    dict["uptimeMs"] = Value(static_cast<int>(stats.uptimeMs()));
    dict["requestsPerSecond"] = Value(static_cast<int>(stats.requestsPerSecond()));
    
    return runtime->makeDict(std::move(dict));
}

// ============================================================================
// Application Lifecycle Functions
// ============================================================================

void register_bedrock_app_functions(FunctionRegistry& reg) {
    
    // ------------------------------------------------------------------------
    // bedrock.create(config?: dict) -> app
    // Creates a new Bedrock application
    // ------------------------------------------------------------------------
    reg.registerFunction("bedrock.create", [](const std::vector<Value>& args) -> Value {
        std::string name = "";
        
        // Check if first arg is a string (app name)
        if (!args.empty() && isString(args[0]) && !isDictRef(args[0])) {
            name = std::get<std::string>(args[0]);
        }
        
        Application* app = ApplicationManager::instance().createApp(name);
        if (!app) {
            throw LanguageException("BedrockError", "Failed to create application");
        }
        
        // Apply config if provided - check first arg for dict
        if (!args.empty() && isDictRef(args[0]) && rt()) {
            AppConfig config = dictToConfig(args[0], rt());
            app->configure(config);
        } 
        // Or check second arg for dict (if first was name)
        else if (args.size() > 1 && isDictRef(args[1]) && rt()) {
            AppConfig config = dictToConfig(args[1], rt());
            app->configure(config);
        }
        
        return Value(makeAppRef(app->id()));
    });
    
    // ------------------------------------------------------------------------
    // bedrock.configure(app: app, config: dict) -> bool
    // Configure application settings
    // ------------------------------------------------------------------------
    reg.registerFunction("bedrock.configure", [](const std::vector<Value>& args) -> Value {
        if (args.size() < 2 || !isAppRef(args[0]) || !isDictRef(args[1])) {
            throw LanguageException("TypeError", 
                "bedrock.configure: requires (app, config)");
        }
        
        std::string id = extractAppId(std::get<std::string>(args[0]));
        Application* app = ApplicationManager::instance().getApp(id);
        if (!app) {
            throw LanguageException("BedrockError", "Invalid application reference");
        }
        
        AppConfig config = dictToConfig(args[1], rt());
        app->configure(config);
        
        return Value(true);
    });
    
    // ------------------------------------------------------------------------
    // bedrock.start(app: app) -> bool
    // Starts the application server
    // ------------------------------------------------------------------------
    reg.registerFunction("bedrock.start", [](const std::vector<Value>& args) -> Value {
        if (args.empty() || !isAppRef(args[0])) {
            throw LanguageException("TypeError", 
                "bedrock.start: requires app argument");
        }
        
        std::string id = extractAppId(std::get<std::string>(args[0]));
        Application* app = ApplicationManager::instance().getApp(id);
        if (!app) {
            throw LanguageException("BedrockError", "Invalid application reference");
        }
        
        if (!app->start()) {
            throw LanguageException("BedrockError", "Failed to start server");
        }
        
        return Value(true);
    });
    
    // ------------------------------------------------------------------------
    // bedrock.stop(app: app) -> bool
    // Stops the application server
    // ------------------------------------------------------------------------
    reg.registerFunction("bedrock.stop", [](const std::vector<Value>& args) -> Value {
        if (args.empty() || !isAppRef(args[0])) {
            throw LanguageException("TypeError", 
                "bedrock.stop: requires app argument");
        }
        
        std::string id = extractAppId(std::get<std::string>(args[0]));
        Application* app = ApplicationManager::instance().getApp(id);
        if (!app) {
            return Value(false);
        }
        
        app->stop();
        return Value(true);
    });
    
    // ------------------------------------------------------------------------
    // bedrock.isRunning(app: app) -> bool
    // Check if server is running
    // ------------------------------------------------------------------------
    reg.registerFunction("bedrock.isRunning", [](const std::vector<Value>& args) -> Value {
        if (args.empty() || !isAppRef(args[0])) {
            throw LanguageException("TypeError", 
                "bedrock.isRunning: requires app argument");
        }
        
        std::string id = extractAppId(std::get<std::string>(args[0]));
        Application* app = ApplicationManager::instance().getApp(id);
        if (!app) {
            return Value(false);
        }
        
        return Value(app->isRunning());
    });
    
    // ------------------------------------------------------------------------
    // bedrock.wait(app: app) -> nil
    // Blocks until the server stops
    // ------------------------------------------------------------------------
    reg.registerFunction("bedrock.wait", [](const std::vector<Value>& args) -> Value {
        if (args.empty() || !isAppRef(args[0])) {
            throw LanguageException("TypeError", 
                "bedrock.wait: requires app argument");
        }
        
        std::string id = extractAppId(std::get<std::string>(args[0]));
        Application* app = ApplicationManager::instance().getApp(id);
        if (!app) {
            throw LanguageException("BedrockError", "Invalid application reference");
        }
        
        app->wait();
        return Value();
    });
    
    // ------------------------------------------------------------------------
    // bedrock.stats(app: app) -> dict
    // Get server statistics
    // ------------------------------------------------------------------------
    reg.registerFunction("bedrock.stats", [](const std::vector<Value>& args) -> Value {
        if (args.empty() || !isAppRef(args[0])) {
            throw LanguageException("TypeError", 
                "bedrock.stats: requires app argument");
        }
        
        std::string id = extractAppId(std::get<std::string>(args[0]));
        Application* app = ApplicationManager::instance().getApp(id);
        if (!app) {
            throw LanguageException("BedrockError", "Invalid application reference");
        }
        
        return buildStatsDict(app->stats(), rt());
    });
    
    // ------------------------------------------------------------------------
    // bedrock.destroy(app: app) -> bool
    // Destroys the application
    // ------------------------------------------------------------------------
    reg.registerFunction("bedrock.destroy", [](const std::vector<Value>& args) -> Value {
        if (args.empty() || !isAppRef(args[0])) {
            throw LanguageException("TypeError", 
                "bedrock.destroy: requires app argument");
        }
        
        std::string id = extractAppId(std::get<std::string>(args[0]));
        return Value(ApplicationManager::instance().removeApp(id));
    });
}

// ============================================================================
// Route Registration Functions
// ============================================================================

void register_bedrock_route_functions(FunctionRegistry& reg) {
    
    // Generic route registration helper
    auto registerRoute = [](const std::string& method, const std::vector<Value>& args) -> Value {
        if (args.size() < 3 || !isAppRef(args[0]) || !isString(args[1]) || !isLambda(args[2])) {
            throw LanguageException("TypeError", 
                "Route registration requires (app, path, handler)");
        }
        
        std::string id = extractAppId(std::get<std::string>(args[0]));
        std::string path = std::get<std::string>(args[1]);
        Value handlerLambda = args[2];
        
        Application* app = ApplicationManager::instance().getApp(id);
        if (!app) {
            throw LanguageException("BedrockError", "Invalid application reference");
        }
        
        // Store handler
        std::string routeKey = makeRouteKey(id, method, path);
        storeRouteHandler(routeKey, handlerLambda);
        
        // Create C++ handler that invokes Quartz lambda
        Handler handler = [routeKey, handlerLambda](Context& ctx, NextFn next) {
            if (!rt()) {
                ctx.res.serverError("Runtime not available");
                return;
            }
            
            try {
                // Build request dict
                Value reqDict = buildRequestDict(ctx.req, rt());
                std::vector<Value> handlerArgs = {reqDict};
                
                // Invoke Quartz handler
                Value result = rt()->invokeLambdaValue(handlerLambda, handlerArgs);
                
                // Convert result to response
                if (isDictRef(result)) {
                    ctx.res = dictToResponse(result, rt());
                } else if (isString(result)) {
                    ctx.res.text(std::get<std::string>(result));
                }
            } catch (const std::exception& e) {
                ctx.hasError = true;
                ctx.errorMessage = e.what();
                ctx.res.serverError(std::string("Handler error: ") + e.what());
            }
        };
        
        // Register with router based on method
        HttpMethod m = stringToMethod(method);
        switch (m) {
            case HttpMethod::GET:     app->get(path, handler); break;
            case HttpMethod::POST:    app->post(path, handler); break;
            case HttpMethod::PUT:     app->put(path, handler); break;
            case HttpMethod::DELETE_: app->del(path, handler); break;
            case HttpMethod::PATCH:   app->patch(path, handler); break;
            case HttpMethod::ANY:     app->any(path, handler); break;
            default:                  app->get(path, handler); break;
        }
        
        return Value(true);
    };
    
    // ------------------------------------------------------------------------
    // bedrock.get(app: app, path: string, handler: fn) -> bool
    // Register GET route
    // ------------------------------------------------------------------------
    reg.registerFunction("bedrock.get", [registerRoute](const std::vector<Value>& args) -> Value {
        return registerRoute("GET", args);
    });
    
    // ------------------------------------------------------------------------
    // bedrock.post(app: app, path: string, handler: fn) -> bool
    // Register POST route
    // ------------------------------------------------------------------------
    reg.registerFunction("bedrock.post", [registerRoute](const std::vector<Value>& args) -> Value {
        return registerRoute("POST", args);
    });
    
    // ------------------------------------------------------------------------
    // bedrock.put(app: app, path: string, handler: fn) -> bool
    // Register PUT route
    // ------------------------------------------------------------------------
    reg.registerFunction("bedrock.put", [registerRoute](const std::vector<Value>& args) -> Value {
        return registerRoute("PUT", args);
    });
    
    // ------------------------------------------------------------------------
    // bedrock.delete(app: app, path: string, handler: fn) -> bool
    // Register DELETE route
    // ------------------------------------------------------------------------
    reg.registerFunction("bedrock.delete", [registerRoute](const std::vector<Value>& args) -> Value {
        return registerRoute("DELETE", args);
    });
    
    // ------------------------------------------------------------------------
    // bedrock.patch(app: app, path: string, handler: fn) -> bool
    // Register PATCH route
    // ------------------------------------------------------------------------
    reg.registerFunction("bedrock.patch", [registerRoute](const std::vector<Value>& args) -> Value {
        return registerRoute("PATCH", args);
    });
    
    // ------------------------------------------------------------------------
    // bedrock.any(app: app, path: string, handler: fn) -> bool
    // Register route for any HTTP method
    // ------------------------------------------------------------------------
    reg.registerFunction("bedrock.any", [registerRoute](const std::vector<Value>& args) -> Value {
        return registerRoute("*", args);
    });
}

// ============================================================================
// Middleware Functions
// ============================================================================

void register_bedrock_middleware_functions(FunctionRegistry& reg) {
    
    // ------------------------------------------------------------------------
    // bedrock.use(app: app, handler: fn) -> bool
    // bedrock.use(app: app, path: string, handler: fn) -> bool
    // Register middleware
    // ------------------------------------------------------------------------
    reg.registerFunction("bedrock.use", [](const std::vector<Value>& args) -> Value {
        if (args.empty() || !isAppRef(args[0])) {
            throw LanguageException("TypeError", 
                "bedrock.use: requires app argument");
        }
        
        std::string id = extractAppId(std::get<std::string>(args[0]));
        Application* app = ApplicationManager::instance().getApp(id);
        if (!app) {
            throw LanguageException("BedrockError", "Invalid application reference");
        }
        
        Value handlerLambda;
        std::string path;
        
        if (args.size() >= 3 && isString(args[1]) && isLambda(args[2])) {
            // Path-scoped middleware
            path = std::get<std::string>(args[1]);
            handlerLambda = args[2];
        } else if (args.size() >= 2 && isLambda(args[1])) {
            // Global middleware
            handlerLambda = args[1];
        } else {
            throw LanguageException("TypeError", 
                "bedrock.use: requires handler function");
        }
        
        Handler middleware = [handlerLambda](Context& ctx, NextFn next) {
            if (!rt()) {
                next();
                return;
            }
            
            try {
                // Build request dict
                Value reqDict = buildRequestDict(ctx.req, rt());
                std::vector<Value> handlerArgs = {reqDict};
                
                // Invoke middleware
                Value result = rt()->invokeLambdaValue(handlerLambda, handlerArgs);
                
                // If middleware returns a response, use it
                if (isDictRef(result)) {
                    auto* dict = rt()->getDict(std::get<DictRef>(result));
                    if (dict && dict->count("body")) {
                        ctx.res = dictToResponse(result, rt());
                        return;  // Don't call next
                    }
                }
                
                // Continue to next handler
                next();
            } catch (const std::exception& e) {
                ctx.hasError = true;
                ctx.errorMessage = e.what();
                // Still call next to allow error handlers to run
                next();
            }
        };
        
        if (path.empty()) {
            app->use(middleware);
        } else {
            app->use(path, middleware);
        }
        
        return Value(true);
    });
    
    // ------------------------------------------------------------------------
    // bedrock.onError(app: app, handler: fn) -> bool
    // Set error handler. Handler: fn(request, error) -> response
    // ------------------------------------------------------------------------
    reg.registerFunction("bedrock.onError", [](const std::vector<Value>& args) -> Value {
        if (args.size() < 2 || !isAppRef(args[0]) || !isLambda(args[1])) {
            throw LanguageException("TypeError", 
                "bedrock.onError: requires (app, handler)");
        }
        
        std::string id = extractAppId(std::get<std::string>(args[0]));
        Value handlerLambda = args[1];
        
        Application* app = ApplicationManager::instance().getApp(id);
        if (!app) {
            throw LanguageException("BedrockError", "Invalid application reference");
        }
        
        storeErrorHandler(id, handlerLambda);
        
        app->onError([handlerLambda](Context& ctx, const std::string& error) {
            if (!rt()) {
                ctx.res.serverError("Runtime error: " + error);
                return;
            }
            
            try {
                Value reqDict = buildRequestDict(ctx.req, rt());
                Value errorVal = Value(error);
                std::vector<Value> handlerArgs = {reqDict, errorVal};
                
                Value result = rt()->invokeLambdaValue(handlerLambda, handlerArgs);
                
                if (isDictRef(result)) {
                    ctx.res = dictToResponse(result, rt());
                } else if (isString(result)) {
                    ctx.res.serverError(std::get<std::string>(result));
                }
            } catch (const std::exception& e) {
                ctx.res.serverError(std::string("Error handler failed: ") + e.what());
            }
        });
        
        return Value(true);
    });
    
    // ------------------------------------------------------------------------
    // bedrock.onNotFound(app: app, handler: fn) -> bool
    // Set 404 handler. Handler: fn(request) -> response
    // ------------------------------------------------------------------------
    reg.registerFunction("bedrock.onNotFound", [](const std::vector<Value>& args) -> Value {
        if (args.size() < 2 || !isAppRef(args[0]) || !isLambda(args[1])) {
            throw LanguageException("TypeError", 
                "bedrock.onNotFound: requires (app, handler)");
        }
        
        std::string id = extractAppId(std::get<std::string>(args[0]));
        Value handlerLambda = args[1];
        
        Application* app = ApplicationManager::instance().getApp(id);
        if (!app) {
            throw LanguageException("BedrockError", "Invalid application reference");
        }
        
        storeNotFoundHandler(id, handlerLambda);
        
        app->onNotFound([handlerLambda](Context& ctx, NextFn next) {
            if (!rt()) {
                ctx.res.notFound("Not Found");
                return;
            }
            
            try {
                Value reqDict = buildRequestDict(ctx.req, rt());
                std::vector<Value> handlerArgs = {reqDict};
                
                Value result = rt()->invokeLambdaValue(handlerLambda, handlerArgs);
                
                if (isDictRef(result)) {
                    ctx.res = dictToResponse(result, rt());
                } else if (isString(result)) {
                    ctx.res.notFound(std::get<std::string>(result));
                }
            } catch (const std::exception& e) {
                ctx.res.notFound("Not Found");
            }
        });
        
        return Value(true);
    });
}

// ============================================================================
// Response Helper Functions
// ============================================================================

void register_bedrock_response_functions(FunctionRegistry& reg) {
    
    // ------------------------------------------------------------------------
    // bedrock.response.ok(body?: string, contentType?: string) -> dict
    // Create 200 OK response
    // ------------------------------------------------------------------------
    reg.registerFunction("bedrock.response.ok", [](const std::vector<Value>& args) -> Value {
        if (!rt()) return Value(0);
        
        std::string body = "";
        std::string contentType = "text/plain; charset=utf-8";
        
        if (!args.empty() && isString(args[0])) {
            body = std::get<std::string>(args[0]);
        }
        if (args.size() > 1 && isString(args[1])) {
            contentType = std::get<std::string>(args[1]);
        }
        
        std::unordered_map<std::string, Value> dict;
        dict["status"] = Value(200);
        dict["body"] = Value(body);
        dict["contentType"] = Value(contentType);
        
        return rt()->makeDict(std::move(dict));
    });
    
    // ------------------------------------------------------------------------
    // bedrock.response.json(data: dict|string) -> dict
    // Create JSON response
    // ------------------------------------------------------------------------
    reg.registerFunction("bedrock.response.json", [](const std::vector<Value>& args) -> Value {
        if (!rt() || args.empty()) return Value(0);
        
        std::string body;
        if (isString(args[0])) {
            body = std::get<std::string>(args[0]);
        } else {
            body = "{}";  // Default empty object
        }
        
        std::unordered_map<std::string, Value> dict;
        dict["status"] = Value(200);
        dict["body"] = Value(body);
        dict["contentType"] = Value("application/json; charset=utf-8");
        
        return rt()->makeDict(std::move(dict));
    });
    
    // ------------------------------------------------------------------------
    // bedrock.response.html(content: string) -> dict
    // Create HTML response
    // ------------------------------------------------------------------------
    reg.registerFunction("bedrock.response.html", [](const std::vector<Value>& args) -> Value {
        if (!rt() || args.empty()) return Value(0);
        
        std::string body = "";
        if (isString(args[0])) {
            body = std::get<std::string>(args[0]);
        }
        
        std::unordered_map<std::string, Value> dict;
        dict["status"] = Value(200);
        dict["body"] = Value(body);
        dict["contentType"] = Value("text/html; charset=utf-8");
        
        return rt()->makeDict(std::move(dict));
    });
    
    // ------------------------------------------------------------------------
    // bedrock.response.redirect(location: string, code?: int) -> dict
    // Create redirect response
    // ------------------------------------------------------------------------
    reg.registerFunction("bedrock.response.redirect", [](const std::vector<Value>& args) -> Value {
        if (!rt() || args.empty() || !isString(args[0])) return Value(0);
        
        std::string location = std::get<std::string>(args[0]);
        int code = 302;
        if (args.size() > 1 && isInt(args[1])) {
            code = std::get<int>(args[1]);
        }
        
        std::unordered_map<std::string, Value> headers;
        headers["Location"] = Value(location);
        
        std::unordered_map<std::string, Value> dict;
        dict["status"] = Value(code);
        dict["body"] = Value("");
        dict["headers"] = rt()->makeDict(std::move(headers));
        
        return rt()->makeDict(std::move(dict));
    });
    
    // ------------------------------------------------------------------------
    // bedrock.response.error(code: int, message?: string) -> dict
    // Create error response
    // ------------------------------------------------------------------------
    reg.registerFunction("bedrock.response.error", [](const std::vector<Value>& args) -> Value {
        if (!rt() || args.empty() || !isInt(args[0])) return Value(0);
        
        int code = std::get<int>(args[0]);
        std::string message = getStatusText(code);
        if (args.size() > 1 && isString(args[1])) {
            message = std::get<std::string>(args[1]);
        }
        
        std::unordered_map<std::string, Value> dict;
        dict["status"] = Value(code);
        dict["body"] = Value(message);
        dict["contentType"] = Value("text/plain; charset=utf-8");
        
        return rt()->makeDict(std::move(dict));
    });
    
    // ------------------------------------------------------------------------
    // bedrock.response.notFound(message?: string) -> dict
    // Create 404 response
    // ------------------------------------------------------------------------
    reg.registerFunction("bedrock.response.notFound", [](const std::vector<Value>& args) -> Value {
        if (!rt()) return Value(0);
        
        std::string message = "Not Found";
        if (!args.empty() && isString(args[0])) {
            message = std::get<std::string>(args[0]);
        }
        
        std::unordered_map<std::string, Value> dict;
        dict["status"] = Value(404);
        dict["body"] = Value(message);
        dict["contentType"] = Value("text/plain; charset=utf-8");
        
        return rt()->makeDict(std::move(dict));
    });
    
    // ------------------------------------------------------------------------
    // bedrock.response.created(body?: string, location?: string) -> dict
    // Create 201 Created response
    // ------------------------------------------------------------------------
    reg.registerFunction("bedrock.response.created", [](const std::vector<Value>& args) -> Value {
        if (!rt()) return Value(0);
        
        std::string body = "";
        if (!args.empty() && isString(args[0])) {
            body = std::get<std::string>(args[0]);
        }
        
        std::unordered_map<std::string, Value> dict;
        dict["status"] = Value(201);
        dict["body"] = Value(body);
        
        if (args.size() > 1 && isString(args[1])) {
            std::unordered_map<std::string, Value> headers;
            headers["Location"] = args[1];
            dict["headers"] = rt()->makeDict(std::move(headers));
        }
        
        return rt()->makeDict(std::move(dict));
    });
    
    // ------------------------------------------------------------------------
    // bedrock.response.noContent() -> dict
    // Create 204 No Content response
    // ------------------------------------------------------------------------
    reg.registerFunction("bedrock.response.noContent", [](const std::vector<Value>& args) -> Value {
        if (!rt()) return Value(0);
        
        std::unordered_map<std::string, Value> dict;
        dict["status"] = Value(204);
        dict["body"] = Value("");
        
        return rt()->makeDict(std::move(dict));
    });
}
