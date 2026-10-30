#include "user.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <istream>
#include <limits>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace sime {

namespace {

constexpr const char* kHeader = "SIME_USER_SENTENCE_V5";
constexpr std::string_view kVocabPrefix = "VOCAB ";
// Ney's absolute-discount values, mirroring the main LM's defaults in
// construct.cc (DefaultDiscounts = {0.0005, 0.5, 0.5}). Subtracted
// from each raw count; lost mass is redistributed via gamma to the
// lower-order distribution.
constexpr float_t kBigramDiscount = 0.5;
constexpr float_t kUnigramDiscount = 0.0005;

template <typename Key>
void IncCount(std::unordered_map<Key, std::uint32_t>& m,
              Key key, std::uint32_t delta) {
    m[key] += delta;
}

template <typename Key>
void DecCount(std::unordered_map<Key, std::uint32_t>& m,
              Key key, std::uint32_t delta) {
    auto it = m.find(key);
    if (it == m.end()) return;
    if (it->second <= delta) m.erase(it);
    else it->second -= delta;
}

// Bigram inc/dec also tracks N1+(prev, *) — distinct-successor count.
// We need to know when count crosses 0 in either direction to update
// the successor count atomically.
template <typename Prev>
void IncBigramTrackSucc(
    std::unordered_map<std::uint64_t, std::uint32_t>& bigram,
    std::unordered_map<Prev, std::uint32_t>& succ,
    Prev prev, std::uint64_t key, std::uint32_t delta) {
    auto& count = bigram[key];  // creates entry with 0 if absent
    bool was_zero = (count == 0);
    count += delta;
    if (was_zero && count > 0) {
        ++succ[prev];
    }
}

template <typename Prev>
void DecBigramTrackSucc(
    std::unordered_map<std::uint64_t, std::uint32_t>& bigram,
    std::unordered_map<Prev, std::uint32_t>& succ,
    Prev prev, std::uint64_t key, std::uint32_t delta) {
    auto it = bigram.find(key);
    if (it == bigram.end()) return;
    bool was_nonzero = (it->second > 0);
    bool became_zero;
    if (it->second <= delta) {
        bigram.erase(it);
        became_zero = was_nonzero;
    } else {
        it->second -= delta;
        became_zero = false;
    }
    if (became_zero) {
        auto sit = succ.find(prev);
        if (sit != succ.end()) {
            if (sit->second <= 1) succ.erase(sit);
            else --sit->second;
        }
    }
}

std::uint32_t Lookup(const std::unordered_map<std::uint64_t, std::uint32_t>& m,
                     std::uint64_t key) {
    auto it = m.find(key);
    return it == m.end() ? 0 : it->second;
}

// Splits a "<id>[:<reading>]" field.
bool ParseItem(std::string_view field, TokenID& id, std::string& reading) {
    const auto colon = field.find(':');
    const auto digits = field.substr(0, colon);
    std::uint64_t raw = 0;
    if (digits.empty()) return false;
    for (char c : digits) {
        if (c < '0' || c > '9') return false;
        raw = raw * 10 + static_cast<std::uint64_t>(c - '0');
        if (raw > std::numeric_limits<TokenID>::max()) return false;
    }
    id = static_cast<TokenID>(raw);
    reading = colon == std::string_view::npos
        ? std::string{} : std::string(field.substr(colon + 1));
    return true;
}

std::vector<std::string_view> SplitSpaces(std::string_view text) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = std::min(text.find(' ', start), text.size());
        out.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return out;
}

} // namespace

void UserSentence::Clear() {
    sentences_.clear();
    token_count_.clear();
    word_count_.clear();
    bigram_.clear();
    distinct_successors_.clear();
    word_bigram_.clear();
    word_successors_.clear();
    successor_tokens_.clear();
    total_unigram_count_ = 0;
}

bool UserSentence::Load(const std::filesystem::path& path,
                       std::string_view vocab_sig) {
    std::ifstream in(path);
    if (!in.is_open()) {
        return false;
    }
    return LoadText(in, vocab_sig);
}

