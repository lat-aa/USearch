/**
 *  @file       gate.cpp
 *  @brief      前置门控：L1/L2→政策∥记忆→RRF→Nanbeige 混合置信→answered|pack|refuse；observe 入队与 Worker。
 */

#include "api.hpp"
#include "verdict.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <thread>
#include <unordered_set>

namespace api {
namespace {

/**
 * kind 判定：无 kind 时按 memory/experience 缺省；禁止 value<string> 抛 type_error。
 * 热路径上每条 hit 会调用多次，故避免临时 std::string。
 */
bool kindIs(Doc const& doc, char const* want) noexcept {
    bool const wantMem = std::strcmp(want, "memory") == 0 || std::strcmp(want, "experience") == 0;
    if (!doc.meta.is_object())
        return wantMem;
    auto it = doc.meta.find("kind");
    if (it == doc.meta.end())
        return wantMem;
    if (std::string const* k = it->get_ptr<std::string const*>())
        return *k == want;
    return wantMem;
}

/** meta 字符串字段；类型不对返回空 view，绝不抛。 */
std::string_view metaStr(json const& meta, char const* key) noexcept {
    if (!meta.is_object())
        return {};
    auto it = meta.find(key);
    if (it == meta.end())
        return {};
    if (std::string const* s = it->get_ptr<std::string const*>())
        return *s;
    return {};
}

std::string extractJsonObject(std::string const& text) {
    auto start = text.find('{');
    auto end = text.rfind('}');
    if (start == std::string::npos || end == std::string::npos || end <= start)
        return {};
    return text.substr(start, end - start + 1);
}

json parseGateModel(std::string const& raw) {
    std::string slice = extractJsonObject(raw);
    if (slice.empty())
        return json::object();
    try {
        return json::parse(slice);
    } catch (...) {
        return json::object();
    }
}

/** meta.conflict 只认 bool；用 get_ptr 避免 value/get 抛 type_error（未捕获即 abort）。 */
bool metaConflicted(json const& meta) noexcept {
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
 * 调用方负责预筛，避免在此深拷贝 Rule；task 单独扫，免构造临时 Doc / 复制 hits。
 */
json detectConflicts(std::vector<std::string const*> const& hard,
                     std::vector<std::string const*> const& ban, std::string_view taskText,
                     std::vector<std::pair<Doc, float>> const& hits) {
    // why 不写具体蛇形旧名，避免源码再传播违规标识；规则名来自 ban（通常含 names）。
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

    // 任务原文也要过红线（Glob 未触发时仍能挡住违规标识）
    emitRed(scanRed(taskText));
    for (auto const& hit : hits) {
        Doc const& doc = hit.first;
        if (metaConflicted(doc.meta)) {
            for (std::string const* name : hard)
                conflicts.push_back({{"rule", *name}, {"why", "memory meta.conflict"}});
        }
        emitRed(scanRed(doc.text));
    }
    return conflicts;
}

float evidenceOf(std::vector<std::pair<Doc, float>> const& hits, std::vector<Resolvedrule> const& rules) {
    float top = hits.empty() ? 0.0f : asSim(hits[0].second);
    float cover = clamp01(static_cast<float>(hits.size()) / 4.0f);
    float pol = 0.0f;
    for (auto const& r : rules) {
        if (r.activation == Activation::Always || r.activation == Activation::Glob)
            pol = (std::max)(pol, 0.35f);
        if (r.activation == Activation::Semantic)
            pol = (std::max)(pol, clamp01(r.score));
    }
    return clamp01(0.5f * top + 0.25f * cover + 0.25f * pol);
}

json buildPack(Runtime& rt, std::string const& task, std::vector<std::pair<Doc, float>> const& hits,
               std::vector<Resolvedrule> const& rules, Decision const& d, json const& modelPack) {
    Gateconfig const& g = rt.config.gate;
    json pack = json::array();
    std::unordered_set<std::string> seen;
    seen.reserve(16);
    std::size_t budget = g.packtok;
    std::size_t used = 0;
    // sectionsFor 与 compression 无关循环变量，提到循环外
    int const baseSecs = rt.decider.sectionsFor(d.compression);
    auto tryAdd = [&](std::string id, std::string span, float score, char const* why) {
        if (!seen.insert(id).second)
            return;
        std::size_t cost = estimate(span) + estimate(id) + 8;
        if (used + cost > budget && !pack.empty())
            return;
        pack.push_back({{"id", std::move(id)},
                        {"span", std::move(span)},
                        {"score", score},
                        {"why", why}});
        used += cost;
    };
    for (auto const& r : rules) {
        if (r.activation != Activation::Always && r.activation != Activation::Glob)
            continue;
        int secs = ruleSections(r.activation, r.score, 1.0f, baseSecs);
        std::string body = compressSections(r.rule.body, task, static_cast<std::size_t>(secs));
        tryAdd("rule:" + r.rule.name, std::move(body), 1.0f, "hard policy");
    }
    for (auto const& [doc, score] : hits) {
        std::string span = compressSections(doc.text, task, 2);
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
    return {{"items", std::move(pack)},
            {"decision", {{"retrieval", retrievalName(d.retrieval)}, {"compression", d.compression}}},
            {"tokens", used}};
}

json finalizeStatus(json model, float evidence, Gateconfig const& g, std::size_t minlen) {
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

    // modelStatus 单独拷贝，避免 verdictOf 返回指向 status 内部的指针后再赋值触发自引用。
    std::string const modelStatus = status;
    status = verdictOf(!conflicts.empty(), !missing.empty(), conf, g.threshold, modelStatus.c_str(),
                       reply.size(), minlen);

    model["status"] = std::move(status);
    model["answerConfidence"] = conf;
    model["evidence"] = evidence;
    model["self"] = self;
    model["conflicts"] = std::move(conflicts);
    return model;
}

/** 临时改写 Encoder 采样参数，作用域结束自动恢复（gate/worker 共用）。 */
struct Encoderguard {
    Encoder& enc;
    float prevTemp;
    std::uint32_t prevMax;
    Encoderguard(Encoder& e, float temp, std::uint32_t maxTok)
        : enc(e), prevTemp(e.temperature), prevMax(e.maxTokens) {
        enc.temperature = temp;
        enc.maxTokens = (std::min)(prevMax ? prevMax : maxTok, maxTok);
    }
    ~Encoderguard() {
        enc.temperature = prevTemp;
        enc.maxTokens = prevMax;
    }
    Encoderguard(Encoderguard const&) = delete;
    Encoderguard& operator=(Encoderguard const&) = delete;
};

} // namespace

void Turnstats::rebuildPrompt(Decision const& d) {
    // 与 /v1 契约一致，但禁止用 rule:id 合成假 pack 冒充门控命中
    json rulesArr = json::array();
    auto const& keptSrc = !ruleKept.empty() ? ruleKept : ruleText;
    for (auto const& [id, body] : keptSrc) {
        if (body.empty())
            continue;
        rulesArr.push_back({{"name", id}, {"body", body}});
    }
    json knowledge = {{"hits", json::array()},
                      {"rules", std::move(rulesArr)},
                      {"decision", d.toJson()}};
    bool hasGatePack = false;
    if (packRaw.is_object()) {
        auto items = packRaw.find("items");
        if (items != packRaw.end() && items->is_array() && !items->empty()) {
            knowledge["pack"] = packRaw;
            hasGatePack = true;
        }
    }
    // 无真实 gate pack：省略 pack 键；计量归零（摘要行 gatePack 0）
    if (!hasGatePack) {
        packn = 0;
        packtok = 0;
    }
    prompt = "Local knowledge JSON follows.\n" + knowledge.dump(2);
    source = "rebuild";
}

void Turnstats::rebuildCorpus() {
    // 统计块唯一正文：## prompt + 完整命令包（禁止再贴 rules/kept/pack）
    if (prompt.empty()) {
        corpus.clear();
        return;
    }
    std::ostringstream oss;
    oss << "## prompt\n" << prompt;
    if (prompt.back() != '\n')
        oss << '\n';
    corpus = oss.str();
}

json Turnstats::toJson() const {
    json out = {{"gate", gate},
                {"cache", cache},
                {"local", local},
                {"queued", queued},
                {"distill", distill},
                {"fingerprint", fingerprint},
                {"retain", retain},
                {"naive", naive},
                {"picked", picked},
                {"kept", kept},
                {"packtok", packtok},
                {"packn", packn}};
    // 未调 gate 禁止用 0 冒充「已测省 0 次」
    if (hasSaved)
        out["saved"] = saved;
    if (hasAnswer)
        out["answer"] = answer;
    if (!source.empty())
        out["source"] = source;
    if (sawRules || sawGate || !prompt.empty()) {
        auto toArr = [](std::vector<std::pair<std::string, std::string>> const& xs) {
            json arr = json::array();
            for (auto const& [id, body] : xs) {
                if (body.empty())
                    continue;
                arr.push_back({{"id", id}, {"body", body}});
            }
            return arr;
        };
        out["rules"] = toArr(ruleText);
        out["clip"] = toArr(!ruleKept.empty() ? ruleKept : ruleText);
        out["pack"] = toArr(packText);
        if (!prompt.empty()) {
            out["prompt"] = prompt;
            out["corpus"] = corpus;
        }
    }
    return out;
}

void noteRules(Runtime& rt, std::vector<Resolvedrule> const& matched) {
    std::vector<std::pair<std::string, std::string>> texts;
    texts.reserve(matched.size());
    for (auto const& r : matched) {
        if (r.rule.body.empty())
            continue;
        texts.emplace_back(r.rule.name, r.rule.body);
    }
    std::sort(texts.begin(), texts.end(),
              [](auto const& a, auto const& b) { return a.first < b.first; });
    std::lock_guard<std::mutex> lock(rt.turn.mutex);
    rt.turn.sawRules = true;
    rt.turn.ruleText = std::move(texts);
    rt.turn.rebuildCorpus();
}

void noteKept(Runtime& rt, std::vector<std::pair<std::string, std::string>> kept) {
    std::sort(kept.begin(), kept.end(),
              [](auto const& a, auto const& b) { return a.first < b.first; });
    std::lock_guard<std::mutex> lock(rt.turn.mutex);
    rt.turn.sawRules = true;
    rt.turn.ruleKept = std::move(kept);
    rt.turn.rebuildCorpus();
}

void notePrompt(Runtime& rt, std::string prompt) {
    if (prompt.empty())
        return;
    std::lock_guard<std::mutex> lock(rt.turn.mutex);
    rt.turn.prompt = std::move(prompt);
    rt.turn.source = "injected";
    rt.turn.rebuildCorpus();
}

void noteGate(Runtime& rt, json const& result, bool didAnn, std::size_t annK, int localChats) {
    std::string status = "pack";
    if (auto it = result.find("status"); it != result.end() && it->is_string())
        status = it->get_ref<std::string const&>();
    std::string cache = "miss";
    if (auto it = result.find("cache"); it != result.end()) {
        if (it->is_string())
            cache = it->get_ref<std::string const&>();
        else if (it->is_null())
            cache = "miss";
    }
    std::string fp;
    if (auto it = result.find("policyFingerprint"); it != result.end() && it->is_string())
        fp = it->get_ref<std::string const&>();
    float answer = 0;
    bool hasAnswer = false;
    if (auto it = result.find("answerConfidence"); it != result.end() && it->is_number()) {
        answer = it->get<float>();
        hasAnswer = true;
    }
    // pack.items[].span 即注入正文；跳过空 span，禁止把空条目写进 corpus
    std::vector<std::pair<std::string, std::string>> packItems;
    std::size_t packtok = 0;
    std::size_t packn = 0;
    if (auto it = result.find("pack"); it != result.end() && it->is_object()) {
        if (auto tok = it->find("tokens"); tok != it->end() && tok->is_number_unsigned())
            packtok = tok->get<std::size_t>();
        else if (auto tok = it->find("tokens"); tok != it->end() && tok->is_number_integer())
            packtok = static_cast<std::size_t>((std::max)(0, tok->get<int>()));
        if (auto items = it->find("items"); items != it->end() && items->is_array()) {
            packItems.reserve(items->size());
            for (auto const& item : *items) {
                if (!item.is_object())
                    continue;
                std::string id = item.value("id", "");
                std::string span = item.value("span", "");
                if (span.empty())
                    span = item.value("text", "");
                if (span.empty())
                    continue;
                packItems.emplace_back(std::move(id), std::move(span));
            }
            packn = packItems.size();
        }
    }
    int saved = 0;
    if (status == "answered" || cache == "L1" || cache == "L2")
        saved = 1;

    std::lock_guard<std::mutex> lock(rt.turn.mutex);
    rt.turn.sawGate = true;
    rt.turn.gate = std::move(status);
    rt.turn.cache = std::move(cache);
    rt.turn.hasSaved = true;
    rt.turn.saved = saved;
    rt.turn.local += localChats;
    rt.turn.fingerprint = std::move(fp);
    rt.turn.packtok = packtok;
    rt.turn.packn = packn;
    rt.turn.packText = std::move(packItems);
    // 保留 gate.pack 对象，供命令包 knowledge["pack"] 原样嵌入
    if (auto it = result.find("pack"); it != result.end())
        rt.turn.packRaw = *it;
    rt.turn.didAnn = didAnn;
    rt.turn.annK = annK;
    if (hasAnswer) {
        rt.turn.answer = answer;
        rt.turn.hasAnswer = true;
    }
    rt.turn.rebuildCorpus();
}

void noteQueued(Runtime& rt, std::string const& id) {
    if (id.empty())
        return;
    std::lock_guard<std::mutex> lock(rt.turn.mutex);
    rt.turn.queued.push_back(id);
}

void noteDistill(Runtime& rt, std::string const& id, bool didChat) {
    if (id.empty())
        return;
    std::lock_guard<std::mutex> lock(rt.turn.mutex);
    rt.turn.distill.push_back(id);
    if (didChat)
        ++rt.turn.local;
}

std::string policyFingerprint(std::vector<Rule> const& rules) {
    std::ostringstream os;
    for (auto const& r : rules) {
        os << r.name << '\n' << r.description << '\n' << r.body << '\n';
        os << (r.always ? '1' : '0') << (r.enabled ? '1' : '0');
        for (auto const& g : r.globs)
            os << g << ';';
        os << '\n';
    }
    std::uint64_t h = fnv1a64(os.str());
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

/**
 * 前置门控热路径优化要点：
 * - 政策指纹用启动缓存 policyFp，免每请求拼规则正文
 * - task 只 embed 一次；L2+记忆合并为一次 ANN，再分区
 * - resolveRules 复用同一 qVec
 * - 去掉对 memoryHits 无效的 RRF（sem 分不进 doc.id）
 * - 冲突扫描只传 name 指针，不深拷贝 Rule / 不复制 hits
 */
json runGate(Runtime& rt, json const& args) {
    std::string task = args.value("task", "");
    if (task.empty())
        task = args.value("query", "");
    if (task.empty())
        return {{"error", "task required"}};

    Gateconfig const& g = rt.config.gate;
    if (rt.policyFp.empty())
        rt.policyFp = policyFingerprint(rt.rules);
    std::string const& fp = rt.policyFp;
    std::string l1key = std::to_string(fnv1a64(task));
    l1key.push_back(':');
    l1key.append(fp);

    {
        std::lock_guard<std::mutex> lock(rt.l1Mutex);
        auto it = rt.l1.find(l1key);
        if (it != rt.l1.end() && it->second.fingerprint == fp) {
            json out = {{"status", "answered"},
                        {"answerConfidence", 1.0},
                        {"reply", it->second.reply},
                        {"policyFingerprint", fp},
                        {"cache", "L1"},
                        {"pack", json::object()}};
            // L1 未跑 ANN / chat
            noteGate(rt, out, false, 0, 0);
            return out;
        }
    }

    // decide 不依赖 ANN，提前做以便一次 search 同时覆盖 L2 k 与记忆 k
    Decideinput din = decideinputFromJson(args);
    if (din.task.empty())
        din.task = task;
    Decision d = rt.decider.decide(din);
    std::size_t const memK = (std::max)(rt.decider.topkFor(d.retrieval), std::size_t{8});
    std::size_t const annK = (std::max)(g.cachek, memK) + g.cachek; // 留余量，降低 L2 被挤出 top-k 的概率

    std::vector<float> qVec = rt.encoder.embed(task);
    auto rawHits = rt.store.search(qVec, annK);

    // L2：同一次 ANN 结果里找 kind=cache + 指纹
    for (auto const& [doc, score] : rawHits) {
        if (!kindIs(doc, "cache"))
            continue;
        if (metaStr(doc.meta, "fingerprint") != fp)
            continue;
        float sim = asSim(score);
        if (sim < g.l2)
            continue;
        json out = {{"status", "answered"},
                    {"answerConfidence", sim},
                    {"reply", doc.text},
                    {"policyFingerprint", fp},
                    {"cache", "L2"},
                    {"pack", json::object()}};
        noteGate(rt, out, true, annK, 0);
        return out;
    }

    Rulequery rq = rulequeryFromJson(args);
    if (rq.task.empty())
        rq.task = task;
    auto matched = resolveRules(rt, rq, qVec);
    // 门控路径也写入 turn.rules，供工具侧消费；展示仍只靠 prompt
    noteRules(rt, matched);

    std::vector<std::pair<Doc, float>> memoryHits;
    memoryHits.reserve((std::min)(rawHits.size(), memK));
    for (auto& h : rawHits) {
        if (memoryHits.size() >= memK)
            break;
        if (kindIs(h.first, "cache"))
            continue;
        // 无 kind 或缺省 memory/experience 均视为记忆（kindIs 已覆盖）
        if (kindIs(h.first, "memory") || kindIs(h.first, "experience"))
            memoryHits.push_back(std::move(h));
    }
    // ANN 已按距离排序；原先 RRF(semIds, memIds) 的 sem 分从不落到 doc.id，排序是空转，已删除。

    // 冲突：命中硬政策 + 全库含「禁止」/names 的规则名指针（不拷 Rule 正文）
    std::vector<std::string const*> hard;
    std::vector<std::string const*> ban;
    hard.reserve(matched.size() + rt.rules.size());
    ban.reserve(rt.rules.size());
    auto hasName = [](std::vector<std::string const*> const& xs, std::string const& n) {
        for (std::string const* p : xs)
            if (p && *p == n)
                return true;
        return false;
    };
    for (auto const& r : matched) {
        if (r.activation != Activation::Always && r.activation != Activation::Glob)
            continue;
        hard.push_back(&r.rule.name);
        if (r.rule.body.find("禁止") != std::string::npos)
            ban.push_back(&r.rule.name);
    }
    for (auto const& r : rt.rules) {
        if (!r.enabled)
            continue;
        bool const bans = r.body.find("禁止") != std::string::npos;
        if (!bans && r.name != "names")
            continue;
        if (hasName(hard, r.name))
            continue;
        hard.push_back(&r.name);
        if (bans || r.name == "names")
            ban.push_back(&r.name);
    }

    json conflicts = detectConflicts(hard, ban, task, memoryHits);
    float evidence = evidenceOf(memoryHits, matched);

    if (!conflicts.empty()) {
        json pack = buildPack(rt, task, memoryHits, matched, d, json::array());
        {
            std::lock_guard<std::mutex> lock(rt.store.mutex);
            (void)rt.store.base.auditPut("gate-" + l1key, "conflict", conflicts);
        }
        json out = {{"status", "refuse"},
                    {"answerConfidence", 0.0},
                    {"evidence", evidence},
                    {"reply", ""},
                    {"conflicts", std::move(conflicts)},
                    {"pack", std::move(pack)},
                    {"policyFingerprint", fp},
                    {"cache", nullptr}};
        noteGate(rt, out, true, annK, 0);
        return out;
    }

    if (evidence < g.minevid && memoryHits.empty()) {
        json pack = buildPack(rt, task, memoryHits, matched, d, json::array());
        json out = {{"status", "pack"},
                    {"answerConfidence", 0.0},
                    {"evidence", evidence},
                    {"reply", ""},
                    {"conflicts", json::array()},
                    {"pack", std::move(pack)},
                    {"policyFingerprint", fp},
                    {"cache", nullptr},
                    {"reason", "insufficient evidence"}};
        noteGate(rt, out, true, annK, 0);
        return out;
    }

    json rulesArr = json::array();
    float semSum = 0.0f;
    for (auto const& r : matched)
        if (r.activation == Activation::Semantic)
            semSum += (std::max)(r.score, 0.0f);
    int base = rt.decider.sectionsFor(d.compression);
    for (auto const& r : matched) {
        int sections = ruleSections(r.activation, r.score, semSum, base);
        rulesArr.push_back({{"name", r.rule.name},
                            {"activation", activationName(r.activation)},
                            {"body", compressSections(r.rule.body, task, static_cast<std::size_t>(sections))}});
    }
    json hitsArr = json::array();
    for (std::size_t i = 0; i < memoryHits.size() && i < 6; ++i) {
        auto const& [doc, score] = memoryHits[i];
        std::string_view tv = doc.text;
        if (tv.size() > 500)
            tv = tv.substr(0, 500);
        hitsArr.push_back({{"id", doc.id}, {"text", std::string(tv)}, {"score", score}});
    }

    static char const* const kGateSys =
        "You are a local gate arbitrator. Output ONE JSON object only, no markdown fences. "
        "Keys: status (answered|pack|refuse), self (0..1), conflicts (array of {rule,why}), "
        "reply (full answer or empty), pack (array of {id,span,why}), missing (array of strings). "
        "If policy conflicts with memory, status=refuse or pack and list conflicts. "
        "Only use answered when evidence is sufficient and you can fully answer.";
    std::string user = json {{"task", task}, {"rules", std::move(rulesArr)}, {"hits", std::move(hitsArr)}}.dump();

    json model;
    int localChats = 0;
    {
        Encoderguard guard(rt.encoder, 0.0f, 1024);
        std::string raw = rt.encoder.chat(kGateSys, user);
        ++localChats;
        model = finalizeStatus(parseGateModel(raw), evidence, g, g.minlen);

        int rounds = 0;
        while (rounds < g.reflect) {
            float conf = 0.0f;
            if (auto it = model.find("answerConfidence"); it != model.end() && it->is_number())
                conf = it->get<float>();
            bool hasConflict = false;
            if (auto it = model.find("conflicts"); it != model.end() && it->is_array() && !it->empty())
                hasConflict = true;
            if (!(conf >= g.threshold - 0.15f && conf < g.threshold) || hasConflict)
                break;
            static char const* const kCritSys =
                "Revise the previous gate JSON. Check hallucination, policy violations, missing info. "
                "Output ONE JSON with the same schema.";
            raw = rt.encoder.chat(kCritSys, model.dump());
            ++localChats;
            model = finalizeStatus(parseGateModel(raw), evidence, g, g.minlen);
            ++rounds;
        }
        model["reflect"] = rounds;
    }

    json modelPack = json::array();
    if (auto it = model.find("pack"); it != model.end() && it->is_array())
        modelPack = *it;
    json pack = buildPack(rt, task, memoryHits, matched, d, modelPack);

    std::string status = "pack";
    if (auto it = model.find("status"); it != model.end() && it->is_string())
        status = it->get_ref<std::string const&>();
    std::string reply;
    if (auto it = model.find("reply"); it != model.end() && it->is_string())
        reply = it->get_ref<std::string const&>();
    int rounds = 0;
    if (auto it = model.find("reflect"); it != model.end() && it->is_number_integer())
        rounds = it->get<int>();

    if (status == "answered" && reply.size() >= g.minlen) {
        {
            std::lock_guard<std::mutex> lock(rt.l1Mutex);
            if (rt.l1.size() > 256)
                rt.l1.clear();
            rt.l1[l1key] = L1entry {reply, fp};
        }
        Doc cacheDoc;
        cacheDoc.id = "cache-" + l1key;
        cacheDoc.text = reply;
        cacheDoc.meta = {{"kind", "cache"}, {"fingerprint", fp}};
        auto vec = rt.encoder.embed(task + "\n" + reply);
        (void)rt.store.upsert(std::move(cacheDoc), vec);
    }

    json conflictsOut = json::array();
    if (auto it = model.find("conflicts"); it != model.end() && it->is_array())
        conflictsOut = *it;
    json missingOut = json::array();
    if (auto it = model.find("missing"); it != model.end() && it->is_array())
        missingOut = *it;
    float answerConf = 0.0f;
    if (auto it = model.find("answerConfidence"); it != model.end() && it->is_number())
        answerConf = it->get<float>();
    float self = 0.0f;
    if (auto it = model.find("self"); it != model.end() && it->is_number())
        self = it->get<float>();

    json out = {{"status", std::move(status)},
                {"answerConfidence", answerConf},
                {"evidence", evidence},
                {"self", self},
                {"reply", std::move(reply)},
                {"conflicts", std::move(conflictsOut)},
                {"missing", std::move(missingOut)},
                {"pack", std::move(pack)},
                {"policyFingerprint", fp},
                {"cache", nullptr},
                {"reflect", rounds}};
    noteGate(rt, out, true, annK, localChats);
    return out;
}

json runObserve(Runtime& rt, json const& args) {
    json payload = args;
    if (args.contains("payload") && args["payload"].is_object())
        payload = args["payload"];
    std::string id = args.value("id", "");
    if (id.empty()) {
        std::string seed = payload.dump();
        id = "obs-" + std::to_string(fnv1a64(seed));
    }
    error_t err;
    {
        std::lock_guard<std::mutex> lock(rt.store.mutex);
        err = rt.store.base.enqueue(id, payload);
    }
    if (err) {
        char const* msg = err.release();
        return {{"ok", false}, {"error", msg ? msg : "enqueue failed"}};
    }
    // 入队即唤醒，避免 Worker 空转到满 400ms
    noteQueued(rt, id);
    rt.workerCv.notify_one();
    return {{"ok", true}, {"id", id}, {"status", "pending"}};
}

/**
 * Worker 热路径：
 * - condition_variable 替代盲目 sleep；有任务时 do-while 连抽到 empty
 * - 静态 system 提示，Encoderguard 恢复采样参数
 * - 蒸馏失败回退原文；mutex 合并 finish/audit
 *
 * 关于「死循环」：内层不是自旋 for(;;)。退出条件绑在 do-while(claimed)——
 * claim 把 pending→running，finish 落到 done/fail，队列空则 claimed 为假并退出。
 * 真正会「看起来挂住」的是 encoder.chat（同步阻塞），不是循环本身。
 */
void workerLoop(Runtime& rt) {
    static char const* const kSys =
        "Distill the observation into a short durable memory note. "
        "Output plain text only: first line title, then summary. "
        "Do not invent policy rules. If content conflicts with coding standards, "
        "prefix with CONFLICT:";

    while (!rt.workerStop.load(std::memory_order_acquire)) {
        expected_gt<Queuerow> claimed;
        {
            std::lock_guard<std::mutex> lock(rt.store.mutex);
            claimed = rt.store.base.claim();
        }
        if (!claimed) {
            // claim 空队列用 error="empty"；error_t 析构会 raise，必须先 release。
            (void)claimed.error.release();
            std::unique_lock<std::mutex> lk(rt.workerMutex);
            rt.workerCv.wait_for(lk, std::chrono::milliseconds(400),
                                 [&] { return rt.workerStop.load(std::memory_order_acquire); });
            continue;
        }

        // 有任务：处理 → 再 claim；empty 或 stop 时条件为假，结构上不可能空转
        do {
            Queuerow row = std::move(claimed.result);
            try {
                json const& p = row.payload;
                std::string title = "observation";
                if (auto it = p.find("title"); it != p.end() && it->is_string())
                    title = it->get_ref<std::string const&>();
                std::string summary;
                if (auto it = p.find("summary"); it != p.end() && it->is_string())
                    summary = it->get_ref<std::string const&>();
                else if (auto it = p.find("text"); it != p.end() && it->is_string())
                    summary = it->get_ref<std::string const&>();
                std::string outcome = "ok";
                if (auto it = p.find("outcome"); it != p.end() && it->is_string())
                    outcome = it->get_ref<std::string const&>();

                std::string user =
                    json {{"title", title}, {"summary", summary}, {"outcome", outcome}}.dump();
                std::string distilled;
                bool didChat = false;
                try {
                    Encoderguard guard(rt.encoder, 0.0f, 512);
                    distilled = rt.encoder.chat(kSys, user);
                    didChat = !distilled.empty();
                } catch (...) {
                    distilled.clear();
                }
                if (distilled.empty()) {
                    distilled.reserve(title.size() + summary.size() + 1);
                    distilled.assign(title);
                    distilled.push_back('\n');
                    distilled.append(summary);
                }
                bool const conflict = distilled.find("CONFLICT") != std::string::npos;

                std::string const memId = "mem-" + row.id;
                Doc doc;
                doc.id = memId;
                doc.text = std::move(distilled);
                doc.meta = {{"kind", "memory"},
                            {"conflict", conflict},
                            {"source", row.id},
                            {"outcome", std::move(outcome)}};
                auto vec = rt.encoder.embed(doc.text);
                if (error_t err = rt.store.upsert(std::move(doc), vec); err) {
                    std::lock_guard<std::mutex> lock(rt.store.mutex);
                    (void)rt.store.base.finish(row.id, "fail");
                } else {
                    noteDistill(rt, memId, didChat);
                    std::lock_guard<std::mutex> lock(rt.store.mutex);
                    if (conflict)
                        (void)rt.store.base.auditPut(row.id, "conflict", {{"id", row.id}});
                    (void)rt.store.base.finish(row.id, "done");
                }
            } catch (...) {
                std::lock_guard<std::mutex> lock(rt.store.mutex);
                (void)rt.store.base.finish(row.id, "fail");
            }

            if (rt.workerStop.load(std::memory_order_acquire))
                break;
            {
                std::lock_guard<std::mutex> lock(rt.store.mutex);
                claimed = rt.store.base.claim();
            }
        } while (static_cast<bool>(claimed));
        // 循环因 empty 结束：吞掉非致命错误，避免 ~error_t 抛到线程顶层 terminate
        (void)claimed.error.release();
    }
}

Runtime::Runtime(Runtime&& other) noexcept
    : root(std::move(other.root)), config(std::move(other.config)), encoder(std::move(other.encoder)),
      store(std::move(other.store)), rules(std::move(other.rules)), policyFp(std::move(other.policyFp)),
      decider(std::move(other.decider)), l1(std::move(other.l1)),
      workerStop(other.workerStop.load()), worker(std::move(other.worker)) {
    other.workerStop.store(true);
}

Runtime& Runtime::operator=(Runtime&& other) noexcept {
    if (this == &other)
        return *this;
    stopWorker();
    root = std::move(other.root);
    config = std::move(other.config);
    encoder = std::move(other.encoder);
    store = std::move(other.store);
    rules = std::move(other.rules);
    policyFp = std::move(other.policyFp);
    decider = std::move(other.decider);
    {
        std::lock_guard<std::mutex> lock(l1Mutex);
        l1 = std::move(other.l1);
    }
    workerStop.store(other.workerStop.load());
    worker = std::move(other.worker);
    other.workerStop.store(true);
    return *this;
}

Runtime::~Runtime() { stopWorker(); }

void Runtime::startWorker() {
    if (worker.joinable())
        return;
    workerStop.store(false, std::memory_order_release);
    worker = std::thread([this] { workerLoop(*this); });
}

void Runtime::stopWorker() {
    workerStop.store(true, std::memory_order_release);
    workerCv.notify_all();
    if (worker.joinable())
        worker.join();
}

} // namespace api
