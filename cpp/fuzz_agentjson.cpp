/**
 * @file fuzz_agentjson.cpp
 * @brief libFuzzer harness for the apex agent result parser.
 *
 * Exercises api::extractAgentResult (stripThink / extractJsonObject / lenient
 * fallback) and the agentOk predicate, covering tag isolation, truncated tags,
 * quote-unbalanced payloads and oversized inputs.
 */

#include "../tools/apex/helpers.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const* data, std::size_t size) {
    std::string_view raw(reinterpret_cast<char const*>(data), size);

    api::json j = api::extractAgentResult(raw);
    (void)api::agentOk(j);

    // 直接锤纯解析辅助，确保它们对任意字节都无未定义行为。
    std::string text(raw);
    (void)api::stripThink(text);
    (void)api::extractJsonObject(text);
    (void)api::lenientStringField(text, "status");
    (void)api::lenientTailField(text, "payload");
    return 0;
}