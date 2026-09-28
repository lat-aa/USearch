/**
 *  @file       decide.cpp
 *  @brief      网关决策引擎：模型档 / 推理深度 / 上下文保留比 / 检索档 / 采样温度的级联路由。
 *  @author     USearch contributors
 *
 *  # 设计意图
 *
 *  决策必须**确定且可解释**：输出是小集合 @ref Features 的纯函数——同输入必同决策，
 *  可离线回放以对照真实结果做校准。本文件不读环境变量；全部阈值与词表来自
 *  @ref Decideconfig（启动时经由 @ref Decider::open 注入）。热路径只读本结构。
 *
 *  级联顺序（后段可覆盖前段，不得颠倒）：
 *  1. 由复杂度分定基线 `model / depth / retrieval / compression`；
 *  2. 时延敏感且未 PreferQuality → 各维降一档并收紧压缩；
 *  3. `Hint::Force*` 硬约束最后覆盖启发式；
 *  4. 置信度：强制 hint → 1.0；否则距分档边界越远越高（边界附近 ≈ 0.5）；
 *  5. 温度：复杂度两段线性插值 + latency / 质量护栏，硬顶在 `Temppolicy::cap`。
 *
 *  # 配置面（缺键启动失败，无产品默认）
 *
 *  `config.decide`：speed / high / low / margin / temp* / w* / maxtask / maxfile /
 *  keep* / topk / lexicon。词表为单一 TOML：`complex` / `medium` 字符串数组。
 *
 *  # 对外面
 *
 *  - HTTP `POST /v1/route`、MCP 工具 `decide`
 *  - `chat` 管线首阶段调用：未显式传 `temperature` 时采用决策温度；
 *    检索 top-k / 规则裁剪段数分别映射自 `retrieval` / `compression`
 *
 *  语义对齐 apex `decide.rs`（CascadeDecider）；标识命名遵守 `.config/rules/names.md`。
 */

#include "decide.hpp"
#include "api.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

#include <toml++/toml.hpp>

