/**
 *  @file       api.hpp
 *  @brief      HTTP/MCP 共享声明。Windows 须先 winsock2 再进 USearch，避免 sockaddr 重定义。
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
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

/** 进程级只读快照。全部字段由 `.config/config.json` 提供，无产品默认值。 */
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
};

struct Experience {
    std::string title;
    std::string summary;
    std::vector<std::string> tags;
    std::vector<std::string> commands;
    std::vector<std::string> files;
    std::string outcome = "ok";
};

std::pair<std::string, std::string> parseFront(std::string const& text);
std::vector<Rule> loadRules(fs::path const& dir);
std::vector<Rule> resolveRules(std::vector<Rule> const& rules, std::string const& query);
Experience experienceFromJson(json const& j);
expected_gt<std::string> saveMark(fs::path const& dir, Experience const& exp);
std::size_t estimate(std::string_view text);
std::size_t estimateMany(std::vector<std::string_view> const& texts);
std::string compressBody(std::string_view body, std::string_view task, std::size_t maxBullets);
std::string compressSections(std::string_view body, std::string_view task, std::size_t maxSections);

struct Runtime {
    fs::path root;
    Config config;
    Encoder encoder;
    Store store;
    std::vector<Rule> rules;
    Runtime() = default;
    Runtime(Runtime const&) = delete;
    Runtime& operator=(Runtime const&) = delete;
    Runtime(Runtime&&) noexcept = default;
    Runtime& operator=(Runtime&&) noexcept = default;
    static expected_gt<Runtime> open(fs::path const& root);
    fs::path workspace() const;
    std::string shell(std::string const& command) const;
    expected_gt<json> saveExperience(Experience const& exp);
};

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

json toolDefs();
json callTool(Runtime& rt, std::string const& name, json const& args);
json mcpHandle(Runtime& rt, json const& req);
void mountOpenai(httplib::Server& svr, Runtime& rt,
                 std::function<bool(httplib::Request const&, httplib::Response&)> gate);
int serve(Runtime& rt);
int runAgent(Runtime& rt, std::string const& instruction);

} // namespace api
