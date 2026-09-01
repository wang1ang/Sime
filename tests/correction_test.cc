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

    // Delimited input locks a completed final even with expansion on:
    // expansion may only complete a trailing lone initial. So
    // "shi'yu'shu'ru'fa" must not surface 石原/十元 (yu -> yuan) either way.
    for (bool expansion : {true, false}) {
        const auto results = engine.DecodeCorrection(
            "shi'yu'shu'ru'fa", "", 0, 20, expansion);
        if (ContainsText(results, "原") || ContainsText(results, "元")) {
            std::cerr << "delimited final leaked a lengthened reading (expansion="
                      << expansion << "):\n";
            for (const auto& candidate : results) {
                std::cerr << "  " << candidate.text << '\n';
            }
            return EXIT_FAILURE;
        }
    }

    // A delimited completed syllable is never merged with a following lone
    // initial nor lengthened, though its pinyin (na/he/xi) is extendable.
    //   na'n     : never merged 南/南宁
    //   nenghe'm : 能喝吗 ok, never 能黑马 (he -> hei)
    //   xi'h     : never 先 (xi -> xian)
    {
        const auto na = engine.DecodeSentence("na'n", 8, /*expansion=*/true);
        // 南 is a valid rare "na" reading (南无), so a single 南 is fine; the
        // bug was "na" lengthened to "nan" (merged 南, or the word 南宁).
        for (const auto& c : na) {
            auto apos = c.units.find('\'');
            std::string first =
                apos == std::string::npos ? c.units : c.units.substr(0, apos);
            if (first == "nan") {
                std::cerr << "na'n lengthened the locked \"na\" to \"nan\": "
                          << c.text << " [" << c.units << "]\n";
                return EXIT_FAILURE;
            }
        }
        if (ContainsText(na, "南宁")) {
            std::cerr << "na'n abbreviation-matched the word 南宁 (nanning)\n";
            return EXIT_FAILURE;
        }
        const auto neng = engine.DecodeSentence("nenghe'm", 8, /*expansion=*/true);
        if (neng.empty() || ContainsText(neng, "黑")) {
            std::cerr << "nenghe'm leaked a lengthened final (黑) or was empty\n";
            return EXIT_FAILURE;
        }
        // Legal pinyin hen+he+ma (很喝吗) must still yield candidates.
        if (engine.DecodeSentence("henhe'm", 8, /*expansion=*/true).empty()) {
            std::cerr << "henhe'm produced no candidates\n";
            return EXIT_FAILURE;
        }
        const auto xi = engine.DecodeSentence("xi'h", 8, /*expansion=*/true);
        if (ContainsText(xi, "先")) {
            std::cerr << "xi'h abbreviation-rewrote the locked \"xi\" to xian (先)\n";
            return EXIT_FAILURE;
        }
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
