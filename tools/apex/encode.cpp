/**
 *  @file       encode.cpp
 *  @brief      Encoder：chat/embed 双锁与采样进锁。
 */

#include "encode.hpp"
#include "api.hpp"

#include "render.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <llama.h>
#include <mutex>
#include <thread>

namespace api {

namespace {

void noteLockWait(Encoder& enc, std::chrono::steady_clock::time_point t0) {
    auto const ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    if (ms <= 0)
        return;
    enc.lockWaitN.fetch_add(1, std::memory_order_relaxed);
    enc.lockWaitSumMs.fetch_add(static_cast<std::uint64_t>(ms), std::memory_order_relaxed);
    std::int64_t prev = enc.lockWaitMaxMs.load(std::memory_order_relaxed);
    while (ms > prev && !enc.lockWaitMaxMs.compare_exchange_weak(prev, ms, std::memory_order_relaxed)) {
    }
}

std::string dryrunChat(json const& messages) {
    std::string text;
    for (auto const& m : messages) {
        if (!m.is_object())
            continue;
        if (!text.empty())
            text.push_back('\n');
        text += messageText(m.value("content", json()));
    }
    // 本地 agent 协议请求（system 提示含 <agent-result>）：无模型时也要给出合规 ok，
    // 否则缺 GGUF 会让 /v1 一律 delegate。蒸馏等其它调用保持旧行为。
    if (text.find("<agent-result>") != std::string::npos)
        return R"(<agent-result>{"status":"ok","tool_calls":[],"payload":"dryrun: 未加载 GGUF，本地 agent 返回占位应答"}</agent-result>)";
    std::string low(text);
    for (char& c : low)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (low.find("summarize") != std::string::npos || low.find("experience") != std::string::npos ||
        low.find("induct") != std::string::npos) {
        return R"({"title":"dryrun","summary":"completed dryrun","tags":["dryrun"],"commands":[],"files":[],"outcome":"ok"})";
    }
    if (text.find("\"hits\":[]") != std::string::npos || low.find("insufficient") != std::string::npos) {
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
      grammar(std::move(other.grammar)), assistantPrefix(std::move(other.assistantPrefix)), ctx(other.ctx),
      gpu(other.gpu), threads(other.threads), model(other.model), context(other.context), chatCtx(other.chatCtx),
      pool(std::move(other.pool)), embedPath(std::move(other.embedPath)), embedModel(other.embedModel),
      embedCtx(other.embedCtx) {
    other.model = nullptr;
    other.context = nullptr;
    other.chatCtx = nullptr;
    other.embedModel = nullptr;
    other.embedCtx = nullptr;
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
    grammar = std::move(other.grammar);
    assistantPrefix = std::move(other.assistantPrefix);
    ctx = other.ctx;
    gpu = other.gpu;
    threads = other.threads;
    model = other.model;
    context = other.context;
    chatCtx = other.chatCtx;
    pool = std::move(other.pool);
    embedPath = std::move(other.embedPath);
    embedModel = other.embedModel;
    embedCtx = other.embedCtx;
    other.model = nullptr;
    other.context = nullptr;
    other.chatCtx = nullptr;
    other.embedModel = nullptr;
    other.embedCtx = nullptr;
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

error_t Encoder::openEmbed(fs::path const& gguf, std::uint32_t ctxSize, int gpuLayers, std::string const& pooling) {
    if (!fs::is_regular_file(gguf))
        return "embed gguf not found";
    llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = gpuLayers;
    embedModel = llama_model_load_from_file(gguf.string().c_str(), mp);
    if (!embedModel)
        return "failed to load embed gguf";

    llama_context_params cp = llama_context_default_params();
    cp.n_ctx = ctxSize ? ctxSize : 1024;
    cp.n_batch = 512;
    cp.n_ubatch = 512;
    cp.n_threads = threads;
    cp.n_threads_batch = threads;
    cp.embeddings = true;
    cp.pooling_type = pooling == "cls"    ? LLAMA_POOLING_TYPE_CLS
                      : pooling == "mean" ? LLAMA_POOLING_TYPE_MEAN
                                          : LLAMA_POOLING_TYPE_LAST;
    embedCtx = llama_init_from_model(embedModel, cp);
    if (!embedCtx) {
        llama_model_free(embedModel);
        embedModel = nullptr;
        return "failed to create embed context";
    }
    embedPath = gguf.string();
    pool = pooling;
    dimensions = static_cast<std::size_t>(llama_model_n_embd(embedModel));
    std::fprintf(stderr, "api: embed model ready %s dim=%zu pooling=%s\n", embedPath.c_str(), dimensions, pool.c_str());
    return {};
}

bool Encoder::dedicatedEmbed() const noexcept {
    return embedCtx != nullptr && embedCtx != context && embedCtx != chatCtx;
}

std::mutex& Encoder::embedLock() noexcept { return dedicatedEmbed() ? embedMutex : chatMutex; }

std::vector<float> Encoder::embed(std::string_view text) {
    auto const t0 = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(embedLock());
    noteLockWait(*this, t0);
    return embedImpl(text);
}

std::vector<float> Encoder::tryEmbed(std::string_view text) {
    std::unique_lock<std::mutex> lock(embedLock(), std::try_to_lock);
    if (!lock.owns_lock())
        return {}; // 忙：不阻塞，交给调用方降级（不变量 2）
    return embedImpl(text);
}

std::vector<float> Encoder::embedImpl(std::string_view text) {
    llama_model* const m = embedModel ? embedModel : model;
    llama_context* const c = embedCtx ? embedCtx : context;
    if (!m || !c)
        return hashEmbed(text, dimensions ? dimensions : 1024);

    llama_vocab const* vocab = llama_model_get_vocab(m);
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

    int nCtx = static_cast<int>(llama_n_ctx(c));
    if (n > nCtx)
        tokens.erase(tokens.begin(), tokens.end() - nCtx);

    clearKv(c);
    {
        int32_t nBatch = static_cast<int32_t>(llama_n_batch(c));
        for (std::size_t off = 0; off < tokens.size();) {
            std::size_t n = (std::min)(tokens.size() - off, static_cast<std::size_t>(nBatch));
            llama_batch batch = llama_batch_get_one(tokens.data() + off, static_cast<int32_t>(n));
            if (llama_decode(c, batch) != 0)
                return std::vector<float>(dimensions, 0.0f);
            off += n;
        }
    }

    float const* emb = llama_get_embeddings_seq(c, 0);
    if (!emb)
        emb = llama_get_embeddings_ith(c, static_cast<int32_t>(tokens.size()) - 1);
    if (!emb)
        return std::vector<float>(dimensions, 0.0f);

    std::vector<float> out(emb, emb + dimensions);
    l2norm(out);
    clearKv(c);
    return out;
}

std::string Encoder::chat(std::string_view system, std::string_view user) { return chat(system, user, {}); }

std::string Encoder::chat(std::string_view system, std::string_view user,
                          std::function<void(std::string_view)> onDelta) {
    json messages = json::array();
    if (!system.empty())
        messages.push_back(textMessage("system", std::string(system)));
    messages.push_back(textMessage("user", std::string(user)));
    return chat(messages, std::move(onDelta));
}

std::string Encoder::chat(json const& messages) { return chat(messages, {}); }

std::string Encoder::chat(json const& messages, std::function<void(std::string_view)> onDelta) {
    return chat(messages, std::move(onDelta), -1.0f, 0, nullptr, nullptr);
}

std::string Encoder::chat(json const& messages, std::function<void(std::string_view)> onDelta, float temp,
                          std::uint32_t maxTok, std::string const* grammarIn, std::string const* prefixIn, bool block) {
    std::unique_lock<std::mutex> lock(chatMutex, std::defer_lock);
    if (block) {
        auto const t0 = std::chrono::steady_clock::now();
        lock.lock();
        noteLockWait(*this, t0);
    } else if (!lock.try_lock()) {
        stealChat.fetch_add(1, std::memory_order_relaxed);
        return {};
    }
    chatBusy.fetch_add(1, std::memory_order_release);
    struct Busy {
        Encoder* e;
        ~Busy() { e->chatBusy.fetch_sub(1, std::memory_order_release); }
    } busy{this};

    struct Restore {
        Encoder* e;
        float prevTemp;
        std::uint32_t prevMax;
        std::string prevGram;
        std::string prevPre;
        ~Restore() {
            e->temperature = prevTemp;
            e->maxTokens = prevMax;
            e->grammar = std::move(prevGram);
            e->assistantPrefix = std::move(prevPre);
        }
    } guard{this, temperature, maxTokens, grammar, assistantPrefix};
    if (temp >= 0.0f)
        temperature = temp;
    if (maxTok > 0)
        maxTokens = (std::min)(guard.prevMax ? guard.prevMax : maxTok, maxTok);
    if (grammarIn)
        grammar = *grammarIn;
    if (prefixIn)
        assistantPrefix = *prefixIn;

    if (!modelReady || !model || !chatCtx)
        return dryrunChat(messages);

    std::vector<std::pair<std::string, std::string>> store;
    store.reserve(messages.size());
    std::size_t total = 0;
    for (auto const& m : messages) {
        if (!m.is_object())
            continue;
        std::string role = m.value("role", "user");
        std::string content = messageText(m.value("content", json()));
        if (content.empty())
            continue;
        total += role.size() + content.size();
        store.emplace_back(std::move(role), std::move(content));
    }
    if (store.empty())
        return {};

    std::vector<llama_chat_message> msgs;
    msgs.reserve(store.size());
    for (auto const& kv : store)
        msgs.push_back({kv.first.c_str(), kv.second.c_str()});

    char const* tmpl = llama_model_chat_template(model, nullptr);
    std::string prompt;
    bool fromTmpl = false;
    if (tmpl && tmpl[0]) {
        std::vector<char> formatted(std::max<std::size_t>(total, 1) * 2 + 1024);
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
        for (auto const& kv : store) {
            if (!prompt.empty())
                prompt.push_back('\n');
            prompt += kv.second;
        }
    }

    // 关闭思考 / 预填：llama.cpp 的模板引擎不支持 enable_thinking，这里等价地补一段 assistant 前缀。
    if (!assistantPrefix.empty()) {
        static char const* const kAss = "<|im_start|>assistant\n";
        std::size_t const k = std::strlen(kAss);
        if (prompt.size() >= k && prompt.compare(prompt.size() - k, k, kAss) == 0)
            prompt += assistantPrefix;
    }

    if (char const* pd = std::getenv("APEX_PROMPT_RAW"); pd && *pd)
        std::fprintf(stderr, "api: formatted prompt fromTmpl=%d bytes=%zu\n%s\n==PROMPT-END==\n", fromTmpl ? 1 : 0,
                     prompt.size(), prompt.c_str());

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
    llama_perf_context_reset(chatCtx);
    llama_sampler_chain_params sparams = llama_sampler_chain_default_params();
    llama_sampler* smpl = llama_sampler_chain_init(sparams);
    if (!grammar.empty()) {
        // 语法约束放在最前：temp/dist 只在语法允许的候选里采样。
        // 语法非法时会抛异常——降到无约束，不能让 /v1 500。
        try {
            if (llama_sampler* g = llama_sampler_init_grammar(vocab, grammar.c_str(), "root"))
                llama_sampler_chain_add(smpl, g);
        } catch (std::exception const& e) {
            std::fprintf(stderr, "api: grammar init failed: %s\n", e.what());
        } catch (...) {
            std::fprintf(stderr, "api: grammar init failed (unknown)\n");
        }
    }
    if (temperature > 0.0f)
        llama_sampler_chain_add(smpl, llama_sampler_init_temp(temperature));
    llama_sampler_chain_add(smpl, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));

    std::string reply;
    auto const tPrefill0 = std::chrono::steady_clock::now();
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
    auto const tPrefill1 = std::chrono::steady_clock::now();
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
    if (char const* pf = std::getenv("APEX_PERF"); pf && *pf) {
        auto const tGen1 = std::chrono::steady_clock::now();
        double const preMs = std::chrono::duration<double, std::milli>(tPrefill1 - tPrefill0).count();
        double const genMs = std::chrono::duration<double, std::milli>(tGen1 - tPrefill1).count();
        std::fprintf(stderr, "api: phase prompt=%d tok %.0fms (%.0f tok/s) | gen=%u tok %.0fms (%.1f tok/s)\n",
                     static_cast<int>(tokens.size()), preMs, preMs > 0 ? tokens.size() / (preMs / 1000.0) : 0.0,
                     produced, genMs, genMs > 0 ? static_cast<double>(produced) / (genMs / 1000.0) : 0.0);
        llama_perf_context_data const pd = llama_perf_context(chatCtx);
        double const peek = pd.t_p_eval_ms > 0 ? pd.n_p_eval / (pd.t_p_eval_ms / 1000.0) : 0.0;
        double const dek = pd.t_eval_ms > 0 ? pd.n_eval / (pd.t_eval_ms / 1000.0) : 0.0;
        std::fprintf(
            stderr, "api: perf prompt=%d tok %.0fms (%.0f tok/s) | gen=%d tok %.0fms (%.1f tok/s) | cost=%.2f s\n",
            pd.n_p_eval, pd.t_p_eval_ms, peek, pd.n_eval, pd.t_eval_ms, dek, (pd.t_p_eval_ms + pd.t_eval_ms) / 1000.0);
    }
    llama_sampler_free(smpl);
    clearKv(chatCtx);
    lastPromptTok.store(tokens.size(), std::memory_order_relaxed);
    lastGenTok.store(produced, std::memory_order_relaxed);
    return reply;
}

std::size_t Encoder::countTokens(std::string_view text) const {
    if (!model || text.empty())
        return 0;
    llama_vocab const* vocab = llama_model_get_vocab(model);
    std::vector<llama_token> tokens(text.size() + 32);
    int n = llama_tokenize(vocab, text.data(), static_cast<int32_t>(text.size()), tokens.data(),
                           static_cast<int32_t>(tokens.size()), false, true);
    if (n < 0) {
        tokens.resize(static_cast<std::size_t>(-n));
        n = llama_tokenize(vocab, text.data(), static_cast<int32_t>(text.size()), tokens.data(),
                           static_cast<int32_t>(tokens.size()), false, true);
    }
    return n > 0 ? static_cast<std::size_t>(n) : 0;
}

bool Encoder::tokensReal() const noexcept { return model != nullptr && modelReady; }

std::string Encoder::distillChat(std::string_view system, std::string_view user, float temp, std::uint32_t maxTok) {
    json messages = json::array();
    if (!system.empty())
        messages.push_back(textMessage("system", std::string(system)));
    messages.push_back(textMessage("user", std::string(user)));
    return chat(messages, {}, temp, maxTok, nullptr, nullptr, false);
}

} // namespace api
