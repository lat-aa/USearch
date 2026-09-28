/**
 * @file helpers.hpp
 * @brief gate 纯辅助：kind/冲突/解析/终态/证据/命令包与人读 corpus。
 *
 * 无 Runtime / 无 LLM，供 gate.cpp 与 tests/apex 共用（避免测试链拉 httplib/llama）。
 * 命名遵守 .config/rules/names.md：单单词文件名，禁止 _/-。
 */
#pragma once


#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace api {

using json = nlohmann::json;

/** 去推理包裹/特殊 token（定义见文末；此处前置声明供 parseGateModel 用）。 */
inline std::string stripThink(std::string text);

/**
 * 取第一个「大括号平衡且引号感知」的 JSON 对象。
 * 推理模型正文里常含 `{`（如示例 JSON），用 rfind('}') 会跨段取错。
 */
inline std::string extractJsonObject(std::string const& text) {
    std::size_t start = text.find('{');
    if (start == std::string::npos)
        return {};
    int depth = 0;
    bool inStr = false;
    bool esc = false;
    for (std::size_t i = start; i < text.size(); ++i) {
        char const c = text[i];
        if (inStr) {
            if (esc)
                esc = false;
            else if (c == '\\')
                esc = true;
            else if (c == '"')
                inStr = false;
            continue;
        }
        if (c == '"') {
            inStr = true;
            continue;
        }
        if (c == '{')
            ++depth;
        else if (c == '}') {
            --depth;
            if (depth == 0)
                return text.substr(start, i - start + 1);
        }
    }
    return {};
}

/** 去掉模型推理包裹（<think>…</think>）与 llama 特殊 token（<|…|>），得到可落盘的纯文本。 */
inline std::string stripThink(std::string text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        if (text.compare(i, 7, "<think>") == 0) {
            std::size_t end = text.find("</think>", i);
            if (end == std::string::npos)
                break;
            i = end + 8;
            continue;
        }
        out.push_back(text[i]);
        ++i;
    }
    std::string cleaned;
    cleaned.reserve(out.size());
    for (std::size_t i = 0; i < out.size();) {
        if (out[i] == '<' && i + 1 < out.size() && out[i + 1] == '|') {
            std::size_t end = out.find("|>", i);
            if (end != std::string::npos) {
                i = end + 2;
                continue;
            }
        }
        cleaned.push_back(out[i]);
        ++i;
    }
    std::size_t b = cleaned.find_first_not_of(" \t\r\n");
    std::size_t e = cleaned.find_last_not_of(" \t\r\n");
    if (b == std::string::npos)
        return {};
    return cleaned.substr(b, e - b + 1);
}
/**
 * 标签隔离：丢弃全部 <think> 推理，只取 <agent-result>…</agent-result> 内部的 JSON。
 * 缺失开始/结束标签、截断、JSON 解析失败 → 返回空对象（判失效 → 上层 delegate）。
 */
inline json extractAgentResult(std::string_view raw) {
    std::string text = stripThink(std::string(raw));
    std::size_t a = text.find("<agent-result>");
    std::size_t b = text.find("</agent-result>");
    if (a == std::string::npos || b == std::string::npos || b <= a)
        return json();
    std::string inner = text.substr(a + 15, b - a - 15); // len("<agent-result>")==15
    std::string slice = extractJsonObject(inner);
    if (slice.empty())
        return json();
    try {
        json j = json::parse(slice);
        return j.is_object() ? j : json();
    } catch (...) {
        return json();
    }
}

/** 协议判定：仅 status=="ok" 视为本地完成，其余（delegate/缺失/非法）一律交上游。 */
inline bool agentOk(json const& j) {
    return j.is_object() && j.value("status", "") == "ok";
}
/** 粗估 token（与 text.cpp estimate 同量级；测试与打包共用，免链 text）。 */
inline std::size_t estimateLoose(std::string_view text) noexcept {
    return (text.size() + 3) / 4;
}

/** 截断段：按空行切，保留前 maxSections 段。 */
inline std::string compressLoose(std::string_view body, std::size_t maxSections) {
    if (maxSections == 0 || body.empty())
        return {};
    std::string out;
    std::size_t sections = 0;
    std::size_t i = 0;
    while (i < body.size() && sections < maxSections) {
        if (!out.empty())
            out.push_back('\n');
        std::size_t start = i;
        while (i < body.size()) {
            if (body[i] == '\n' && i + 1 < body.size() && body[i + 1] == '\n')
                break;
            ++i;
        }
        out.append(body.data() + start, i - start);
        ++sections;
        while (i < body.size() && body[i] == '\n')
            ++i;
    }
    return out;
}