namespace api {

namespace {

bool containsAny(std::string_view hay, std::vector<std::string> const& keys) {
    for (auto const& k : keys)
        if (!k.empty() && hay.find(k) != std::string_view::npos)
            return true;
    return false;
}

} // namespace

char const* modelName(Model m) noexcept {
    switch (m) {
    case Model::Weak: return "weak";
    case Model::Standard: return "standard";
    case Model::Strong: return "strong";
    }
    return "standard";
}

char const* depthName(Depth d) noexcept {
    switch (d) {
    case Depth::Shallow: return "shallow";
    case Depth::Medium: return "medium";
    case Depth::Deep: return "deep";
    }
    return "medium";
}

char const* retrievalName(Retrieval r) noexcept {
    switch (r) {
    case Retrieval::L0: return "L0";
    case Retrieval::L1: return "L1";
    case Retrieval::L2: return "L2";
    case Retrieval::L3: return "L3";
    }
    return "L2";
}

Model modelDowngrade(Model m) noexcept {
    switch (m) {
    case Model::Weak: return Model::Weak;
    case Model::Standard: return Model::Weak;
    case Model::Strong: return Model::Standard;
    }
    return Model::Weak;
}

Depth depthDowngrade(Depth d) noexcept {
    switch (d) {
    case Depth::Shallow: return Depth::Shallow;
    case Depth::Medium: return Depth::Shallow;
    case Depth::Deep: return Depth::Medium;
    }
    return Depth::Shallow;
}

Retrieval retrievalDowngrade(Retrieval r) noexcept {
    switch (r) {
    case Retrieval::L0: return Retrieval::L0;
    case Retrieval::L1: return Retrieval::L0;
    case Retrieval::L2: return Retrieval::L1;
    case Retrieval::L3: return Retrieval::L2;
    }
    return Retrieval::L0;
}

Temppolicy Temppolicy::sanitized() const {
    // 不变量：low ≤ mid ≤ high ≤ cap，且均 ∈ [0, 2]。cap 是绝对上限：超界下压 mid/high，不抬高 cap。
    auto clamp02 = [](float x) { return (std::max)(0.0f, (std::min)(2.0f, x)); };
    float lo = clamp02(low);
    float mi = clamp02(mid);
    float hi = clamp02(high);
    float ca = clamp02(cap);
    if (mi > ca)
        mi = ca;
    if (hi > ca)
        hi = ca;
    if (mi < lo)
        mi = (std::min)(lo, ca);
    if (hi < mi)
        hi = mi;
    return Temppolicy{lo, mi, hi, ca};
}

float Temppolicy::resolve(std::uint8_t complexity, std::uint8_t lowC, std::uint8_t highC, bool latencySensitive,
                          bool qualityPreferred) const {
    Temppolicy p = sanitized();
    float score = static_cast<float>(complexity);
    float loC = static_cast<float>(lowC);
    float hiC = static_cast<float>((std::max)(highC, static_cast<std::uint8_t>(lowC + 1)));

    // 两段插值：[<lowC]→low；[highC,]→high；中间经 midC 折点 low→mid→high。
    float temp = 0.0f;
    if (score < loC)
        temp = p.low;
    else if (score >= hiC)
        temp = p.high;
    else {
        float midC = (loC + hiC) * 0.5f;
        if (score <= midC) {
            float t = (score - loC) / (std::max)(midC - loC, 1e-6f);
            temp = p.low + (p.mid - p.low) * t;
        } else {
            float t = (score - midC) / (std::max)(hiC - midC, 1e-6f);
            temp = p.mid + (p.high - p.mid) * t;
        }
    }

    // 护栏互斥：latency 拉低；PreferQuality 微升。编码场景硬顶仍在 cap。
    if (latencySensitive && !qualityPreferred) {
        temp = (std::min)(temp, p.mid);
        temp = 0.7f * temp + 0.3f * p.low;
    }
    if (qualityPreferred && !latencySensitive)
        temp = (std::min)(temp + 0.05f, p.cap);

    temp = (std::max)(p.low, (std::min)(p.cap, temp));
    return std::round(temp * 100.0f) / 100.0f;
}

expected_gt<Lexicon> loadLexicon(fs::path const& path) {
    expected_gt<Lexicon> out;
    if (!fs::is_regular_file(path))
        return out.failed("lexicon file missing");
    toml::table tbl;
    try {
        tbl = toml::parse_file(path.string());
    } catch (toml::parse_error const&) {
        return out.failed("lexicon parse failed");
    } catch (...) {
        return out.failed("lexicon parse failed");
    }
    auto readWords = [&](char const* key, std::vector<std::string>& dst) -> bool {
        if (!tbl.contains(key))
            return false;
        auto* arr = tbl[key].as_array();
        if (!arr || arr->empty())
            return false;
        dst.clear();
        dst.reserve(arr->size());
        for (auto&& node : *arr) {
            auto* s = node.as_string();
            if (!s)
                return false;
            std::string w{s->get()};
            if (!w.empty())
                dst.push_back(std::move(w));
        }
        return !dst.empty();
    };
    Lexicon lex;
    if (!readWords("complex", lex.complex))
        return out.failed("lexicon.complex must be non-empty string array");
    if (!readWords("medium", lex.medium))
        return out.failed("lexicon.medium must be non-empty string array");
    out.result = std::move(lex);
    return out;
}

expected_gt<Decider> Decider::open(Decideconfig const& cfg, Lexicon lexicon) {
    expected_gt<Decider> out;
    if (lexicon.complex.empty() || lexicon.medium.empty())
        return out.failed("decide lexicon must be non-empty");
    if (cfg.low >= cfg.high)
        return out.failed("decide.low must be < decide.high");
    if (cfg.maxtask == 0 || cfg.maxfile == 0)
        return out.failed("decide.maxtask/maxfile must be > 0");
    for (std::size_t k : cfg.topk)
        if (k == 0)
            return out.failed("decide.topk entries must be > 0");
    if (!(cfg.keeplow > 0.0f && cfg.keeplow <= cfg.keepmid && cfg.keepmid <= cfg.keephigh && cfg.keephigh <= 1.0f))
        return out.failed("decide.keep* must satisfy 0 < keeplow <= keepmid <= keephigh <= 1");

    Decider d;
    d.speedMs = cfg.speed;
    d.highComplexity = cfg.high;
    d.lowComplexity = cfg.low;
    d.margin = cfg.margin;
    d.temperature = cfg.temperature.sanitized();
    d.wmedium = cfg.wmedium;
    d.wcomplex = cfg.wcomplex;
    d.maxtask = cfg.maxtask;
    d.maxfile = cfg.maxfile;
    d.keeplow = cfg.keeplow;
    d.keepmid = cfg.keepmid;
    d.keephigh = cfg.keephigh;
    d.topk = cfg.topk;
    d.complex = std::move(lexicon.complex);
    d.medium = std::move(lexicon.medium);
    // 词表统一小写，与 features() 里 asciiLower(task) 对齐。
    for (auto& w : d.complex)
        w = asciiLower(w);
    for (auto& w : d.medium)
        w = asciiLower(w);
    out.result = std::move(d);
    return out;
}

std::size_t Decider::topkFor(Retrieval r) const noexcept {
    switch (r) {
    case Retrieval::L0: return topk[0];
    case Retrieval::L1: return topk[1];
    case Retrieval::L2: return topk[2];
    case Retrieval::L3: return topk[3];
    }
    return topk[2];
}

int Decider::sectionsFor(float compression) const noexcept {
    if (compression >= keepmid)
        return 3;
    if (compression >= keeplow)
        return 2;
    return 1;
}

json Decision::toJson() const {
    json reasons = json::array();
    for (auto const& r : this->reasons)
        reasons.push_back(r);
    auto r3 = [](float x) { return std::round(x * 1000.0f) / 1000.0f; };
    auto r2 = [](float x) { return std::round(x * 100.0f) / 100.0f; };
    return {{"model", modelName(model)},
            {"depth", depthName(depth)},
            {"compression", r3(compression)},
            {"retrieval", retrievalName(retrieval)},
            {"confidence", r3(confidence)},
            {"temperature", r2(temperature)},
            {"reasons", reasons}};
}

json Deciderecord::toJson() const {
    // 单对象展平 features+decision，便于轻量遥测 / 日志 grep，无需嵌套解析。
    json reasons = json::array();
    for (auto const& r : decision.reasons)
        reasons.push_back(r);
    return {{"taskLen", features.taskLen},
            {"fileCount", features.fileCount},
            {"complexKeyword", features.complexKeyword},
            {"mediumKeyword", features.mediumKeyword},
            {"latencySensitive", features.latencySensitive},
            {"qualityPreferred", features.qualityPreferred},
            {"complexity", features.complexity},
            {"model", modelName(decision.model)},
            {"depth", depthName(decision.depth)},
            {"retrieval", retrievalName(decision.retrieval)},
            {"compression", decision.compression},
            {"confidence", decision.confidence},
            {"temperature", decision.temperature},
            {"reasons", std::move(reasons)}};
}

Hint parseHint(std::string const& s) {
    // 接受单单词（speed）或 apex 风格别名（preferspeed）；未知串勿调用——见 decideinputFromJson 白名单。
    std::string h = asciiLower(s);
    if (h == "speed" || h == "preferspeed")
        return Hint::PreferSpeed;
    if (h == "quality" || h == "preferquality")
        return Hint::PreferQuality;
    if (h == "weak" || h == "forceweak")
        return Hint::ForceWeak;
    if (h == "standard" || h == "forcestandard")
        return Hint::ForceStandard;
    if (h == "strong" || h == "forcestrong")
        return Hint::ForceStrong;
    if (h == "shallow" || h == "forceshallow")
        return Hint::ForceShallow;
    if (h == "medium" || h == "forcemedium")
        return Hint::ForceMedium;
    if (h == "deep" || h == "forcedeep")
        return Hint::ForceDeep;
    return Hint::PreferSpeed;
}

Decideinput decideinputFromJson(json const& body) {
    // 线格式：task|query、files[]、latency（ms）、hints[]。未知 hint 静默丢弃，避免脏输入炸档。
    Decideinput in;
    if (body.contains("task") && body["task"].is_string())
        in.task = body["task"].get<std::string>();
    else if (body.contains("query") && body["query"].is_string())
        in.task = body["query"].get<std::string>();
    if (body.contains("files") && body["files"].is_array())
        for (auto const& f : body["files"])
            if (f.is_string())
                in.files.push_back(f.get<std::string>());
    if (body.contains("latency") && body["latency"].is_number_unsigned())
        in.latency = body["latency"].get<std::uint64_t>();
    else if (body.contains("latency") && body["latency"].is_number_integer())
        in.latency = static_cast<std::uint64_t>((std::max)(0, body["latency"].get<int>()));
    if (body.contains("hints") && body["hints"].is_array()) {
        for (auto const& h : body["hints"]) {
            if (!h.is_string())
                continue;
            std::string s = asciiLower(h.get<std::string>());
            if (s == "speed" || s == "preferspeed" || s == "quality" || s == "preferquality" || s == "weak" ||
                s == "forceweak" || s == "standard" || s == "forcestandard" || s == "strong" || s == "forcestrong" ||
                s == "shallow" || s == "forceshallow" || s == "medium" || s == "forcemedium" || s == "deep" ||
                s == "forcedeep")
                in.hints.push_back(parseHint(s));
        }
    }
    return in;
}

Features Decider::features(Decideinput const& input) const {
    // 复杂度分（0..100）= min(maxtask,len)/(maxtask/50) + min(maxfile,files)*(60/maxfile)
    //                 + (medium?wmedium:0) + (complex?wcomplex:0)
    // 默认 maxtask=200 → /4；maxfile=5 → *6。廉价、可回放；不调模型、不查索引。
    std::string lower = asciiLower(input.task);
    Features f;
    f.taskLen = input.task.size();
    f.fileCount = input.files.size();
    f.complexKeyword = containsAny(lower, complex);
    f.mediumKeyword = containsAny(lower, medium);
    f.latencySensitive = (input.latency && *input.latency < speedMs) ||
                         std::find(input.hints.begin(), input.hints.end(), Hint::PreferSpeed) != input.hints.end();
    f.qualityPreferred = std::find(input.hints.begin(), input.hints.end(), Hint::PreferQuality) != input.hints.end();

    // 同维多次 Force*：后者覆盖前者（与 hints 数组顺序一致）。
    for (Hint h : input.hints) {
        switch (h) {
        case Hint::ForceWeak: f.forcedModel = Model::Weak; break;
        case Hint::ForceStandard: f.forcedModel = Model::Standard; break;
        case Hint::ForceStrong: f.forcedModel = Model::Strong; break;
        case Hint::ForceShallow: f.forcedDepth = Depth::Shallow; break;
        case Hint::ForceMedium: f.forcedDepth = Depth::Medium; break;
        case Hint::ForceDeep: f.forcedDepth = Depth::Deep; break;
        default: break;
        }
    }

    std::size_t taskCap = (std::max)(maxtask, std::size_t{1});
    std::size_t fileCap = (std::max)(maxfile, std::size_t{1});
    // 保持与历史默认（200→/4、5→*6）同比例：满分贡献各为 50 / 30。
    unsigned taskContrib = static_cast<unsigned>((std::min)(f.taskLen, taskCap) * 50 / taskCap);
    unsigned fileContrib = static_cast<unsigned>((std::min)(f.fileCount, fileCap) * 30 / fileCap);
    unsigned mediumContrib = f.mediumKeyword ? wmedium : 0u;
    unsigned complexContrib = f.complexKeyword ? wcomplex : 0u;
    unsigned sum = taskContrib + fileContrib + mediumContrib + complexContrib;
    f.complexity = static_cast<std::uint8_t>((std::min)(sum, 100u));
    return f;
}

Decision Decider::decideFrom(Features const& f) const {
    Decision d;
    d.reasons.clear();

    // —— 1. 复杂度分档定基线 ——
    if (f.complexity >= highComplexity) {
        d.reasons.push_back("complexity " + std::to_string(f.complexity) + " >= " + std::to_string(highComplexity) +
                            " (high)");
        d.model = Model::Strong;
        d.depth = Depth::Deep;
        d.retrieval = Retrieval::L3;
        d.compression = keephigh;
    } else if (f.complexity < lowComplexity) {
        d.reasons.push_back("complexity " + std::to_string(f.complexity) + " < " + std::to_string(lowComplexity) +
                            " (low)");
        d.model = Model::Weak;
        d.depth = Depth::Shallow;
        d.retrieval = Retrieval::L0;
        d.compression = keeplow;
    } else {
        d.reasons.push_back("complexity " + std::to_string(f.complexity) + " in [" + std::to_string(lowComplexity) +
                            ", " + std::to_string(highComplexity) + ") (mid)");
        d.model = Model::Standard;
        d.depth = Depth::Medium;
        d.retrieval = Retrieval::L2;
        d.compression = keepmid;
    }

    // —— 2. 时延护栏（PreferQuality 可豁免）——
    if (f.latencySensitive && !f.qualityPreferred) {
        d.model = modelDowngrade(d.model);
        d.depth = depthDowngrade(d.depth);
        d.retrieval = retrievalDowngrade(d.retrieval);
        d.compression = (std::max)(d.compression * 0.5f, keeplow * 0.25f);
        d.reasons.push_back("latency-sensitive: downgraded");
    }

    // —— 3. 硬约束最后覆盖 ——
    if (f.forcedModel) {
        d.model = *f.forcedModel;
        d.reasons.push_back(std::string("forced model: ") + modelName(d.model));
    }
    if (f.forcedDepth) {
        d.depth = *f.forcedDepth;
        d.reasons.push_back(std::string("forced depth: ") + depthName(d.depth));
    }

    // —— 4. 置信度：强制 → 1；否则距 low/high 边界的归一化距离 ——
    if (f.forcedModel || f.forcedDepth) {
        d.confidence = 1.0f;
    } else {
        float score = static_cast<float>(f.complexity);
        float low = static_cast<float>(lowComplexity);
        float high = static_cast<float>(highComplexity);
        float marg = static_cast<float>((std::max)(margin, static_cast<std::uint8_t>(1)));
        float distance = (std::min)(std::fabs(score - low), std::fabs(score - high));
        float normalized = (std::min)(distance / (marg * 2.0f), 1.0f);
        d.confidence = 0.5f + 0.5f * normalized;
    }

    // —— 5. 温度 ——
    d.temperature =
        temperature.resolve(f.complexity, lowComplexity, highComplexity, f.latencySensitive && !f.qualityPreferred,
                            f.qualityPreferred && !f.latencySensitive);
    {
        std::ostringstream os;
        os << "temperature=" << d.temperature << " (lerp+guards)";
        d.reasons.push_back(os.str());
    }
    return d;
}

Decision Decider::decide(Decideinput const& input) const { return decideFrom(features(input)); }

} // namespace api
