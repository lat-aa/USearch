/**
 *  @file       gate.cpp
 *  @brief      前置门控：L1/L2→政策∥记忆→RRF→Nanbeige 混合置信→answered|pack|refuse；observe 入队与 Worker。
 */

#include "api.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <thread>

namespace api {
namespace {

float clamp01(float x) {
    if (x < 0.0f)
        return 0.0f;
    if (x > 1.0f)
        return 1.0f;
    return x;
}

/** RRF：两路排序列表融合成 id→分（不把 Always/Glob 政策放进此融合）。 */
std::unordered_map<std::string, float> rrfMerge(std::vector<std::string> const& a,
                                                std::vector<std::string> const& b, int k = 60) {
    std::unordered_map<std::string, float> out;
    for (std::size_t i = 0; i < a.size(); ++i)
        out[a[i]] += 1.0f / static_cast<float>(k + static_cast<int>(i) + 1);
    for (std::size_t i = 0; i < b.size(); ++i)
        out[b[i]] += 1.0f / static_cast<float>(k + static_cast<int>(i) + 1);
    return out;
}

bool kindIs(Doc const& doc, char const* want) {
    if (!doc.meta.is_object() || !doc.meta.contains("kind"))
        return std::string(want) == "memory" || std::string(want) == "experience";
    return doc.meta.value("kind", "") == want;
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

/** 固定红线词表（字面量，非本仓库标识符）。位图第 i 位 = 第 i 条命中。 */
struct Redtable {
    static constexpr char const* tokens[] = {"model_ready", "model-ready", "save_observation",
                                             "observation_queue"};
    static constexpr std::size_t lengths[] = {11, 11, 16, 17};
    static constexpr unsigned bits[] = {1u, 2u, 4u, 8u};
    static constexpr std::size_t count = 4;
    static constexpr unsigned all = 0xFu;
};

/**
 * 单遍扫描正文，用首字节过滤 + memcmp 确认，把 4 次独立 find 收成一次 O(n)。
 * 全命中即提前退出；门控热路径上 hits 不多，但每条记忆文本可能很长。
 */
unsigned scanRed(std::string const& text) noexcept {
    char const* data = text.data();
    std::size_t const n = text.size();
    unsigned mask = 0;
    for (std::size_t i = 0; i < n; ++i) {
        unsigned char const c = static_cast<unsigned char>(data[i]);
        if (c == 'm') {
            if (!(mask & Redtable::bits[0]) && i + Redtable::lengths[0] <= n &&
                std::memcmp(data + i, Redtable::tokens[0], Redtable::lengths[0]) == 0)
                mask |= Redtable::bits[0];
            if (!(mask & Redtable::bits[1]) && i + Redtable::lengths[1] <= n &&
                std::memcmp(data + i, Redtable::tokens[1], Redtable::lengths[1]) == 0)
                mask |= Redtable::bits[1];
        } else if (c == 's') {
            if (!(mask & Redtable::bits[2]) && i + Redtable::lengths[2] <= n &&
                std::memcmp(data + i, Redtable::tokens[2], Redtable::lengths[2]) == 0)
                mask |= Redtable::bits[2];
        } else if (c == 'o') {
            if (!(mask & Redtable::bits[3]) && i + Redtable::lengths[3] <= n &&
                std::memcmp(data + i, Redtable::tokens[3], Redtable::lengths[3]) == 0)
                mask |= Redtable::bits[3];
        }
        if (mask == Redtable::all)
            break;
    }
    return mask;
}

/**
 * 轻量冲突：硬政策（Always/Glob）与记忆正文/元数据撞车。
 *
 * 原实现是 O(|rules|·|hits|·|red|)：每条规则对每条记忆重复 find「禁止」与红线词。
 * 这里拆成两阶段——规则侧只扫一遍，文档侧每条记忆只扫一遍——再按需做小笛卡尔积输出，
 * 语义不变：红线词只对 body 含「禁止」的硬政策报警；meta.conflict 对所有硬政策报警。
 */
json detectConflicts(std::vector<Resolvedrule> const& rules,
                     std::vector<std::pair<Doc, float>> const& hits) {
    // 指针指向 Resolvedrule 内已有的 name，避免拷贝字符串
    std::vector<std::string const*> hard;
    std::vector<std::string const*> ban;
    hard.reserve(rules.size());
    ban.reserve(rules.size());
    for (auto const& r : rules) {
        if (r.activation != Activation::Always && r.activation != Activation::Glob)
            continue;
        hard.push_back(&r.rule.name);
        // 「禁止」只判定一次；原先嵌在 H×K 内，规则正文越长浪费越大
        if (r.rule.body.find("禁止") != std::string::npos)
            ban.push_back(&r.rule.name);
    }
    if (hard.empty() || hits.empty())
        return json::array();

    // 静态 why，避免热循环里反复拼 string
    static std::string const whyRed[] = {
        "memory contains banned token model_ready",
        "memory contains banned token model-ready",
        "memory contains banned token save_observation",
        "memory contains banned token observation_queue",
    };
    static std::string const whyMeta = "memory meta.conflict";

    json conflicts = json::array();
    // 上界：每条记忆最多 (ban·red + hard) 条；按上界 reserve，避免 push 时反复扩容
    conflicts.get_ref<json::array_t&>().reserve(hits.size() * (ban.size() * Redtable::count + hard.size()));

    for (auto const& hit : hits) {
        Doc const& doc = hit.first;
        bool const metaConflict = doc.meta.is_object() && doc.meta.value("conflict", false);
        if (metaConflict) {
            for (std::string const* name : hard)
                conflicts.push_back({{"rule", *name}, {"why", whyMeta}});
        }
        if (ban.empty())
            continue;
        unsigned const mask = scanRed(doc.text);
        if (mask == 0)
            continue;
        for (std::size_t i = 0; i < Redtable::count; ++i) {
            if ((mask & Redtable::bits[i]) == 0)
                continue;
            for (std::string const* name : ban)
                conflicts.push_back({{"rule", *name}, {"why", whyRed[i]}});
        }
    }
    return conflicts;
}

/** Store::search 对外多为 cosine 距离（越小越近）；转为相似度。 */
float asSim(float distanceOrSim) {
    if (distanceOrSim < 0.0f)
        return 0.0f;
    // 已是相似度（粗排路径偶发 1-score）时夹紧；否则按距离转相似度。
    if (distanceOrSim <= 1.0f)
        return clamp01(1.0f - distanceOrSim);
    return 0.0f;
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
    std::size_t budget = g.packtok;
    std::size_t used = 0;
    auto tryAdd = [&](std::string const& id, std::string const& span, float score, std::string const& why) {
        std::size_t cost = estimate(span) + estimate(id) + 8;
        if (used + cost > budget && !pack.empty())
            return;
        pack.push_back({{"id", id}, {"span", span}, {"score", score}, {"why", why}});
        used += cost;
    };
    for (auto const& r : rules) {
        if (r.activation != Activation::Always && r.activation != Activation::Glob)
            continue;
        int secs = ruleSections(r.activation, r.score, 1.0f, rt.decider.sectionsFor(d.compression));
        std::string body = compressSections(r.rule.body, task, static_cast<std::size_t>(secs));
        tryAdd("rule:" + r.rule.name, body, 1.0f, "hard policy");
    }
    for (auto const& [doc, score] : hits) {
        std::string span = compressSections(doc.text, task, 2);
        if (span.size() > 400)
            span = span.substr(0, 400);
        tryAdd(doc.id, span, score, "memory");
    }
    if (modelPack.is_array()) {
        for (auto const& m : modelPack) {
            if (!m.is_object())
                continue;
            std::string id = m.value("id", "");
            bool found = false;
            for (auto const& p : pack)
                if (p.value("id", "") == id)
                    found = true;
            if (found)
                continue;
            tryAdd(id, m.value("span", ""), 0.0f, m.value("why", "model"));
        }
    }
    return {{"items", std::move(pack)},
            {"decision", {{"retrieval", retrievalName(d.retrieval)}, {"compression", d.compression}}},
            {"tokens", used}};
}

json finalizeStatus(json model, float evidence, Gateconfig const& g, std::size_t minlen) {
    float self = 0.0f;
    if (model.contains("self") && model["self"].is_number())
        self = clamp01(model["self"].get<float>());
    float conf = clamp01(g.wevid * evidence + g.wself * self);
    json conflicts = model.value("conflicts", json::array());
    if (!conflicts.is_array())
        conflicts = json::array();
    json missing = model.value("missing", json::array());
    std::string status = model.value("status", "pack");
    std::string reply = model.value("reply", "");

    if (!conflicts.empty())
        status = (status == "refuse") ? "refuse" : "pack";
    else if (missing.is_array() && !missing.empty())
        status = "pack";
    else if (conf < g.threshold)
        status = "pack";
    else if (status == "answered" && reply.size() < minlen)
        status = "pack";
    else if (status != "answered" && status != "refuse")
        status = "pack";

    model["status"] = status;
    model["answerConfidence"] = conf;
    model["evidence"] = evidence;
    model["self"] = self;
    model["conflicts"] = conflicts;
    return model;
}

} // namespace

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

json runGate(Runtime& rt, json const& args) {
    std::string task = args.value("task", "");
    if (task.empty())
        task = args.value("query", "");
    if (task.empty())
        return {{"error", "task required"}};

    Gateconfig const& g = rt.config.gate;
    std::string fp = policyFingerprint(rt.rules);
    std::string l1key = std::to_string(fnv1a64(task)) + ":" + fp;

    {
        std::lock_guard<std::mutex> lock(rt.l1Mutex);
        auto it = rt.l1.find(l1key);
        if (it != rt.l1.end() && it->second.fingerprint == fp)
            return {{"status", "answered"},
                    {"answerConfidence", 1.0},
                    {"reply", it->second.reply},
                    {"policyFingerprint", fp},
                    {"cache", "L1"},
                    {"pack", json::object()}};
    }

    // L2：语义缓存（kind=cache）且指纹匹配
    {
        auto q = rt.encoder.embed(task);
        auto hits = rt.store.search(q, g.cachek);
        for (auto const& [doc, score] : hits) {
            if (!kindIs(doc, "cache"))
                continue;
            if (doc.meta.value("fingerprint", "") != fp)
                continue;
            float sim = asSim(score);
            if (sim < g.l2)
                continue;
            return {{"status", "answered"},
                    {"answerConfidence", sim},
                    {"reply", doc.text},
                    {"policyFingerprint", fp},
                    {"cache", "L2"},
                    {"pack", json::object()}};
        }
    }

    Decideinput din = decideinputFromJson(args);
    if (din.task.empty())
        din.task = task;
    Decision d = rt.decider.decide(din);

    Rulequery rq = rulequeryFromJson(args);
    if (rq.task.empty())
        rq.task = task;
    auto matched = resolveRules(rt, rq);

    std::size_t k = rt.decider.topkFor(d.retrieval);
    auto rawHits = rt.store.search(rt.encoder.embed(task), (std::max)(k, std::size_t{8}));
    std::vector<std::pair<Doc, float>> memoryHits;
    for (auto& h : rawHits) {
        if (kindIs(h.first, "cache"))
            continue;
        // 默认无 kind 或 memory/experience 均视为记忆
        if (kindIs(h.first, "memory") || kindIs(h.first, "experience") ||
            !h.first.meta.contains("kind"))
            memoryHits.push_back(std::move(h));
    }

    std::vector<std::string> semIds;
    std::vector<std::string> memIds;
    for (auto const& r : matched)
        if (r.activation == Activation::Semantic)
            semIds.push_back("rule:" + r.rule.name);
    for (auto const& [doc, score] : memoryHits) {
        (void)score;
        memIds.push_back(doc.id);
    }
    auto fused = rrfMerge(semIds, memIds);
    std::sort(memoryHits.begin(), memoryHits.end(), [&](auto const& a, auto const& b) {
        return fused[a.first.id] > fused[b.first.id];
    });

    // 冲突预检同时扫已命中政策 + 全库含命名红线的规则（避免 Glob 未触发时漏检）。
    std::vector<Resolvedrule> policyScan = matched;
    for (auto const& r : rt.rules) {
        if (!r.enabled)
            continue;
        if (r.body.find("禁止") == std::string::npos && r.name != "names")
            continue;
        bool already = false;
        for (auto const& m : matched)
            if (m.rule.name == r.name)
                already = true;
        if (already)
            continue;
        Resolvedrule extra;
        extra.rule = r;
        // detectConflicts 只认 Always/Glob；此处抬成 Always 仅用于冲突扫描，不改真实激活。
        extra.activation = Activation::Always;
        policyScan.push_back(std::move(extra));
    }
    std::vector<std::pair<Doc, float>> conflictHay = memoryHits;
    {
        Doc taskDoc;
        taskDoc.id = "task";
        taskDoc.text = task;
        conflictHay.insert(conflictHay.begin(), {std::move(taskDoc), 1.0f});
    }
    json conflicts = detectConflicts(policyScan, conflictHay);
    float evidence = evidenceOf(memoryHits, matched);

    if (!conflicts.empty()) {
        json pack = buildPack(rt, task, memoryHits, matched, d, json::array());
        (void)rt.store.base.auditPut("gate-" + l1key, "conflict", conflicts);
        return {{"status", "refuse"},
                {"answerConfidence", 0.0},
                {"evidence", evidence},
                {"reply", ""},
                {"conflicts", conflicts},
                {"pack", pack},
                {"policyFingerprint", fp},
                {"cache", nullptr}};
    }

    if (evidence < g.minevid && memoryHits.empty()) {
        json pack = buildPack(rt, task, memoryHits, matched, d, json::array());
        return {{"status", "pack"},
                {"answerConfidence", 0.0},
                {"evidence", evidence},
                {"reply", ""},
                {"conflicts", json::array()},
                {"pack", pack},
                {"policyFingerprint", fp},
                {"cache", nullptr},
                {"reason", "insufficient evidence"}};
    }

    // 政策裁剪体 + 记忆摘要 → Nanbeige Draft
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
        std::string t = doc.text;
        if (t.size() > 500)
            t = t.substr(0, 500);
        hitsArr.push_back({{"id", doc.id}, {"text", t}, {"score", score}});
    }

