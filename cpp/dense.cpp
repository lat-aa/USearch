/**
 *  @file       dense.cpp
 *  @brief      Encoder + Store：hash/Nanbeige 嵌入门面；USearch ANN + SQLite 载荷 + 可选 SQ8。
 */

#include "api.hpp"

#include <llama.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <thread>

namespace api {

namespace {

std::string dryrunChat(std::string_view system, std::string_view user) {
    std::string low(user);
    for (char& c : low)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (low.find("summarize") != std::string::npos || low.find("experience") != std::string::npos ||
        low.find("induct") != std::string::npos) {
        return R"({"title":"dryrun","summary":"completed dryrun","tags":["dryrun"],"commands":[],"files":[],"outcome":"ok"})";
    }
    if (system.find("\"hits\":[]") != std::string::npos || low.find("insufficient") != std::string::npos) {
        return R"({"decision":"generate","commands":["echo dryrun"],"summary":"no local hits"})";
    }
    return R"({"decision":"enough","tools":[{"name":"status","arguments":{}}],"summary":"use local tools"})";
}

void l2norm(std::vector<float>& v) {
    float norm = 0.0f;
    for (float x : v)
        norm += x * x;
    norm = std::sqrt((std::max)(norm, 1e-12f));
    for (float& x : v)
        x /= norm;
}

void clearKv(llama_context* context) {
    if (!context)
        return;
    llama_memory_clear(llama_get_memory(context), true);
}

} // namespace

std::uint64_t fnv1a64(std::string_view text) noexcept {
    std::uint64_t hash = 14695981039346656037ull;
    for (unsigned char c : text) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::vector<float> hashEmbed(std::string_view text, std::size_t dimensions) {
    std::vector<float> out(dimensions, 0.0f);
    std::uint64_t state = fnv1a64(text);
    for (std::size_t i = 0; i != dimensions; ++i) {
        state ^= (state << 13);
        state ^= (state >> 7);
        state ^= (state << 17);
        state += 0x9E3779B97F4A7C15ull + i * 0x100000001B3ull;
        out[i] = static_cast<float>(static_cast<std::int32_t>(state & 0xFFFFFFFFu)) * (1.0f / 2147483648.0f);
    }
    l2norm(out);
    return out;
}

void Encoder::close() {
    if (chatCtx) {
        llama_free(chatCtx);
        chatCtx = nullptr;
    }
    if (context) {
        llama_free(context);
        context = nullptr;
    }
    if (model) {
        llama_model_free(model);
        model = nullptr;
    }
    modelReady = false;
}

Encoder::~Encoder() { close(); }

Encoder::Encoder(Encoder&& other) noexcept
    : dimensions(other.dimensions), modelReady(other.modelReady), ggufPath(std::move(other.ggufPath)),
      modelId(std::move(other.modelId)), temperature(other.temperature), maxTokens(other.maxTokens),
      ctx(other.ctx), gpu(other.gpu), threads(other.threads), model(other.model), context(other.context),
      chatCtx(other.chatCtx) {
    other.model = nullptr;
    other.context = nullptr;
    other.chatCtx = nullptr;
    other.modelReady = false;
}

Encoder& Encoder::operator=(Encoder&& other) noexcept {
    if (this == &other)
        return *this;
    close();
    dimensions = other.dimensions;
    modelReady = other.modelReady;
    ggufPath = std::move(other.ggufPath);
    modelId = std::move(other.modelId);
    temperature = other.temperature;
    maxTokens = other.maxTokens;
    ctx = other.ctx;
    gpu = other.gpu;
    threads = other.threads;
    model = other.model;
    context = other.context;
    chatCtx = other.chatCtx;
    other.model = nullptr;
    other.context = nullptr;
    other.chatCtx = nullptr;
    other.modelReady = false;
    return *this;
}

error_t Encoder::open(fs::path const& gguf, std::uint32_t ctxSize, int gpuLayers, int nThreads, float temp,
                      std::uint32_t maxTok) {
    close();
    temperature = temp;
    maxTokens = maxTok;
    ctx = ctxSize;
    gpu = gpuLayers;
    threads = nThreads > 0 ? nThreads : static_cast<int>(std::thread::hardware_concurrency());
    if (threads <= 0)
        threads = 4;
    ggufPath = gguf.string();
    if (!fs::is_regular_file(gguf)) {
        std::fprintf(stderr, "api: 缺少 gguf %s；使用 hash 嵌入\n", ggufPath.c_str());
        dimensions = 1024;
        modelReady = false;
        return {};
    }

    llama_backend_init();
    llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = gpuLayers;
    model = llama_model_load_from_file(ggufPath.c_str(), mparams);
    if (!model)
        return "failed to load gguf";

    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx = ctxSize ? ctxSize : 4096;
    cparams.n_batch = 512;
    cparams.n_ubatch = 512;
    cparams.n_threads = threads;
    cparams.n_threads_batch = threads;
    cparams.embeddings = true;
    cparams.pooling_type = LLAMA_POOLING_TYPE_LAST;
    context = llama_init_from_model(model, cparams);
    if (!context) {
        close();
        return "failed to create llama embed context";
    }

    llama_context_params chatParams = cparams;
    chatParams.embeddings = false;
    chatParams.pooling_type = LLAMA_POOLING_TYPE_NONE;
    chatCtx = llama_init_from_model(model, chatParams);
    if (!chatCtx) {
        close();
        return "failed to create llama chat context";
    }

    dimensions = static_cast<std::size_t>(llama_model_n_embd(model));
    modelReady = true;
    std::fprintf(stderr, "api: llama ready %s dim=%zu ctx=%u gpu=%d threads=%d\n", ggufPath.c_str(), dimensions,
                 cparams.n_ctx, gpuLayers, threads);
    return {};
}

std::vector<float> Encoder::embed(std::string_view text) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!modelReady || !model || !context)
        return hashEmbed(text, dimensions ? dimensions : 1024);

