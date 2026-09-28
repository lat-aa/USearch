/**
 * @file slim.hpp
 * @brief 固定 schema 轻量 JSON 抽取：gate/decide 热路径免整树 DOM。
 *
 * 只认顶层键 task|query、files[]、manual[]、latency、hints[]；其余键跳过。
 * 非法输入 ok=false；成功时至少得到根对象（task 可为空，由调用方校验）。
 */
#pragma once

#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace api {

using json = nlohmann::json;

struct Slimargs {
    std::string task;
    std::vector<std::string> files;
    std::vector<std::string> manual;
    std::optional<std::uint64_t> latency;
    std::vector<std::string> hints;
    bool ok = false;
};

namespace slimdetail {

inline void skipWs(std::string_view& s) noexcept {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
}

inline bool consume(std::string_view& s, char c) noexcept {
    skipWs(s);
    if (s.empty() || s.front() != c)
        return false;
    s.remove_prefix(1);
    return true;
}

/** 解析 JSON 字符串（含 \" \\ 等常见转义）；失败返回 nullopt。 */
inline std::optional<std::string> parseString(std::string_view& s) {
    skipWs(s);
    if (s.empty() || s.front() != '"')
        return std::nullopt;
    s.remove_prefix(1);
    std::string out;
    out.reserve(32);
    while (!s.empty()) {
        char c = s.front();
        s.remove_prefix(1);
        if (c == '"')
            return out;
        if (c == '\\') {
            if (s.empty())
                return std::nullopt;
            char e = s.front();
            s.remove_prefix(1);
            switch (e) {
            case '"':
            case '\\':
            case '/': out.push_back(e); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u':
                // 跳过 4 hex；不解码为码点（gate 请求几乎不用），避免半套 UTF-16
                if (s.size() < 4)
                    return std::nullopt;
                s.remove_prefix(4);
                out.push_back('?');
                break;
            default: return std::nullopt;
            }
        } else {
            out.push_back(c);
        }
    }
    return std::nullopt;
}

/** 跳过一个 JSON 值（对象/数组/字面量），用于忽略未知键。 */
inline bool skipValue(std::string_view& s) {
    skipWs(s);
    if (s.empty())
        return false;
    char c = s.front();
    if (c == '"')
        return parseString(s).has_value();
    if (c == '{' || c == '[') {
        char open = c;
        char close = (c == '{') ? '}' : ']';
        int depth = 0;
        bool inStr = false;
        bool esc = false;
        for (std::size_t i = 0; i < s.size(); ++i) {
            char ch = s[i];
            if (inStr) {
                if (esc)
                    esc = false;
                else if (ch == '\\')
                    esc = true;
                else if (ch == '"')
                    inStr = false;
                continue;
            }
            if (ch == '"') {
                inStr = true;
                continue;
            }
            if (ch == open)
                ++depth;
            else if (ch == close) {
                --depth;
                if (depth == 0) {
                    s.remove_prefix(i + 1);
                    return true;
                }
            }
        }
        return false;
    }
    // number / true / false / null
    while (!s.empty() && !std::isspace(static_cast<unsigned char>(s.front())) && s.front() != ',' && s.front() != '}' &&
           s.front() != ']')
        s.remove_prefix(1);
    return true;
}

inline std::optional<std::uint64_t> parseUint(std::string_view& s) {
    skipWs(s);
    if (s.empty() || !std::isdigit(static_cast<unsigned char>(s.front())))
        return std::nullopt;
    std::uint64_t v = 0;
    while (!s.empty() && std::isdigit(static_cast<unsigned char>(s.front()))) {
        v = v * 10 + static_cast<std::uint64_t>(s.front() - '0');
        s.remove_prefix(1);
    }
    return v;
}

inline bool parseStringArray(std::string_view& s, std::vector<std::string>& out) {
    if (!consume(s, '['))
        return false;
    skipWs(s);
    if (consume(s, ']'))
        return true;
    for (;;) {
        auto item = parseString(s);
        if (!item)
            return false;
        out.push_back(std::move(*item));
        skipWs(s);
        if (consume(s, ']'))
            return true;
        if (!consume(s, ','))
            return false;
    }
}

} // namespace slimdetail

/**
 * 从请求体抽取 gate/decide 固定字段。
 * @return ok=false 表示根不是对象或语法坏掉；ok=true 时字段可为默认空。
 */
inline Slimargs parseSlim(std::string_view body) {
    using namespace slimdetail;
    Slimargs a;
    skipWs(body);
    if (!consume(body, '{'))
        return a;
    skipWs(body);
    if (consume(body, '}')) {
        a.ok = true;
        return a;
    }
    for (;;) {
        auto key = parseString(body);
        if (!key || !consume(body, ':'))
            return Slimargs{};
        if (*key == "task" || *key == "query") {
            auto v = parseString(body);
            if (!v)
                return Slimargs{};
            // task 优先；仅当 task 仍空时才用 query
            if (*key == "task" || a.task.empty())
                a.task = std::move(*v);
        } else if (*key == "files") {
            if (!parseStringArray(body, a.files))
                return Slimargs{};
        } else if (*key == "manual") {
            if (!parseStringArray(body, a.manual))
                return Slimargs{};
        } else if (*key == "latency") {
            auto n = parseUint(body);
            if (!n)
                return Slimargs{};
            a.latency = *n;
        } else if (*key == "hints") {
            if (!parseStringArray(body, a.hints))
                return Slimargs{};
        } else {
            if (!skipValue(body))
                return Slimargs{};
        }
        skipWs(body);
        if (consume(body, '}')) {
            a.ok = true;
            return a;
        }
        if (!consume(body, ','))
            return Slimargs{};
    }
}

/** 转成 decideinputFromJson /route 仍能吃的最小 DOM（仅必要键）。 */
inline json slimToJson(Slimargs const& a) {
    json j = json::object();
    j["task"] = a.task;
    if (!a.files.empty())
        j["files"] = a.files;
    if (!a.manual.empty())
        j["manual"] = a.manual;
    if (a.latency)
        j["latency"] = *a.latency;
    if (!a.hints.empty())
        j["hints"] = a.hints;
    return j;
}

} // namespace api
