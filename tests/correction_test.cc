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
    {
        sime::Dict shuangpin_dict;
        if (!shuangpin_dict.Load(SIME_TEST_DICT, false) ||
            !shuangpin_dict.Dat(sime::Dict::LetterPinyin).Empty() ||
            shuangpin_dict.Dat(sime::Dict::LetterEn).Empty()) {
            std::cerr << "Shuangpin dictionary loaded the wrong DATs\n";
            return EXIT_FAILURE;
        }
    }

    sime::Sime engine(SIME_TEST_DICT, SIME_TEST_CNT);
    if (!engine.Ready()) {
        std::cerr << "Could not load Sime test models\n";
        return EXIT_FAILURE;
    }
    sime::Sime shuangpin_engine(SIME_TEST_DICT, SIME_TEST_CNT,
                                SIME_TEST_SP_INDEX);
    if (!shuangpin_engine.Ready()) {
        std::cerr << "Could not load Sime Shuangpin test models\n";
        return EXIT_FAILURE;
    }
    sime::Sime xiaohe_engine(SIME_TEST_DICT, SIME_TEST_CNT,
                             SIME_TEST_XIAOHE_SP_INDEX);
    sime::Sime ziranma_engine(SIME_TEST_DICT, SIME_TEST_CNT,
                               SIME_TEST_ZIRANMA_SP_INDEX);
    if (!xiaohe_engine.Ready() || !ziranma_engine.Ready()) {
        std::cerr << "Could not load alternate Shuangpin test models\n";
        return EXIT_FAILURE;
    }
    if (!ContainsText(xiaohe_engine.DecodeSentence("nihc", 8, true), "你好")) {
        std::cerr << "Xiaohe index did not decode nihc as 你好\n";
        return EXIT_FAILURE;
    }
    if (!ContainsText(ziranma_engine.DecodeSentence("nihk", 8, true), "你好")) {
        std::cerr << "Ziranma index did not decode nihk as 你好\n";
        return EXIT_FAILURE;
    }

    const auto mixed_shuangpin =
        shuangpin_engine.DecodeSentence("fixyixw", 2, /*expansion=*/true);
    if (!ContainsText(mixed_shuangpin, "fix一下")) {
        std::cerr << "fixyixw no longer decodes as fix一下\n";
        for (const auto& candidate : mixed_shuangpin) {
            std::cerr << "  " << candidate.text << " [" << candidate.units
                      << "] score=" << candidate.score << '\n';
        }
        return EXIT_FAILURE;
    }
    bool mixed_spans_ok = false;
    for (const auto& candidate : mixed_shuangpin) {
        if (candidate.text == "fix一下") {
            mixed_spans_ok = candidate.segment_keys == std::vector<std::size_t>{3, 2, 2} &&
                             candidate.segment_chars == std::vector<std::size_t>{3, 1, 1};
            break;
        }
    }
    if (!mixed_spans_ok) {
        std::cerr << "fixyixw did not expose English/character spans from the decoder\n";
        return EXIT_FAILURE;
    }

    bool partial_english_ok = false;
    const auto partial_english =
        shuangpin_engine.DecodeSentence("woyeO", 8, /*expansion=*/true);
    for (const auto& candidate : partial_english) {
        if (candidate.text == "我也O") {
            partial_english_ok =
                candidate.segment_keys == std::vector<std::size_t>{2, 2, 1} &&
                candidate.segment_chars == std::vector<std::size_t>{1, 1, 1};
            break;
        }
    }
    if (!partial_english_ok) {
        std::cerr << "woyeO did not preserve the unmatched English letter\n";
        return EXIT_FAILURE;
    }
    if (!ContainsText(engine.DecodeSentence("woyeO", 8, /*expansion=*/true),
                      "我也O")) {
        std::cerr << "full-pinyin woyeO did not preserve the unmatched English letter\n";
        return EXIT_FAILURE;
    }

    bool incomplete_spans_ok = false;
    const auto incomplete_shuangpin =
        shuangpin_engine.DecodeSentence("kdqru", 8, /*expansion=*/true);
    for (const auto& candidate : incomplete_shuangpin) {
        if (candidate.text == "矿泉水") {
            incomplete_spans_ok =
                candidate.segment_keys == std::vector<std::size_t>{2, 2, 1} &&
                candidate.segment_chars == std::vector<std::size_t>{1, 1, 1};
            break;
        }
    }
    if (!incomplete_spans_ok) {
        std::cerr << "kdqru did not expose decoder-aligned character spans\n";
        for (const auto& candidate : incomplete_shuangpin) {
            if (candidate.text != "矿泉水") continue;
            std::cerr << "  keys:";
            for (const auto span : candidate.segment_keys) std::cerr << ' ' << span;
            std::cerr << " chars:";
            for (const auto span : candidate.segment_chars) std::cerr << ' ' << span;
            std::cerr << '\n';
        }
        return EXIT_FAILURE;
    }

    const auto sp_corrections =
        shuangpin_engine.DecodeCorrection("x;jwbi", "性", 2, 60,
                                          /*expansion=*/true);
    bool split_word_correction = false;
    for (const auto& candidate : sp_corrections) {
        if (candidate.text == "假币") {
            split_word_correction =
                candidate.segment_keys == std::vector<std::size_t>{2, 2} &&
                candidate.segment_chars == std::vector<std::size_t>{1, 1};
            break;
        }
    }
    if (!split_word_correction) {
        std::cerr << "index correction did not return suffix-only character spans\n";
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

    // Full pinyin (no apostrophe) still abbreviation-expands: beij -> 北京.
    if (!ContainsText(engine.DecodeSentence("beij", 8, /*expansion=*/true),
                      "北京")) {
        std::cerr << "beij no longer completes to 北京\n";
        return EXIT_FAILURE;
    }

    // A delimited word completion whose head syllables match exactly and
    // whose only extension is the trailing initial must survive: hami'g
    // (ha + mi + the initial of gua) -> 哈密瓜.
    if (!ContainsText(engine.DecodeSentence("hami'g", 8, /*expansion=*/true),
                      "哈密瓜")) {
        std::cerr << "hami'g no longer completes to 哈密瓜\n";
        return EXIT_FAILURE;
    }

    // A lone trailing retroflex initial (zh/ch/sh) is ONE incomplete syllable
    // and completes to exactly one: kuangquan'sh -> 矿泉水, never split into
    // s + h (矿全社会) nor spilled into an extra syllable (矿泉水厂).
    {
        const auto r = engine.DecodeSentence("kuangquan'sh", 8, /*expansion=*/true);
        if (!ContainsText(r, "矿泉水")) {
            std::cerr << "kuangquan'sh no longer completes to 矿泉水\n";
            return EXIT_FAILURE;
        }
        if (ContainsText(r, "社会") || ContainsText(r, "厂")) {
            std::cerr << "kuangquan'sh spilled a lone initial into extra syllables:\n";
            for (const auto& c : r) std::cerr << "  " << c.text << '\n';
            return EXIT_FAILURE;
        }
    }

    // qru = quan + the initial of shen: quan'sh must offer 全身.
    if (!ContainsText(engine.DecodeSentence("quan'sh", 8, /*expansion=*/true),
                      "全身")) {
        std::cerr << "quan'sh no longer offers 全身\n";
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

    // --- Anchor-constrained decode (Chinese) ---
    auto token_for = [&](std::string_view code, std::string_view want) -> sime::TokenID {
        for (const auto& r : shuangpin_engine.DecodeStr(code, 40))
            if (r.text == want && !r.tokens.empty()) return r.tokens[0];
        return 0;
    };
    const sime::TokenID tok_zhong = token_for("vs", "中");
    const sime::TokenID tok_zhong3 = token_for("vs", "种");
    const sime::TokenID tok_guo = token_for("go", "国");
    const sime::TokenID tok_ren = token_for("rf", "人");
    if (!tok_zhong || !tok_zhong3 || !tok_guo || !tok_ren) {
        std::cerr << "anchor test could not resolve seed tokens\n";
        return EXIT_FAILURE;
    }

    // Anchoring 中 at [0,2) keeps 中国 (the word spans past letter 2 but its
    // first piece aligns to [0,2)=中) and drops every 种/other homophone path.
    {
        const auto r = shuangpin_engine.DecodeSentenceWithAnchors(
            "vsgo", {}, {{0, 2, false, tok_zhong, "中"}}, 0, true);
        if (!ContainsText(r, "中国") || ContainsText(r, "种")) {
            std::cerr << "anchor 中@[0,2) should keep 中国 and drop 种\n";
            return EXIT_FAILURE;
        }
    }
    // Anchoring 种 at [0,2) is the mirror: 种过 stays, 中 disappears.
    {
        const auto r = shuangpin_engine.DecodeSentenceWithAnchors(
            "vsgo", {}, {{0, 2, false, tok_zhong3, "种"}}, 0, true);
        if (ContainsText(r, "中") || r.empty()) {
            std::cerr << "anchor 种@[0,2) should drop every 中 path\n";
            return EXIT_FAILURE;
        }
    }
    // Per-char anchors 国@[2,4) + 人@[4,6) leave word grouping free; the top
    // path is still the full 中国人权 and the free tail stays 权 (not 全).
    {
        const auto r = shuangpin_engine.DecodeSentenceWithAnchors(
            "vsgorfqr", {},
            {{2, 4, false, tok_guo, "国"}, {4, 6, false, tok_ren, "人"}}, 0, true);
        if (r.empty() || r[0].text != "中国人权") {
            std::cerr << "anchor 国+人 should keep 中国人权 as top; got: "
                      << (r.empty() ? "(none)" : r[0].text) << '\n';
            return EXIT_FAILURE;
        }
    }
    // English anchor: love@[2,6) over woloveni is one hard-bounded literal, so
    // the top path is 我/沃 + love + 你 (wo and ni decode freely around it).
    {
        const auto r = shuangpin_engine.DecodeSentenceWithAnchors(
            "woloveni", {}, {{2, 6, true, 0, "love"}}, 0, true);
        if (r.empty() || r[0].text.find("love") == std::string::npos ||
            r[0].cnt != 8) {
            std::cerr << "english anchor love@[2,6) should keep ...love... at "
                         "full coverage; got: "
                      << (r.empty() ? "(none)" : r[0].text) << '\n';
            return EXIT_FAILURE;
        }
    }
    return EXIT_SUCCESS;
}
