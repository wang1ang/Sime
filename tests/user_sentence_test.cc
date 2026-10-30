#include "sime.h"
#include "user.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
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

// Score of the candidate rendering `text` exactly, or a sentinel.
double ScoreOf(const std::vector<sime::DecodeResult>& results,
               std::string_view text) {
    for (const auto& r : results) {
        if (r.text == text) return r.score;
    }
    return -1e30;
}

}  // namespace

int main() {
    using sime::UserSentence;

    // A pick counts under its reading only; a reading-less pick counts
    // toward every reading of the token.
    {
        UserSentence user;
        user.Add({}, {7}, {"hang"});
        Expect(user.UnigramCount(7, "hang") == 1, "reading counted");
        Expect(user.UnigramCount(7, "xing") == 0, "other reading untouched");
        user.Add({7}, {8}, {"ye"});
        Expect(user.BigramCount(7, 8, "ye") == 1, "bigram after prev token");
        Expect(user.BigramCount(7, 8, "xie") == 0, "bigram keyed by reading");
        user.Add({}, {7});
        Expect(user.UnigramCount(7, "xing") == 1, "reading-less pick is shared");
        Expect(user.UnigramCount(7, "hang") == 2, "reading-less pick adds up");
        Expect(user.CostAdjustment(sime::NotToken, nullptr, 9, "hang", 5.0) == 0.0,
               "unknown token is not adjusted");
        Expect(user.CostAdjustment(sime::NotToken, nullptr, 7, "hang", 5.0) < 0.0,
               "learned reading is boosted");
    }

    // A bigram after a prev token with a known reading counts only under
    // that reading; an unknown prev reading (host text) sums them all.
    {
        UserSentence user;
        user.Add({}, {7, 8}, {"hang", "ye"});
        user.Add({}, {7, 9}, {"xing", "dong"});
        Expect(user.BigramCount(7, 8, "ye", "hang") == 1, "prev reading kept");
        Expect(user.BigramCount(7, 8, "ye", "xing") == 0,
               "bigram keyed by prev reading");
        Expect(user.BigramCount(7, 9, "dong", "xing") == 1 &&
                   user.BigramCount(7, 9, "dong") == 1,
               "unknown prev reading sums readings");
        Expect(user.CostAdjustment(7, "hang", 8, "ye", 5.0) <
                   user.CostAdjustment(7, "xing", 8, "ye", 5.0),
               "prev reading conditions the boost");
    }

    // A pick chains onto the latest paragraph when the context is longer
    // than the paragraph, and not when the overlapping readings disagree.
    {
        UserSentence user;
        user.Add({1, 2, 3}, {7}, {"hang"});
        user.Add({1, 2, 3, 7}, {8}, {"ye"}, {"", "", "", "hang"});
        Expect(user.SentenceCount() == 1 &&
                   user.BigramCount(7, 8, "ye", "hang") == 1,
               "longer context chains the pick");
        user.Add({1, 7, 8}, {9}, {"dong"}, {"", "xing", "ye"});
        Expect(user.SentenceCount() == 2, "disagreeing reading breaks the chain");
    }

    // Readings survive a save/load round trip, and a vocabulary migration
    // where the re-cut token covers the same text.
    {
        UserSentence user;
        user.Add({}, {7, 8}, {"yin", "hang"});
        user.Add({8}, {9}, {"hao"});
        std::stringstream text;
        auto token_text = [](sime::TokenID id) {
            return id == 7 ? std::string("银") : id == 8 ? std::string("行")
                                                         : std::string("好");
        };
        Expect(user.SaveText(text, "sig", token_text), "save");
        const std::string saved = text.str();

        UserSentence loaded;
        std::stringstream in(saved);
        Expect(loaded.LoadText(in, "sig"), "load");
        Expect(loaded.UnigramCount(8, "hang") == 1 &&
                   loaded.UnigramCount(8, "xing") == 0,
               "reading reloaded");
        Expect(loaded.BigramCount(8, 9, "hao") == 1 &&
                   loaded.BigramCount(8, 9, "hai") == 0,
               "bigram reading reloaded");

        const auto path = std::filesystem::temp_directory_path() /
                          "sime_user_sentence_test.txt";
        { std::ofstream(path) << saved; }
        UserSentence migrated;
        // New vocabulary: 银行 is one token (70); 好 is 90.
        auto tokenize = [](std::string_view s) {
            std::vector<std::pair<sime::TokenID, std::string>> out;
            if (s == "银行") out.emplace_back(70, "银行");
            if (s == "好") out.emplace_back(90, "好");
            return out;
        };
        Expect(migrated.LoadAndMigrate(path, tokenize), "migrate");
        Expect(migrated.UnigramCount(90, "hao") == 1 &&
                   migrated.UnigramCount(90, "hai") == 0,
               "an unchanged token keeps its reading");
        Expect(migrated.UnigramCount(70, "anything") == 1,
               "a re-cut token is learned without a reading");
        std::filesystem::remove(path);
    }

    // End to end: a pick learned as one reading raises that reading only.
    {
        sime::Sime engine(SIME_TEST_DICT, SIME_TEST_CNT);
        if (!engine.Ready()) {
            std::cerr << "Could not load Sime test models\n";
            return EXIT_FAILURE;
        }
        engine.SetUserSentenceEnabled(true);
        const auto xing_before = ScoreOf(engine.DecodeStr("xing", 60), "行");
        const auto hang_before = ScoreOf(engine.DecodeStr("hang", 60), "行");
        Expect(xing_before > -1e29 && hang_before > -1e29, "行 decodes both ways");

        const auto picks = engine.DecodeStr("hang", 60);
        const sime::DecodeResult* hang = nullptr;
        for (const auto& r : picks) {
            if (r.text == "行") hang = &r;
        }
        Expect(hang && hang->readings.size() == hang->tokens.size() &&
                   hang->readings.front() == "hang",
               "decode reports the decoded reading");
        if (hang) {
            for (int i = 0; i < 5; ++i) {
                engine.LearnUserSentence({}, hang->tokens, hang->readings);
            }
        }
        Expect(ScoreOf(engine.DecodeStr("hang", 60), "行") > hang_before,
               "learned reading ranks higher");
        Expect(ScoreOf(engine.DecodeStr("xing", 60), "行") == xing_before,
               "other reading is unchanged");

        // The keyboard's own commit carries its reading into the next
        // decode: the learned bigram applies only after 行 read as hang.
        if (hang) {
            const auto ye = engine.DecodeSentence("ye", {}, 0, true);
            const sime::DecodeResult* pick = nullptr;
            for (const auto& r : ye) {
                if (r.text == "业") pick = &r;
            }
            Expect(pick != nullptr, "ye decodes 业");
            if (pick) {
                for (int i = 0; i < 5; ++i) {
                    engine.LearnUserSentence({}, hang->tokens, hang->readings);
                    engine.LearnUserSentence(hang->tokens, pick->tokens,
                                             pick->readings, {"hang"});
                }
                const auto after_hang = ScoreOf(
                    engine.DecodeSentence("ye", hang->tokens, 0, true, {"hang"}),
                    "业");
                const auto after_xing = ScoreOf(
                    engine.DecodeSentence("ye", hang->tokens, 0, true, {"xing"}),
                    "业");
                Expect(after_hang > after_xing,
                       "context reading selects the learned bigram");
            }
        }
    }

    // Disabled: picks are still learned but don't affect decoding.
    {
        sime::Sime engine(SIME_TEST_DICT, SIME_TEST_CNT);
        if (!engine.Ready()) {
            std::cerr << "Could not load Sime test models\n";
            return EXIT_FAILURE;
        }
        const auto before = ScoreOf(engine.DecodeStr("hang", 60), "行");
        const auto picks = engine.DecodeStr("hang", 60);
        const sime::DecodeResult* hang = nullptr;
        for (const auto& r : picks) {
            if (r.text == "行") hang = &r;
        }
        Expect(hang != nullptr, "hang decodes 行");
        if (hang) {
            for (int i = 0; i < 5; ++i) {
                engine.LearnUserSentence({}, hang->tokens, hang->readings);
            }
            Expect(engine.GetUserSentence().UnigramCount(
                       hang->tokens.front(), "hang") == 5,
                   "disabled engine still learns");
            Expect(ScoreOf(engine.DecodeStr("hang", 60), "行") == before,
                   "disabled engine decodes without history");
            engine.SetUserSentenceEnabled(true);
            Expect(ScoreOf(engine.DecodeStr("hang", 60), "行") > before,
                   "enabling applies history learned while disabled");
        }
    }

    // Association: a learned successor the LM ranks too low still shows up,
    // only while history is enabled.
    {
        sime::Sime engine(SIME_TEST_DICT, SIME_TEST_CNT);
        if (!engine.Ready()) {
            std::cerr << "Could not load Sime test models\n";
            return EXIT_FAILURE;
        }
        auto token_of = [&](std::string_view pinyin, std::string_view text) {
            for (const auto& r : engine.DecodeStr(pinyin, 60)) {
                if (r.text == text) return r.tokens;
            }
            return std::vector<sime::TokenID>{};
        };
        const auto mingtian = token_of("mingtian", "明天");
        const auto huli = token_of("huli", "狐狸");
        Expect(mingtian.size() == 1 && huli.size() == 1, "test words decode");
        if (mingtian.size() == 1 && huli.size() == 1) {
            Expect(ScoreOf(engine.Associate(mingtian, 9), "狐狸") < -1e29,
                   "LM alone does not associate 狐狸 after 明天");
            // A bigram is learned only after a context that was learned too.
            for (int i = 0; i < 3; ++i) {
                engine.LearnUserSentence({}, mingtian);
                engine.LearnUserSentence(mingtian, huli);
            }
            Expect(ScoreOf(engine.Associate(mingtian, 9), "狐狸") < -1e29,
                   "disabled history leaves association unchanged");
            engine.SetUserSentenceEnabled(true);
            Expect(ScoreOf(engine.Associate(mingtian, 9), "狐狸") > -1e29,
                   "learned successor is associated");
            Expect(ScoreOf(engine.Associate({}, 9), "狐狸") < -1e29,
                   "empty context associates nothing");
        }
    }

    if (failures > 0) return EXIT_FAILURE;
    std::cout << "user sentence tests passed\n";
    return EXIT_SUCCESS;
}
