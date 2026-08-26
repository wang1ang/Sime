#include "sime.h"

#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

bool ExpectPrefix(const std::vector<sime::DecodeResult>& results,
                  std::initializer_list<std::string_view> expected) {
    if (results.size() < expected.size()) return false;
    std::size_t index = 0;
    for (const auto text : expected) {
        if (results[index++].text != text) return false;
    }
    return true;
}

// True if any candidate's display text contains `needle`.
bool ContainsText(const std::vector<sime::DecodeResult>& results,
                  std::string_view needle) {
    for (const auto& candidate : results) {
        if (candidate.text.find(needle) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

int main() {
    sime::Sime engine(SIME_TEST_DICT, SIME_TEST_CNT);
    if (!engine.Ready()) {
        std::cerr << "Could not load Sime test models\n";
        return EXIT_FAILURE;
    }

    const auto sentence = engine.DecodeSentence("xingjiabi", 0);
    if (sentence.empty() || sentence.front().text != "性价比") {
        std::cerr << "xingjiabi top sentence changed\n";
        return EXIT_FAILURE;
    }

    const auto candidates = engine.DecodeCorrection(
        "xing'jia'bi", "性", 1, 60);
    if (!ExpectPrefix(candidates, {"价比", "假币", "家比"})) {
        std::cerr << "Unexpected constrained candidates:\n";
        for (const auto& candidate : candidates) {
            std::cerr << "  " << candidate.text << '\n';
        }
        return EXIT_FAILURE;
    }

    // Shuangpin feeds apostrophe-delimited units and disables expansion so a
    // completed syllable's final stays locked. With expansion the decoder
    // abbreviation-matches the locked "yu" to longer finals (石原/yuan,
    // 十元/yuan); with expansion off those must disappear. Assert both
    // directions so the flag can't silently become a no-op.
    const auto expanded = engine.DecodeCorrection(
        "shi'yu'shu'ru'fa", "", 0, 20, /*expansion=*/true);
    if (!ContainsText(expanded, "原") && !ContainsText(expanded, "元")) {
        std::cerr << "expansion=true no longer offers a lengthened final; "
                     "the flag test is now vacuous\n";
        return EXIT_FAILURE;
    }

    const auto locked = engine.DecodeCorrection(
        "shi'yu'shu'ru'fa", "", 0, 20, /*expansion=*/false);
    if (ContainsText(locked, "原") || ContainsText(locked, "元")) {
        std::cerr << "expansion=false leaked a lengthened final:\n";
        for (const auto& candidate : locked) {
            std::cerr << "  " << candidate.text << '\n';
        }
        return EXIT_FAILURE;
    }

    // A lone Shuangpin initial is an incomplete syllable; only expansion
    // (tail completion) can offer candidates for it. Main decode therefore
    // must keep expansion on — disabling it here left a bare initial with an
    // empty candidate bar. Assert both directions so the boundary holds.
    if (engine.DecodeSentence("n", 0, /*expansion=*/true).empty()) {
        std::cerr << "expansion=true dropped lone-initial completion\n";
        return EXIT_FAILURE;
    }
    if (!engine.DecodeSentence("n", 0, /*expansion=*/false).empty()) {
        std::cerr << "expansion=false unexpectedly completed a lone initial\n";
        return EXIT_FAILURE;
    }

    // A trailing initial mid-sentence must also complete: "wanq" (Shuangpin
    // wj+q, i.e. wan + the initial of quan) should surface the word 完全 via
    // expansion, and must not without it.
    if (!ContainsText(engine.DecodeSentence("wanq", 8, /*expansion=*/true),
                      "完全")) {
        std::cerr << "expansion=true no longer completes wanq to 完全\n";
        return EXIT_FAILURE;
    }
    if (ContainsText(engine.DecodeSentence("wanq", 8, /*expansion=*/false),
                     "完全")) {
        std::cerr << "expansion=false unexpectedly completed wanq to 完全\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