    llama_vocab const* vocab = llama_model_get_vocab(model);
    std::vector<llama_token> tokens(text.size() + 32);
    int n = llama_tokenize(vocab, text.data(), static_cast<int32_t>(text.size()), tokens.data(),
                          static_cast<int32_t>(tokens.size()), true, true);
    if (n < 0) {
        tokens.resize(static_cast<std::size_t>(-n));
        n = llama_tokenize(vocab, text.data(), static_cast<int32_t>(text.size()), tokens.data(),
                          static_cast<int32_t>(tokens.size()), true, true);
    }
    if (n <= 0)
        return hashEmbed(text, dimensions);
    tokens.resize(static_cast<std::size_t>(n));

    int nCtx = static_cast<int>(llama_n_ctx(context));
    if (n > nCtx)
        tokens.erase(tokens.begin(), tokens.end() - nCtx);

    clearKv(context);
    {
        int32_t nBatch = static_cast<int32_t>(llama_n_batch(context));
        for (std::size_t off = 0; off < tokens.size();) {
            std::size_t n = (std::min)(tokens.size() - off, static_cast<std::size_t>(nBatch));
            llama_batch batch = llama_batch_get_one(tokens.data() + off, static_cast<int32_t>(n));
            if (llama_decode(context, batch) != 0)
                return std::vector<float>(dimensions, 0.0f);
            off += n;
        }
    }

    float const* emb = llama_get_embeddings_seq(context, 0);
    if (!emb)
        emb = llama_get_embeddings_ith(context, static_cast<int32_t>(tokens.size()) - 1);
    if (!emb)
        return std::vector<float>(dimensions, 0.0f);

    std::vector<float> out(emb, emb + dimensions);
    l2norm(out);
    clearKv(context);
    return out;
}

std::string Encoder::chat(std::string_view system, std::string_view user) {
    return chat(system, user, {});
}

