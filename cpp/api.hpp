/**
 *  @file       api.hpp
 *  @brief      HTTP/MCP 共享声明。Windows 须先 winsock2 再进 USearch，避免 sockaddr 重定义。
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

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <usearch/index_dense.hpp>

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
    Temppolicy temperature {};
    std::uint8_t wmedium = 0;
    std::uint8_t wcomplex = 0;
    std::size_t maxtask = 0;
    std::size_t maxfile = 0;
    float keeplow = 0;
    float keepmid = 0;
    float keephigh = 0;
    std::array<std::size_t, 4> topk {};
    std::string lexicon; ///< 词表路径（默认 `.config/decide/config.toml`，含 complex / medium）
};

/** 前置门控参数（全部来自 `config.gate`；缺键启动失败）。 */
struct Gateconfig {
    float threshold = 0;  ///< answerConfidence 直答阈值
    float l2 = 0;         ///< L2 语义缓存最低相似度
    std::size_t cachek = 0;
    int reflect = 0; ///< 0|1|2 额外反思轮
    float minevid = 0;
    float wevid = 0;
    float wself = 0;
    std::size_t packtok = 0;
    std::size_t minlen = 0;
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
    Chat chat {};
    Decideconfig decide {};
    Gateconfig gate {};
    std::size_t shadow = 0;
    std::uint32_t rate = 0;
    double refill = 0.0;
    static expected_gt<Config> load(fs::path const& path);
};

fs::path findRoot();
fs::path joinRoot(fs::path const& root, std::string const& relative);
std::uint64_t fnv1a64(std::string_view text) noexcept;
std::vector<float> hashEmbed(std::string_view text, std::size_t dimensions);

namespace sq8 {
constexpr float epsSlack = 1.0001f;
constexpr float i8i16Inv = 1.0f / (127.0f * 32767.0f);
std::size_t candidateBudget(std::size_t k);
float epsFromMaxScale(std::size_t dim, float maxScale);
float quantizeRow(float const* row, std::int8_t* dst, std::size_t dim);
void buildShadow(float const* data, std::size_t n, std::size_t dim, std::vector<std::int8_t>& q8,
                 std::vector<float>& qscale, float& eps);
std::vector<std::int16_t> quantizeQueryI16(float const* q, std::size_t dim);
std::int32_t i8Dot(std::int8_t const* doc, std::int16_t const* q, std::size_t dim);
float coarseScore(std::int32_t dot, float scale);
bool selectCandidates(float const* est, std::size_t n, std::size_t k, float eps,
                      std::vector<std::size_t>& out);
float dot8(float const* a, float const* b, std::size_t dim);
std::vector<std::pair<std::size_t, float>> selectTopKExact(
    std::size_t n, std::size_t k, float threshold, std::size_t const* rows, std::size_t nrows,
    std::function<float(std::size_t)> scoreAt);
} // namespace sq8

struct Doc {
    std::string id;
    std::string text;
    json meta = json::object();
};

/** SQLite 旁路：open/close/get/put/del/list。 */
struct Base {
    void* db = nullptr;
    Base() = default;
    Base(Base const&) = delete;
    Base& operator=(Base const&) = delete;
    Base(Base&& other) noexcept;
    Base& operator=(Base&& other) noexcept;
    ~Base();
    static expected_gt<Base> open(fs::path const& path);
    void close();
    error_t begin();
    error_t commit();
    error_t rollback();
    expected_gt<Doc> get(std::string const& id);
    error_t put(Doc const& doc, std::uint64_t key);
    error_t del(std::string const& id);
    expected_gt<std::vector<Doc>> list();
    /** 入队 pending；立即返回（沉淀重活在 Worker）。 */
    error_t enqueue(std::string const& id, json const& payload);
    /** 原子领取一条 pending→running；无任务则 failed。 */
    expected_gt<Queuerow> claim();
    error_t finish(std::string const& id, std::string const& status);
    error_t auditPut(std::string const& id, std::string const& kind, json const& detail);
};

struct Encoder {
    std::size_t dimensions = 0;
    bool modelReady = false;
    std::string ggufPath;
    std::string modelId = "Nanbeige4.1-3B";
    float temperature = 0.0f;
    std::uint32_t maxTokens = 0;
    std::uint32_t ctx = 0;
    int gpu = 0;
    int threads = 0;
    ::llama_model* model = nullptr;
    ::llama_context* context = nullptr;
    ::llama_context* chatCtx = nullptr;
    std::mutex mutex;
    Encoder() = default;
    ~Encoder();
    Encoder(Encoder const&) = delete;
    Encoder& operator=(Encoder const&) = delete;
    Encoder(Encoder&& other) noexcept;
    Encoder& operator=(Encoder&& other) noexcept;
    void close();
    error_t open(fs::path const& gguf, std::uint32_t ctxSize, int gpuLayers, int nThreads, float temp,
                 std::uint32_t maxTok);
    std::vector<float> embed(std::string_view text);
    std::string chat(std::string_view system, std::string_view user);
    std::string chat(std::string_view system, std::string_view user, std::function<void(std::string_view)> onDelta);
};

