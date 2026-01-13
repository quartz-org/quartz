// ============================================================================
// Bedrock - Router and Middleware Implementation
// ============================================================================

#include "bedrock_types.h"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <fstream>

#ifndef QZ_PLATFORM_WINDOWS
#include <sys/stat.h>
#endif

namespace bedrock {

// ============================================================================
// URL Encoding/Decoding
// ============================================================================

std::string urlDecode(std::string_view encoded) {
    std::string result;
    result.reserve(encoded.size());
    
    for (size_t i = 0; i < encoded.size(); ++i) {
        if (encoded[i] == '%' && i + 2 < encoded.size()) {
            int value;
            std::string temp(encoded.substr(i + 1, 2));
            std::istringstream iss(temp);
            if (iss >> std::hex >> value) {
                result += static_cast<char>(value);
                i += 2;
                continue;
            }
        } else if (encoded[i] == '+') {
            result += ' ';
            continue;
        }
        result += encoded[i];
    }
    return result;
}

std::string urlEncode(std::string_view str) {
    std::ostringstream encoded;
    encoded.fill('0');
    encoded << std::hex;
    
    for (unsigned char c : str) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded << c;
        } else {
            encoded << '%' << std::setw(2) << static_cast<int>(c);
        }
    }
    return encoded.str();
}

std::unordered_map<std::string, std::string> parseQueryString(std::string_view query) {
    std::unordered_map<std::string, std::string> result;
    if (query.empty()) return result;
    
    size_t start = 0;
    while (start < query.size()) {
        size_t end = query.find('&', start);
        if (end == std::string_view::npos) end = query.size();
        
        std::string_view pair = query.substr(start, end - start);
        size_t eqPos = pair.find('=');
        if (eqPos != std::string_view::npos) {
            std::string key = urlDecode(pair.substr(0, eqPos));
            std::string value = urlDecode(pair.substr(eqPos + 1));
            result[key] = value;
        } else if (!pair.empty()) {
            result[urlDecode(pair)] = "";
        }
        
        start = end + 1;
    }
    return result;
}

std::unordered_map<std::string, std::string> parseCookies(std::string_view cookieHeader) {
    std::unordered_map<std::string, std::string> result;
    if (cookieHeader.empty()) return result;
    
    size_t start = 0;
    while (start < cookieHeader.size()) {
        size_t end = cookieHeader.find(';', start);
        if (end == std::string_view::npos) end = cookieHeader.size();
        
        std::string_view pair = cookieHeader.substr(start, end - start);
        // Trim leading whitespace
        size_t vstart = pair.find_first_not_of(" \t");
        if (vstart != std::string_view::npos) {
            pair = pair.substr(vstart);
            size_t eqPos = pair.find('=');
            if (eqPos != std::string_view::npos) {
                std::string key(pair.substr(0, eqPos));
                std::string value(pair.substr(eqPos + 1));
                result[key] = value;
            }
        }
        
        start = end + 1;
    }
    return result;
}