inline int pct01(json const& j, char const* key) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_number())
        return -1;
    float x = it->get<float>();
    if (!(x >= 0.f) || !(x <= 1.f))
        return -1;
    return static_cast<int>(std::lround(x * 100.f));
}

/** 规则正文写入 corpus 前去掉 CR。 */
inline std::string bodyForCorpus(std::string_view body) {
    std::string out;
    out.reserve(body.size());
    for (char c : body) {
        if (c != '\r')
            out.push_back(c);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == ' '))
        out.pop_back();
    return out;
}

/**
 * 从 knowledge 对象生成人读 markdown（不含 ## prompt 头）。
 * 供 Turnstats::rebuildCorpus 与单测共用。
 */
inline std::string formatCorpusBody(json const& knowledge) {
    std::ostringstream oss;
    if (auto it = knowledge.find("decision"); it != knowledge.end() && it->is_object()) {
        auto const& d = *it;
        oss << "路由 " << d.value("model", "?") << " · " << d.value("depth", "?") << " · "
            << d.value("retrieval", "?");
        int const retainPct = pct01(d, "compression");
        if (retainPct >= 0)
            oss << " · 保留 " << retainPct << "%";
        int const confPct = pct01(d, "confidence");
        if (confPct >= 0)
            oss << " · 置信 " << confPct << "%";
        if (auto t = d.find("temperature"); t != d.end() && t->is_number())
            oss << " · temp " << t->get<float>();
        oss << '\n';
        if (auto rs = d.find("reasons"); rs != d.end() && rs->is_array() && !rs->empty()) {
            oss << "依据";
            for (auto const& r : *rs) {
                if (!r.is_string())
                    continue;
                oss << " · " << r.get_ref<std::string const&>();
            }
            oss << '\n';
        }
    }

    std::size_t hitN = 0;
    if (auto hits = knowledge.find("hits"); hits != knowledge.end() && hits->is_array())
        hitN = hits->size();
    oss << "hits " << (hitN == 0 ? "无" : std::to_string(hitN)) << '\n';

    if (auto pack = knowledge.find("pack"); pack != knowledge.end() && pack->is_object()) {
        std::size_t n = 0;
        if (auto items = pack->find("items"); items != pack->end() && items->is_array())
            n = items->size();
        oss << "gatePack " << n << " 条\n";
    }

    if (auto rules = knowledge.find("rules"); rules != knowledge.end() && rules->is_array()) {
        std::vector<std::string> names;
        names.reserve(rules->size());
        for (auto const& r : *rules) {
            if (!r.is_object())
                continue;
            std::string name = r.value("name", r.value("id", ""));
            if (!name.empty())
                names.push_back(std::move(name));
        }
        oss << "规则";
        if (names.empty()) {
            oss << " 无\n";
        } else {
            for (std::size_t i = 0; i < names.size(); ++i)
                oss << (i == 0 ? " " : " · ") << names[i];
            oss << '\n';
            for (auto const& r : *rules) {
                if (!r.is_object())
                    continue;
                std::string name = r.value("name", r.value("id", ""));
                std::string body = r.value("body", "");
                if (name.empty() || body.empty())
                    continue;
                oss << '\n' << "### " << name << '\n' << bodyForCorpus(body) << '\n';
            }
        }
    }
    return oss.str();
}

/**
 * 统计块 corpus：解析 `Local knowledge JSON follows.` 后的 JSON 为人读 markdown。
 * 解析失败时回退原文，避免丢信息。
 */
inline std::string formatCorpusText(std::string const& prompt) {
    if (prompt.empty())
        return {};
    constexpr char const* kPrefix = "Local knowledge JSON follows.";
    json knowledge;
    bool parsed = false;
    if (prompt.rfind(kPrefix, 0) == 0) {
        std::size_t i = std::strlen(kPrefix);
        while (i < prompt.size() && (prompt[i] == '\n' || prompt[i] == '\r'))
            ++i;
        try {
            knowledge = json::parse(prompt.substr(i));
            parsed = knowledge.is_object();
        } catch (...) {
            parsed = false;
        }
    }
    std::ostringstream oss;
    oss << "## prompt\n";
    if (!parsed) {
        oss << prompt;
        if (!prompt.empty() && prompt.back() != '\n')
            oss << '\n';
        return oss.str();
    }
    oss << formatCorpusBody(knowledge);
    return oss.str();
}

/**
 * 组装模型侧 prompt（完整 knowledge JSON）。
 * decisionJson 须已是 Decision::toJson() 形态。
 */
inline std::string formatPrompt(json const& rulesArr, json const& decisionJson, json const& hitsArr,
                                json const* packRawOrNull) {
    json knowledge = {{"hits", hitsArr.is_null() ? json::array() : hitsArr},
                      {"rules", rulesArr},
                      {"decision", decisionJson}};
    if (packRawOrNull && packRawOrNull->is_object()) {
        auto items = packRawOrNull->find("items");
        if (items != packRawOrNull->end() && items->is_array() && !items->empty())
            knowledge["pack"] = *packRawOrNull;
    }
    return std::string("Local knowledge JSON follows.\n") + knowledge.dump(2);
}

