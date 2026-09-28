/**
 * @file rules.cpp
 * @brief 无 Encoder 的词法规则激活合同：验证生产 resolveRules(vector, query) 与配额。
 */
#include "../../tools/apex/rules.hpp"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

using namespace api;

static Rule mk(std::string name, std::string desc, std::vector<std::string> globs, bool always) {
    Rule r;
    r.name = std::move(name);
    r.description = std::move(desc);
    r.globs = std::move(globs);
    r.always = always;
    return r;
}

static bool has(std::vector<Resolvedrule> const& out, std::string const& name) {
    for (auto const& r : out)
        if (r.rule.name == name)
            return true;
    return false;
}

int main() {
    std::vector<Rule> rules;
    rules.push_back(mk("always-rule", "always on", {}, true));
    rules.push_back(mk("glob-rule", "cpp only", {"**/*.cpp"}, false));
    rules.push_back(mk("sem-rule", "concurrency locking", {}, false));

    // Always 必命中
    {
        Rulequery q;
        assert(has(resolveRules(rules, q), "always-rule"));
    }
    // Glob 命中：activation=Glob；globs 非空的规则不进 Semantic
    {
        Rulequery q;
        q.files = {"tools/apex/mcp.cpp"};
        auto out = resolveRules(rules, q);
        bool globHit = false;
        for (auto const& r : out) {
            assert(r.rule.name != "sem-rule"); // 无 task → 无词法命中
            if (r.rule.name == "glob-rule") {
                globHit = true;
                assert(r.activation == Activation::Glob);
            }
        }
        assert(globHit);
    }
    // Glob 未命中
    {
        Rulequery q;
        q.files = {"README.md"};
        assert(!has(resolveRules(rules, q), "glob-rule"));
    }
    // 空 task：不产生语义命中
    {
        Rulequery q;
        for (auto const& r : resolveRules(rules, q))
            assert(r.activation != Activation::Semantic);
    }
    // 词法语义：命中 task token
    {
        Rulequery q;
        q.task = "concurrency";
        auto out = resolveRules(rules, q);
        bool sem = false;
        for (auto const& r : out)
            if (r.rule.name == "sem-rule") {
                sem = true;
                assert(r.activation == Activation::Semantic);
                assert(r.score > 0.0f);
            }
        assert(sem);
    }
    // Manual：显式 @name
    {
        Rulequery q;
        q.manual = {"sem-rule"};
        auto out = resolveRules(rules, q);
        bool man = false;
        for (auto const& r : out)
            if (r.rule.name == "sem-rule") {
                man = true;
                assert(r.activation == Activation::Manual);
            }
        assert(man);
    }
    // 配额：Always=满额，其余=半额起，Semantic ∈ [1, base]
    assert(ruleSections(Activation::Always, 0.0f, 0.0f, 8) == 8);
    assert(ruleSections(Activation::Glob, 0.0f, 0.0f, 8) == 4);
    assert(ruleSections(Activation::Manual, 0.0f, 0.0f, 8) == 4);
    assert(ruleSections(Activation::Semantic, 1.0f, 4.0f, 8) == 4);

    std::puts("apex rules: ok");
    return 0;
}