    std::string system =
        "You are a local gate arbitrator. Output ONE JSON object only, no markdown fences. "
        "Keys: status (answered|pack|refuse), self (0..1), conflicts (array of {rule,why}), "
        "reply (full answer or empty), pack (array of {id,span,why}), missing (array of strings). "
        "If policy conflicts with memory, status=refuse or pack and list conflicts. "
        "Only use answered when evidence is sufficient and you can fully answer.";
    std::string user = json {{"task", task}, {"rules", rulesArr}, {"hits", hitsArr}}.dump();

    float prevTemp = rt.encoder.temperature;
    std::uint32_t prevMax = rt.encoder.maxTokens;
    rt.encoder.temperature = 0.0f;
    rt.encoder.maxTokens = (std::min)(rt.encoder.maxTokens, std::uint32_t{1024});
    std::string raw = rt.encoder.chat(system, user);
    json model = parseGateModel(raw);
    model = finalizeStatus(model, evidence, g, g.minlen);

    // 边界置信：可选 Critique
    int rounds = 0;
    while (rounds < g.reflect) {
        float conf = model.value("answerConfidence", 0.0f);
        if (!(conf >= g.threshold - 0.15f && conf < g.threshold) || !model.value("conflicts", json::array()).empty())
            break;
        std::string critSys =
            "Revise the previous gate JSON. Check hallucination, policy violations, missing info. "
            "Output ONE JSON with the same schema.";
        std::string critUser = model.dump();
        raw = rt.encoder.chat(critSys, critUser);
        model = parseGateModel(raw);
        model = finalizeStatus(model, evidence, g, g.minlen);
        ++rounds;
    }

