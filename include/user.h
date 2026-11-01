#pragma once

#include "common.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sime {

class UserSentence {
public:
    using Sentence = std::vector<TokenID>;
    using TokenTextFn = std::function<std::string(TokenID)>;

    // Decoded pinyin per token (DecodeResult::readings); "" when unknown.
    using Readings = std::vector<std::string>;

    // One candidate pick; a polyphone (行 hang/xing) is learned under the
    // reading the user accepted.
    struct Selection {
        Sentence tokens;
        Readings readings;  // aligned with tokens
    };

    // One paragraph = a sequence of consecutive selections that share
    // an unbroken context (i.e. weren't separated by punctuation, IM
    // switch or focus loss). Bigram counting flattens all selections.
    struct Entry {
        std::vector<Selection> selections;
    };

    UserSentence() = default;

    void Clear();

    // vocab_sig: opaque tag identifying the vocabulary the stored
    // token IDs were saved against. Mismatch on Load → reject the file.
    // token_text: optional id→string callback used by Save to emit
    // a tab-separated, per-token-aligned annotation column for
    // human-readable inspection. Null = ids only, no annotation.
    bool Load(const std::filesystem::path& path,
              std::string_view vocab_sig = {});
    bool Save(const std::filesystem::path& path,
              std::string_view vocab_sig = {},
              const TokenTextFn& token_text = {}) const;
    bool LoadText(std::istream& in, std::string_view vocab_sig = {});
    bool SaveText(std::ostream& out,
                  std::string_view vocab_sig = {},
                  const TokenTextFn& token_text = {}) const;

    // Migration path for vocab-mismatched files. Reads the file
    // ignoring stored token IDs, joins each paragraph's text column
    // back to a continuous string, and re-tokenizes via `tokenize`
    // (CutByScan bound by Sime). Each new token becomes
    // its own Selection. Caller persists the migrated state with a
    // fresh vocab_sig.
    using TokenizeFn =
        std::function<std::vector<std::pair<TokenID, std::string>>(
            std::string_view)>;
    bool LoadAndMigrate(const std::filesystem::path& path,
                        const TokenizeFn& tokenize);

    void SetMaxSentenceCount(std::size_t count);
    std::size_t MaxSentenceCount() const { return max_sentence_count_; }
    std::size_t SentenceCount() const { return sentences_.size(); }

    void SetWeight(float_t weight);
    float_t Weight() const { return weight_; }

    // Records a single user pick after `context`, the recent committed
    // tokens. The pick extends the latest paragraph when its tail and
    // `context` agree where they overlap; otherwise a new paragraph starts.
    // `readings` is aligned with `tokens` and `context_readings` with
    // `context`; either may be empty when unknown.
    void Add(const Sentence& context, const Sentence& tokens,
             const Readings& readings = {},
             const Readings& context_readings = {});

    // Counts of `cur` read as `reading` (after `prev` read as
    // `prev_reading`); picks learned without a reading count toward every
    // reading. An empty `prev_reading`, as for host text, sums all of them.
    std::uint32_t UnigramCount(TokenID token, std::string_view reading = {}) const;
    std::uint32_t BigramCount(TokenID prev, TokenID cur,
                              std::string_view reading = {},
                              std::string_view prev_reading = {}) const;
    std::uint32_t TotalUnigramCount() const { return total_unigram_count_; }
    // N1+(prev, *) — number of distinct successors after `prev`.
    // Used by Ney's absolute-discounting backoff weight (γ).
    std::uint32_t DistinctSuccessors(TokenID prev) const;
    // Tokens picked right after `prev`, under any reading.
    std::vector<TokenID> Successors(TokenID prev) const;
    // Cost adjustment to add to LM cost for a transition prev→cur, where
    // each is read as its reading (an edge's pieces; null = unknown).
    // `lm_step` is the LM cost (positive, = -log P) for the same
    // transition. Returns ≤ 0; never makes the cost worse than LM
    // alone. Mirrors libime's UserLanguageModel::score: linear mixture
    // in probability space, max-floored at the LM score.
    float_t CostAdjustment(TokenID prev, const char* prev_reading,
                           TokenID cur, const char* reading,
                           float_t lm_step) const;

    const std::deque<Entry>& Sentences() const { return sentences_; }

private:
    // (token, reading) packed into one key; an empty reading keys the
    // token alone.
    static std::uint64_t WordKey(TokenID token, std::string_view reading);
    static std::uint64_t BigramKey(std::uint64_t prev, std::uint64_t word);
    static Selection CleanSelection(const Sentence& tokens,
                                    const Readings& readings);
    static bool TailsAgree(const Entry& entry, const Sentence& context,
                           const Readings& context_readings);
    std::uint32_t WordCount(std::uint64_t word) const;
    std::uint32_t BigramWordCount(TokenID prev, std::uint64_t word) const;
    std::uint32_t WordBigramCount(std::uint64_t prev, std::uint64_t word) const;
    void CountBigram(TokenID prev, std::string_view prev_reading,
                     std::uint64_t word, int delta);

    void StartParagraph(Selection selection);
    void AppendToLatest(Selection selection);
    void AdjustEntryCounts(const Entry& entry, int delta);
    void Rebuild();

    std::size_t max_sentence_count_ = 8192;
    // libime's UserLanguageModel uses 0.2 as its default mixture
    // weight (constants.h: DEFAULT_USER_LANGUAGE_MODEL_USER_WEIGHT).
    float_t weight_ = 0.2;
    std::deque<Entry> sentences_;
    // Per token over all readings: the bigram history's count, and a
    // cheap miss test before hashing an edge's reading.
    std::unordered_map<TokenID, std::uint32_t> token_count_;
    std::unordered_map<std::uint64_t, std::uint32_t> word_count_;
    // (prev token, cur word) → count.
    std::unordered_map<std::uint64_t, std::uint32_t> bigram_;
    // N1+(prev, *): distinct-successor count per prev token. Maintained
    // incrementally when bigram counts cross the 0 boundary.
    std::unordered_map<TokenID, std::uint32_t> distinct_successors_;
    // The same keyed by the prev word, so a known prev reading is told apart.
    std::unordered_map<std::uint64_t, std::uint32_t> word_bigram_;
    std::unordered_map<std::uint64_t, std::uint32_t> word_successors_;
    // prev token → successor token → count, so association can offer
    // successors the LM ranks too low to fetch.
    std::unordered_map<TokenID, std::unordered_map<TokenID, std::uint32_t>>
        successor_tokens_;
    std::uint32_t total_unigram_count_ = 0;
};

} // namespace sime
