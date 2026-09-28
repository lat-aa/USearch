/**
 *  @file       worker.cpp
 *  @brief      observe 入队与 Worker 蒸馏；Turnstats / note*（请求级 TLS）。
 */

#include "worker.hpp"
#include "api.hpp"
#include "render.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <sstream>
#include <thread>

namespace api {
namespace {
thread_local Turnstats* tlsTurn = nullptr;

bool userHot(Runtime const& rt) {
    return workerHot(rt.encoder.chatBusy.load(std::memory_order_acquire) != 0,
                     rt.lastUserMs.load(std::memory_order_relaxed), steadyNowMs());
}
} // namespace

Turnscope::Turnscope() : prev(tlsTurn) { tlsTurn = &local; }
Turnscope::~Turnscope() { tlsTurn = prev; }

Turnstats& activeTurn(Runtime& rt) { return tlsTurn ? *tlsTurn : rt.turn; }

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
    prompt = json::array(
        {textMessage("system", formatPrompt(rulesArr, d.toJson(), json::array(), hasGatePack ? &packRaw : nullptr))});
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
    json out = {{"gate", gate},     {"cache", cache},     {"local", local},
                {"queued", queued}, {"distill", distill}, {"fingerprint", fingerprint},
                {"retain", retain}, {"naive", naive},     {"picked", picked},
                {"kept", kept},     {"packtok", packtok}, {"packn", packn}};
    // 未调 gate 禁止用 0 冒充「已测省 0 次」
    if (hasSaved)
        out["saved"] = saved;
    if (hasAnswer)
        out["answer"] = answer;
    if (!source.empty())
        out["source"] = source;
    if (sawRules || !prompt.empty()) {
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
    std::sort(texts.begin(), texts.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
    Turnstats& t = activeTurn(rt);
    std::lock_guard<std::mutex> lock(t.mutex);
    t.sawRules = true;
    t.ruleText = std::move(texts);
    t.rebuildCorpus();
}

void noteKept(Runtime& rt, std::vector<std::pair<std::string, std::string>> kept) {
    std::sort(kept.begin(), kept.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
    Turnstats& t = activeTurn(rt);
    std::lock_guard<std::mutex> lock(t.mutex);
    t.sawRules = true;
    t.ruleKept = std::move(kept);
    t.rebuildCorpus();
}

void notePrompt(Runtime& rt, json prompt) {
    if (!prompt.is_array() || prompt.empty())
        return;
    Turnstats& t = activeTurn(rt);
    std::lock_guard<std::mutex> lock(t.mutex);
    t.prompt = std::move(prompt);
    t.source = "injected";
    rt.promptInjected.fetch_add(1, std::memory_order_relaxed);
    t.rebuildCorpus();
}

void noteQueued(Runtime& rt, std::string const& id) {
    if (id.empty())
        return;
    Turnstats& t = activeTurn(rt);
    std::lock_guard<std::mutex> lock(t.mutex);
    t.queued.push_back(id);
}

void noteDistill(Runtime& rt, std::string const& id, bool didChat) {
    if (id.empty())
        return;
    Turnstats& t = activeTurn(rt);
    std::lock_guard<std::mutex> lock(t.mutex);
    t.distill.push_back(id);
    if (didChat)
        ++t.local;
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
    rt.observeCalls.fetch_add(1, std::memory_order_relaxed);
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
    static char const* const kSys = "Distill the observation into a short durable memory note. "
                                    "Output plain text only: first line title, then summary. "
                                    "Do not invent policy rules. If content conflicts with coding standards, "
                                    "prefix with CONFLICT:";

    while (!rt.workerStop.load(std::memory_order_acquire)) {
        if (userHot(rt)) {
            rt.yieldSkip.fetch_add(1, std::memory_order_relaxed);
            std::unique_lock<std::mutex> lk(rt.workerMutex);
            rt.workerCv.wait_for(lk, std::chrono::milliseconds(250),
                                 [&] { return rt.workerStop.load(std::memory_order_acquire); });
            continue;
        }
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
            if (userHot(rt)) {
                rt.yieldSkip.fetch_add(1, std::memory_order_relaxed);
                for (int w = 0; w < 20 && userHot(rt) && !rt.workerStop.load(std::memory_order_acquire); ++w)
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
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

                std::string user = json{{"title", title}, {"summary", summary}, {"outcome", outcome}}.dump();
                std::string distilled;
                bool didChat = false;
                try {
                    distilled = rt.encoder.distillChat(kSys, user, 0.0f, 160);
                    distilled = stripThink(std::move(distilled));
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

                // 版本化合并：同 title 的旧 memory 标记 deprecated，保留最新高置信版本。
                std::size_t version = 1;
                {
                    std::lock_guard<std::mutex> lock(rt.store.mutex);
                    std::size_t maxv = 0;
                    for (auto& kv : rt.store.docs) {
                        auto& old = kv.second;
                        if (old.meta.value("kind", "") != "memory")
                            continue;
                        if (old.meta.value("title", "") != title)
                            continue;
                        std::size_t v = old.meta.value("version", static_cast<std::size_t>(0));
                        if (v > maxv)
                            maxv = v;
                        old.meta["deprecated"] = true;
                        (void)rt.store.base.put(old, Store::keyOf(old.id));
                    }
                    version = maxv + 1;
                }

                Doc doc;
                doc.id = memId;
                doc.text = std::move(distilled);
                doc.meta = {
                    {"kind", "memory"},     {"title", title},   {"version", version},           {"deprecated", false},
                    {"conflict", conflict}, {"source", row.id}, {"outcome", std::move(outcome)}};
                auto vec = rt.encoder.embed(doc.text);

                // 指数退避重试：USearch 写失败重试 3 次（100/200/400ms），仍失败落 fail 队列。
                bool stored = false;
                for (int attempt = 0; attempt < 3 && !stored; ++attempt) {
                    error_t err = rt.store.upsert(doc, vec);
                    if (!err) {
                        stored = true;
                    } else {
                        (void)err.release();
                        if (attempt < 2)
                            std::this_thread::sleep_for(std::chrono::milliseconds(100u << attempt));
                    }
                }
                if (stored) {
                    noteDistill(rt, memId, didChat);
                    std::lock_guard<std::mutex> lock(rt.store.mutex);
                    if (conflict)
                        (void)rt.store.base.auditPut(row.id, "conflict", {{"id", row.id}});
                    (void)rt.store.base.finish(row.id, "done");
                } else {
                    std::lock_guard<std::mutex> lock(rt.store.mutex);
                    (void)rt.store.base.auditPut(row.id, "fail", {{"reason", "upsert retry exhausted"}});
                    (void)rt.store.base.finish(row.id, "fail");
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
      decider(std::move(other.decider)), l1(std::move(other.l1)), l1tick(other.l1tick), fuse(std::move(other.fuse)),
      workerStop(other.workerStop.load()), worker(std::move(other.worker)) {
    other.l1tick = 0;
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
        l1tick = other.l1tick;
        other.l1tick = 0;
    }
    fuse = std::move(other.fuse);
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
