/**
 *  @file       rules.hpp
 *  @brief      Policy：规则加载/激活（Always/Glob/Semantic/Manual）+ 纯词法解析（inline）。
 *
 *  词法路径全在此 inline（无 Encoder、无 Runtime，可被 apexrules 直接验证）；
 *  Runtime 语义分支、经验落盘、token 估计/压缩在 rules.cpp。
 */
#pragma once

#include "types.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace api {

struct Runtime; // 仅按引用出现；定义见 api.hpp

inline std::string asciiLower(std::string_view text) {
    std::string out(text);
    for (char& c : out)
        if (static_cast<unsigned char>(c) <= 127)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

inline std::string normalizePath(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (char c : raw)
        out.push_back(c == '\\' ? '/' : c);
    while (out.size() >= 2 && out[0] == '.' && out[1] == '/')
        out.erase(0, 2);
    while (!out.empty() && out.back() == '/')
        out.pop_back();
    // collapse //
    std::string compact;
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (out[i] == '/' && !compact.empty() && compact.back() == '/')
            continue;
        compact.push_back(out[i]);
    }
    return compact;
}

inline std::string normalizeGlob(std::string_view raw) {
    std::string g = normalizePath(raw);
    if (g.find('/') == std::string::npos)
        g = "**/" + g;
    // collapse consecutive **
    for (;;) {
        auto pos = g.find("**/**");
        if (pos == std::string::npos)
            break;
        g.replace(pos, 5, "**");
    }
    return g;
}

