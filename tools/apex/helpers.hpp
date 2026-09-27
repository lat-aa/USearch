/**
 * @file helpers.hpp
 * @brief gate 纯辅助：kind/冲突/解析/终态/证据/命令包与人读 corpus。
 *
 * 无 Runtime / 无 LLM，供 gate.cpp 与 tests/apex 共用（避免测试链拉 httplib/llama）。
 * 命名遵守 .config/rules/names.md：单单词文件名，禁止 _/-。
 */
#pragma once

#include "verdict.hpp"

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

/** 与 api::Doc 字段对齐的轻量视图；测试不必链 Store。 */
struct Hitdoc {
    std::string id;
    std::string text;
    json meta = json::object();
};

/** 与 Resolvedrule 对齐的轻量视图（仅证据/打包需要的字段）。 */
struct Rulehit {
    std::string name;
    std::string body;
    /** 0=Always 1=Glob 2=Semantic 3=Manual，与 Activation 序一致。 */
    std::uint8_t activation = 2;
    float score = 0.0f;
};

/** buildPack 所需配置（从 Gateconfig / Decider 抽出，避免依赖 Runtime）。 */
struct Packcfg {
    std::size_t packtok = 2048;
    int baseSecs = 3;
};

/** finalizeStatus 所需门控权重（避免拉完整 Gateconfig）。 */
struct Gateweights {
    float threshold = 0.85f;
    float wevid = 0.45f;
    float wself = 0.55f;
    std::size_t minlen = 8;
};

/**
 * kind 判定：无 kind 时按 memory/experience 缺省；禁止 value<string> 抛 type_error。
 */
inline bool kindIs(json const& meta, char const* want) noexcept {
    bool const wantMem = std::strcmp(want, "memory") == 0 || std::strcmp(want, "experience") == 0;
    if (!meta.is_object())
        return wantMem;
    auto it = meta.find("kind");
    if (it == meta.end())
        return wantMem;
    if (std::string const* k = it->get_ptr<std::string const*>())
        return *k == want;
    return wantMem;
}

inline bool kindIs(Hitdoc const& doc, char const* want) noexcept {
    return kindIs(doc.meta, want);
}

/** meta 字符串字段；类型不对返回空 view，绝不抛。 */
inline std::string_view metaStr(json const& meta, char const* key) noexcept {
    if (!meta.is_object())
        return {};
    auto it = meta.find(key);
    if (it == meta.end())
        return {};
    if (std::string const* s = it->get_ptr<std::string const*>())
        return *s;
    return {};
}

inline std::string extractJsonObject(std::string const& text) {
    auto start = text.find('{');
    auto end = text.rfind('}');
    if (start == std::string::npos || end == std::string::npos || end <= start)
        return {};
    return text.substr(start, end - start + 1);
}

inline json parseGateModel(std::string const& raw) {
    std::string slice = extractJsonObject(raw);
    if (slice.empty())
        return json::object();
    try {
        return json::parse(slice);
    } catch (...) {
        return json::object();
    }
}

/** meta.conflict 只认 bool；用 get_ptr 避免 value/get 抛 type_error。 */
inline bool metaConflicted(json const& meta) noexcept {
    if (!meta.is_object())
        return false;
    auto it = meta.find("conflict");
    if (it == meta.end())
        return false;
    if (bool const* flag = it->get_ptr<bool const*>())
        return *flag;
    return false;
}

/**
 * 轻量冲突：硬政策名指针 + 禁用词政策名指针 + task 正文 + 记忆命中。
 */
inline json detectConflicts(std::vector<std::string const*> const& hard,
                            std::vector<std::string const*> const& ban, std::string_view taskText,
                            std::vector<std::pair<Hitdoc, float>> const& hits) {
    constexpr char const* kWhyNames = "names: identifier must not use underscore or hyphen";
    if (hard.empty())
        return json::array();

    json conflicts = json::array();
    auto emitRed = [&](bool red) {
        if (!red || ban.empty())
            return;
        for (std::string const* name : ban)
            conflicts.push_back({{"rule", *name}, {"why", kWhyNames}});
    };

    emitRed(scanRed(taskText));
    for (auto const& hit : hits) {
        Hitdoc const& doc = hit.first;
        if (metaConflicted(doc.meta)) {
            for (std::string const* name : hard)
                conflicts.push_back({{"rule", *name}, {"why", "memory meta.conflict"}});
        }
        emitRed(scanRed(doc.text));
    }
    return conflicts;
}

