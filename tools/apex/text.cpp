/**
 *  @file       text.cpp
 *  @brief      规则加载/激活（Always/Glob/Semantic/Manual）、token 估计、抽取式压缩、经验落盘。
 */

#include "api.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <functional>
#include <sstream>
#include <unordered_map>

namespace api {

namespace {

std::string trimAscii(std::string s) {
    auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos)
        return {};
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

std::string unquote(std::string s) {
    s = trimAscii(std::move(s));
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\'')))
        return s.substr(1, s.size() - 2);
    return s;
}

bool parseBool(std::string_view s, bool& out) {
    std::string v(s);
    for (char& c : v)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (v == "true" || v == "yes" || v == "1") {
        out = true;
        return true;
    }
    if (v == "false" || v == "no" || v == "0") {
        out = false;
        return true;
    }
    return false;
}

std::string asciiLower(std::string_view text) {
    std::string out(text);
    for (char& c : out)
        if (static_cast<unsigned char>(c) <= 127)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string normalizePath(std::string_view raw) {
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

std::string normalizeGlob(std::string_view raw) {
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

std::vector<std::string> splitSeg(std::string const& s) {
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
bool segmentGlob(std::string_view pat, std::string_view seg) {
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

bool matchGlobSegs(std::vector<std::string> const& pat, std::size_t pi, std::vector<std::string> const& path,
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

float cosine(std::vector<float> const& a, std::vector<float> const& b) {
    if (a.empty() || a.size() != b.size())
        return 0.0f;
    double dot = 0, na = 0, nb = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        dot += static_cast<double>(a[i]) * b[i];
        na += static_cast<double>(a[i]) * a[i];
        nb += static_cast<double>(b[i]) * b[i];
    }
    if (na <= 0 || nb <= 0)
        return 0.0f;
    return static_cast<float>(dot / (std::sqrt(na) * std::sqrt(nb)));
}

float lexicalScore(std::string const& taskLower, Rule const& rule) {
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

std::vector<Resolvedrule> resolveCore(std::vector<Rule> const& rules, Rulequery const& q,
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

} // namespace

char const* activationName(Activation a) noexcept {
    switch (a) {
    case Activation::Always: return "always";
    case Activation::Glob: return "glob";
    case Activation::Semantic: return "semantic";
    case Activation::Manual: return "manual";
    }
    return "semantic";
}

json Resolvedrule::toJson() const {
    json files = json::array();
    for (auto const& f : triggers)
        files.push_back(f);
    return {{"name", rule.name},   {"description", rule.description},
            {"body", rule.body},   {"activation", activationName(activation)},
            {"score", score},      {"always", rule.always},
            {"globs", rule.globs}, {"triggers", files}};
}

bool globMatch(std::string_view pattern, std::string_view path) {
    auto pat = splitSeg(normalizeGlob(pattern));
    auto segs = splitSeg(normalizePath(path));
    return matchGlobSegs(pat, 0, segs, 0);
}

int ruleSections(Activation act, float score, float scoreSum, int baseSections) noexcept {
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

std::pair<Frontmatter, std::string> parseFront(std::string const& text) {
    Frontmatter meta;
    if (text.size() < 3 || text.substr(0, 3) != "---")
        return {meta, text};
    auto end = text.find("\n---", 3);
    if (end == std::string::npos)
        return {meta, text};
    std::string yaml = text.substr(3, end - 3);
    std::string body = text.substr(end + 4);
    while (!body.empty() && (body[0] == '\n' || body[0] == '\r'))
        body.erase(body.begin());

    std::istringstream ss(yaml);
    std::string line;
    std::string listKey;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        std::string trimmed = trimAscii(line);
        if (trimmed.empty() || trimmed[0] == '#')
            continue;
        if (!listKey.empty()) {
            if (!trimmed.empty() && trimmed[0] == '-') {
                std::string item = trimAscii(trimmed.substr(1));
                item = unquote(item);
                if (listKey == "globs" && !item.empty())
                    meta.globs.push_back(std::move(item));
                continue;
            }
            listKey.clear();
        }
        auto colon = trimmed.find(':');
        if (colon == std::string::npos)
            continue;
        std::string key = trimAscii(trimmed.substr(0, colon));
        std::string val = trimAscii(trimmed.substr(colon + 1));
        for (char& c : key)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (key == "name") {
            meta.name = unquote(val);
        } else if (key == "description") {
            meta.description = unquote(val);
        } else if (key == "always" || key == "alwaysapply") {
            bool b = false;
            if (parseBool(val, b))
                meta.always = b;
        } else if (key == "enabled") {
            bool b = true;
            if (parseBool(val, b))
                meta.enabled = b;
        } else if (key == "globs" || key == "glob") {
            if (val.empty()) {
                listKey = "globs";
            } else if (val.front() == '[') {
                // [a, b] 简表
                std::string inner = val.substr(1);
                if (!inner.empty() && inner.back() == ']')
                    inner.pop_back();
                std::istringstream ls(inner);
                std::string part;
                while (std::getline(ls, part, ',')) {
                    part = unquote(trimAscii(part));
                    if (!part.empty())
                        meta.globs.push_back(std::move(part));
                }
            } else {
                meta.globs.push_back(unquote(val));
            }
        }
    }
    return {meta, body};
}

std::vector<Rule> loadRules(fs::path const& dir) {
    std::vector<Rule> out;
    if (!fs::is_directory(dir))
        return out;
    for (auto const& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".md")
            continue;
        std::ifstream in(entry.path());
        std::ostringstream ss;
        ss << in.rdbuf();
        auto [meta, body] = parseFront(ss.str());
        Rule rule;
        rule.name = meta.name.empty() ? entry.path().stem().string() : meta.name;
        rule.description = std::move(meta.description);
        rule.body = std::move(body);
        rule.path = entry.path().string();
        rule.always = meta.always;
        rule.enabled = meta.enabled;
        rule.globs = std::move(meta.globs);
        out.push_back(std::move(rule));
    }
    std::sort(out.begin(), out.end(), [](Rule const& a, Rule const& b) { return a.name < b.name; });
    return out;
}

Rulequery rulequeryFromJson(json const& body) {
    Rulequery q;
    if (body.contains("task") && body["task"].is_string())
        q.task = body["task"].get<std::string>();
    else if (body.contains("query") && body["query"].is_string())
        q.task = body["query"].get<std::string>();
    if (body.contains("files") && body["files"].is_array())
        for (auto const& f : body["files"])
            if (f.is_string())
                q.files.push_back(f.get<std::string>());
    if (body.contains("manual") && body["manual"].is_array())
        for (auto const& m : body["manual"])
            if (m.is_string())
                q.manual.push_back(m.get<std::string>());
    return q;
}

std::vector<Resolvedrule> resolveRules(std::vector<Rule> const& rules, Rulequery const& q) {
    std::string taskLower = asciiLower(q.task);
    return resolveCore(rules, q, [&](Rule const& r) { return lexicalScore(taskLower, r); }, 3);
}

std::vector<Resolvedrule> resolveRules(Runtime& rt, Rulequery const& q, std::vector<float> const& qVec) {
    std::string taskLower = asciiLower(q.task);
    bool const dense = !qVec.empty();
    return resolveCore(
        rt.rules, q,
        [&](Rule const& r) -> float {
            if (dense) {
                auto dVec = rt.encoder.embed(r.description);
                float c = cosine(qVec, dVec);
                // 与词法混合：稠密为主，词法托底
                return (std::max)(c, lexicalScore(taskLower, r) * 0.05f);
            }
            return lexicalScore(taskLower, r);
        },
        3);
}

std::vector<Resolvedrule> resolveRules(Runtime& rt, Rulequery const& q) {
    std::vector<float> qVec;
    if (!q.task.empty())
        qVec = rt.encoder.embed(q.task);
    return resolveRules(rt, q, qVec);
}

Experience experienceFromJson(json const& j) {
    Experience e;
    e.title = j.value("title", "experience");
    e.summary = j.value("summary", "");
    e.outcome = j.value("outcome", "ok");
    if (j.contains("tags") && j["tags"].is_array())
        for (auto const& x : j["tags"])
            if (x.is_string())
                e.tags.push_back(x.get<std::string>());
    if (j.contains("commands") && j["commands"].is_array())
        for (auto const& x : j["commands"])
            if (x.is_string())
                e.commands.push_back(x.get<std::string>());
    if (j.contains("files") && j["files"].is_array())
        for (auto const& x : j["files"])
            if (x.is_string())
                e.files.push_back(x.get<std::string>());
    return e;
}

expected_gt<std::string> saveMark(fs::path const& dir, Experience const& exp) {
    expected_gt<std::string> out;
    try {
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char stamp[32];
        if (std::strftime(stamp, sizeof(stamp), "%Y%m%d%H%M%S", &tm) == 0)
            return out.failed("time format failed");
        fs::path folder = dir / std::string(stamp, 4) / std::string(stamp + 4, 2);
        fs::create_directories(folder);
        fs::path path = folder / (std::string(stamp) + ".md");
        std::ostringstream body;
        body << "# " << exp.title << "\n\n" << exp.summary << "\n\n## tags\n\n";
        for (auto const& tag : exp.tags)
            body << "- `" << tag << "`\n";
        body << "\n## commands\n\n";
        for (auto const& cmd : exp.commands)
            body << "- `" << cmd << "`\n";
        body << "\n## files\n\n";
        for (auto const& file : exp.files)
            body << "- `" << file << "`\n";
        body << "\n## outcome\n\n" << exp.outcome << "\n";
        std::ofstream(path) << body.str();
        out.result = path.string();
    } catch (...) {
        return out.failed("markdown write failed");
    }
    return out;
}

static bool wideChar(char32_t c) {
    return (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3040 && c <= 0x30FF) ||
           (c >= 0xAC00 && c <= 0xD7AF) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFF00 && c <= 0xFFEF);
}

/** UTF-8 步进：宽字符按 1 token，ASCII 跑批按 ceil(n/4)。 */
std::size_t estimate(std::string_view text) {
    std::size_t total = 0;
    std::size_t run = 0;
    for (std::size_t i = 0; i < text.size();) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        char32_t cp = 0;
        std::size_t len = 1;
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < text.size()) {
            cp = ((c & 0x1F) << 6) | (static_cast<unsigned char>(text[i + 1]) & 0x3F);
            len = 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < text.size()) {
            cp = ((c & 0x0F) << 12) | ((static_cast<unsigned char>(text[i + 1]) & 0x3F) << 6) |
                 (static_cast<unsigned char>(text[i + 2]) & 0x3F);
            len = 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < text.size()) {
            cp = ((c & 0x07) << 18) | ((static_cast<unsigned char>(text[i + 1]) & 0x3F) << 12) |
                 ((static_cast<unsigned char>(text[i + 2]) & 0x3F) << 6) |
                 (static_cast<unsigned char>(text[i + 3]) & 0x3F);
            len = 4;
        } else {
            cp = c;
        }
        i += len;
        if (wideChar(cp)) {
            total += (run + 3) / 4 + 1;
            run = 0;
        } else {
            ++run;
        }
    }
    return total + (run + 3) / 4;
}

std::size_t estimateMany(std::vector<std::string_view> const& texts) {
    std::size_t n = 0;
    for (auto t : texts)
        n += estimate(t);
    return n;
}

static std::vector<std::string> wordsOf(std::string_view text) {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&] {
        if (!cur.empty()) {
            out.push_back(cur);
            cur.clear();
        }
    };
    for (unsigned char c : text) {
        if (std::isalnum(c))
            cur.push_back(static_cast<char>(std::tolower(c)));
        else
            flush();
    }
    flush();
    return out;
}

static bool isBullet(std::string_view line) {
    auto t = line;
    while (!t.empty() && (t.front() == ' ' || t.front() == '\t'))
        t.remove_prefix(1);
    if (t.empty())
        return false;
    if (t.front() == '-' || t.front() == '*' || t.front() == '+')
        return true;
    return std::isdigit(static_cast<unsigned char>(t.front())) && t.size() > 1 && t[1] == '.';
}

static bool isHard(std::string_view line) {
    std::string lower(line);
    for (char& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    char const* en[] = {"must", "never", "do not", "don't", "banned", "deprecated", "required"};
    for (auto* n : en)
        if (lower.find(n) != std::string::npos)
            return true;
    char const* zh[] = {"禁止", "严禁", "不得", "不许", "不要", "切勿", "绝不", "必须", "务必",
                        "只能", "仅可", "不可", "杜绝", "避免", "一律", "强制", "应当"};
    for (auto* n : zh)
        if (std::string(line).find(n) != std::string::npos)
            return true;
    return false;
}

static float overlap(std::string_view line, std::vector<std::string> const& query) {
    if (query.empty())
        return 0.0f;
    auto lw = wordsOf(line);
    if (lw.empty())
        return 0.0f;
    std::size_t hits = 0;
    for (auto const& w : lw)
        for (auto const& q : query)
            if (w == q) {
                ++hits;
                break;
            }
    return static_cast<float>(hits) / static_cast<float>(lw.size());
}

std::string compressBody(std::string_view body, std::string_view task, std::size_t maxBullets) {
    auto query = wordsOf(task);
    maxBullets = (std::max)(maxBullets, std::size_t{1});
    std::vector<std::string> lines;
    {
        std::istringstream ss{std::string(body)};
        std::string line;
        while (std::getline(ss, line))
            lines.push_back(line);
    }
    std::vector<std::pair<float, std::size_t>> normal;
    std::vector<std::size_t> hard;
    for (std::size_t i = 0; i != lines.size(); ++i) {
        if (!isBullet(lines[i]))
            continue;
        if (isHard(lines[i]))
            hard.push_back(i);
        else
            normal.emplace_back(overlap(lines[i], query), i);
    }
    std::sort(normal.begin(), normal.end(), [](auto const& a, auto const& b) { return a.first > b.first; });
    std::vector<char> keep(lines.size(), 0);
    std::size_t slots = maxBullets;
    if (!hard.empty()) {
        keep[hard.front()] = 1;
        --slots;
        for (std::size_t i = 1; i < hard.size(); ++i)
            keep[hard[i]] = 1;
    }
    for (auto const& [_, idx] : normal) {
        if (slots == 0)
            break;
        keep[idx] = 1;
        --slots;
    }
    for (std::size_t i = 0; i != lines.size(); ++i)
        if (!isBullet(lines[i]) && isHard(lines[i]))
            keep[i] = 1;
    std::ostringstream out;
    bool first = true;
    for (std::size_t i = 0; i != lines.size(); ++i) {
        if (!keep[i])
            continue;
        if (!first)
            out << '\n';
        first = false;
        out << lines[i];
    }
    return out.str();
}

static int headingLevel(std::string_view line) {
    std::size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
        ++i;
    std::size_t n = 0;
    while (i + n < line.size() && line[i + n] == '#')
        ++n;
    if (n >= 1 && n <= 6 && i + n < line.size() && line[i + n] == ' ')
        return static_cast<int>(n);
    return 0;
}

std::string compressSections(std::string_view body, std::string_view task, std::size_t maxSections) {
    auto query = wordsOf(task);
    maxSections = (std::max)(maxSections, std::size_t{1});
    std::vector<std::string> lines;
    {
        std::istringstream ss{std::string(body)};
        std::string line;
        while (std::getline(ss, line))
            lines.push_back(line);
    }
    std::string preamble;
    std::vector<std::string> sections;
    std::string cur;
    bool inSec = false;
    for (auto const& line : lines) {
        if (headingLevel(line)) {
            if (inSec)
                sections.push_back(cur);
            cur = line;
            inSec = true;
        } else if (inSec) {
            cur.push_back('\n');
            cur += line;
        } else {
            if (!preamble.empty())
                preamble.push_back('\n');
            preamble += line;
        }
    }
    if (inSec)
        sections.push_back(cur);
    if (sections.empty())
        return compressBody(body, task, maxSections);
    std::vector<std::pair<float, std::size_t>> scored;
    for (std::size_t i = 0; i != sections.size(); ++i) {
        float score = 0.0f;
        std::istringstream ss(sections[i]);
        std::string line;
        bool first = true;
        while (std::getline(ss, line)) {
            if (isHard(line))
                score += 3.0f;
            score += overlap(line, query) * 2.0f;
            if (first) {
                score += overlap(line, query) * 3.0f;
                first = false;
            }
        }
        scored.emplace_back(score, i);
    }
    std::sort(scored.begin(), scored.end(), [](auto const& a, auto const& b) {
        if (a.first != b.first)
            return a.first > b.first;
        return a.second < b.second;
    });
    std::vector<std::size_t> keep;
    for (std::size_t i = 0; i < scored.size() && i < maxSections; ++i)
        keep.push_back(scored[i].second);
    for (std::size_t i = 0; i != sections.size(); ++i) {
        std::istringstream ss(sections[i]);
        std::string line;
        bool hard = false;
        while (std::getline(ss, line))
            if (isHard(line)) {
                hard = true;
                break;
            }
        if (hard && std::find(keep.begin(), keep.end(), i) == keep.end())
            keep.push_back(i);
    }
    std::sort(keep.begin(), keep.end());
    std::ostringstream out;
    if (!preamble.empty())
        out << preamble;
    for (auto i : keep) {
        if (!out.str().empty())
            out << "\n\n";
        out << sections[i];
    }
    std::string result = out.str();
    return result.empty() ? std::string(body) : result;
}

} // namespace api
