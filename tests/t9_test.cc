#include "dict.h"
#include "sime.h"

#include <cstdlib>
#include <iostream>
#include <numeric>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void Expect(bool ok, std::string_view what) {
    if (!ok) {
        std::cerr << "FAIL: " << what << "\n";
        ++failures;
    }
}

std::size_t KeySum(const sime::DecodeResult& r) {
    return std::accumulate(r.segment_keys.begin(), r.segment_keys.end(),
                           std::size_t{0});
}

bool Contains(const std::vector<sime::DecodeResult>& results,
              std::string_view text) {
    for (const auto& r : results) {
        if (r.text == text) return true;
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

    // Digits decode on the main sentence path, with per-syllable spans and
    // letter units of the same length as the digits.
    {
        const auto r = engine.DecodeSentence("94664486", 2, true);
        Expect(!r.empty() && r[0].text == "中国", "94664486 -> 中国");
        Expect(!r.empty() && r[0].units == "zhong'guo", "94664486 units");
        Expect(!r.empty() && r[0].segment_keys ==
                   std::vector<std::size_t>({5, 3}), "94664486 spans");
    }
    {
        const auto r = engine.DecodeSentence("64426", 2, true);
        Expect(!r.empty() && r[0].text == "你好", "64426 -> 你好");
    }
    // The 1 key separates syllables.
    {
        const auto r = engine.DecodeSentence("94'26", 2, true);
        Expect(!r.empty() && r[0].text == "西安", "94'26 -> 西安");
        Expect(!r.empty() && KeySum(r[0]) == 5, "94'26 spans cover raw");
    }
    // Full pinyin separators are counted too.
    {
        const auto r = engine.DecodeSentence("xi'an", 2, true);
        Expect(!r.empty() && KeySum(r[0]) == 5, "xi'an spans cover raw");
    }
    // Words, correction and anchors take digits as well.
    {
        const auto r = engine.DecodeStr("94", 20);
        Expect(Contains(r, "一"), "DecodeStr 94 lists 一");
    }
    {
        const auto r = engine.DecodeCorrection("64426", "你", 2, 20, true);
        Expect(Contains(r, "好"), "correction after 你 lists 好");
    }
    {
        const auto mi = engine.Tokenize("米");
        sime::Anchor anchor;
        anchor.a = 0;
        anchor.b = 2;
        anchor.token = mi.empty() ? 0 : mi[0];
        anchor.text = "米";
        const auto r = engine.DecodeSentenceWithAnchors("64426", {}, {anchor}, 2);
        Expect(!r.empty() && r[0].text.rfind("米", 0) == 0, "anchor 米 holds");
    }
    // A reading anchor pins a syllable and re-decodes around it.
    {
        sime::Anchor gao;
        gao.a = 2;
        gao.b = 5;
        gao.reading = "gao";
        const auto r = engine.DecodeSentenceWithAnchors("64426", {}, {gao}, 4);
        Expect(!r.empty(), "reading anchor gao decodes");
        for (const auto& c : r) {
            const auto sep = c.units.find('\'');
            Expect(sep == std::string::npos ||
                       c.units.substr(sep + 1, 3) == "gao",
                   "reading anchor gao holds");
        }
    }
    {
        sime::Anchor ni;
        ni.a = 0;
        ni.b = 2;
        ni.text = "你";
        sime::Anchor gao;
        gao.a = 2;
        gao.b = 5;
        gao.reading = "gao";
        const auto r = engine.DecodeSentenceWithAnchors("64426", {}, {ni, gao}, 2);
        Expect(!r.empty() && r[0].units == "ni'gao" && r[0].text.rfind("你", 0) == 0,
               "char and reading anchors combine");
    }
    {
        sime::Anchor an;
        an.a = 2;
        an.b = 5;
        an.reading = "an";
        const auto r = engine.DecodeSentenceWithAnchors("94'26", {}, {an}, 2);
        Expect(!r.empty() && r[0].units == "xi'an", "reading anchor after 1 key");
    }
    // Filter syllables for a digit run, longest first.
    {
        const auto s = sime::Dict::T9PinyinSyllables("94", 32);
        bool xi = false, yi = false, z = false;
        for (const auto& syllable : s) {
            xi |= syllable == "xi";
            yi |= syllable == "yi";
            z |= syllable == "zi";
        }
        Expect(xi && yi && z, "T9PinyinSyllables 94");
    }

    if (failures) return EXIT_FAILURE;
    std::cout << "t9 ok\n";
    return EXIT_SUCCESS;
}
