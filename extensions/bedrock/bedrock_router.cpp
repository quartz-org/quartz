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

std::string urlDecode(const std::string& encoded) {
    std::string result;
    result.reserve(encoded.size());
    
    for (size_t i = 0; i < encoded.size(); ++i) {
        if (encoded[i] == '%' && i + 2 < encoded.size()) {
            int value;
            std::istringstream iss(encoded.substr(i + 1, 2));
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

std::string urlEncode(const std::string& str) {
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

std::unordered_map<std::string, std::string> parseQueryString(const std::string& query) {
    std::unordered_map<std::string, std::string> result;
    if (query.empty()) return result;
    
    std::istringstream stream(query);
    std::string pair;
    
    while (std::getline(stream, pair, '&')) {
        size_t eqPos = pair.find('=');
        if (eqPos != std::string::npos) {
            std::string key = urlDecode(pair.substr(0, eqPos));
            std::string value = urlDecode(pair.substr(eqPos + 1));
            result[key] = value;
        } else if (!pair.empty()) {
            result[urlDecode(pair)] = "";
        }
    }
    return result;
}

std::unordered_map<std::string, std::string> parseCookies(const std::string& cookieHeader) {
    std::unordered_map<std::string, std::string> result;
    if (cookieHeader.empty()) return result;
    
    std::istringstream stream(cookieHeader);
    std::string pair;
    
    while (std::getline(stream, pair, ';')) {
        // Trim leading whitespace
        size_t start = pair.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        pair = pair.substr(start);
        
        size_t eqPos = pair.find('=');
        if (eqPos != std::string::npos) {
            std::string key = pair.substr(0, eqPos);
            std::string value = pair.substr(eqPos + 1);
            result[key] = value;
        }
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

std::string normalizePath(const std::string& path) {
    if (path.empty() || path == "/") return "/";
    
    std::vector<std::string> segments;
    std::istringstream stream(path);
    std::string segment;
    
    while (std::getline(stream, segment, '/')) {
        if (segment.empty() || segment == ".") continue;
        if (segment == "..") {
            if (!segments.empty()) segments.pop_back();
        } else {
            segments.push_back(segment);
        }
    }
    
    std::string result = "/";
    for (size_t i = 0; i < segments.size(); ++i) {
        result += segments[i];
        if (i < segments.size() - 1) result += "/";
    }
    
    return result;
}

std::string pathJoin(const std::string& base, const std::string& path) {
    if (base.empty()) return path;
    if (path.empty()) return base;
    
    bool baseHasSlash = base.back() == '/';
    bool pathHasSlash = path.front() == '/';
    
    if (baseHasSlash && pathHasSlash) {
        return base + path.substr(1);
    } else if (!baseHasSlash && !pathHasSlash) {
        return base + "/" + path;
    }
    return base + path;
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

std::string Request::getHeader(const std::string& name) const {
    std::string lowerName = name;
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
    
    for (const auto& [key, value] : headers) {
        std::string lowerKey = key;
        std::transform(lowerKey.begin(), lowerKey.end(), lowerKey.begin(), ::tolower);
        if (lowerKey == lowerName) return value;
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
    return contentType.find("application/json") != std::string::npos;
}

bool Request::isForm() const {
    return contentType.find("application/x-www-form-urlencoded") != std::string::npos;
}

bool Request::isMultipart() const {
    return contentType.find("multipart/form-data") != std::string::npos;
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
    std::ostringstream ss;
    
    // Status line
    ss << "HTTP/1.1 " << statusCode << " " << statusText << "\r\n";
    
    // Headers
    for (const auto& [name, value] : headers) {
        ss << name << ": " << value << "\r\n";
    }
    
    // Add Content-Length if not present and we have a body
    if (headers.find("Content-Length") == headers.end() && !body.empty()) {
        ss << "Content-Length: " << body.size() << "\r\n";
    }
    
    // Add Server header
    if (headers.find("Server") == headers.end()) {
        ss << "Server: Bedrock/1.0 (Quartz)\r\n";
    }
    
    // End headers
    ss << "\r\n";
    
    // Body
    if (!body.empty()) {
        ss << body;
    }
    
    return ss.str();
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
    routes_.insert(it, std::move(route));
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

bool Router::findRoute(HttpMethod method, const std::string& path,
                       Route*& outRoute, std::unordered_map<std::string, std::string>& outParams) {
    // First check sub-routers
    for (auto& [mountPath, subRouter] : subRouters_) {
        if (path.find(mountPath) == 0 || mountPath.empty()) {
            if (subRouter->findRoute(method, path, outRoute, outParams)) {
                return true;
            }
        }
    }
    
    // Then check own routes
    for (Route& route : routes_) {
        if (route.matches(method, path, outParams)) {
            outRoute = &route;
            return true;
        }
    }
    
    return false;
}

} // namespace bedrock