bool UserSentence::Save(const std::filesystem::path& path,
                       std::string_view vocab_sig,
                       const TokenTextFn& token_text) const {
    if (auto parent = path.parent_path(); !parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return false;
        }
    }

    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp);
        if (!out.is_open()) {
            return false;
        }
        if (!SaveText(out, vocab_sig, token_text)) {
            out.close();
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

bool UserSentence::LoadText(std::istream& in, std::string_view vocab_sig) {
    std::string line;
    if (!std::getline(in, line) || line != kHeader) {
        return false;
    }

    if (!std::getline(in, line)) {
        return false;
    }
    if (line.size() < kVocabPrefix.size() ||
        std::string_view(line).substr(0, kVocabPrefix.size()) != kVocabPrefix) {
        return false;
    }
    std::string_view stored_sig =
        std::string_view(line).substr(kVocabPrefix.size());
    if (!vocab_sig.empty() && stored_sig != vocab_sig) {
        return false;
    }

    std::deque<Entry> loaded;
    Entry current;
    auto flush = [&]() {
        if (!current.selections.empty()) {
            loaded.push_back(std::move(current));
            current = Entry{};
        }
    };

    while (std::getline(in, line)) {
        if (line.empty()) {
            flush();
            if (loaded.size() >= max_sentence_count_) {
                break;
            }
            continue;
        }

        // Per-selection line: "<id>[:<reading>] ...[\t<annotation>]"
        // The annotation is informational only — we ignore it on load.
        auto tab = line.find('\t');
        std::string_view ids_part = (tab == std::string::npos)
                                        ? std::string_view(line)
                                        : std::string_view(line).substr(0, tab);

        Sentence tokens;
        Readings readings;
        for (auto field : SplitSpaces(ids_part)) {
            if (field.empty()) continue;
            TokenID id = NotToken;
            std::string reading;
            if (!ParseItem(field, id, reading)) {
                tokens.clear();
                break;
            }
            tokens.push_back(id);
            readings.push_back(std::move(reading));
        }
        auto sel = CleanSelection(tokens, readings);
        if (sel.tokens.empty()) {
            continue;
        }
        current.selections.push_back(std::move(sel));
    }
    flush();

    sentences_ = std::move(loaded);
    Rebuild();
    return true;
}

bool UserSentence::LoadAndMigrate(const std::filesystem::path& path,
                                  const TokenizeFn& tokenize) {
    std::ifstream in(path);
    if (!in.is_open()) {
        return false;
    }

    std::string line;
    if (!std::getline(in, line) || line != kHeader) {
        return false;
    }
    // Skip the stored VOCAB line — caller's vocab_sig will be written
    // on the next save.
    if (!std::getline(in, line) ||
        line.size() < kVocabPrefix.size() ||
        std::string_view(line).substr(0, kVocabPrefix.size()) != kVocabPrefix) {
        return false;
    }

    std::deque<Entry> migrated;
    Entry current;

    auto flush_paragraph = [&]() {
        if (!current.selections.empty()) {
            migrated.push_back(std::move(current));
            current = Entry{};
        }
    };

    while (std::getline(in, line)) {
        if (line.empty()) {
            flush_paragraph();
            if (migrated.size() >= max_sentence_count_) {
                break;
            }
            continue;
        }
        // Take only the text column (after tab); old IDs are useless
        // under the new vocab. Strip the per-token spaces we wrote at
        // save time so the segmenter sees an unbroken run.
        auto tab = line.find('\t');
        if (tab == std::string::npos) {
            continue;
        }
        const std::string_view annotation =
            std::string_view(line).substr(tab + 1);
        std::string text(annotation);
        text.erase(std::remove(text.begin(), text.end(), ' '), text.end());
        if (text.empty()) {
            continue;
        }

        // A reading survives when a new token covers exactly the text
        // an old token did; the annotation is aligned with the items.
        const auto items = SplitSpaces(std::string_view(line).substr(0, tab));
        const auto old_texts = SplitSpaces(annotation);
        std::unordered_map<std::string, std::string> reading_at;
        if (items.size() == old_texts.size()) {
            std::size_t offset = 0;
            for (std::size_t i = 0; i < items.size(); ++i) {
                TokenID id = NotToken;
                std::string reading;
                if (ParseItem(items[i], id, reading) && !reading.empty()) {
                    reading_at[std::to_string(offset) + ' ' +
                               std::string(old_texts[i])] = std::move(reading);
                }
                offset += old_texts[i].size();
            }
        }

        // Per-line tokenize: each original selection becomes one new
        // Selection in the migrated file, preserving the user's pick
        // grouping. If any token in the slice is OOV under the new
        // vocab, drop the whole selection.
        auto cuts = tokenize(text);
        Sentence sel_tokens;
        Readings sel_readings;
        bool has_unk = false;
        std::size_t offset = 0;
        for (const auto& [id, txt] : cuts) {
            if (id == NotToken || id == static_cast<TokenID>(-1)) {
                has_unk = true;
                break;
            }
            sel_tokens.push_back(id);
            auto it = reading_at.find(std::to_string(offset) + ' ' + txt);
            sel_readings.push_back(it == reading_at.end() ? std::string{}
                                                          : it->second);
            offset += txt.size();
        }
        if (has_unk || sel_tokens.empty()) {
            continue;
        }
        current.selections.push_back(
            {std::move(sel_tokens), std::move(sel_readings)});
    }
    flush_paragraph();

    sentences_ = std::move(migrated);
    Rebuild();
    return true;
}

bool UserSentence::SaveText(std::ostream& out,
                           std::string_view vocab_sig,
                           const TokenTextFn& token_text) const {
    out << kHeader << '\n';
    out << kVocabPrefix << vocab_sig << '\n';
    bool first_entry = true;
    for (const auto& entry : sentences_) {
        if (!first_entry) {
            out << '\n';
        }
        first_entry = false;
        for (const auto& sel : entry.selections) {
            for (std::size_t i = 0; i < sel.tokens.size(); ++i) {
                if (i != 0) {
                    out << ' ';
                }
                out << sel.tokens[i];
                if (!sel.readings[i].empty()) {
                    out << ':' << sel.readings[i];
                }
            }
            if (token_text) {
                bool any = false;
                std::string annotation;
                for (std::size_t i = 0; i < sel.tokens.size(); ++i) {
                    auto t = token_text(sel.tokens[i]);
                    if (i != 0) {
                        annotation.push_back(' ');
                    }
                    annotation.append(t);
                    if (!t.empty()) {
                        any = true;
                    }
                }
                if (any) {
                    out << '\t' << annotation;
                }
            }
            out << '\n';
        }
    }
    return static_cast<bool>(out);
}

void UserSentence::SetMaxSentenceCount(std::size_t count) {
    max_sentence_count_ = count;
    while (sentences_.size() > max_sentence_count_) {
        AdjustEntryCounts(sentences_.back(), -1);
        sentences_.pop_back();
    }
}

void UserSentence::SetWeight(float_t weight) {
    if (weight < 0.0) {
        weight_ = 0.0;
    } else {
        weight_ = weight;
    }
}

void UserSentence::Add(const Sentence& context, const Sentence& tokens,
                       const Readings& readings,
                       const Readings& context_readings) {
    Selection sel = CleanSelection(tokens, readings);
    if (sel.tokens.empty()) {
        return;
    }
    if (!sentences_.empty() &&
        TailsAgree(sentences_.front(), context, context_readings)) {
        AppendToLatest(std::move(sel));
        return;
    }
    StartParagraph(std::move(sel));
}

std::uint32_t UserSentence::WordCount(std::uint64_t word) const {
    return Lookup(word_count_, word);
}

std::uint32_t UserSentence::BigramWordCount(TokenID prev,
                                            std::uint64_t word) const {
    return Lookup(bigram_, BigramKey(prev, word));
}

std::uint32_t UserSentence::WordBigramCount(std::uint64_t prev,
                                            std::uint64_t word) const {
    return Lookup(word_bigram_, BigramKey(prev, word));
}

std::uint32_t UserSentence::UnigramCount(TokenID token,
                                         std::string_view reading) const {
    if (token == NotToken || !token_count_.contains(token)) {
        return 0;
    }
    const auto any = WordKey(token, {});
    if (reading.empty()) {
        return WordCount(any);
    }
    return WordCount(WordKey(token, reading)) + WordCount(any);
}

std::uint32_t UserSentence::BigramCount(TokenID prev, TokenID cur,
                                        std::string_view reading,
                                        std::string_view prev_reading) const {
    if (prev == NotToken || cur == NotToken) {
        return 0;
    }
    const auto any = WordKey(cur, {});
    if (prev_reading.empty()) {
        if (reading.empty()) {
            return BigramWordCount(prev, any);
        }
        return BigramWordCount(prev, WordKey(cur, reading)) +
               BigramWordCount(prev, any);
    }
    std::uint32_t count = 0;
    for (const auto p : {WordKey(prev, prev_reading), WordKey(prev, {})}) {
        count += WordBigramCount(p, any);
        if (!reading.empty()) {
            count += WordBigramCount(p, WordKey(cur, reading));
        }
    }
    return count;
}

std::uint32_t UserSentence::DistinctSuccessors(TokenID prev) const {
    if (prev == NotToken) {
        return 0;
    }
    auto it = distinct_successors_.find(prev);
    return it == distinct_successors_.end() ? 0 : it->second;
}

std::vector<TokenID> UserSentence::Successors(TokenID prev) const {
    std::vector<TokenID> out;
    auto it = successor_tokens_.find(prev);
    if (it == successor_tokens_.end()) return out;
    out.reserve(it->second.size());
    for (const auto& [token, count] : it->second) out.push_back(token);
    return out;
}

float_t UserSentence::CostAdjustment(TokenID prev, const char* prev_reading,
                                     TokenID cur, const char* reading,
                                     float_t lm_step) const {
    if (cur == NotToken || weight_ <= 0.0 || weight_ >= 1.0 ||
        total_unigram_count_ == 0) {
        return 0.0;
    }
    const std::string_view cur_reading = reading ? reading : "";
    auto cur_count = UnigramCount(cur, cur_reading);
    if (cur_count == 0) {
        return 0.0;
    }

    // Interpolated Absolute Discounting at both levels — same family
    // and same per-order D values as the main LM (construct.cc):
    //   gamma(h)    = D · N1+(h, *) / c(h)
    //   P(w|h)      = max(c(h,w) - D, 0) / c(h)  +  gamma(h) · P(w|h')
    // The unigram tail "terminates at zero" rather than backing off
    // to a uniform base. Numerically near-equivalent to the main LM
    // (D_unigram = 0.0005 is tiny) and conceptually clean: the outer
    // mixture-with-LM provides the real fallback for tokens the user
    // has never picked.
    const float_t total = static_cast<float_t>(total_unigram_count_);
    float_t pr_unigram =
        std::max(static_cast<float_t>(cur_count) - kUnigramDiscount, 0.0) /
        total;

    float_t pr_user = pr_unigram;
    if (prev != NotToken) {
        // A known prev reading conditions on that reading's history only.
        const std::string_view prev_word = prev_reading ? prev_reading : "";
        std::uint32_t prev_count = 0;
        std::uint32_t n_succ = 0;
        if (prev_word.empty()) {
            auto it = token_count_.find(prev);
            prev_count = it == token_count_.end() ? 0 : it->second;
            n_succ = DistinctSuccessors(prev);
        } else if (token_count_.contains(prev)) {
            prev_count = UnigramCount(prev, prev_word);
            n_succ = Lookup(word_successors_, WordKey(prev, prev_word)) +
                     Lookup(word_successors_, WordKey(prev, {}));
        }
        if (prev_count > 0) {
            auto bigram_count = BigramCount(prev, cur, cur_reading, prev_word);
            const float_t denom = static_cast<float_t>(prev_count);
            const float_t alpha =
                std::max(static_cast<float_t>(bigram_count) - kBigramDiscount,
                         0.0) /
                denom;
            const float_t gamma =
                kBigramDiscount * static_cast<float_t>(n_succ) / denom;
            pr_user = alpha + gamma * pr_unigram;
        }
    }
    pr_user = std::min<float_t>(pr_user, 1.0);
    if (pr_user <= 0.0) {
        return 0.0;
    }

    // Mixture with LM in probability space, max-floored at lm_log_prob
    // so user history can only improve the score, never worsen it.
    const float_t lm_log_prob = -lm_step;
    const float_t user_log_prob = std::log(pr_user);
    const float_t a = lm_log_prob + std::log(1.0 - weight_);
    const float_t b = user_log_prob + std::log(weight_);
    float_t mix;
    if (a > b) {
        mix = a + std::log1p(std::exp(b - a));
    } else {
        mix = b + std::log1p(std::exp(a - b));
    }
    const float_t mixed_log_prob = std::max(lm_log_prob, mix);
    return lm_log_prob - mixed_log_prob;
}

std::uint64_t UserSentence::WordKey(TokenID token, std::string_view reading) {
    if (reading.empty()) {
        return token;
    }
    // FNV-1a; the low bit keeps the high half nonzero, apart from the
    // reading-less key. Readings only need to differ within one token.
    std::uint32_t h = 2166136261U;
    for (char c : reading) {
        h = (h ^ static_cast<unsigned char>(c)) * 16777619U;
    }
    return (static_cast<std::uint64_t>(h | 1U) << 32U) | token;
}

std::uint64_t UserSentence::BigramKey(std::uint64_t prev, std::uint64_t word) {
    // splitmix64 finalizer over both halves.
    std::uint64_t x = word ^ (prev * 0x9E3779B97F4A7C15ULL);
    x = (x ^ (x >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27U)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31U);
}

UserSentence::Selection UserSentence::CleanSelection(const Sentence& tokens,
                                                     const Readings& readings) {
    const bool aligned = readings.size() == tokens.size();
    Selection clean;
    clean.tokens.reserve(tokens.size());
    clean.readings.reserve(tokens.size());
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i] == NotToken) continue;
        clean.tokens.push_back(tokens[i]);
        clean.readings.push_back(aligned ? readings[i] : std::string{});
    }
    return clean;
}