std::string Encoder::chat(std::string_view system, std::string_view user,
                          std::function<void(std::string_view)> onDelta) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!modelReady || !model || !chatCtx)
        return dryrunChat(system, user);

    std::string sysOwned(system);
    std::string userOwned(user);
    std::vector<llama_chat_message> msgs;
    if (!sysOwned.empty())
        msgs.push_back({"system", sysOwned.c_str()});
    msgs.push_back({"user", userOwned.c_str()});

    char const* tmpl = llama_model_chat_template(model, nullptr);
    std::string prompt;
    bool fromTmpl = false;
    if (tmpl && tmpl[0]) {
        std::vector<char> formatted(std::max<std::size_t>(sysOwned.size() + userOwned.size(), 1) * 2 + 1024);
        int32_t newLen = llama_chat_apply_template(tmpl, msgs.data(), msgs.size(), true, formatted.data(),
                                                   static_cast<int32_t>(formatted.size()));
        if (newLen > 0) {
            if (static_cast<std::size_t>(newLen) > formatted.size()) {
                formatted.resize(static_cast<std::size_t>(newLen) + 1);
                newLen = llama_chat_apply_template(tmpl, msgs.data(), msgs.size(), true, formatted.data(),
                                                   static_cast<int32_t>(formatted.size()));
            }
            if (newLen > 0) {
                prompt.assign(formatted.data(), static_cast<std::size_t>(newLen));
                fromTmpl = true;
            }
        }
    }
    if (prompt.empty()) {
        if (!sysOwned.empty()) {
            prompt = sysOwned;
            prompt.push_back('\n');
        }
        prompt += userOwned;
    }

    bool addSpecial = !fromTmpl;
    llama_vocab const* vocab = llama_model_get_vocab(model);
    std::vector<llama_token> tokens(prompt.size() + 32);
    int n = llama_tokenize(vocab, prompt.data(), static_cast<int32_t>(prompt.size()), tokens.data(),
                          static_cast<int32_t>(tokens.size()), addSpecial, true);
    if (n < 0) {
        tokens.resize(static_cast<std::size_t>(-n));
        n = llama_tokenize(vocab, prompt.data(), static_cast<int32_t>(prompt.size()), tokens.data(),
                          static_cast<int32_t>(tokens.size()), addSpecial, true);
    }
    if (n <= 0)
        return {};
    tokens.resize(static_cast<std::size_t>(n));

    clearKv(chatCtx);
    llama_sampler_chain_params sparams = llama_sampler_chain_default_params();
    llama_sampler* smpl = llama_sampler_chain_init(sparams);
    if (temperature > 0.0f)
        llama_sampler_chain_add(smpl, llama_sampler_init_temp(temperature));
    llama_sampler_chain_add(smpl, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));

    std::string reply;
    {
        int32_t nBatch = static_cast<int32_t>(llama_n_batch(chatCtx));
        for (std::size_t off = 0; off < tokens.size();) {
            std::size_t nTok = (std::min)(tokens.size() - off, static_cast<std::size_t>(nBatch));
            llama_batch batch = llama_batch_get_one(tokens.data() + off, static_cast<int32_t>(nTok));
            if (llama_decode(chatCtx, batch) != 0) {
                llama_sampler_free(smpl);
                clearKv(chatCtx);
                return {};
            }
            off += nTok;
        }
    }
    llama_token id = 0;
    std::uint32_t produced = 0;
    while (produced < maxTokens) {
        id = llama_sampler_sample(smpl, chatCtx, -1);
        llama_sampler_accept(smpl, id);
        if (llama_vocab_is_eog(vocab, id))
            break;
        char buf[256];
        int nPiece = llama_token_to_piece(vocab, id, buf, sizeof(buf), 0, true);
        if (nPiece < 0) {
            std::vector<char> big(static_cast<std::size_t>(-nPiece));
            nPiece = llama_token_to_piece(vocab, id, big.data(), static_cast<int32_t>(big.size()), 0, true);
            if (nPiece > 0) {
                std::string_view piece(big.data(), static_cast<std::size_t>(nPiece));
                reply.append(piece);
                if (onDelta)
                    onDelta(piece);
            }
        } else if (nPiece > 0) {
            std::string_view piece(buf, static_cast<std::size_t>(nPiece));
            reply.append(piece);
            if (onDelta)
                onDelta(piece);
        }
        llama_batch batch = llama_batch_get_one(&id, 1);
        if (llama_decode(chatCtx, batch) != 0)
            break;
        ++produced;
    }
    llama_sampler_free(smpl);
    clearKv(chatCtx);
    return reply;
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

expected_gt<Store> Store::make(std::size_t dimensions, fs::path indexPath, fs::path basePath,
                               std::size_t shadowGate) {
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
            order.size(), k, -std::numeric_limits<float>::infinity(), ok ? cand.data() : nullptr,
            ok ? cand.size() : 0, [&](std::size_t i) {
                return sq8::dot8(floats.data() + i * dimensions, query.data(), dimensions);
            });
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