/** USearch 图 + SQLite 载荷 + 可选 SQ8；upsert=put→add/save→commit。 */
struct Store {
    Dense index;
    std::unordered_map<std::string, Doc> docs;
    std::vector<float> floats;
    std::vector<std::string> order;
    std::vector<std::int8_t> q8;
    std::vector<float> qscale;
    float eps = 0.0f;
    bool quant = false;
    std::size_t shadowGate = 4096;
    Base base;
    fs::path indexPath;
    std::size_t dimensions = 0;
    std::mutex mutex;
    Store() = default;
    Store(Store const&) = delete;
    Store& operator=(Store const&) = delete;
    Store(Store&& other) noexcept;
    Store& operator=(Store&& other) noexcept;
    static expected_gt<Store> make(std::size_t dimensions, fs::path indexPath, fs::path basePath,
                                   std::size_t shadowGate);
    static std::uint64_t keyOf(std::string_view id) noexcept;
    error_t hydrate();
    error_t persistIndex();
    void rebuildShadow();
    error_t upsert(Doc doc, std::vector<float> const& vector);
    error_t remove(std::string const& id);
    std::vector<std::pair<Doc, float>> search(std::vector<float> const& query, std::size_t k);
};

struct Rule {
    std::string name;
    std::string description;
    std::string body;
    std::string path;
    bool always = false;   ///< 无条件激活
    bool enabled = true;
    std::vector<std::string> globs; ///< 路径 glob；非空则走 Glob，不进 Semantic
};

/** 规则激活原因（对齐 apex：Always → Glob → Semantic → Manual）。 */
enum class Activation : std::uint8_t { Always, Glob, Semantic, Manual };

char const* activationName(Activation a) noexcept;

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
    float score = 0.0f; ///< Semantic 相似度；其余为 0
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

struct Runtime;

std::pair<Frontmatter, std::string> parseFront(std::string const& text);
std::vector<Rule> loadRules(fs::path const& dir);
/** 纯词法语义（无 Encoder）；供轻量路径。 */
std::vector<Resolvedrule> resolveRules(std::vector<Rule> const& rules, Rulequery const& q);
/** 语义分支优先用 Encoder 稠密向量，否则回退 hashEmbed / 词法。 */
std::vector<Resolvedrule> resolveRules(Runtime& rt, Rulequery const& q);
/** 复用已算好的 task 向量，避免门控路径上二次 embed。 */
std::vector<Resolvedrule> resolveRules(Runtime& rt, Rulequery const& q, std::vector<float> const& qVec);
/** 从 JSON 填 Rulequery：task|query、files、manual。 */
Rulequery rulequeryFromJson(json const& body);
bool globMatch(std::string_view pattern, std::string_view path);

struct Experience {
    std::string title;
    std::string summary;
    std::vector<std::string> tags;
    std::vector<std::string> commands;
    std::vector<std::string> files;
    std::string outcome = "ok";
};

Experience experienceFromJson(json const& j);
expected_gt<std::string> saveMark(fs::path const& dir, Experience const& exp);
std::size_t estimate(std::string_view text);
std::size_t estimateMany(std::vector<std::string_view> const& texts);
std::string compressBody(std::string_view body, std::string_view task, std::size_t maxBullets);
std::string compressSections(std::string_view body, std::string_view task, std::size_t maxSections);
/** 按激活档分配 section 配额：Always=满额，Semantic 按分占比，其余=半额起。 */
int ruleSections(Activation act, float score, float scoreSum, int baseSections) noexcept;

bool authOk(Runtime const& rt, httplib::Request const& req);
void setJson(httplib::Response& res, json const& body, int status = 200);
std::string messageText(json const& content);

struct Bucket {
    double capacity = 0;
    double tokens = 0;
    double refill = 1.0;
    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
    explicit Bucket(std::uint32_t cap = 0, double refillPerSec = 1.0);
    bool tryAcquire();
};

struct Stage {
    std::string name;
    std::function<bool(Runtime&, json&)> run;
};

struct Pipeline {
    std::vector<Stage> stages;
    Pipeline& stage(Stage s);
    bool run(Runtime& rt, json& ctx);
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

char const* modelName(Model m) noexcept;
char const* depthName(Depth d) noexcept;
char const* retrievalName(Retrieval r) noexcept;
Model modelDowngrade(Model m) noexcept;
Depth depthDowngrade(Depth d) noexcept;
Retrieval retrievalDowngrade(Retrieval r) noexcept;

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

struct Decider {
    std::uint64_t speedMs = 0;
    std::uint8_t highComplexity = 0;
    std::uint8_t lowComplexity = 0;
    std::uint8_t margin = 0;
    Temppolicy temperature {};
    std::uint8_t wmedium = 0;
    std::uint8_t wcomplex = 0;
    std::size_t maxtask = 0;
    std::size_t maxfile = 0;
    float keeplow = 0;
    float keepmid = 0;
    float keephigh = 0;
    std::array<std::size_t, 4> topk {};
    std::vector<std::string> complex;
    std::vector<std::string> medium;

