/**
 *  @file       store.cpp
 *  @brief      USearch ANN + SQLite 载荷 + 可选 shadow（SQ8）。
 */

#include "store.hpp"
#include "api.hpp"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <mutex>

namespace api {
Store::Store(Store&& other) noexcept
    : index(std::move(other.index)), docs(std::move(other.docs)), floats(std::move(other.floats)),
      order(std::move(other.order)), q8(std::move(other.q8)), qscale(std::move(other.qscale)), eps(other.eps),
      quant(other.quant), shadowGate(other.shadowGate), base(std::move(other.base)),
      indexPath(std::move(other.indexPath)), dimensions(other.dimensions) {}

Store& Store::operator=(Store&& other) noexcept {
    if (this == &other)
        return *this;
    index = std::move(other.index);
    docs = std::move(other.docs);
    floats = std::move(other.floats);
    order = std::move(other.order);
    q8 = std::move(other.q8);
    qscale = std::move(other.qscale);
    eps = other.eps;
    quant = other.quant;
    shadowGate = other.shadowGate;
    base = std::move(other.base);
    indexPath = std::move(other.indexPath);
    dimensions = other.dimensions;
    return *this;
}

expected_gt<Store> Store::make(std::size_t dimensions, fs::path indexPath, fs::path basePath, std::size_t shadowGate) {
    expected_gt<Store> out;
    Store store;
    store.dimensions = dimensions;
    store.indexPath = std::move(indexPath);
    store.shadowGate = shadowGate;
    auto opened = Base::open(basePath);
    if (!opened)
        return out.failed(opened.error.release());
    store.base = std::move(opened.result);
    auto made = Dense::make(metric_punned_t(dimensions, metric_kind_t::cos_k));
    if (!made)
        return out.failed(made.error.release());
    store.index = std::move(made.index);
    if (!store.index.try_reserve(index_limits_t(1024)))
        return out.failed("failed to reserve usearch index");
    if (error_t err = store.hydrate(); err) {
        std::fprintf(stderr, "api: hydrate warn: %s\n", err.what());
        (void)err.release();
    }
    out.result = std::move(store);
    return out;
}

std::uint64_t Store::keyOf(std::string_view id) noexcept { return fnv1a64(id); }

error_t Store::hydrate() {
    auto listed = base.list();
    if (!listed)
        return listed.error.release();
    docs.clear();
    order.clear();
    floats.clear();
    for (auto& doc : listed.result) {
        order.push_back(doc.id);
        docs.emplace(doc.id, std::move(doc));
    }
    // 向量不在 SQLite：重启后靠 USearch 图；影子缓冲在首次 search/upsert 重建前为空。
    floats.assign(order.size() * dimensions, 0.0f);
    if (fs::is_regular_file(indexPath)) {
        auto loaded = index.load(indexPath.string().c_str());
        if (!loaded)
            return loaded.error.release();
        if (index.dimensions() != dimensions)
            return "index dim mismatch; delete index file and rebuild";
        if (!index.try_reserve(index_limits_t((std::max)(docs.size() + 1024, std::size_t{1024}))))
            return "reserve after load failed";
        // 从索引回填 floats，供 SQ8。
        for (std::size_t i = 0; i != order.size(); ++i) {
            auto key = keyOf(order[i]);
            // 缺向量则该行保持零；影子会标毒化 scale。
            (void)index.get(key, floats.data() + i * dimensions);
        }
    }
    rebuildShadow();
    return {};
}

error_t Store::persistIndex() {
    try {
        if (indexPath.has_parent_path())
            fs::create_directories(indexPath.parent_path());
        auto saved = index.save(indexPath.string().c_str());
        if (!saved)
            return saved.error.release();
    } catch (...) {
        return "persist index failed";
    }
    return {};
}

void Store::rebuildShadow() {
    quant = false;
    q8.clear();
    qscale.clear();
    eps = 0.0f;
    std::size_t n = order.size();
    if (n == 0 || dimensions == 0)
        return;
    if (n < shadowGate && shadowGate != 0)
        return;
    sq8::buildShadow(floats.data(), n, dimensions, q8, qscale, eps);
    quant = !q8.empty();
}

error_t Store::upsert(Doc doc, std::vector<float> const& vector) {
    if (vector.size() != dimensions)
        return "vector dimension mismatch";
    if (doc.id.empty())
        return "id required";
    std::lock_guard<std::mutex> lock(mutex);
    auto key = keyOf(doc.id);
    if (error_t e = base.begin(); e)
        return e;
    if (error_t e = base.put(doc, key); e) {
        (void)base.rollback();
        return e;
    }
    index.remove(key);
    auto added = index.add(key, vector.data());
    if (!added) {
        (void)base.rollback();
        return added.error.release();
    }
    if (error_t e = persistIndex(); e) {
        index.remove(key);
        (void)base.rollback();
        return e;
    }
    if (error_t e = base.commit(); e)
        return e;

    auto it = std::find(order.begin(), order.end(), doc.id);
    std::size_t row;
    if (it == order.end()) {
        row = order.size();
        order.push_back(doc.id);
        floats.resize(order.size() * dimensions);
    } else {
        row = static_cast<std::size_t>(it - order.begin());
    }
    std::copy(vector.begin(), vector.end(), floats.begin() + static_cast<std::ptrdiff_t>(row * dimensions));
    docs[doc.id] = std::move(doc);
    rebuildShadow();
    return {};
}

error_t Store::remove(std::string const& id) {
    std::lock_guard<std::mutex> lock(mutex);
    if (error_t e = base.begin(); e)
        return e;
    if (error_t e = base.del(id); e) {
        (void)base.rollback();
        return e;
    }
    index.remove(keyOf(id));
    if (error_t e = persistIndex(); e) {
        (void)base.rollback();
        return e;
    }
    if (error_t e = base.commit(); e)
        return e;
    auto it = std::find(order.begin(), order.end(), id);
    if (it != order.end()) {
        std::size_t row = static_cast<std::size_t>(it - order.begin());
        order.erase(it);
        if (order.empty() || dimensions == 0) {
            floats.clear();
        } else {
            std::vector<float> next(order.size() * dimensions, 0.0f);
            std::size_t oldN = floats.size() / dimensions;
            std::size_t dst = 0;
            for (std::size_t r = 0; r != oldN; ++r) {
                if (r == row)
                    continue;
                if (dst >= order.size())
                    break;
                std::copy(floats.begin() + static_cast<std::ptrdiff_t>(r * dimensions),
                          floats.begin() + static_cast<std::ptrdiff_t>((r + 1) * dimensions),
                          next.begin() + static_cast<std::ptrdiff_t>(dst * dimensions));
                ++dst;
            }
            floats.swap(next);
        }
    }
    docs.erase(id);
    rebuildShadow();
    return {};
}

std::vector<std::pair<Doc, float>> Store::search(std::vector<float> const& query, std::size_t k) {
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<std::pair<Doc, float>> hits;
    if (query.size() != dimensions || k == 0)
        return hits;

    // 有影子且语料够大时：SQ8 粗排 + f32 精排（与 Apex 一致，结果精确）。
    if (quant && order.size() >= k) {
        auto q16 = sq8::quantizeQueryI16(query.data(), dimensions);
        std::vector<float> est(order.size(), -std::numeric_limits<float>::infinity());
        for (std::size_t i = 0; i != order.size(); ++i) {
            auto d = sq8::i8Dot(q8.data() + i * dimensions, q16.data(), dimensions);
            est[i] = sq8::coarseScore(d, qscale[i]);
        }
        std::vector<std::size_t> cand;
        bool ok = sq8::selectCandidates(est.data(), order.size(), k, eps, cand);
        auto scored = sq8::selectTopKExact(
            order.size(), k, -std::numeric_limits<float>::infinity(), ok ? cand.data() : nullptr, ok ? cand.size() : 0,
            [&](std::size_t i) { return sq8::dot8(floats.data() + i * dimensions, query.data(), dimensions); });
        for (auto const& [row, score] : scored) {
            if (row >= order.size())
                continue;
            auto it = docs.find(order[row]);
            if (it != docs.end())
                hits.emplace_back(it->second, 1.0f - score); // 对外仍报「距离」风格时：cosine 距离≈1-sim
        }
        // 上式把相似度转成距离以贴近 USearch cos 距离；若 hits 非空则返回。
        if (!hits.empty())
            return hits;
    }

    auto results = index.search(query.data(), k);
    if (!results)
        return hits;
    std::vector<default_key_t> keys(k);
    std::vector<float> distances(k);
    std::size_t n = results.dump_to(keys.data(), distances.data());
    for (std::size_t i = 0; i != n; ++i) {
        for (auto const& [id, doc] : docs) {
            if (keyOf(id) == static_cast<std::uint64_t>(keys[i])) {
                hits.emplace_back(doc, distances[i]);
                break;
            }
        }
    }
    return hits;
}

} // namespace api
