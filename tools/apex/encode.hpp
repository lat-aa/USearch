/**
 *  @file       encode.hpp
 *  @brief      ModelMemory：llama 编码/生成（Encoder）+ 无模型 hash 嵌入。
 */
#pragma once

#include "types.hpp"

namespace api {

std::vector<float> hashEmbed(std::string_view text, std::size_t dimensions);

struct Encoder {
    std::size_t dimensions = 0;
    bool modelReady = false;
    std::string ggufPath;
    std::string modelId = "Nanbeige4.2-3B";
    float temperature = 0.0f;
    std::uint32_t maxTokens = 0;
    /// GBNF 语法约束（空 = 不约束）；仅本地 agent 用，保证 <agent-result> 必现
    std::string grammar;
    /// 追加到 assistant 起始头之后的 prefill（如 Nanbeige 关闭思考：<think>\n\n</think>\n\n）
    std::string assistantPrefix;
    std::uint32_t ctx = 0;
    int gpu = 0;
    int threads = 0;
    ::llama_model* model = nullptr;
    ::llama_context* context = nullptr;
    ::llama_context* chatCtx = nullptr;
    /// 专用嵌入模型（空 = 复用 chat 模型）：bge-m3 等
    std::string pool = "lasttoken"; ///< 池化：cls|mean|lasttoken
    std::string embedPath;
    ::llama_model* embedModel = nullptr;
    ::llama_context* embedCtx = nullptr;
    /// 专用 embed 上下文时与 chat 并行；复用 chat 模型时 embed 走 chatMutex。
    std::mutex chatMutex;
    std::mutex embedMutex;
    std::atomic<int> chatBusy{0}; ///< 持 chatMutex 生成中；Worker 硬让路读此值
    std::atomic<std::uint64_t> lockWaitN{0};
    std::atomic<std::uint64_t> lockWaitSumMs{0};
    std::atomic<std::int64_t> lockWaitMaxMs{0};
    std::atomic<std::uint64_t> stealChat{0}; ///< Worker try_lock chat 失败次数（活跃期应为 0）
    Encoder() = default;
    ~Encoder();
    Encoder(Encoder const&) = delete;
    Encoder& operator=(Encoder const&) = delete;
    Encoder(Encoder&& other) noexcept;
    Encoder& operator=(Encoder&& other) noexcept;
    void close();
    error_t open(fs::path const& gguf, std::uint32_t ctxSize, int gpuLayers, int nThreads, float temp,
                 std::uint32_t maxTok);
    /** 加载专用嵌入模型（bge-m3 等）；pooling ∈ cls|mean|lasttoken。失败即回退复用 chat 模型。 */
    error_t openEmbed(fs::path const& gguf, std::uint32_t ctxSize, int gpuLayers, std::string const& pooling);
    std::vector<float> embed(std::string_view text);
    /** 非阻塞嵌入：锁被占时立即返回空，调用方降级（Policy/presync 热路径必须走这条）。 */
    std::vector<float> tryEmbed(std::string_view text);
    /** 实际嵌入实现（调用方须已持 embed 对应锁）。 */
    std::vector<float> embedImpl(std::string_view text);
    /** 是否有独立 embed 上下文：是则可与 distill chat 并行。 */
    bool dedicatedEmbed() const noexcept;
    std::mutex& embedLock() noexcept;
    std::string chat(std::string_view system, std::string_view user);
    std::string chat(std::string_view system, std::string_view user, std::function<void(std::string_view)> onDelta);
    std::string chat(json const& messages);
    std::string chat(json const& messages, std::function<void(std::string_view)> onDelta);
    /** opts 在持 chatMutex 内应用并恢复，禁止无锁改采样字段。 */
    std::string chat(json const& messages, std::function<void(std::string_view)> onDelta, float temp,
                     std::uint32_t maxTok, std::string const* grammar, std::string const* prefix, bool block = true);
    /** Worker 蒸馏：try_lock chatMutex；抢不到立即空串（stealChat++），禁止堵用户。 */
    std::string distillChat(std::string_view system, std::string_view user, float temp, std::uint32_t maxTok);
};

} // namespace api