    /** 从配置 + 已载入词表构造；词表须非空。 */
    static expected_gt<Decider> open(Decideconfig const& cfg, Lexicon lexicon);
    Features features(Decideinput const& input) const;
    Decision decide(Decideinput const& input) const;
    Decision decideFrom(Features const& f) const;
    /** Retrieval → config.decide.topk[L0..L3]。 */
    std::size_t topkFor(Retrieval r) const noexcept;
    /** compression 保留比 → 规则裁剪段数：≥keepmid→3，≥keeplow→2，否则 1。 */
    int sectionsFor(float compression) const noexcept;
};

expected_gt<Lexicon> loadLexicon(fs::path const& path);

/** L1 精确缓存条目（进程内；键含政策指纹）。 */
struct L1entry {
    std::string reply;
    std::string fingerprint;
};

/**
 * 本轮实测计数（进程内）。
 * 供 `cost.turn` / 🔖 叙事；缺测字段在 toJson 中省略，禁止用 0 冒充未上报。
 * 含 mutex，不可整体移动——Runtime 移动时丢弃本轮统计即可。
 */
struct Turnstats {
    std::mutex mutex;
    std::string gate = "none";  ///< answered|pack|refuse|none
    std::string cache = "none"; ///< L1|L2|miss|none
    bool hasSaved = false;      ///< 未调 gate 时 toJson 省略 saved
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
    bool sawGate = false;
    bool didAnn = false;
    std::size_t annK = 0;
    /** 命中规则全文 / pack 条目正文，供拼 corpus。 */
    std::vector<std::pair<std::string, std::string>> ruleText;
    std::vector<std::pair<std::string, std::string>> packText;
    std::string corpus;

    /** 调用方须已持有 mutex。按契约拼 ## rules / ## pack。 */
    void rebuildCorpus();
    /** 调用方须已持有 mutex。 */
    json toJson() const;
};

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
    /** 本轮 gate/rules/observe/worker 实测；cost 读出。 */
    Turnstats turn;
    /** Worker 空转等待；observe 入队后 notify，避免固定 400ms 轮询。 */
    std::mutex workerMutex;
    std::condition_variable workerCv;
    std::atomic<bool> workerStop {false};
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

/** 政策目录指纹：规则变更使 L1/L2 缓存失效。 */
std::string policyFingerprint(std::vector<Rule> const& rules);
/** 前置门控：L1/L2→政策∥记忆→RRF→Nanbeige 混合置信→answered|pack|refuse。 */
json runGate(Runtime& rt, json const& args);
/** observe 仅入队；Worker 后台蒸馏写 memory。 */
json runObserve(Runtime& rt, json const& args);
void workerLoop(Runtime& rt);
/** 把本轮命中规则全文记入 turn（供 corpus ## rules）。 */
void noteRules(Runtime& rt, std::vector<Resolvedrule> const& matched);
/** 把 gate 结局记入 turn；localChats=本路径 Nanbeige chat 次数。 */
void noteGate(Runtime& rt, json const& result, bool didAnn, std::size_t annK, int localChats);
/** observe 入队 id。 */
void noteQueued(Runtime& rt, std::string const& id);
/** Worker 落盘 memory id；didChat 表示跑过本地蒸馏 chat。 */
void noteDistill(Runtime& rt, std::string const& id, bool didChat);

/** 解析 route/decide 请求体；键名：task|query、files、latency、hints。 */
Decideinput decideinputFromJson(json const& body);
/** 解析单个 hint 字符串；未知值勿调用（由 decideinputFromJson 白名单过滤）。 */
Hint parseHint(std::string const& s);

/** MCP 请求附带的客户端元数据（来自 HTTP 头；缺省时字段为空）。 */
struct Mcpclient {
    std::string actualModel;       ///< X-Apex-Actual-Model
    std::string actualModelSource; ///< X-Apex-Actual-Model-Source
};

json toolDefs();
json callTool(Runtime& rt, std::string const& name, json const& args, Mcpclient const& client = {});
json mcpHandle(Runtime& rt, json const& req, Mcpclient const& client = {});
void mountOpenai(httplib::Server& svr, Runtime& rt,
                 std::function<bool(httplib::Request const&, httplib::Response&)> gate);
int serve(Runtime& rt);
int runAgent(Runtime& rt, std::string const& instruction);

} // namespace api
