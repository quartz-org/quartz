#include "function_registry.h"
#include "runtime.h"
#include "types.h"

#include <regex>
#include <string>
#include <vector>

static inline Runtime* rt() {
    return global_runtime_ptr;
}

static inline bool isString(const Value& v) {
    return std::holds_alternative<std::string>(v);
}

static inline std::string getString(const Value& v, const std::string& def = "") {
    return isString(v) ? std::get<std::string>(v) : def;
}

void register_regex_functions(FunctionRegistry& reg) {
    
    // ========================================================================
    // system.regex.test(str, pattern) -> bool
    // Tests if a string matches a regex pattern
    // ========================================================================
    reg.registerFunction("system.regex.test", [](const std::vector<Value>& args) -> Value {
        if (args.size() < 2 || !isString(args[0]) || !isString(args[1])) {
            return Value(false);
        }
        
        std::string str = getString(args[0]);
        std::string pattern = getString(args[1]);
        
        try {
            std::regex re(pattern);
            return Value(std::regex_search(str, re));
        } catch (const std::regex_error&) {
            return Value(false);
        }
    });
    
    // ========================================================================
    // system.regex.match(str, pattern) -> array or empty array
    // Returns first match with groups, or empty array if no match
    // [fullMatch, group1, group2, ...]
    // ========================================================================
    reg.registerFunction("system.regex.match", [](const std::vector<Value>& args) -> Value {
        if (!rt() || args.size() < 2 || !isString(args[0]) || !isString(args[1])) {
            return rt() ? rt()->makeArray({}) : Value{};
        }
        
        std::string str = getString(args[0]);
        std::string pattern = getString(args[1]);
        
        try {
            std::regex re(pattern);
            std::smatch match;
            
            if (std::regex_search(str, match, re)) {
                std::vector<Value> result;
                result.reserve(match.size());
                for (size_t i = 0; i < match.size(); ++i) {
                    result.push_back(Value(match[i].str()));
                }
                return rt()->makeArray(std::move(result));
            }
            return rt()->makeArray({});
        } catch (const std::regex_error&) {
            return rt()->makeArray({});
        }
    });
    
    // ========================================================================
    // system.regex.matchAll(str, pattern) -> array of arrays
    // Returns all matches, each with groups
    // [[fullMatch1, group1, ...], [fullMatch2, group1, ...], ...]
    // ========================================================================
    reg.registerFunction("system.regex.matchAll", [](const std::vector<Value>& args) -> Value {
        if (!rt() || args.size() < 2 || !isString(args[0]) || !isString(args[1])) {
            return rt() ? rt()->makeArray({}) : Value{};
        }
        
        std::string str = getString(args[0]);
        std::string pattern = getString(args[1]);
        
        try {
            std::regex re(pattern);
            std::vector<Value> allMatches;
            
            auto begin = std::sregex_iterator(str.begin(), str.end(), re);
            auto end = std::sregex_iterator();
            
            for (auto it = begin; it != end; ++it) {
                std::smatch match = *it;
                std::vector<Value> groups;
                groups.reserve(match.size());
                for (size_t i = 0; i < match.size(); ++i) {
                    groups.push_back(Value(match[i].str()));
                }
                allMatches.push_back(rt()->makeArray(std::move(groups)));
            }
            
            return rt()->makeArray(std::move(allMatches));
        } catch (const std::regex_error&) {
            return rt()->makeArray({});
        }
    });
    
    // ========================================================================
    // system.regex.replace(str, pattern, replacement) -> string
    // Replaces all occurrences of pattern with replacement
    // Supports backreferences: $1, $2, etc.
    // ========================================================================
    reg.registerFunction("system.regex.replace", [](const std::vector<Value>& args) -> Value {
        if (args.size() < 3 || !isString(args[0]) || !isString(args[1]) || !isString(args[2])) {
            return args.empty() ? Value(std::string("")) : args[0];
        }
        
        std::string str = getString(args[0]);
        std::string pattern = getString(args[1]);
        std::string replacement = getString(args[2]);
        
        try {
            std::regex re(pattern);
            return Value(std::regex_replace(str, re, replacement));
        } catch (const std::regex_error&) {
            return Value(str);
        }
    });
    
    // ========================================================================
    // system.regex.replaceFirst(str, pattern, replacement) -> string
    // Replaces only the first occurrence
    // ========================================================================
    reg.registerFunction("system.regex.replaceFirst", [](const std::vector<Value>& args) -> Value {
        if (args.size() < 3 || !isString(args[0]) || !isString(args[1]) || !isString(args[2])) {
            return args.empty() ? Value(std::string("")) : args[0];
        }
        
        std::string str = getString(args[0]);
        std::string pattern = getString(args[1]);
        std::string replacement = getString(args[2]);
        
        try {
            std::regex re(pattern);
            return Value(std::regex_replace(str, re, replacement, 
                std::regex_constants::format_first_only));
        } catch (const std::regex_error&) {
            return Value(str);
        }
    });
    
    // ========================================================================
    // system.regex.split(str, pattern) -> array of strings
    // Splits string by regex pattern
    // ========================================================================
    reg.registerFunction("system.regex.split", [](const std::vector<Value>& args) -> Value {
        if (!rt() || args.size() < 2 || !isString(args[0]) || !isString(args[1])) {
            return rt() ? rt()->makeArray({}) : Value{};
        }
        
        std::string str = getString(args[0]);
        std::string pattern = getString(args[1]);
        
        try {
            std::regex re(pattern);
            std::vector<Value> result;
            
            std::sregex_token_iterator begin(str.begin(), str.end(), re, -1);
            std::sregex_token_iterator end;
            
            for (auto it = begin; it != end; ++it) {
                result.push_back(Value(it->str()));
            }
            
            return rt()->makeArray(std::move(result));
        } catch (const std::regex_error&) {
            // On error, return array with original string
            std::vector<Value> result;
            result.push_back(Value(str));
            return rt()->makeArray(std::move(result));
        }
    });
    
    // ========================================================================
    // system.regex.findIndex(str, pattern) -> int
    // Returns the index of the first match, or -1 if not found
    // ========================================================================
    reg.registerFunction("system.regex.findIndex", [](const std::vector<Value>& args) -> Value {
        if (args.size() < 2 || !isString(args[0]) || !isString(args[1])) {
            return Value(-1);
        }
        
        std::string str = getString(args[0]);
        std::string pattern = getString(args[1]);
        
        try {
            std::regex re(pattern);
            std::smatch match;
            
            if (std::regex_search(str, match, re)) {
                return Value(static_cast<int>(match.position(0)));
            }
            return Value(-1);
        } catch (const std::regex_error&) {
            return Value(-1);
        }
    });
    
    // ========================================================================
    // system.regex.count(str, pattern) -> int
    // Counts the number of matches
    // ========================================================================
    reg.registerFunction("system.regex.count", [](const std::vector<Value>& args) -> Value {
        if (args.size() < 2 || !isString(args[0]) || !isString(args[1])) {
            return Value(0);
        }
        
        std::string str = getString(args[0]);
        std::string pattern = getString(args[1]);
        
        try {
            std::regex re(pattern);
            auto begin = std::sregex_iterator(str.begin(), str.end(), re);
            auto end = std::sregex_iterator();
            return Value(static_cast<int>(std::distance(begin, end)));
        } catch (const std::regex_error&) {
            return Value(0);
        }
    });
    
    // ========================================================================
    // system.regex.isValid(pattern) -> bool
    // Checks if a pattern is a valid regex
    // ========================================================================
    reg.registerFunction("system.regex.isValid", [](const std::vector<Value>& args) -> Value {
        if (args.empty() || !isString(args[0])) {
            return Value(false);
        }
        
        std::string pattern = getString(args[0]);
        
        try {
            std::regex re(pattern);
            return Value(true);
        } catch (const std::regex_error&) {
            return Value(false);
        }
    });
    
    // ========================================================================
    // system.regex.escape(str) -> string
    // Escapes special regex characters in a string
    // ========================================================================
    reg.registerFunction("system.regex.escape", [](const std::vector<Value>& args) -> Value {
        if (args.empty() || !isString(args[0])) {
            return Value(std::string(""));
        }
        
        std::string str = getString(args[0]);
        std::string escaped;
        escaped.reserve(str.size() * 2);
        
        for (char c : str) {
            switch (c) {
                case '^': case '$': case '.': case '*': case '+':
                case '?': case '(': case ')': case '[': case ']':
                case '{': case '}': case '|': case '\\':
                    escaped += '\\';
                    escaped += c;
                    break;
                default:
                    escaped += c;
            }
        }
        
        return Value(escaped);
    });
    
    // ========================================================================
    // system.regex.extract(str, pattern) -> array
    // Extracts all matched substrings (first group if present, else full match)
    // ========================================================================
    reg.registerFunction("system.regex.extract", [](const std::vector<Value>& args) -> Value {
        if (!rt() || args.size() < 2 || !isString(args[0]) || !isString(args[1])) {
            return rt() ? rt()->makeArray({}) : Value{};
        }
        
        std::string str = getString(args[0]);
        std::string pattern = getString(args[1]);
        
        try {
            std::regex re(pattern);
            std::vector<Value> result;
            
            auto begin = std::sregex_iterator(str.begin(), str.end(), re);
            auto end = std::sregex_iterator();
            
            for (auto it = begin; it != end; ++it) {
                std::smatch match = *it;
                // If there's a capture group, use it; otherwise use full match
                if (match.size() > 1) {
                    result.push_back(Value(match[1].str()));
                } else {
                    result.push_back(Value(match[0].str()));
                }
            }
            
            return rt()->makeArray(std::move(result));
        } catch (const std::regex_error&) {
            return rt()->makeArray({});
        }
    });
}