// ---- 规范消息：{role, content:[{type:"text", text}]} ----

/** content 展平为文本：字符串直取；数组逐部件取 text；对象取 text。 */
inline std::string textOf(json const& content) {
    if (content.is_string())
        return content.get<std::string>();
    if (content.is_array()) {
        std::ostringstream ss;
        for (auto const& part : content) {
            if (part.is_string())
                ss << part.get<std::string>() << '\n';
            else if (part.is_object() && part.contains("text") && part["text"].is_string())
                ss << part["text"].get<std::string>() << '\n';
        }
        return ss.str();
    }
    if (content.is_object() && content.contains("text") && content["text"].is_string())
        return content["text"].get<std::string>();
    return content.dump();
}

/** 规范文本部件：{type:"text", text}。 */
inline json textPart(std::string text) {
    json part;
    part["type"] = "text";
    part["text"] = std::move(text);
    return part;
}

/** 规范消息：{role, content:[{type:"text",text}]}。 */
inline json textMessage(std::string role, std::string text) {
    json msg;
    msg["role"] = std::move(role);
    msg["content"] = json::array({textPart(std::move(text))});
    return msg;
}

/** role 归一：developer 当 system；空当 user。 */
inline std::string normalizeRole(std::string const& role) {
    if (role == "developer")
        return "system";
    if (role.empty())
        return "user";
    return role;
}

/** content 归一：只保留文本部件 {type:"text",text}，非文本部件丢弃。 */
inline json normalizeContent(json const& content) {
    json parts = json::array();
    if (content.is_string()) {
        parts.push_back(textPart(content.get<std::string>()));
    } else if (content.is_array()) {
        for (auto const& part : content) {
            if (part.is_string())
                parts.push_back(textPart(part.get<std::string>()));
            else if (part.is_object() && part.contains("text") && part["text"].is_string())
                parts.push_back(textPart(part["text"].get<std::string>()));
        }
    } else if (content.is_object() && content.contains("text") && content["text"].is_string()) {
        parts.push_back(textPart(content["text"].get<std::string>()));
    }
    return parts;
}

/** 规范消息数组：每条 {role, content:[{type:"text",text}]}。 */
inline json normalizeMessages(json const& messages) {
    json out = json::array();
    if (!messages.is_array())
        return out;
    for (auto const& m : messages) {
        if (!m.is_object())
            continue;
        json norm;
        norm["role"] = normalizeRole(m.value("role", ""));
        norm["content"] = normalizeContent(m.value("content", json()));
        out.push_back(std::move(norm));
    }
    return out;
}

/** Codex Responses `input` → 规范 chat messages（content 恒为文本部件数组）。 */
inline json messagesFromResponses(json const& body) {
    json messages = json::array();
    if (body.contains("instructions") && body["instructions"].is_string()) {
        std::string ins = body["instructions"].get<std::string>();
        if (!ins.empty())
            messages.push_back(textMessage("system", std::move(ins)));
    }
    json input = body.contains("input") ? body["input"] : json();
    auto push = [&](std::string const& role, json const& content) {
        json norm;
        norm["role"] = normalizeRole(role);
        norm["content"] = normalizeContent(content);
        messages.push_back(std::move(norm));
    };
    if (input.is_string()) {
        push("user", input.get<std::string>());
        return messages;
    }
    if (!input.is_array())
        return messages;
    for (auto const& item : input) {
        if (item.is_string()) {
            push("user", item.get<std::string>());
            continue;
        }
        if (!item.is_object())
            continue;
        std::string type = item.value("type", "");
        if (type == "function_call_output")
            continue;
        std::string role = item.value("role", type == "message" ? "user" : "");
        if (item.contains("content"))
            push(role, item["content"]);
        else if (item.contains("text") && item["text"].is_string())
            push(role, item["text"].get<std::string>());
    }
    return messages;
}

/** 统计块 corpus：从规范 messages 取 knowledge 消息文本，生成人读 markdown。 */
inline std::string formatCorpus(json const& messages) {
    if (!messages.is_array())
        return {};
    constexpr char const* kPrefix = "Local knowledge JSON follows.";
    for (auto const& m : messages) {
        if (!m.is_object())
            continue;
        std::string text = textOf(m.value("content", json()));
        if (text.rfind(kPrefix, 0) == 0)
            return formatCorpusText(text);
    }
    if (!messages.empty() && messages.front().is_object())
        return formatCorpusText(textOf(messages.front().value("content", json())));
    return {};
}

} // namespace api
