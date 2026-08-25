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
    return EXIT_SUCCESS;
}
