/**
 *  @file       cache.cpp
 *  @brief      L1 精确缓存与 L2 语义缓存；热路径 tryEmbed，忙则 fail-open。
 */
#include "cache.hpp"
#include "api.hpp"
#include "render.hpp"
#include <mutex>
namespace api {
/** L1 精确缓存键：政策指纹 + 任务哈希。 */
std::string l1Key(std::string const& policyFp, std::string const& task) {
    std::string t = task;
    std::size_t b = t.find_first_not_of(" \t\r\n");
    std::size_t e = t.find_last_not_of(" \t\r\n");
    if (b == std::string::npos)
        t.clear();
    else
        t = t.substr(b, e - b + 1);
    return policyFp + ":" + std::to_string(fnv1a64(t));
}

/** L1 命中返回 payload/source；未命中/过期返回 false。 */
bool l1Hit(Runtime& rt, std::string const& key, std::string& payload, std::string& source) {
    std::lock_guard<std::mutex> lock(rt.l1Mutex);
    auto it = rt.l1.find(key);
    if (it == rt.l1.end())
        return false;
    if (it->second.fingerprint != rt.policyFp)
        return false;
    if (it->second.expiresAtMs != 0 && steadyNowMs() >= it->second.expiresAtMs)
        return false;
    payload = it->second.reply;
    source = it->second.source;
    return true;
}

/** L2 语义缓存命中：kind=cache + 政策指纹一致 + 余弦相似度 >= 阈值。 */
bool l2Hit(Runtime& rt, std::string const& task, std::string& payload, std::string& source) {
    if (task.empty())
        return false;
    // 非阻塞：Worker 蒸馏会长期持 encoder 锁；此处忙则跳过 L2，保证 presync 永不被拖慢。
    auto q = rt.encoder.tryEmbed(task);
    if (q.empty())
        return false;
    auto hits = rt.store.search(q, 8);
    float minSim = rt.config.cache.l2Sim;
    for (auto const& [doc, score] : hits) {
        if (doc.meta.value("kind", "") != "cache")
            continue;
        if (doc.meta.value("fingerprint", "") != rt.policyFp)
            continue;
        float sim = 1.0f - score; // cos 距离 → 相似度
        if (sim >= minSim) {
            payload = doc.text;
            source = doc.meta.value("source", "local");
            return true;
        }
    }
    return false;
}

/** 本地 ok / 上游结果都入 L1+L2；失败只记日志不阻断应答。 */
void cachePut(Runtime& rt, std::string const& task, std::string const& payload, std::string const& source) {
    if (payload.empty())
        return;
    if (rt.config.cache.enableL1) {
        std::lock_guard<std::mutex> lock(rt.l1Mutex);
        l1Insert(rt.l1, rt.l1tick, l1Key(rt.policyFp, task), payload, source, rt.policyFp, rt.config.cache.l1Ttl);
    }
    if (rt.config.cache.enableL2 && !task.empty()) {
        Doc doc;
        doc.id = "cache-" + std::to_string(fnv1a64(task));
        doc.text = payload;
        doc.meta = {{"kind", "cache"}, {"fingerprint", rt.policyFp}, {"source", source}};
        auto vec = rt.encoder.tryEmbed(task);
        if (vec.empty())
            return; // 忙则跳过 L2 写，不堵应答路径
        if (error_t err = rt.store.upsert(std::move(doc), vec); err)
            (void)err.release();
    }
}
} // namespace api