bool UserSentence::TailsAgree(const Entry& entry, const Sentence& context,
                             const Readings& context_readings) {
    const auto ctx = CleanSelection(context, context_readings);
    if (ctx.tokens.empty()) {
        return false;
    }
    std::size_t k = ctx.tokens.size();
    for (auto sel = entry.selections.rbegin();
         sel != entry.selections.rend() && k > 0; ++sel) {
        for (std::size_t i = sel->tokens.size(); i > 0 && k > 0; --i, --k) {
            if (sel->tokens[i - 1] != ctx.tokens[k - 1]) {
                return false;
            }
            const auto& a = sel->readings[i - 1];
            const auto& b = ctx.readings[k - 1];
            if (!a.empty() && !b.empty() && a != b) {
                return false;
            }
        }
    }
    return true;
}

void UserSentence::CountBigram(TokenID prev, std::string_view prev_reading,
                               std::uint64_t word, int delta) {
    const auto d = static_cast<std::uint32_t>(delta > 0 ? delta : -delta);
    const auto prev_word = WordKey(prev, prev_reading);
    const auto cur = static_cast<TokenID>(word & 0xFFFFFFFFU);
    if (delta > 0) {
        successor_tokens_[prev][cur] += d;
        IncBigramTrackSucc(bigram_, distinct_successors_, prev,
                           BigramKey(prev, word), d);
        IncBigramTrackSucc(word_bigram_, word_successors_, prev_word,
                           BigramKey(prev_word, word), d);
    } else {
        if (auto it = successor_tokens_.find(prev);
            it != successor_tokens_.end()) {
            DecCount(it->second, cur, d);
            if (it->second.empty()) successor_tokens_.erase(it);
        }
        DecBigramTrackSucc(bigram_, distinct_successors_, prev,
                           BigramKey(prev, word), d);
        DecBigramTrackSucc(word_bigram_, word_successors_, prev_word,
                           BigramKey(prev_word, word), d);
    }
}

