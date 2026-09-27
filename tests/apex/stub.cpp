/**
 * @file stub.cpp
 * @brief apexgate 单测桩：替换 Encoder/Store/Base/fnv，禁止链 llama/sqlite。
 */
#include "../../tools/apex/api.hpp"

#include <cstring>
#include <unordered_map>
#include <vector>

namespace api {
namespace {

struct Stubstate {
    std::vector<std::pair<Doc, float>> hits;
    std::string chatReply = R"({"status":"answered","self":0.95,"reply":"stub answer text ok","conflicts":[],"missing":[],"pack":[]})";
    std::vector<float> emb = {1.f, 0.f, 0.f, 0.f};
    std::unordered_map<std::string, json> queue;
    std::vector<std::string> audits;
};

Stubstate& state() {
    static Stubstate s;
    return s;
}

} // namespace

void stubReset() { state() = Stubstate {}; }
void stubSetHits(std::vector<std::pair<Doc, float>> hits) { state().hits = std::move(hits); }
void stubSetChat(std::string reply) { state().chatReply = std::move(reply); }
std::vector<std::string> const& stubAudits() { return state().audits; }

std::uint64_t fnv1a64(std::string_view text) noexcept {
    std::uint64_t h = 14695981039346656037ull;
    for (unsigned char c : text) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

std::vector<float> hashEmbed(std::string_view, std::size_t dimensions) {
    return std::vector<float>(dimensions ? dimensions : 4, 0.25f);
}

void Encoder::close() {
    model = nullptr;
    context = nullptr;
    chatCtx = nullptr;
    modelReady = false;
}
Encoder::~Encoder() { close(); }
Encoder::Encoder(Encoder&& other) noexcept { *this = std::move(other); }
Encoder& Encoder::operator=(Encoder&& other) noexcept {
    if (this == &other)
        return *this;
    close();
    dimensions = other.dimensions;
    modelReady = other.modelReady;
    temperature = other.temperature;
    maxTokens = other.maxTokens;
    other.modelReady = false;
    return *this;
}
error_t Encoder::open(fs::path const&, std::uint32_t, int, int, float, std::uint32_t) {
    dimensions = 4;
    modelReady = true;
    return {};
}
std::vector<float> Encoder::embed(std::string_view) { return state().emb; }
std::string Encoder::chat(std::string_view, std::string_view) { return state().chatReply; }
std::string Encoder::chat(std::string_view s, std::string_view u,
                          std::function<void(std::string_view)>) {
    return chat(s, u);
}

Base::Base(Base&& other) noexcept : db(other.db) { other.db = nullptr; }
Base& Base::operator=(Base&& other) noexcept {
    if (this != &other) {
        close();
        db = other.db;
        other.db = nullptr;
    }
    return *this;
}
Base::~Base() { close(); }
expected_gt<Base> Base::open(fs::path const&) {
    expected_gt<Base> out;
    out.result.db = reinterpret_cast<void*>(1);
    return out;
}
void Base::close() { db = nullptr; }
error_t Base::begin() { return {}; }
error_t Base::commit() { return {}; }
error_t Base::rollback() { return {}; }
expected_gt<Doc> Base::get(std::string const&) {
    expected_gt<Doc> out;
    return out.failed("stub");
}
error_t Base::put(Doc const&, std::uint64_t) { return {}; }
error_t Base::del(std::string const&) { return {}; }
expected_gt<std::vector<Doc>> Base::list() {
    expected_gt<std::vector<Doc>> out;
    out.result = {};
    return out;
}
error_t Base::enqueue(std::string const& id, json const& payload) {
    state().queue[id] = payload;
    return {};
}
expected_gt<Queuerow> Base::claim() {
    expected_gt<Queuerow> out;
    if (state().queue.empty())
        return out.failed("empty");
    auto it = state().queue.begin();
    Queuerow row;
    row.id = it->first;
    row.payload = it->second;
    row.status = "running";
    state().queue.erase(it);
    out.result = std::move(row);
    return out;
}
error_t Base::finish(std::string const&, std::string const&) { return {}; }
error_t Base::auditPut(std::string const& id, std::string const& kind, json const&) {
    state().audits.push_back(id + ":" + kind);
    return {};
}

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
expected_gt<Store> Store::make(std::size_t dimensions, fs::path, fs::path, std::size_t) {
    expected_gt<Store> out;
    out.result.dimensions = dimensions;
    return out;
}
std::uint64_t Store::keyOf(std::string_view id) noexcept { return fnv1a64(id); }
error_t Store::hydrate() { return {}; }
error_t Store::persistIndex() { return {}; }
void Store::rebuildShadow() {}
error_t Store::upsert(Doc, std::vector<float> const&) { return {}; }
error_t Store::remove(std::string const&) { return {}; }
std::vector<std::pair<Doc, float>> Store::search(std::vector<float> const&, std::size_t) {
    return state().hits;
}

} // namespace api