std::string getMimeType(const std::string& extension) {
    static const std::unordered_map<std::string, std::string> mimeTypes = {
        {".html", "text/html; charset=utf-8"},
        {".htm", "text/html; charset=utf-8"},
        {".css", "text/css; charset=utf-8"},
        {".js", "application/javascript; charset=utf-8"},
        {".mjs", "application/javascript; charset=utf-8"},
        {".json", "application/json; charset=utf-8"},
        {".xml", "application/xml; charset=utf-8"},
        {".txt", "text/plain; charset=utf-8"},
        {".md", "text/markdown; charset=utf-8"},
        {".csv", "text/csv; charset=utf-8"},
        {".png", "image/png"},
        {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"},
        {".gif", "image/gif"},
        {".webp", "image/webp"},
        {".svg", "image/svg+xml"},
        {".ico", "image/x-icon"},
        {".bmp", "image/bmp"},
        {".pdf", "application/pdf"},
        {".zip", "application/zip"},
        {".gz", "application/gzip"},
        {".tar", "application/x-tar"},
        {".rar", "application/vnd.rar"},
        {".7z", "application/x-7z-compressed"},
        {".woff", "font/woff"},
        {".woff2", "font/woff2"},
        {".ttf", "font/ttf"},
        {".otf", "font/otf"},
        {".eot", "application/vnd.ms-fontobject"},
        {".mp3", "audio/mpeg"},
        {".wav", "audio/wav"},
        {".ogg", "audio/ogg"},
        {".mp4", "video/mp4"},
        {".webm", "video/webm"},
        {".avi", "video/x-msvideo"},
        {".mov", "video/quicktime"},
        {".wasm", "application/wasm"},
        {".map", "application/json"},
    };
    
    std::string ext = extension;
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    
    auto it = mimeTypes.find(ext);
    return it != mimeTypes.end() ? it->second : "application/octet-stream";
}

std::string getStatusText(int code) {
    switch (code) {
        case 100: return "Continue";
        case 101: return "Switching Protocols";
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 206: return "Partial Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 303: return "See Other";
        case 304: return "Not Modified";
        case 307: return "Temporary Redirect";
        case 308: return "Permanent Redirect";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 406: return "Not Acceptable";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 410: return "Gone";
        case 411: return "Length Required";
        case 413: return "Payload Too Large";
        case 414: return "URI Too Long";
        case 415: return "Unsupported Media Type";
        case 416: return "Range Not Satisfiable";
        case 422: return "Unprocessable Entity";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default: return "Unknown";
    }
}

std::string normalizePath(std::string_view path) {
    if (path.empty() || path == "/") return "/";
    
    std::vector<std::string_view> segments;
    size_t start = 0;
    while (start < path.size()) {
        if (path[start] == '/') { start++; continue; }
        size_t end = path.find('/', start);
        if (end == std::string_view::npos) end = path.size();
        
        std::string_view segment = path.substr(start, end - start);
        if (segment == ".") {
            // Skip
        } else if (segment == "..") {
            if (!segments.empty()) segments.pop_back();
        } else if (!segment.empty()) {
            segments.push_back(segment);
        }
        
        start = end;
    }
    
    std::string result = "/";
    for (size_t i = 0; i < segments.size(); ++i) {
        result.append(segments[i].data(), segments[i].size());
        if (i < segments.size() - 1) result += "/";
    }
    
    return result;
}

std::string pathJoin(std::string_view base, std::string_view path) {
    if (base.empty()) return std::string(path);
    if (path.empty()) return std::string(base);
    
    bool baseHasSlash = base.back() == '/';
    bool pathHasSlash = path.front() == '/';
    
    std::string result;
    result.reserve(base.size() + path.size() + 1);
    result.append(base.data(), base.size());
    
    if (baseHasSlash && pathHasSlash) {
        result.pop_back();
        result.append(path.data(), path.size());
    } else if (!baseHasSlash && !pathHasSlash) {
        result += "/";
        result.append(path.data(), path.size());
    } else {
        result.append(path.data(), path.size());
    }
    return result;
}

bool fileExists(const std::string& path) {
#ifdef QZ_PLATFORM_WINDOWS
    DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
#endif
}

std::string readFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return "";
    
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

std::string getExtension(const std::string& path) {
    size_t pos = path.rfind('.');
    if (pos != std::string::npos && pos < path.size() - 1) {
        return path.substr(pos);
    }
    return "";
}

// ============================================================================
// Request Implementation
// ============================================================================

std::string_view Request::getHeader(std::string_view name) const {
    for (const auto& header : headers) {
        if (header.name.size() == name.size()) {
            // Case-insensitive comparison
            bool match = true;
            for (size_t i = 0; i < name.size(); ++i) {
                if (std::tolower(header.name[i]) != std::tolower(name[i])) {
                    match = false;
                    break;
                }
            }
            if (match) return header.value;
        }
    }
    return "";
}

std::string Request::getParam(const std::string& name) const {
    auto it = params.find(name);
    if (it != params.end()) return it->second;
    
    it = query.find(name);
    if (it != query.end()) return it->second;
    
    return "";
}

std::string Request::getQuery(const std::string& name) const {
    auto it = query.find(name);
    return it != query.end() ? it->second : "";
}

std::string Request::getCookie(const std::string& name) const {
    auto it = cookies.find(name);
    return it != cookies.end() ? it->second : "";
}

std::string Request::getLocal(const std::string& name) const {
    auto it = locals.find(name);
    return it != locals.end() ? it->second : "";
}

void Request::setLocal(const std::string& name, const std::string& value) {
    locals[name] = value;
}

int64_t Request::elapsedMs() const {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count();
}

bool Request::isJson() const {
    return contentType.find("application/json") != std::string_view::npos;
}

bool Request::isForm() const {
    return contentType.find("application/x-www-form-urlencoded") != std::string_view::npos;
}

bool Request::isMultipart() const {
    return contentType.find("multipart/form-data") != std::string_view::npos;
}

// ============================================================================
// Response Implementation
// ============================================================================

Response& Response::status(int code) {
    statusCode = code;
    statusText = getStatusText(code);
    return *this;
}

Response& Response::status(int code, const std::string& text) {
    statusCode = code;
    statusText = text;
    return *this;
}

Response& Response::header(const std::string& name, const std::string& value) {
    headers[name] = value;
    return *this;
}

Response& Response::contentType(const std::string& type) {
    headers["Content-Type"] = type;
    return *this;
}

Response& Response::contentLength(size_t len) {
    headers["Content-Length"] = std::to_string(len);
    return *this;
}

Response& Response::cookie(const std::string& name, const std::string& value,
                           int maxAge, const std::string& path,
                           bool httpOnly, bool secure,
                           const std::string& sameSite) {
    std::ostringstream ss;
    ss << name << "=" << value;
    ss << "; Path=" << path;
    if (maxAge >= 0) ss << "; Max-Age=" << maxAge;
    if (httpOnly) ss << "; HttpOnly";
    if (secure) ss << "; Secure";
    if (!sameSite.empty()) ss << "; SameSite=" << sameSite;
    
    headers["Set-Cookie"] = ss.str();
    return *this;
}

Response& Response::text(const std::string& content) {
    body = content;
    headers["Content-Type"] = "text/plain; charset=utf-8";
    headers["Content-Length"] = std::to_string(content.size());
    sent = true;
    return *this;
}

Response& Response::html(const std::string& content) {
    body = content;
    headers["Content-Type"] = "text/html; charset=utf-8";
    headers["Content-Length"] = std::to_string(content.size());
    sent = true;
    return *this;
}

Response& Response::json(const std::string& jsonContent) {
    body = jsonContent;
    headers["Content-Type"] = "application/json; charset=utf-8";
    headers["Content-Length"] = std::to_string(jsonContent.size());
    sent = true;
    return *this;
}

Response& Response::send(const std::string& content, const std::string& ct) {
    body = content;
    headers["Content-Type"] = ct;
    headers["Content-Length"] = std::to_string(content.size());
    sent = true;
    return *this;
}

Response& Response::redirect(const std::string& location, int code) {
    statusCode = code;
    statusText = getStatusText(code);
    headers["Location"] = location;
    body.clear();
    sent = true;
    return *this;
}

Response& Response::notFound(const std::string& message) {
    return status(404).text(message);
}

Response& Response::badRequest(const std::string& message) {
    return status(400).text(message);
}

Response& Response::serverError(const std::string& message) {
    return status(500).text(message);
}

Response& Response::noContent() {
    statusCode = 204;
    statusText = "No Content";
    body.clear();
    headers.erase("Content-Type");
    headers.erase("Content-Length");
    sent = true;
    return *this;
}

std::string Response::build() const {
    auto iov = buildIov();
    std::string result;
    size_t total = 0;
    for (const auto& v : iov) total += v.len;
    result.reserve(total);
    for (const auto& v : iov) result.append(v.data, v.len);
    return result;
}

std::vector<Response::BufferView> Response::buildIov() const {
    std::vector<BufferView> iov;
    
    // We need some static strings for fixed parts of the response
    // In a real high-perf server, these would be pre-allocated or shared.
    // For now, we build the header portion in a string and return a view of it.
    // POTENTIAL ISSUE: Life cycle of the header string.
    // Let's use a thread-local or member-owned buffer for headers.
    
    static thread_local std::string headerBuffer;
    headerBuffer.clear();
    headerBuffer.reserve(512);

    headerBuffer += "HTTP/1.1 ";
    headerBuffer += std::to_string(statusCode);
    headerBuffer += " ";
    headerBuffer += statusText;
    headerBuffer += "\r\n";
    
    for (const auto& [name, value] : headers) {
        headerBuffer += name;
        headerBuffer += ": ";
        headerBuffer += value;
        headerBuffer += "\r\n";
    }
    
    if (headers.find("Content-Length") == headers.end() && !body.empty()) {
        headerBuffer += "Content-Length: ";
        headerBuffer += std::to_string(body.size());
        headerBuffer += "\r\n";
    }
    
    if (headers.find("Server") == headers.end()) {
        headerBuffer += "Server: Bedrock/1.1 (Quartz-Optimized)\r\n";
    }
    
    headerBuffer += "\r\n";
    
    // Add header view
    iov.push_back({headerBuffer.data(), headerBuffer.size()});
    
    // Add body view
    if (!body.empty()) {
        iov.push_back({body.data(), body.size()});
    }
    
    return iov;
}

Response Response::ok(const std::string& body, const std::string& ct) {
    Response r;
    r.statusCode = 200;
    r.statusText = "OK";
    if (!body.empty()) {
        r.body = body;
        r.headers["Content-Type"] = ct;
        r.headers["Content-Length"] = std::to_string(body.size());
    }
    r.sent = true;
    return r;
}

Response Response::created(const std::string& body, const std::string& location) {
    Response r;
    r.statusCode = 201;
    r.statusText = "Created";
    if (!location.empty()) {
        r.headers["Location"] = location;
    }
    if (!body.empty()) {
        r.body = body;
        r.headers["Content-Length"] = std::to_string(body.size());
    }
    r.sent = true;
    return r;
}

Response Response::accepted() {
    Response r;
    r.statusCode = 202;
    r.statusText = "Accepted";
    r.sent = true;
    return r;
}

Response Response::jsonResponse(const std::string& json) {
    Response r;
    r.statusCode = 200;
    r.statusText = "OK";
    r.body = json;
    r.headers["Content-Type"] = "application/json; charset=utf-8";
    r.headers["Content-Length"] = std::to_string(json.size());
    r.sent = true;
    return r;
}

Response Response::htmlResponse(const std::string& html) {
    Response r;
    r.statusCode = 200;
    r.statusText = "OK";
    r.body = html;
    r.headers["Content-Type"] = "text/html; charset=utf-8";
    r.headers["Content-Length"] = std::to_string(html.size());
    r.sent = true;
    return r;
}

Response Response::errorResponse(int code, const std::string& message) {
    Response r;
    r.statusCode = code;
    r.statusText = getStatusText(code);
    r.body = message;
    r.headers["Content-Type"] = "text/plain; charset=utf-8";
    r.headers["Content-Length"] = std::to_string(message.size());
    r.sent = true;
    return r;
}

// ============================================================================
// RoutePattern Implementation
// ============================================================================

RoutePattern RoutePattern::compile(const std::string& pattern) {
    RoutePattern rp;
    rp.pattern = pattern;
    rp.isStatic = true;
    rp.priority = 100;  // Start high, reduce for wildcards
    
    if (pattern.empty() || pattern == "/") {
        rp.regex = std::regex("^/$");
        return rp;
    }
    
    std::string regexStr = "^";
    std::istringstream stream(pattern);
    std::string segment;
    size_t position = 0;
    
    while (std::getline(stream, segment, '/')) {
        if (segment.empty()) continue;
        
        regexStr += "/";
        
        if (segment[0] == ':') {
            // Named parameter :name
            rp.isStatic = false;
            rp.priority -= 10;
            
            RouteParam param;
            param.position = position;
            param.isWildcard = false;
            
            // Check for regex constraint :name(regex)
            size_t parenPos = segment.find('(');
            if (parenPos != std::string::npos && segment.back() == ')') {
                param.name = segment.substr(1, parenPos - 1);
                std::string constraint = segment.substr(parenPos + 1, segment.size() - parenPos - 2);
                regexStr += "(" + constraint + ")";
            } else {
                param.name = segment.substr(1);
                regexStr += "([^/]+)";  // Match anything except /
            }
            
            rp.params.push_back(param);
        } else if (segment == "*") {
            // Single-segment wildcard
            rp.isStatic = false;
            rp.priority -= 20;
            
            RouteParam param;
            param.name = "*";
            param.position = position;
            param.isWildcard = true;
            regexStr += "([^/]+)";
            
            rp.params.push_back(param);
        } else if (segment == "**") {
            // Multi-segment wildcard (catch-all)
            rp.isStatic = false;
            rp.priority -= 50;
            
            RouteParam param;
            param.name = "**";
            param.position = position;
            param.isWildcard = true;
            regexStr += "(.*)";
            
            rp.params.push_back(param);
        } else {
            // Static segment
            // Escape regex special chars
            for (char c : segment) {
                if (c == '.' || c == '+' || c == '?' || c == '*' || c == '[' || 
                    c == ']' || c == '(' || c == ')' || c == '{' || c == '}' ||
                    c == '\\' || c == '^' || c == '$' || c == '|') {
                    regexStr += '\\';
                }
                regexStr += c;
            }
        }
        
        position++;
    }
    
    regexStr += "/?$";  // Optional trailing slash
    rp.regex = std::regex(regexStr, std::regex_constants::icase);
    
    return rp;
}

bool RoutePattern::matches(const std::string& path, 
                           std::unordered_map<std::string, std::string>& outParams) const {
    std::smatch match;
    if (!std::regex_match(path, match, regex)) {
        return false;
    }
    
    // Extract parameters
    for (size_t i = 0; i < params.size() && i + 1 < match.size(); ++i) {
        outParams[params[i].name] = match[i + 1].str();
    }
    
    return true;
}

// ============================================================================
// Route Implementation
// ============================================================================

bool Route::matches(HttpMethod m, const std::string& path,
                    std::unordered_map<std::string, std::string>& outParams) const {
    // Check method
    if (method != HttpMethod::ANY && method != m) {
        return false;
    }
    
    // Check path pattern
    return pattern.matches(path, outParams);
}

// ============================================================================
// Router Implementation
// ============================================================================

Router::Router(const std::string& prefix) : prefix_(prefix) {}

void Router::addRoute(HttpMethod method, const std::string& path, 
                      Handler handler, const std::string& name) {
    Route route;
    route.method = method;
    route.pattern = RoutePattern::compile(pathJoin(prefix_, path));
    route.handlers.push_back(handler);
    route.name = name;
    
    // Insert in order of priority (higher priority first)
    auto it = routes_.begin();
    while (it != routes_.end() && it->pattern.priority > route.pattern.priority) {
        ++it;
    }
    auto insertedIt = routes_.insert(it, std::move(route));
    
    // Add to Radix Tree for fast lookup
    addToRadix(*insertedIt);
}

void Router::addToRadix(Route& route) {
    std::string_view path = route.pattern.pattern;
    RadixNode* current = &radixRoot_;
    
    size_t start = 0;
    while (start < path.size()) {
        if (path[start] == '/') { start++; continue; }
        size_t end = path.find('/', start);
        if (end == std::string_view::npos) end = path.size();
        std::string_view seg = path.substr(start, end - start);
        
        if (!seg.empty() && seg[0] == ':') {
            if (!current->paramChild) {
                current->paramChild = std::make_unique<RadixNode>();
                current->paramName = std::string(seg.substr(1));
            }
            current = current->paramChild.get();
        } else if (seg == "*" || seg == "**") {
            if (!current->wildcardChild) {
                current->wildcardChild = std::make_unique<RadixNode>();
            }
            current = current->wildcardChild.get();
        } else {
            current = current->getOrCreateStatic(seg);
        }
        
        start = end;
    }
    current->route = &route;
}

Router& Router::get(const std::string& path, Handler handler) {
    addRoute(HttpMethod::GET, path, handler);
    return *this;
}

Router& Router::post(const std::string& path, Handler handler) {
    addRoute(HttpMethod::POST, path, handler);
    return *this;
}

Router& Router::put(const std::string& path, Handler handler) {
    addRoute(HttpMethod::PUT, path, handler);
    return *this;
}

Router& Router::del(const std::string& path, Handler handler) {
    addRoute(HttpMethod::DELETE_, path, handler);
    return *this;
}

Router& Router::patch(const std::string& path, Handler handler) {
    addRoute(HttpMethod::PATCH, path, handler);
    return *this;
}

Router& Router::head(const std::string& path, Handler handler) {
    addRoute(HttpMethod::HEAD, path, handler);
    return *this;
}

Router& Router::options(const std::string& path, Handler handler) {
    addRoute(HttpMethod::OPTIONS, path, handler);
    return *this;
}

Router& Router::any(const std::string& path, Handler handler) {
    addRoute(HttpMethod::ANY, path, handler);
    return *this;
}

Router& Router::route(const std::vector<HttpMethod>& methods, const std::string& path, Handler handler) {
    for (HttpMethod m : methods) {
        addRoute(m, path, handler);
    }
    return *this;
}

Router& Router::get(const std::string& path, Handler handler, const std::string& name) {
    addRoute(HttpMethod::GET, path, handler, name);
    return *this;
}

Router& Router::use(Handler middleware) {
    middleware_.emplace_back("", middleware);
    return *this;
}

Router& Router::use(const std::string& path, Handler middleware) {
    middleware_.emplace_back(pathJoin(prefix_, path), middleware);
    return *this;
}

Router& Router::mount(const std::string& path, std::shared_ptr<Router> subRouter) {
    subRouters_.emplace_back(pathJoin(prefix_, path), subRouter);
    return *this;
}

bool Router::findRoute(HttpMethod method, std::string_view path,
                       Route*& outRoute, std::unordered_map<std::string, std::string>& outParams) {
    // Check sub-routers first (they have their own prefix handled by findRoute)
    for (auto& [mountPath, subRouter] : subRouters_) {
        if (path.find(mountPath) == 0) {
            if (subRouter->findRoute(method, path, outRoute, outParams)) {
                return true;
            }
        }
    }
    
    // Use Radix Tree for fast lookup of own routes
    RadixNode* current = &radixRoot_;
    size_t start = 0;
    
    while (start < path.size()) {
        if (path[start] == '/') { start++; continue; }
        size_t end = path.find('/', start);
        if (end == std::string_view::npos) end = path.size();
        std::string_view seg = path.substr(start, end - start);
        
        // Try static match first
        auto it = current->staticChildren.find(std::string(seg));
        if (it != current->staticChildren.end()) {
            current = it->second.get();
        } else if (current->paramChild) {
            outParams[current->paramName] = std::string(seg);
            current = current->paramChild.get();
        } else if (current->wildcardChild) {
            current = current->wildcardChild.get();
        } else {
            return false;
        }
        
        start = end;
    }
    
    if (current->route && (current->route->method == HttpMethod::ANY || current->route->method == method)) {
        outRoute = current->route;
        return true;
    }
    
    return false;
}

} // namespace bedrock
