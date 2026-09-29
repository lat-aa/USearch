/**
 *  @file       worker.hpp
 *  @brief      Async：observe 入队 / Worker 蒸馏 / 请求级 Turnstats。
 */
#pragma once

#include "render.hpp"
#include "types.hpp"

#include <chrono>

namespace api {

struct Runtime;
struct Turncall;

/** epoch 毫秒（墙钟）：hook 用它判定「这份账本是不是本轮的」（归因），避免贴到别的请求。 */
inline std::int64_t epochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/** Worker 硬让路判据（纯函数，供 apexworker 直测）：busy 或窗口内视为用户热路径。 */
inline bool workerHot(bool chatBusy, std::int64_t lastUserMs, std::int64_t nowMs,
                      std::int64_t windowMs = 2000) noexcept {
    if (chatBusy)
        return true;
    return lastUserMs != 0 && nowMs - lastUserMs < windowMs;
}

/**
 * 本轮实测计数（进程内）。
 * 供 `cost.turn` / 🔖 叙事；缺测字段在 toJson 中省略，禁止用 0 冒充未上报。
 * 含 mutex，不可整体移动——Runtime 移动时丢弃本轮统计即可。
 */
struct Turnstats {
    std::mutex mutex;
    std::string gate = "none";  ///< 历史字段：门控已移除，恒 none
    std::string cache = "none"; ///< L1|L2|miss|none
    bool hasSaved = false;      ///< 未测省量时 toJson 省略 saved
    int saved = 0;              ///< 省主 LLM 次数
    int local = 0;              ///< 本轮 Nanbeige chat 次数
    std::vector<std::string> queued;
    std::vector<std::string> distill;
    std::string fingerprint;
    float retain = 0;
    std::size_t naive = 0;
    std::size_t picked = 0;
    std::size_t kept = 0;
    std::size_t packtok = 0;
    std::size_t packn = 0;
    float answer = 0;
    bool hasAnswer = false;
    bool sawRules = false;
    bool didAnn = false;
    std::size_t annK = 0;
    /** 工具侧：全文 / 裁剪后 / 真实 gate pack（不进统计块重复粘贴）。 */
    std::vector<std::pair<std::string, std::string>> ruleText;
    std::vector<std::pair<std::string, std::string>> ruleKept;
    std::vector<std::pair<std::string, std::string>> packText;
    /** gate 返回的 pack 对象；仅 items 非空时写入 prompt.pack。 */
    json packRaw = json::object();
    /** 模型侧命令包：`Local knowledge JSON follows.` + 完整 knowledge JSON（禁止省略 body）。 */
    json prompt = json::array();
    /** injected=/v1 notePrompt；rebuild=cost 重建；空=未生成。 */
    std::string source;
    /** 统计块正文：人读 markdown（路由一行 + 规则 ###）；禁止再贴 rules/kept/pack 段。 */
    std::string corpus;
    /** 请求开始时的计数器快照：本轮增量 = 当前值 - 快照（0 计数的口径与 /ready 同源）。 */
    bool hasSnap = false;
    std::uint64_t snapL1 = 0, snapL2 = 0, snapOk = 0, snapUp = 0;
    std::uint64_t snapChats = 0, snapDistills = 0, snapEmbeds = 0, snapSearches = 0, snapHits = 0, snapTools = 0;

    /** 调用方须已持有 mutex。从 prompt JSON 生成人读 corpus；勿把 JSON 原样贴进聊天。 */
    void rebuildCorpus();
    /** 调用方须已持有 mutex。重建 prompt 并标 source=rebuild；无真实 gate pack 时省略 pack 键。 */
    void rebuildPrompt(Decision const& d);
    /** 调用方须已持有 mutex。 */
    json toJson() const;
};

/** 请求级 Turnstats：构造时绑定 TLS，析构还原；并发 Edge 互不覆盖。 */
struct Turnscope {
    Turnstats local;
    Turnstats* prev = nullptr;
    Turnscope();
    ~Turnscope();
    Turnscope(Turnscope const&) = delete;
    Turnscope& operator=(Turnscope const&) = delete;
};

/** 政策目录指纹：规则变更使 L1/L2 缓存失效。 */
std::string policyFingerprint(std::vector<Rule> const& rules);

/** 请求开始快照（/v1 处理器 Turnscope 之后调用一次）：本轮增量 = 当前 - 快照。 */
void turnSnap(Runtime& rt);
/** 从快照算出本轮增量账本（未 snap 时全 0）。 */
Turncall turnDelta(Runtime& rt);

/** observe 仅入队；Worker 后台蒸馏写 memory。 */
json runObserve(Runtime& rt, json const& args);
void workerLoop(Runtime& rt);
/** 当前线程的 Turnstats（TLS 优先，否则进程级 turn）。 */
Turnstats& activeTurn(Runtime& rt);

void noteRules(Runtime& rt, std::vector<Resolvedrule> const& matched);
/** 记入裁剪后正文（turn.clip；与 optimized_tokens 对齐）。 */
void noteKept(Runtime& rt, std::vector<std::pair<std::string, std::string>> kept);
/** /v1 主路径写入实测命令包；source=injected，覆盖 rebuild。 */
void notePrompt(Runtime& rt, json prompt);
/** observe 入队 id。 */
void noteQueued(Runtime& rt, std::string const& id);
/** Worker 落盘 memory id；didChat 表示跑过本地蒸馏 chat。 */
void noteDistill(Runtime& rt, std::string const& id, bool didChat);

} // namespace api
