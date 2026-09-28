/**
 *  @file       api.hpp
 *  @brief      装配根：Runtime（聚合四平面）+ runAgent 声明；其余声明见各平面头。
 *
 *  依赖单向：本文件汇总各平面头；平面头只 include types.hpp/render.hpp 并前向声明 Runtime。
 */
#pragma once

#include "agent.hpp"
#include "cache.hpp"
#include "config.hpp"
#include "decide.hpp"
#include "delegate.hpp"
#include "encode.hpp"
#include "http.hpp"
#include "mcp.hpp"
#include "openai.hpp"
#include "render.hpp"
#include "rules.hpp"
#include "shadow.hpp"
#include "store.hpp"
#include "types.hpp"
#include "worker.hpp"

namespace api {

struct Runtime {
    fs::path root;
    Config config;
    Encoder encoder;
    Store store;
    std::vector<Rule> rules;
    /** 启动时算一次；规则热重载前不变，免去每请求拼正文指纹。 */
    std::string policyFp;
    Decider decider;
    std::mutex l1Mutex;
    std::unordered_map<std::string, L1entry> l1;
    /** 单调时钟；命中/写入时 ++，供 LRU 比较（持 l1Mutex）。 */
    std::uint64_t l1tick = 0;
    /** 进程级兜底 turn（Worker queued/distill）；请求线程用 Turnscope。 */
    Turnstats turn;
    /** /v1 命中计数（进程级）。 */
    std::atomic<std::uint64_t> v1Responses{0};
    std::atomic<std::uint64_t> v1Chat{0};
    std::atomic<std::uint64_t> promptInjected{0};
    std::atomic<std::uint64_t> agentOk{0};
    std::atomic<std::uint64_t> agentDelegate{0};
    std::atomic<std::uint64_t> agentParsefail{0};
    std::atomic<std::uint64_t> agentRounds{0};  ///< 本地 agent 实际执行的工具轮次
    std::atomic<std::int64_t> lastUserMs{0};    ///< 最近一次用户请求时刻（Worker 让路用）
    std::atomic<std::uint64_t> presyncCalls{0}; ///< 仅 /v1/presync
    std::atomic<std::uint64_t> observeCalls{0}; ///< MCP observe 入队次数（沉淀）
    std::atomic<std::uint64_t> yieldSkip{0};    ///< Worker 因用户活跃跳过 claim/chat
    std::atomic<std::uint64_t> cacheL1{0};      ///< L1 精确缓存命中
    std::atomic<std::uint64_t> cacheL2{0};      ///< L2 语义缓存命中
    Fuse fuse;                                  ///< 本地 agent 解析熔断
    /** Worker 空转等待；observe 入队后 notify，避免固定 400ms 轮询。 */
    std::mutex workerMutex;
    std::condition_variable workerCv;
    std::atomic<bool> workerStop{false};
    std::thread worker;
    Runtime() = default;
    Runtime(Runtime const&) = delete;
    Runtime& operator=(Runtime const&) = delete;
    Runtime(Runtime&& other) noexcept;
    Runtime& operator=(Runtime&& other) noexcept;
    ~Runtime();
    static expected_gt<Runtime> open(fs::path const& root);
    fs::path workspace() const;
    std::string shell(std::string const& command) const;
    expected_gt<json> saveExperience(Experience const& exp);
    void startWorker();
    void stopWorker();
};

int runAgent(Runtime& rt, std::string const& instruction);

} // namespace api
