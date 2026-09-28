/**
 *  @file       config.cpp
 *  @brief      Config::load 与仓库根解析。
 */
#include "api.hpp"
#include <cstdlib>
#include <toml++/toml.hpp>

namespace api {

fs::path findRoot() {
    fs::path cwd = fs::current_path();
    for (fs::path p = cwd; !p.empty(); p = p.parent_path()) {
        if (fs::is_regular_file(p / ".config" / "config.toml") ||
            fs::is_regular_file(p / ".config" / "config.example.toml"))
            return p;
        if (p == p.root_path())
            break;
    }
    return cwd;
}

fs::path joinRoot(fs::path const& root, std::string const& relative) {
    fs::path p(relative);
    return p.is_absolute() ? p : root / p;
}

namespace {

char const* kMissingToml = "missing config; copy .config/config.example.toml to .config/config.toml";

bool asString(toml::node_view<toml::node> n, std::string& out) {
    auto* s = n.as_string();
    if (!s)
        return false;
    out = std::string{s->get()};
    return true;
}

bool asI64(toml::node_view<toml::node> n, std::int64_t& out) {
    auto* v = n.as_integer();
    if (!v)
        return false;
    out = v->get();
    return true;
}

bool asF64(toml::node_view<toml::node> n, double& out) {
    if (auto* f = n.as_floating_point()) {
        out = f->get();
        return true;
    }
    // TOML 允许整数写在浮点位（如 refill = 1）
    if (auto* i = n.as_integer()) {
        out = static_cast<double>(i->get());
        return true;
    }
    return false;
}

} // namespace

expected_gt<Config> Config::load(fs::path const& path) {
    expected_gt<Config> out;
    if (!fs::is_regular_file(path))
        return out.failed(kMissingToml);
    toml::table root;
    try {
        root = toml::parse_file(path.string());
    } catch (toml::parse_error const& err) {
        return out.failed(std::string(err.description()).c_str());
    } catch (...) {
        return out.failed("config parse failed");
    }

    char const* required[] = {"gguf",   "ctx",  "gpu",       "threads", "pooling",   "listen",
                              "index",  "base", "knowledge", "rules",   "workspace", "token",
                              "shadow", "rate", "refill",    "chat",    "decide"};
    for (char const* key : required) {
        if (!root.contains(key))
            return out.failed("config missing required key");
    }

    auto* chatTbl = root["chat"].as_table();
    if (!chatTbl || !chatTbl->contains("temperature") || !chatTbl->contains("max"))
        return out.failed("config chat requires temperature and max");
    auto* decideTbl = root["decide"].as_table();
    if (!decideTbl)
        return out.failed("config decide must be table");
    char const* decideKeys[] = {"speed",    "high",    "low",      "margin",   "templow", "tempmid",
                                "temphigh", "tempcap", "wmedium",  "wcomplex", "maxtask", "maxfile",
                                "keeplow",  "keepmid", "keephigh", "topk",     "lexicon"};
    for (char const* key : decideKeys) {
        if (!decideTbl->contains(key))
            return out.failed("config decide missing required key");
    }
    auto* topkArr = (*decideTbl)["topk"].as_array();
    if (!topkArr || topkArr->size() != 4)
        return out.failed("config decide.topk must be array of 4");

    Config c;
    auto failType = [&](char const* msg) -> expected_gt<Config> {
        expected_gt<Config> e;
        return e.failed(msg);
    };
    std::int64_t i64 = 0;
    double f64 = 0.0;

    if (!asString(root["gguf"], c.gguf))
        return failType("gguf must be string");
    if (!asI64(root["ctx"], i64) || i64 < 0)
        return failType("ctx must be unsigned integer");
    c.ctx = static_cast<std::uint32_t>(i64);
    if (!asI64(root["gpu"], i64))
        return failType("gpu must be integer");
    c.gpu = static_cast<int>(i64);
    if (!asI64(root["threads"], i64))
        return failType("threads must be integer");
    c.threads = static_cast<int>(i64);
    if (!asString(root["pooling"], c.pooling))
        return failType("pooling must be string");
    if (!asString(root["listen"], c.listen))
        return failType("listen must be string");
    if (!asString(root["index"], c.index))
        return failType("index must be string");
    if (!asString(root["base"], c.base))
        return failType("base must be string");
    if (!asString(root["knowledge"], c.knowledge))
        return failType("knowledge must be string");
    if (!asString(root["rules"], c.rules))
        return failType("rules must be string");
    if (!asString(root["workspace"], c.workspace))
        return failType("workspace must be string");
    if (!asString(root["token"], c.token))
        return failType("token must be string");
    if (!asI64(root["shadow"], i64) || i64 < 0)
        return failType("shadow must be unsigned integer");
    c.shadow = static_cast<std::size_t>(i64);
    if (!asI64(root["rate"], i64) || i64 < 0)
        return failType("rate must be unsigned integer");
    c.rate = static_cast<std::uint32_t>(i64);
    if (!asF64(root["refill"], f64))
        return failType("refill must be number");
    c.refill = f64;

    if (!asF64((*chatTbl)["temperature"], f64))
        return failType("chat.temperature must be number");
    c.chat.temperature = static_cast<float>(f64);
    if (!asI64((*chatTbl)["max"], i64) || i64 < 0)
        return failType("chat.max must be unsigned integer");
    c.chat.max = static_cast<std::uint32_t>(i64);

    toml::table& d = *decideTbl;
    if (!asI64(d["speed"], i64) || i64 < 0)
        return failType("decide.speed must be unsigned integer");
    c.decide.speed = static_cast<std::uint64_t>(i64);
    if (!asI64(d["high"], i64) || i64 < 0 || i64 > 100)
        return failType("decide.high must be 0..100");
    c.decide.high = static_cast<std::uint8_t>(i64);
    if (!asI64(d["low"], i64) || i64 < 0 || i64 > 100)
        return failType("decide.low must be 0..100");
    c.decide.low = static_cast<std::uint8_t>(i64);
    if (!asI64(d["margin"], i64) || i64 < 0 || i64 > 100)
        return failType("decide.margin must be 0..100");
    c.decide.margin = static_cast<std::uint8_t>(i64);
    if (!asF64(d["templow"], f64))
        return failType("decide.templow must be number");
    c.decide.temperature.low = static_cast<float>(f64);
    if (!asF64(d["tempmid"], f64))
        return failType("decide.tempmid must be number");
    c.decide.temperature.mid = static_cast<float>(f64);
    if (!asF64(d["temphigh"], f64))
        return failType("decide.temphigh must be number");
    c.decide.temperature.high = static_cast<float>(f64);
    if (!asF64(d["tempcap"], f64))
        return failType("decide.tempcap must be number");
    c.decide.temperature.cap = static_cast<float>(f64);
    if (!asI64(d["wmedium"], i64) || i64 < 0 || i64 > 100)
        return failType("decide.wmedium must be 0..100");
    c.decide.wmedium = static_cast<std::uint8_t>(i64);
    if (!asI64(d["wcomplex"], i64) || i64 < 0 || i64 > 100)
        return failType("decide.wcomplex must be 0..100");
    c.decide.wcomplex = static_cast<std::uint8_t>(i64);
    if (!asI64(d["maxtask"], i64) || i64 <= 0)
        return failType("decide.maxtask must be > 0");
    c.decide.maxtask = static_cast<std::size_t>(i64);
    if (!asI64(d["maxfile"], i64) || i64 <= 0)
        return failType("decide.maxfile must be > 0");
    c.decide.maxfile = static_cast<std::size_t>(i64);
    if (!asF64(d["keeplow"], f64))
        return failType("decide.keeplow must be number");
    c.decide.keeplow = static_cast<float>(f64);
    if (!asF64(d["keepmid"], f64))
        return failType("decide.keepmid must be number");
    c.decide.keepmid = static_cast<float>(f64);
    if (!asF64(d["keephigh"], f64))
        return failType("decide.keephigh must be number");
    c.decide.keephigh = static_cast<float>(f64);
    for (std::size_t i = 0; i < 4; ++i) {
        auto* elem = (*topkArr)[i].as_integer();
        if (!elem || elem->get() <= 0)
            return failType("decide.topk entries must be > 0");
        c.decide.topk[i] = static_cast<std::size_t>(elem->get());
    }
    if (!asString(d["lexicon"], c.decide.lexicon))
        return failType("decide.lexicon must be string");

    // [upstream] / [agent] / [cache] / [retrieval]：可选段，缺省用结构体默认值
    if (auto* up = root["upstream"].as_table()) {
        if (auto v = (*up)["base_url"].value<std::string>())
            c.upstream.base = *v;
        if (auto v = (*up)["key_env"].value<std::string>())
            c.upstream.keyenv = *v;
        if (auto v = (*up)["model"].value<std::string>())
            c.upstream.model = *v;
    }
    if (auto* ag = root["agent"].as_table()) {
        if (auto v = (*ag)["enabled"].value<bool>())
            c.agent.enabled = *v;
        if (auto v = (*ag)["skip_complex"].value<bool>())
            c.agent.skipComplex = *v;
        if (auto v = (*ag)["max_tool_rounds"].value<std::int64_t>(); v && *v >= 0)
            c.agent.maxRounds = static_cast<std::size_t>(*v);
        if (auto v = (*ag)["token_budget_ratio"].value<double>())
            c.agent.tokenBudget = static_cast<float>(*v);
        if (auto v = (*ag)["enable_fuse"].value<bool>())
            c.agent.enableFuse = *v;
        if (auto v = (*ag)["fuse_fail_threshold"].value<std::int64_t>(); v && *v >= 0)
            c.agent.fuseFail = static_cast<std::size_t>(*v);
        if (auto v = (*ag)["fuse_recovery_seconds"].value<std::int64_t>(); v && *v >= 0)
            c.agent.fuseRecover = static_cast<std::uint32_t>(*v);
    }
    if (auto* ca = root["cache"].as_table()) {
        if (auto v = (*ca)["enable_l1"].value<bool>())
            c.cache.enableL1 = *v;
        if (auto v = (*ca)["l1_ttl_seconds"].value<std::int64_t>(); v && *v >= 0)
            c.cache.l1Ttl = static_cast<std::uint32_t>(*v);
        if (auto v = (*ca)["enable_l2"].value<bool>())
            c.cache.enableL2 = *v;
        if (auto v = (*ca)["l2_similarity_threshold"].value<double>())
            c.cache.l2Sim = static_cast<float>(*v);
    }
    if (auto* em = root["embed"].as_table()) {
        if (auto v = (*em)["gguf"].value<std::string>())
            c.embed.gguf = *v;
        if (auto v = (*em)["gpu"].value<std::int64_t>())
            c.embed.gpu = static_cast<int>(*v);
    }
    if (auto* rt = root["retrieval"].as_table()) {
        if (auto v = (*rt)["rule_weight_multiplier"].value<double>())
            c.retrieval.ruleWeight = static_cast<float>(*v);
        if (auto v = (*rt)["top_k"].value<std::int64_t>(); v && *v > 0)
            c.retrieval.topK = static_cast<std::size_t>(*v);
    }

    if (c.gguf.empty() || c.listen.empty() || c.index.empty() || c.base.empty() || c.knowledge.empty() ||
        c.rules.empty() || c.workspace.empty() || c.pooling.empty())
        return out.failed("config path or pooling must be non-empty");
    if (c.decide.lexicon.empty())
        return out.failed("decide.lexicon path must be non-empty");
    if (c.pooling != "lasttoken" && c.pooling != "cls" && c.pooling != "mean")
        return out.failed("pooling must be lasttoken|cls|mean");
    if (c.listen.find(':') == std::string::npos)
        return out.failed("listen must be host:port");
    out.result = std::move(c);
    return out;
}

} // namespace api

