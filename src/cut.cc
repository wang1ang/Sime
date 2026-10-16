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

// Byte offset of every character start, then input.size().
std::vector<std::size_t> CharOffsets(std::string_view input) {
    std::vector<std::size_t> offsets;
    for (std::size_t i = 0; i < input.size();) {
        offsets.push_back(i);
        i += std::min(Utf8CharBytes(static_cast<unsigned char>(input[i])),
                      input.size() - i);
    }
    offsets.push_back(input.size());
    return offsets;
}

struct CutEdge {
    std::size_t end;  // byte end position
    TokenID id;
};

using CutEdges = std::vector<std::vector<CutEdge>>;

bool HasEdgeTo(const std::vector<CutEdge>& out, std::size_t end) {
    return std::any_of(out.begin(), out.end(),
                       [&](const CutEdge& e) { return e.end == end; });
}

// Where no token covers a lone character, add an UNK edge so Viterbi can
// still advance; Scorer backs off to UnknownPenalty.
void AddUnkEdges(const std::vector<std::size_t>& offsets, CutEdges& edges) {
    for (std::size_t k = 0; k + 1 < offsets.size(); ++k) {
        if (!HasEdgeTo(edges[offsets[k]], offsets[k + 1])) {
            edges[offsets[k]].push_back({offsets[k + 1], CutUnkToken});
        }
    }
}

std::vector<CutToken> SearchLattice(const Scorer& scorer, std::string_view input,
                                    const CutEdges& edges) {
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
    const auto offsets = CharOffsets(input);
    CutEdges edges(input.size());
    // One prefix lookup at each character boundary.
    for (std::size_t k = 0; k + 1 < offsets.size(); ++k) {
        const std::size_t i = offsets[k];
        for (const auto& m : dat_.PrefixSearch(input.substr(i), PrefixMax)) {
            edges[i].push_back({i + m.length, static_cast<TokenID>(m.value)});
        }
    }
    AddUnkEdges(offsets, edges);
    return SearchLattice(scorer_, input, edges);
}

std::vector<CutToken> CutByScan(const Dict& dict, const Scorer& scorer,
                                std::string_view input) {
    if (input.empty()) return {};
    const auto offsets = CharOffsets(input);
    std::u32string chars;
    for (std::size_t k = 0; k + 1 < offsets.size(); ++k) {
        const std::u32string one =
            ustr::ToU32(input.substr(offsets[k], offsets[k + 1] - offsets[k]));
        // An undecodable byte matches no token.
        chars.push_back(one.size() == 1 ? one[0] : static_cast<char32_t>(-1));
    }

    std::unordered_map<char32_t, std::vector<std::size_t>> starts;
    for (std::size_t k = 0; k < chars.size(); ++k) starts[chars[k]].push_back(k);

    CutEdges edges(input.size());
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
            // Same text under several ids: keep the first, as ContextToken.
            auto& out = edges[offsets[k]];
            if (!HasEdgeTo(out, offsets[k + len])) out.push_back({offsets[k + len], id});
        }
    }
    AddUnkEdges(offsets, edges);
    return SearchLattice(scorer, input, edges);
}

} // namespace sime