    rt.encoder.temperature = prevTemp;
    rt.encoder.maxTokens = prevMax;

    json pack = buildPack(rt, task, memoryHits, matched, d, model.value("pack", json::array()));
    std::string status = model.value("status", "pack");
    std::string reply = model.value("reply", "");

    if (status == "answered" && reply.size() >= g.minlen) {
        {
            std::lock_guard<std::mutex> lock(rt.l1Mutex);
            if (rt.l1.size() > 256)
                rt.l1.clear();
            rt.l1[l1key] = L1entry {reply, fp};
        }
        // 晋升 L2 cache（失败忽略，不阻断）
        Doc cacheDoc;
        cacheDoc.id = "cache-" + l1key;
        cacheDoc.text = reply;
        cacheDoc.meta = {{"kind", "cache"}, {"fingerprint", fp}};
        auto vec = rt.encoder.embed(task + "\n" + reply);
        (void)rt.store.upsert(std::move(cacheDoc), vec);
    }

    return {{"status", status},
            {"answerConfidence", model.value("answerConfidence", 0.0)},
            {"evidence", evidence},
            {"self", model.value("self", 0.0)},
            {"reply", reply},
            {"conflicts", model.value("conflicts", json::array())},
            {"missing", model.value("missing", json::array())},
            {"pack", pack},
            {"policyFingerprint", fp},
            {"cache", nullptr},
            {"reflect", rounds}};
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
    if (error_t err = rt.store.base.enqueue(id, payload); err) {
        char const* msg = err.release();
        return {{"ok", false}, {"error", msg ? msg : "enqueue failed"}};
    }
    return {{"ok", true}, {"id", id}, {"status", "pending"}};
}

void workerLoop(Runtime& rt) {
    while (!rt.workerStop.load()) {
        auto claimed = rt.store.base.claim();
        if (!claimed) {
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            continue;
        }
        Queuerow row = std::move(claimed.result);
        try {
            json const& p = row.payload;
            std::string title = p.value("title", "observation");
            std::string summary = p.value("summary", p.value("text", ""));
            std::string outcome = p.value("outcome", "ok");
            std::string system =
                "Distill the observation into a short durable memory note. "
                "Output plain text only: first line title, then summary. "
                "Do not invent policy rules. If content conflicts with coding standards, "
                "prefix with CONFLICT:";
            std::string user = json {{"title", title}, {"summary", summary}, {"outcome", outcome}}.dump();
            float prevTemp = rt.encoder.temperature;
            rt.encoder.temperature = 0.0f;
            std::string distilled = rt.encoder.chat(system, user);
            rt.encoder.temperature = prevTemp;
            bool conflict = distilled.find("CONFLICT") != std::string::npos;
            Doc doc;
            doc.id = "mem-" + row.id;
            doc.text = distilled.empty() ? (title + "\n" + summary) : distilled;
            doc.meta = {{"kind", "memory"},
                        {"conflict", conflict},
                        {"source", row.id},
                        {"outcome", outcome}};
            auto vec = rt.encoder.embed(doc.text);
            if (error_t err = rt.store.upsert(std::move(doc), vec); err) {
                (void)rt.store.base.finish(row.id, "fail");
                continue;
            }
            if (conflict)
                (void)rt.store.base.auditPut(row.id, "conflict", {{"id", row.id}});
            (void)rt.store.base.finish(row.id, "done");
        } catch (...) {
            (void)rt.store.base.finish(row.id, "fail");
        }
    }
}

Runtime::Runtime(Runtime&& other) noexcept
    : root(std::move(other.root)), config(std::move(other.config)), encoder(std::move(other.encoder)),
      store(std::move(other.store)), rules(std::move(other.rules)), decider(std::move(other.decider)),
      l1(std::move(other.l1)), workerStop(other.workerStop.load()), worker(std::move(other.worker)) {
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
    workerStop.store(false);
    worker = std::thread([this] { workerLoop(*this); });
}

void Runtime::stopWorker() {
    workerStop.store(true);
    if (worker.joinable())
        worker.join();
}

} // namespace api
