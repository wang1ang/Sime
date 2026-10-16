#include "cut.h"

#include "state.h"
#include "ustr.h"

#include <algorithm>
#include <cstdint>
#include <unordered_map>

namespace sime {

namespace {

constexpr std::size_t BeamSize = 60;
constexpr std::size_t PrefixMax = 256;

// Number of bytes in the UTF-8 codepoint starting with `lead`.
// Returns 1 for invalid leads so the loop always makes progress.
std::size_t Utf8CharBytes(unsigned char lead) {
    if (lead < 0x80) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 1;
}

struct CutEdge {
    std::size_t end;  // byte end position
    TokenID id;
};

// Where no token covers a lone character, add an UNK edge so Viterbi can
// still advance; Scorer backs off to UnknownPenalty.
void AddUnkEdges(std::string_view input, std::vector<std::vector<CutEdge>>& edges) {
    const std::size_t N = input.size();
    for (std::size_t i = 0; i < N;) {
        std::size_t char_bytes = Utf8CharBytes(static_cast<unsigned char>(input[i]));
        if (i + char_bytes > N) char_bytes = N - i;
        const bool covered = std::any_of(edges[i].begin(), edges[i].end(),
            [&](const CutEdge& e) { return e.end == i + char_bytes; });
        if (!covered) edges[i].push_back({i + char_bytes, CutUnkToken});
        i += char_bytes;
    }
}

std::vector<CutToken> SearchLattice(const Scorer& scorer, std::string_view input,
                                    const std::vector<std::vector<CutEdge>>& edges) {
    std::vector<CutToken> out;
    const std::size_t N = input.size();
    // Positions between char boundaries have no edges and no incoming
    // states, so their NetStates stay empty.
    std::vector<NetStates> net(N + 1);
    for (auto& col : net) col.SetMaxTop(BeamSize);
    State init(0.0, 0, scorer.StartPos(), nullptr, 0);
    net[0].Insert(init);

    for (std::size_t col = 0; col < N; ++col) {
        if (edges[col].empty()) continue;
        for (auto it = net[col].begin(); it != net[col].end(); ++it) {
            const auto& value = *it;
            for (const auto& e : edges[col]) {
                Scorer::Pos cur_pos = value.pos;
                Scorer::Pos next_pos{};
                float_t step = scorer.ScoreMove(cur_pos, e.id, next_pos);
                scorer.Back(next_pos);
                State next(value.score + step, e.end, next_pos,
                           &value, e.id);
                net[e.end].Insert(next);
            }
        }
    }

    auto tail = net[N].GetStates();
    if (tail.empty()) return out;

    // Backtrace the best tail state.
    const State* s = &tail[0];
    while (s != nullptr && s->backtrace_state != nullptr) {
        const State* prev = s->backtrace_state;
        std::size_t start = prev->now;
        std::size_t end = s->now;

        CutToken tok;
        tok.text = std::string(input.substr(start, end - start));
        if (s->backtrace_token == CutUnkToken) {
            tok.id = 0;
            tok.is_unk = true;
        } else {
            tok.id = s->backtrace_token;
        }
        out.push_back(std::move(tok));
        s = prev;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

} // namespace

Cutter::Cutter(const Dict& dict, const Scorer& scorer)
    : dict_(dict), scorer_(scorer) {
    // Gather (utf8_text, id) pairs for every dict token.
    std::vector<std::pair<std::string, TokenID>> pairs;
    pairs.reserve(dict_.TokenCount());
    for (std::uint32_t id = StartToken; id < dict_.TokenCount(); ++id) {
        const char32_t* chars = dict_.TokenAt(id);
        if (!chars || chars[0] == 0) continue;
        std::u32string u32;
        for (std::size_t i = 0; chars[i] != 0; ++i) u32.push_back(chars[i]);
        std::string text = ustr::FromU32(u32);
        if (text.empty()) continue;
        pairs.emplace_back(std::move(text), id);
    }

    // Build demands sorted, unique keys.
    std::sort(pairs.begin(), pairs.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    std::vector<std::string> keys;
    std::vector<std::uint32_t> values;
    keys.reserve(pairs.size());
    values.reserve(pairs.size());
    for (std::size_t i = 0; i < pairs.size(); ++i) {
        if (i > 0 && pairs[i].first == pairs[i - 1].first) continue;
        keys.push_back(pairs[i].first);
        values.push_back(pairs[i].second);
    }
    dat_.Build(keys, values);
}

std::vector<CutToken> Cutter::Cut(std::string_view input) const {
    if (input.empty() || dat_.Empty()) return {};
    const std::size_t N = input.size();
    std::vector<std::vector<CutEdge>> edges(N);
    // Walk UTF-8 codepoint boundaries; at each boundary do one prefix lookup.
    for (std::size_t i = 0; i < N;) {
        std::size_t char_bytes = Utf8CharBytes(
            static_cast<unsigned char>(input[i]));
        if (i + char_bytes > N) char_bytes = N - i;
        for (const auto& m : dat_.PrefixSearch(input.substr(i), PrefixMax)) {
            edges[i].push_back({i + m.length, static_cast<TokenID>(m.value)});
        }
        i += char_bytes;
    }
    AddUnkEdges(input, edges);
    return SearchLattice(scorer_, input, edges);
}

std::vector<CutToken> CutByScan(const Dict& dict, const Scorer& scorer,
                                std::string_view input) {
    if (input.empty()) return {};
    const std::size_t N = input.size();
    // Codepoints of the input with their byte offsets (plus the end).
    std::u32string chars;
    std::vector<std::size_t> offsets;
    for (std::size_t i = 0; i < N;) {
        std::size_t char_bytes = Utf8CharBytes(static_cast<unsigned char>(input[i]));
        if (i + char_bytes > N) char_bytes = N - i;
        const std::u32string one = ustr::ToU32(input.substr(i, char_bytes));
        // An undecodable byte matches no token.
        chars.push_back(one.size() == 1 ? one[0] : static_cast<char32_t>(-1));
        offsets.push_back(i);
        i += char_bytes;
    }
    offsets.push_back(N);

    std::unordered_map<char32_t, std::vector<std::size_t>> starts;
    for (std::size_t k = 0; k < chars.size(); ++k) starts[chars[k]].push_back(k);

    std::vector<std::vector<CutEdge>> edges(N);
    for (std::uint32_t id = StartToken; id < dict.TokenCount(); ++id) {
        const char32_t* token = dict.TokenAt(id);
        if (!token || token[0] == 0) continue;
        const auto found = starts.find(token[0]);
        if (found == starts.end()) continue;
        for (const std::size_t k : found->second) {
            std::size_t len = 0;
            while (token[len] != 0 && k + len < chars.size() &&
                   token[len] == chars[k + len]) {
                ++len;
            }
            if (token[len] != 0) continue;
            const std::size_t from = offsets[k];
            const std::size_t to = offsets[k + len];
            // Same text under several ids: keep the first, as ContextToken.
            auto& out = edges[from];
            if (std::none_of(out.begin(), out.end(),
                             [&](const CutEdge& e) { return e.end == to; })) {
                out.push_back({to, id});
            }
        }
    }
    AddUnkEdges(input, edges);
    return SearchLattice(scorer, input, edges);
}

} // namespace sime
