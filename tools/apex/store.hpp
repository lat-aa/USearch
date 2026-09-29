/**
 *  @file       store.hpp
 *  @brief      ModelMemory：SQLite 载荷（Base）+ USearch 图 + 可选 SQ8 影子（Store）。
 */
#pragma once

#include "types.hpp"

namespace api {

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

/** Store 观测快照（持锁读取）。 */
struct Storestats {
    std::size_t rows = 0;
    std::size_t docs = 0;
    bool quant = false;
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
    /// 检索用量（bge-m3 侧）：调用次数与命中条数，全部经 Store::search 单一入口累计。
    std::atomic<std::uint64_t> searches{0};
    std::atomic<std::uint64_t> hits{0};

    /** 观测快照：行数 / 文档数 / 是否 SQ8 影子（持 mutex）。 */
    Storestats stats();
};

} // namespace api
