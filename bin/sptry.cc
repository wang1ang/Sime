// Interactive probe for a shuangpin index built by sime-spbuild. Type a
// shuangpin code (e.g. "nihk" for 你好, "vsgo" for 中国); prints exact matches
// then prefix completions.
//
// The index holds only shuangpin key -> value; the words live in the source
// sime.dict, so both are loaded and the value is resolved against sime.dict
// exactly as the runtime would. Lookup only — no LM ranking, so candidate order
// is the index's own, not frequency-sorted.
//
// Usage: sime-sptry <sime.dict> <sp.index>

#include "dict.h"
#include "trie.h"
#include "ustr.h"

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

using sime::Dict;

std::string Word(const Dict& d, uint32_t id) {
    const char32_t* w = d.TokenAt(id);
    return w ? sime::ustr::FromU32(std::u32string(w)) : std::string("?");
}

// Resolve one index value against the source dict and print its words.
void ShowValue(const Dict& d, uint32_t value, int& shown, int limit) {
    Dict::Entry e = d.GetEntry(Dict::LetterPinyin, value);
    for (uint32_t i = 0; i < e.count && shown < limit; ++i) {
        std::cout << "  " << Word(d, e.items[i].id) << "  ["
                  << (e.items[i].pieces ? e.items[i].pieces : "") << "]\n";
        ++shown;
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: sime-sptry <sime.dict> <sp.index>\n";
        return 1;
    }
    Dict dict;
    if (!dict.Load(argv[1])) {
        std::cerr << "sptry: failed to load " << argv[1] << "\n";
        return 1;
    }
    std::ifstream in(argv[2], std::ios::binary);
    if (!in.is_open()) {
        std::cerr << "sptry: failed to open " << argv[2] << "\n";
        return 1;
    }
    std::vector<char> blob((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    trie::DoubleArray dat;
    if (!dat.Deserialize(blob.data(), blob.size())) {
        std::cerr << "sptry: failed to parse index " << argv[2] << "\n";
        return 1;
    }
    std::cout << "loaded. type shuangpin code, empty line to quit.\n";

    std::string line;
    while (std::cout << "> " && std::getline(std::cin, line)) {
        if (line.empty()) break;
        int shown = 0;
        uint32_t exact = 0;
        if (dat.Get(line, exact)) {
            std::cout << "[exact]\n";
            ShowValue(dict, exact, shown, 12);
        }
        auto comps = dat.FindWordsWithPrefix(line, 24);
        if (!comps.empty()) {
            std::cout << "[prefix completions]\n";
            for (const auto& r : comps) {
                if (shown >= 24) break;
                ShowValue(dict, r.value, shown, 24);
            }
        }
        if (shown == 0) std::cout << "  (no match)\n";
    }
    return 0;
}
