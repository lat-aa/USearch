/**
 *  @file       gate.cpp
 *  @brief      前置门控：L1/L2→政策∥记忆→RRF→Nanbeige 混合置信→answered|pack|refuse；observe 入队与 Worker。
 */

#include "api.hpp"
#include "helpers.hpp"

#include <algorithm>
#include <cstdio>
#include <sstream>
#include <thread>

namespace api {
namespace {

/** Doc → Hitdoc（helpers 热路径视图）。 */
Hitdoc toHit(Doc const& d) {
    return Hitdoc {d.id, d.text, d.meta};
}

std::vector<std::pair<Hitdoc, float>> toHits(std::vector<std::pair<Doc, float>> const& xs) {
    std::vector<std::pair<Hitdoc, float>> out;
    out.reserve(xs.size());
    for (auto const& h : xs)
        out.emplace_back(toHit(h.first), h.second);
    return out;
}

std::vector<Rulehit> toRulehits(std::vector<Resolvedrule> const& xs) {
    std::vector<Rulehit> out;
    out.reserve(xs.size());
    for (auto const& r : xs)
        out.push_back(Rulehit {r.rule.name, r.rule.body, static_cast<std::uint8_t>(r.activation), r.score});
    return out;
}

bool docKind(Doc const& doc, char const* want) noexcept {
    // 限定 api::kindIs，避免与本函数同名递归/重载歧义
    return api::kindIs(doc.meta, want);
}

json buildPack(Runtime& rt, std::string const& task, std::vector<std::pair<Doc, float>> const& hits,
               std::vector<Resolvedrule> const& rules, Decision const& d, json const& modelPack) {
    Packcfg cfg;
    cfg.packtok = rt.config.gate.packtok;
    cfg.baseSecs = rt.decider.sectionsFor(d.compression);
    json decisionSlice = {{"retrieval", retrievalName(d.retrieval)}, {"compression", d.compression}};
    return api::buildPack(cfg, task, toHits(hits), toRulehits(rules), decisionSlice, modelPack);
}

json finalizeGate(json model, float evidence, Gateconfig const& g, std::size_t minlen) {
    Gateweights w;
    w.threshold = g.threshold;
    w.wevid = g.wevid;
    w.wself = g.wself;
    w.minlen = minlen;
    return api::finalizeStatus(std::move(model), evidence, w);
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
    bool hasGatePack = false;
    if (packRaw.is_object()) {
        auto items = packRaw.find("items");
        if (items != packRaw.end() && items->is_array() && !items->empty())
            hasGatePack = true;
    }
    // 无真实 gate pack：省略 pack 键；计量归零（摘要行 gatePack 0）
    if (!hasGatePack) {
        packn = 0;
        packtok = 0;
    }
    // prompt = 模型注入契约（JSON）；corpus = 聊天统计块人读摘要，见 rebuildCorpus
    prompt = formatPrompt(rulesArr, d.toJson(), json::array(), hasGatePack ? &packRaw : nullptr);
    source = "rebuild";
}

void Turnstats::rebuildCorpus() {
    // 人读 markdown 算法在 helpers::formatCorpus；此处只赋值
    if (prompt.empty()) {
        corpus.clear();
        return;
    }
    corpus = formatCorpus(prompt);
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
    // 生产路径：ports 绑到本进程 Encoder/Store；单测走 runGateCore 注入桩
    Gateports ports;
    ports.embed = [&](std::string_view t) { return rt.encoder.embed(t); };
    ports.chat = [&](std::string_view sys, std::string_view user) { return rt.encoder.chat(sys, user); };
    ports.search = [&](std::vector<float> const& q, std::size_t k) { return rt.store.search(q, k); };
    ports.upsert = [&](Doc doc, std::vector<float> const& v) { return rt.store.upsert(std::move(doc), v); };
    ports.audit = [&](std::string const& id, std::string const& kind, json const& detail) {
        std::lock_guard<std::mutex> lock(rt.store.mutex);
        (void)rt.store.base.auditPut(id, kind, detail);
    };
    return runGateCore(rt, args, ports);
}

json runGateCore(Runtime& rt, json const& args, Gateports& ports) {
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

    std::vector<float> qVec = ports.embed ? ports.embed(task) : std::vector<float> {};
    auto rawHits = ports.search ? ports.search(qVec, annK) : std::vector<std::pair<Doc, float>> {};

    // L2：同一次 ANN 结果里找 kind=cache + 指纹
    for (auto const& [doc, score] : rawHits) {
        if (!docKind(doc, "cache"))
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
        if (docKind(h.first, "cache"))
            continue;
        // 无 kind 或缺省 memory/experience 均视为记忆（kindIs 已覆盖）
        if (docKind(h.first, "memory") || docKind(h.first, "experience"))
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

    json conflicts = detectConflicts(hard, ban, task, toHits(memoryHits));
    float evidence = evidenceOf(toHits(memoryHits), toRulehits(matched));

    if (!conflicts.empty()) {
        json pack = buildPack(rt, task, memoryHits, matched, d, json::array());
        if (ports.audit)
            ports.audit("gate-" + l1key, "conflict", conflicts);
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
        // 无 Encoder 时仍走 ports.chat；单测桩不必碰 temperature
        float prevTemp = rt.encoder.temperature;
        std::uint32_t prevMax = rt.encoder.maxTokens;
        rt.encoder.temperature = 0.0f;
        rt.encoder.maxTokens = (std::min)(prevMax ? prevMax : 1024u, 1024u);
        std::string raw = ports.chat ? ports.chat(kGateSys, user) : std::string {};
        ++localChats;
        model = finalizeGate(parseGateModel(raw), evidence, g, g.minlen);

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
            raw = ports.chat ? ports.chat(kCritSys, model.dump()) : std::string {};
            ++localChats;
            model = finalizeGate(parseGateModel(raw), evidence, g, g.minlen);
            ++rounds;
        }
        model["reflect"] = rounds;
        rt.encoder.temperature = prevTemp;
        rt.encoder.maxTokens = prevMax;
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
        auto vec = ports.embed ? ports.embed(task + "\n" + reply) : std::vector<float> {};
        if (ports.upsert)
            (void)ports.upsert(std::move(cacheDoc), vec);
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