inline float evidenceOf(std::vector<std::pair<Hitdoc, float>> const& hits,
                        std::vector<Rulehit> const& rules) {
    float top = hits.empty() ? 0.0f : asSim(hits[0].second);
    float cover = clamp01(static_cast<float>(hits.size()) / 4.0f);
    float pol = 0.0f;
    for (auto const& r : rules) {
        if (r.activation == 0 || r.activation == 1) // Always / Glob
            pol = (std::max)(pol, 0.35f);
        if (r.activation == 2) // Semantic
            pol = (std::max)(pol, clamp01(r.score));
    }
    return clamp01(0.5f * top + 0.25f * cover + 0.25f * pol);
}

inline json finalizeStatus(json model, float evidence, Gateweights const& g) {
    float self = 0.0f;
    if (auto it = model.find("self"); it != model.end() && it->is_number())
        self = clamp01(it->get<float>());
    float conf = mixConfidence(evidence, self, g.wevid, g.wself);

    json conflicts = json::array();
    if (auto it = model.find("conflicts"); it != model.end() && it->is_array())
        conflicts = *it;
    json missing = json::array();
    if (auto it = model.find("missing"); it != model.end() && it->is_array())
        missing = *it;

    std::string status = "pack";
    if (auto it = model.find("status"); it != model.end() && it->is_string())
        status = it->get<std::string>();
    std::string reply;
    if (auto it = model.find("reply"); it != model.end() && it->is_string())
        reply = it->get_ref<std::string const&>();

    std::string const modelStatus = status;
    status = verdictOf(!conflicts.empty(), !missing.empty(), conf, g.threshold, modelStatus.c_str(),
                       reply.size(), g.minlen);

    model["status"] = std::move(status);
    model["answerConfidence"] = conf;
    model["evidence"] = evidence;
    model["self"] = self;
    model["conflicts"] = std::move(conflicts);
    return model;
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

/**
 * 组装 gate.pack；estimate/compress 用松散实现，生产路径 gate.cpp 可再包一层用精确 estimate。
 */
inline json buildPack(Packcfg const& cfg, std::string const& task,
                      std::vector<std::pair<Hitdoc, float>> const& hits,
                      std::vector<Rulehit> const& rules, json const& decisionSlice,
                      json const& modelPack) {
    json pack = json::array();
    std::unordered_set<std::string> seen;
    seen.reserve(16);
    std::size_t budget = cfg.packtok;
    std::size_t used = 0;
    int const baseSecs = cfg.baseSecs;
    auto tryAdd = [&](std::string id, std::string span, float score, char const* why) {
        if (!seen.insert(id).second)
            return;
        std::size_t cost = estimateLoose(span) + estimateLoose(id) + 8;
        if (used + cost > budget && !pack.empty())
            return;
        pack.push_back({{"id", std::move(id)},
                        {"span", std::move(span)},
                        {"score", score},
                        {"why", why}});
        used += cost;
    };
    for (auto const& r : rules) {
        if (r.activation != 0 && r.activation != 1)
            continue;
        std::string body = compressLoose(r.body, static_cast<std::size_t>(baseSecs));
        tryAdd("rule:" + r.name, std::move(body), 1.0f, "hard policy");
    }
    (void)task;
    for (auto const& [doc, score] : hits) {
        std::string span = compressLoose(doc.text, 2);
        if (span.size() > 400)
            span.resize(400);
        tryAdd(doc.id, std::move(span), score, "memory");
    }
    if (modelPack.is_array()) {
        for (auto const& m : modelPack) {
            if (!m.is_object())
                continue;
            std::string id = m.value("id", "");
            if (id.empty() || seen.count(id))
                continue;
            tryAdd(std::move(id), m.value("span", ""), 0.0f, "model");
        }
    }
    return {{"items", std::move(pack)}, {"decision", decisionSlice}, {"tokens", used}};
}

/** 0..1 → 百分整数；非法则 -1。 */
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
inline std::string formatCorpus(std::string const& prompt) {
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

} // namespace api