void UserSentence::StartParagraph(Selection selection) {
    if (max_sentence_count_ == 0) {
        return;
    }
    Entry entry;
    entry.selections.push_back(std::move(selection));
    AdjustEntryCounts(entry, +1);
    sentences_.push_front(std::move(entry));
    while (sentences_.size() > max_sentence_count_) {
        AdjustEntryCounts(sentences_.back(), -1);
        sentences_.pop_back();
    }
}

void UserSentence::AppendToLatest(Selection selection) {
    if (sentences_.empty()) {
        StartParagraph(std::move(selection));
        return;
    }
    auto& latest = sentences_.front();

    // Incremental count update: tail of existing flattened tokens is
    // the "previous" for the first token of the new selection. After
    // that, internal bigrams of the new selection contribute too.
    TokenID prev_tail = NotToken;
    std::string prev_tail_reading;
    if (!latest.selections.empty()) {
        const auto& last_sel = latest.selections.back();
        if (!last_sel.tokens.empty()) {
            prev_tail = last_sel.tokens.back();
            prev_tail_reading = last_sel.readings.back();
        }
    }
    for (std::size_t i = 0; i < selection.tokens.size(); ++i) {
        const auto token = selection.tokens[i];
        const auto word = WordKey(token, selection.readings[i]);
        IncCount(token_count_, token, 1);
        IncCount(word_count_, word, 1);
        ++total_unigram_count_;
        const TokenID prev = i == 0 ? prev_tail : selection.tokens[i - 1];
        if (prev != NotToken) {
            CountBigram(prev, i == 0 ? prev_tail_reading
                                     : selection.readings[i - 1],
                        word, 1);
        }
    }

    latest.selections.push_back(std::move(selection));
}

