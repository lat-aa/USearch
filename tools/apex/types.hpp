/**
 *  @file       types.hpp
 *  @brief      平台前导 + usearch/json 别名 + 全进程共享纯数据类型。
 *
 *  Windows 须先 winsock2 再进 USearch，避免 sockaddr 重定义；各平面头统一 include 本文件。
 *  纯类型不含 Runtime / httplib，禁止在此放实现。
 */
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32) || defined(_WIN64) || defined(WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#ifdef byte
#undef byte
#endif
#endif

#include <dense/dense.hpp>
#include <nlohmann/json.hpp>

struct llama_model;
struct llama_context;

namespace api {

using namespace unum::usearch;
// glibc 的 ::error_t（bits/types/error_t.h）会与 usearch::error_t 冲突；钉死为本命名空间别名。
using error_t = unum::usearch::error_t;
using json = nlohmann::json;
namespace fs = std::filesystem;
using Dense = index_dense_gt<>;

struct Chat {
    float temperature = 0.0f;
    std::uint32_t max = 0;
};

/**
 * `/v1` 采样温度政策（编码场景偏低）。
 * 注入 @ref Decider，热路径只读本结构。不变量经 sanitized：low ≤ mid ≤ high ≤ cap ∈ [0,2]。
 */
struct Temppolicy {
    float low = 0.0f;
    float mid = 0.0f;
    float high = 0.0f;
    float cap = 0.0f;
    Temppolicy sanitized() const;
    float resolve(std::uint8_t complexity, std::uint8_t lowC, std::uint8_t highC, bool latencySensitive,
                  bool qualityPreferred) const;
};

/**
 * 决策引擎配置（全部来自 `config.decide` + 词表文件；缺键启动失败）。
 * 词表路径相对仓库根；运行时载入 @ref Decider::complex / medium。
 */
struct Decideconfig {
    std::uint64_t speed = 0;
    std::uint8_t high = 0;
    std::uint8_t low = 0;
    std::uint8_t margin = 0;
    Temppolicy temperature{};
    std::uint8_t wmedium = 0;
    std::uint8_t wcomplex = 0;
    std::size_t maxtask = 0;
    std::size_t maxfile = 0;
    float keeplow = 0;
    float keepmid = 0;
    float keephigh = 0;
    std::array<std::size_t, 4> topk{};
    std::string lexicon; ///< 词表路径（默认 `.config/decide/config.toml`，含 complex / medium）
};

/** 本地 agent 参数（替代旧 [gate] 路由）。 */
struct Agentconfig {
    bool enabled = true;     ///< false = /v1 直接走上游兜底（本地模型不合规/无 GPU 时用）
    bool skipComplex = true; ///< decide 判为 Strong(复杂) 时跳过本地，直接上游（省掉本地白跑 ~5-10s）
    std::size_t maxRounds = 3;
    float tokenBudget = 0.7f;
    bool enableFuse = true;
    std::size_t fuseFail = 5;
    std::uint32_t fuseRecover = 300;
};
/** 专用嵌入模型（bge-m3 等）；空 = 复用 chat 模型。 */
struct Embedconfig {
    std::string gguf;
    int gpu = -1000; ///< -1000 = 沿用 Config.gpu；0 = CPU（Pod 内 GPU 加载第二模型不稳时用）
};

/** 上游兜底（delegate 转发到远端大模型）。 */
struct Upstreamconfig {
    std::string base;
    std::string keyenv;
    std::string model = "deepseek-chat"; ///< 上游 /chat/completions 的 model 名
};
/** 独立短路缓存（纯命中，不评分）。 */
struct Cacheconfig {
    bool enableL1 = true;
    std::uint32_t l1Ttl = 3600;
    bool enableL2 = true;
    float l2Sim = 0.92f;
};
/** 检索与重排。 */
struct Retrievalconfig {
    float ruleWeight = 2.0f;
    std::size_t topK = 8;
};

/** observe 入队行（SQLite queue；不做重活）。 */
struct Queuerow {
    std::string id;
    json payload = json::object();
    std::string status; ///< pending|running|done|fail
    std::int64_t created = 0;
    std::int64_t updated = 0;
};

/** 进程级只读快照。全部字段由 `.config/config.toml` 提供，无产品默认值。 */
struct Config {
    std::string gguf;
    std::uint32_t ctx = 0;
    int gpu = 0;
    int threads = 0;
    std::string pooling;
    std::string listen;
    std::string index;
    std::string base;
    std::string knowledge;
    std::string rules;
    std::string workspace;
    std::string token;
    Chat chat{};
    Decideconfig decide{};
    Agentconfig agent{};
    Embedconfig embed{};
    Upstreamconfig upstream{};
    Cacheconfig cache{};
    Retrievalconfig retrieval{};
    std::size_t shadow = 0;
    std::uint32_t rate = 0;
    double refill = 0.0;
    static expected_gt<Config> load(fs::path const& path);
};

struct Doc {
    std::string id;
    std::string text;
    json meta = json::object();
};

struct Rule {
    std::string name;
    std::string description;
    std::string body;
    std::string path;
    bool always = false; ///< 无条件激活
    bool enabled = true;
    std::vector<std::string> globs; ///< 路径 glob；非空则走 Glob，不进 Semantic
    /** 启动时缓存的 description 向量；热路径禁止再 embed 每条规则。 */
    std::vector<float> descVec;
};

/** 规则激活原因（对齐 apex：Always → Glob → Semantic → Manual）。 */
enum class Activation : std::uint8_t { Always, Glob, Semantic, Manual };

/** 一次规则解析输入。 */
struct Rulequery {
    std::string task;
    std::vector<std::string> files;
    std::vector<std::string> manual; ///< 显式 @name
};

/** 一条被选中的规则。 */
struct Resolvedrule {
    Rule rule;
    Activation activation = Activation::Semantic;
    float score = 0.0f;                ///< Semantic 相似度；其余为 0
    std::vector<std::string> triggers; ///< Glob 命中的文件
    json toJson() const;
};

struct Frontmatter {
    std::string name;
    std::string description;
    bool always = false;
    bool enabled = true;
    std::vector<std::string> globs;
};

struct Experience {
    std::string title;
    std::string summary;
    std::vector<std::string> tags;
    std::vector<std::string> commands;
    std::vector<std::string> files;
    std::string outcome = "ok";
};

/** 模型能力档（建议档，≠ 会话真实模型名 / GGUF id）。 */
enum class Model : std::uint8_t { Weak, Standard, Strong };
/** 推理 / 思考深度（映射到 max_tokens 或上游 reasoning 强度由调用方解释）。 */
enum class Depth : std::uint8_t { Shallow, Medium, Deep };
/**
 * 检索深度：L0 窄召回 → L3 宽语义。
 * @see Decider::topkFor 映射到向量 search 的 k（来自 config.decide.topk）。
 */
enum class Retrieval : std::uint8_t { L0, L1, L2, L3 };

/**
 * 输入携带的硬约束 / 偏好。
 * Force* 压过启发式；PreferSpeed 视同时延敏感；PreferQuality 豁免时延降档并微升温度。
 */
enum class Hint : std::uint8_t {
    ForceWeak,
    ForceStandard,
    ForceStrong,
    ForceShallow,
    ForceMedium,
    ForceDeep,
    PreferSpeed,
    PreferQuality,
};

/** 一次路由决策结果；reasons 供人读与校准，不参与哈希。 */
struct Decision {
    Model model = Model::Standard;
    Depth depth = Depth::Medium;
    /** 上下文*保留*比例：1.0 全留；越低裁剪越狠（勿称作「压缩率」）。 */
    float compression = 1.0f;
    Retrieval retrieval = Retrieval::L2;
    /** 0..1；强制 hint 为 1.0；靠近分档边界时更低。 */
    float confidence = 0.5f;
    /** 请求未显式传 temperature 时由 chat 采用。 */
    float temperature = 0.2f;
    std::vector<std::string> reasons;
    json toJson() const;
};

/** 决策输入（HTTP/MCP JSON 经 decideinputFromJson 填入）。 */
struct Decideinput {
    std::string task;
    std::vector<std::string> files;
    /** 可选时延预算（毫秒）；小于 speedMs 则视为 latencySensitive。 */
    std::optional<std::uint64_t> latency;
    std::vector<Hint> hints;
};

/**
 * 可回放特征集：决策是它的纯函数。
 * @see Decider::features @see Decider::decideFrom
 */
struct Features {
    std::size_t taskLen = 0;
    std::size_t fileCount = 0;
    bool complexKeyword = false;
    bool mediumKeyword = false;
    bool latencySensitive = false;
    bool qualityPreferred = false;
    std::optional<Model> forcedModel;
    std::optional<Depth> forcedDepth;
    /** 0..100 复杂度分。 */
    std::uint8_t complexity = 0;
};

/** 一次可回放单元：特征 + 决策（展平 JSON 见 toJson）。 */
struct Deciderecord {
    Features features;
    Decision decision;
    json toJson() const;
};

/**
 * 确定性级联决策器。全部阈值与词表来自 @ref Decideconfig（启动时载入）。
 * 无 I/O、无全局可变状态；同 Features → 同 Decision。
 */
struct Lexicon {
    std::vector<std::string> complex;
    std::vector<std::string> medium;
};

} // namespace api