inline std::vector<std::string> splitSeg(std::string const& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '/') {
            if (!cur.empty())
                out.push_back(std::move(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty())
        out.push_back(std::move(cur));
    return out;
}

/** 段内 `*` / `?`；`**` 跨零或多段。 */
inline bool segmentGlob(std::string_view pat, std::string_view seg) {
    std::size_t i = 0, j = 0;
    std::size_t star = std::string::npos, match = 0;
    while (j < seg.size()) {
        if (i < pat.size() && (pat[i] == '?' || pat[i] == seg[j])) {
            ++i;
            ++j;
        } else if (i < pat.size() && pat[i] == '*') {
            star = i++;
            match = j;
        } else if (star != std::string::npos) {
            i = star + 1;
            j = ++match;
        } else {
            return false;
        }
    }
    while (i < pat.size() && pat[i] == '*')
        ++i;
    return i == pat.size();
}

inline bool matchGlobSegs(std::vector<std::string> const& pat, std::size_t pi, std::vector<std::string> const& path,
                          std::size_t qi) {
    if (pi == pat.size())
        return qi == path.size();
    if (pat[pi] == "**") {
        if (matchGlobSegs(pat, pi + 1, path, qi))
            return true;
        if (qi < path.size() && matchGlobSegs(pat, pi, path, qi + 1))
            return true;
        return false;
    }
    if (qi == path.size())
        return false;
    if (!segmentGlob(pat[pi], path[qi]))
        return false;
    return matchGlobSegs(pat, pi + 1, path, qi + 1);
}

inline bool globMatch(std::string_view pattern, std::string_view path) {
    auto pat = splitSeg(normalizeGlob(pattern));
    auto segs = splitSeg(normalizePath(path));
    return matchGlobSegs(pat, 0, segs, 0);
}

/** 词法相似度：task 命中 description/name/body 的加权和（无 Encoder）。 */
inline float lexicalScore(std::string const& taskLower, Rule const& rule) {
    if (taskLower.empty())
        return 0.0f;
    float score = 0.0f;
    std::istringstream ss(taskLower);
    std::string token;
    auto hit = [](std::string const& hay, std::string_view needle) {
        return asciiLower(hay).find(needle) != std::string::npos;
    };
    while (ss >> token) {
        if (token.size() < 2)
            continue;
        if (hit(rule.description, token))
            score += 3.0f;
        if (hit(rule.name, token))
            score += 2.0f;
        if (hit(rule.body, token))
            score += 0.5f;
    }
    return score;
}

/** 激活档序：Always → Glob → Semantic → Manual；同档 Semantic 按分降序，余按索引升序。 */
inline std::vector<Resolvedrule> resolveCore(std::vector<Rule> const& rules, Rulequery const& q,
                                             std::function<float(Rule const&)> semanticScore, std::size_t topK) {
    struct Sel {
        std::size_t idx;
        Activation act;
        float score;
        std::vector<std::string> triggers;
    };
    std::unordered_map<std::size_t, Sel> selected;

    for (std::size_t i = 0; i < rules.size(); ++i) {
        if (!rules[i].enabled)
            continue;
        if (rules[i].always)
            selected.emplace(i, Sel{i, Activation::Always, 0.0f, {}});
    }

    std::vector<std::string> files = q.files;
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
    for (std::size_t i = 0; i < rules.size(); ++i) {
        if (!rules[i].enabled || rules[i].always || rules[i].globs.empty())
            continue;
        if (selected.count(i))
            continue;
        std::vector<std::string> hits;
        for (auto const& f : files)
            for (auto const& g : rules[i].globs)
                if (globMatch(g, f)) {
                    hits.push_back(f);
                    break;
                }
        if (!hits.empty())
            selected.emplace(i, Sel{i, Activation::Glob, 0.0f, std::move(hits)});
    }

    struct Cand {
        std::size_t idx;
        float score;
    };
    std::vector<Cand> sem;
    for (std::size_t i = 0; i < rules.size(); ++i) {
        if (!rules[i].enabled || rules[i].always || !rules[i].globs.empty())
            continue;
        if (rules[i].description.empty())
            continue;
        if (selected.count(i))
            continue;
        float s = semanticScore(rules[i]);
        if (s > 0.0f)
            sem.push_back({i, s});
    }
    std::sort(sem.begin(), sem.end(), [](Cand const& a, Cand const& b) { return a.score > b.score; });
    for (std::size_t n = 0; n < sem.size() && n < topK; ++n)
        selected.emplace(sem[n].idx, Sel{sem[n].idx, Activation::Semantic, sem[n].score, {}});

    for (auto const& m : q.manual) {
        std::string want = asciiLower(m);
        for (std::size_t i = 0; i < rules.size(); ++i) {
            if (!rules[i].enabled)
                continue;
            if (asciiLower(rules[i].name) != want)
                continue;
            selected.emplace(i, Sel{i, Activation::Manual, 0.0f, {}});
        }
    }

    std::vector<Sel> order;
    order.reserve(selected.size());
    for (auto& kv : selected)
        order.push_back(std::move(kv.second));
    std::sort(order.begin(), order.end(), [](Sel const& a, Sel const& b) {
        auto rank = [](Activation act) -> int {
            switch (act) {
            case Activation::Always: return 0;
            case Activation::Glob: return 1;
            case Activation::Semantic: return 2;
            case Activation::Manual: return 3;
            }
            return 9;
        };
        int ra = rank(a.act), rb = rank(b.act);
        if (ra != rb)
            return ra < rb;
        if (a.act == Activation::Semantic && a.score != b.score)
            return a.score > b.score;
        return a.idx < b.idx;
    });

    std::vector<Resolvedrule> out;
    out.reserve(order.size());
    for (auto const& s : order) {
        Resolvedrule r;
        r.rule = rules[s.idx];
        r.activation = s.act;
        r.score = s.score;
        r.triggers = s.triggers;
        out.push_back(std::move(r));
    }
    return out;
}

inline char const* activationName(Activation a) noexcept {
    switch (a) {
    case Activation::Always: return "always";
    case Activation::Glob: return "glob";
    case Activation::Semantic: return "semantic";
    case Activation::Manual: return "manual";
    }
    return "semantic";
}

/** 按激活档分配 section 配额：Always=满额，Semantic 按分占比，其余=半额起。 */
inline int ruleSections(Activation act, float score, float scoreSum, int baseSections) noexcept {
    if (baseSections <= 0)
        return 1;
    if (act == Activation::Always)
        return baseSections;
    if (act == Activation::Semantic) {
        float sum = (std::max)(scoreSum, 1e-6f);
        int share = static_cast<int>(std::lround((score / sum) * (baseSections * 2.0f)));
        return (std::max)(1, (std::min)(baseSections, share));
    }
    return (std::max)(1, baseSections / 2);
}

/** 纯词法语义（无 Encoder）；供轻量路径与单测。 */
inline std::vector<Resolvedrule> resolveRules(std::vector<Rule> const& rules, Rulequery const& q) {
    std::string taskLower = asciiLower(q.task);
    return resolveCore(rules, q, [&](Rule const& r) { return lexicalScore(taskLower, r); }, 3);
}

std::pair<Frontmatter, std::string> parseFront(std::string const& text);
std::vector<Rule> loadRules(fs::path const& dir);
/** 语义分支：task 用 tryEmbed；规则用 descVec 缓存；忙则词法降级。 */
std::vector<Resolvedrule> resolveRules(Runtime& rt, Rulequery const& q);
/** 复用已算好的 task 向量；规则侧只读 descVec，不再阻塞 embed。 */
std::vector<Resolvedrule> resolveRules(Runtime& rt, Rulequery const& q, std::vector<float> const& qVec);
/** 规则加载后缓存 description 向量（允许阻塞，仅启动/热重载）。 */
void warmRuleVecs(Runtime& rt);
/** 从 JSON 填 Rulequery：task|query、files、manual。 */
Rulequery rulequeryFromJson(json const& body);

Experience experienceFromJson(json const& j);
expected_gt<std::string> saveMark(fs::path const& dir, Experience const& exp);
std::size_t estimate(std::string_view text);
std::size_t estimateMany(std::vector<std::string_view> const& texts);
std::string compressBody(std::string_view body, std::string_view task, std::size_t maxBullets);
std::string compressSections(std::string_view body, std::string_view task, std::size_t maxSections);

} // namespace api