void UserSentence::AdjustEntryCounts(const Entry& entry, int delta) {
    if (delta == 0) {
        return;
    }
    const auto d = static_cast<std::uint32_t>(delta > 0 ? delta : -delta);
    TokenID prev = NotToken;
    std::string_view prev_reading;
    for (const auto& sel : entry.selections) {
        for (std::size_t i = 0; i < sel.tokens.size(); ++i) {
            const auto token = sel.tokens[i];
            const auto word = WordKey(token, sel.readings[i]);
            if (delta > 0) {
                IncCount(token_count_, token, d);
                IncCount(word_count_, word, d);
                total_unigram_count_ += d;
            } else {
                DecCount(token_count_, token, d);
                DecCount(word_count_, word, d);
                total_unigram_count_ =
                    total_unigram_count_ >= d ? total_unigram_count_ - d : 0;
            }
            if (prev != NotToken) {
                CountBigram(prev, prev_reading, word, delta);
            }
            prev = token;
            prev_reading = sel.readings[i];
        }
    }
}

void UserSentence::Rebuild() {
    token_count_.clear();
    word_count_.clear();
    bigram_.clear();
    distinct_successors_.clear();
    word_bigram_.clear();
    word_successors_.clear();
    successor_tokens_.clear();
    total_unigram_count_ = 0;
    for (const auto& entry : sentences_) {
        AdjustEntryCounts(entry, +1);
    }
}

} // namespace sime
