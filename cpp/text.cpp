/**
 *  @file       text.cpp
 *  @brief      规则加载/匹配、token 估计、抽取式压缩、经验 Markdown 落盘。
 */

#include "api.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>

namespace api {

std::pair<std::string, std::string> parseFront(std::string const& text) {
    if (text.size() < 3 || text.substr(0, 3) != "---")
        return {{}, text};
    auto end = text.find("\n---", 3);
    if (end == std::string::npos)
        return {{}, text};
    std::string yaml = text.substr(3, end - 3);
    std::string body = text.substr(end + 4);
    while (!body.empty() && (body[0] == '\n' || body[0] == '\r'))
        body.erase(body.begin());
    std::string description;
    std::istringstream ss(yaml);
    std::string line;
    while (std::getline(ss, line)) {
        auto pos = line.find("description:");
        if (pos != std::string::npos) {
            description = line.substr(pos + 12);
            while (!description.empty() && description.front() == ' ')
                description.erase(description.begin());
            if (!description.empty() && description.front() == '"') {
                description.erase(description.begin());
                if (!description.empty() && description.back() == '"')
                    description.pop_back();
            }
        }
    }
    return {description, body};
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
        auto [description, body] = parseFront(ss.str());
        Rule rule;
        rule.name = entry.path().stem().string();
        rule.description = std::move(description);
        rule.body = std::move(body);
        rule.path = entry.path().string();
        out.push_back(std::move(rule));
    }
    std::sort(out.begin(), out.end(), [](Rule const& a, Rule const& b) { return a.name < b.name; });
    return out;
}

std::vector<Rule> resolveRules(std::vector<Rule> const& rules, std::string const& query) {
    std::string q = query;
    for (char& c : q)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    struct Scored {
        std::size_t score;
        Rule const* rule;
    };
    std::vector<Scored> scored;
    for (auto const& rule : rules) {
        std::size_t score = 0;
        auto contains = [&](std::string const& hay, std::string_view needle) {
            std::string h = hay;
            for (char& c : h)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return h.find(needle) != std::string::npos;
        };
        std::istringstream ss(q);
        std::string token;
        while (ss >> token) {
            if (contains(rule.name, token))
                score += 3;
            if (contains(rule.description, token))
                score += 2;
            if (contains(rule.body, token))
                score += 1;
        }
        if (score)
            scored.push_back({score, &rule});
    }
    if (scored.empty()) {
        std::vector<Rule> fallback;
        for (std::size_t i = 0; i < rules.size() && i < 3; ++i)
            fallback.push_back(rules[i]);
        return fallback;
    }
    std::sort(scored.begin(), scored.end(), [](Scored const& a, Scored const& b) { return a.score > b.score; });
    std::vector<Rule> out;
    for (std::size_t i = 0; i < scored.size() && i < 5; ++i)
        out.push_back(*scored[i].rule);
    return out;
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
        std::tm tm {};
#if defined(_WIN32)
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char year[8], month[4], stamp[32];
        std::snprintf(year, sizeof(year), "%04d", tm.tm_year + 1900);
        std::snprintf(month, sizeof(month), "%02d", tm.tm_mon + 1);
        std::snprintf(stamp, sizeof(stamp), "%04d%02d%02d%02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                      tm.tm_hour, tm.tm_min, tm.tm_sec);
        fs::path folder = dir / year / month;
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
    char const* zh[] = {"禁止", "严禁", "不得", "不许", "不要", "切勿", "绝不", "必须",
                        "务必", "只能", "仅可", "不可", "杜绝", "避免", "一律", "强制", "应当"};
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
    std::sort(normal.begin(), normal.end(),
              [](auto const& a, auto const& b) { return a.first > b.first; });
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
