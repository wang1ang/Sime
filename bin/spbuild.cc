// Offline builder for a shuangpin index.
//
// Re-keys sime.dict's LetterPinyin trie from full-pinyin ("ni'hao") to shuangpin
// keystrokes ("nihk") via a syllable->code map, emitting a bare DoubleArray of
// shuangpin key -> the SAME value the source trie stored. It holds only the
// index; the value resolves against the already-loaded sime.dict at runtime, so
// no words/side-table/token-table are duplicated. Scheme knowledge lives only in
// the map file, keeping the engine scheme-agnostic.
//
// Usage: sime-spbuild <in.dict> <map.txt> <out.sp.index>
//   map.txt: lines of "<full-pinyin-syllable> <shuangpin-code>", '#' comments.

#include "dict.h"
#include "trie.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using sime::Dict;

std::unordered_map<std::string, std::string> LoadMap(const std::string& path) {
    std::unordered_map<std::string, std::string> m;
    std::ifstream in(path);
    if (!in.is_open()) {
        std::cerr << "spbuild: cannot open map " << path << "\n";
        std::exit(1);
    }
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line[0] == '#') continue;
        std::istringstream ss(line);
        std::string syl, code;
        if (!(ss >> syl >> code)) continue;
        m[syl] = code;
    }
    return m;
}

// Recode one full-pinyin key ("ni'hao") into a shuangpin key ("nihk").
// Returns false and names the offending syllable if the map lacks it.
bool RecodeKey(const std::string& key,
               const std::unordered_map<std::string, std::string>& map,
               std::string& out, std::string& missing) {
    out.clear();
    std::size_t pos = 0;
    while (pos <= key.size()) {
        std::size_t next = key.find('\'', pos);
        std::string syl = key.substr(
            pos, next == std::string::npos ? std::string::npos : next - pos);
        if (!syl.empty()) {
            auto it = map.find(syl);
            if (it == map.end()) { missing = syl; return false; }
            out += it->second;
        }
        if (next == std::string::npos) break;
        pos = next + 1;
    }
    return true;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc != 4) {
        std::cerr << "Usage: sime-spbuild <in.dict> <map.txt> <out.sp.index>\n";
        return 1;
    }
    const std::string in_path = argv[1];
    const std::string map_path = argv[2];
    const std::string out_path = argv[3];

    Dict dict;
    if (!dict.Load(in_path)) {
        std::cerr << "spbuild: failed to load " << in_path << "\n";
        return 1;
    }
    auto map = LoadMap(map_path);

    // Enumerate the full-pinyin index; recode each key, keep its source value.
    // std::map keeps keys sorted, which DoubleArray::Build requires.
    std::map<std::string, uint32_t> sp;
    std::size_t skipped = 0;
    std::size_t collisions = 0;
    std::vector<std::string> missing_syls;

    // Full-pinyin keys that can't be typed in this shuangpin scheme: "lo"/"lue"
    // expand to luo/lve, so these shadowed spellings never reach the engine.
    // Dropping them keeps each shuangpin key bound to one value (no collision).
    static const std::set<std::string> kDrop = {"lo", "qin'lue"};

    dict.Dat(Dict::LetterPinyin).ForEachKey(
        [&](const std::string& key, uint32_t value) {
            if (kDrop.count(key)) return;
            std::string spkey, missing;
            if (!RecodeKey(key, map, spkey, missing)) {
                // Interjection-type syllables (ng/hm) have no shuangpin form.
                ++skipped;
                missing_syls.push_back(missing);
                return;
            }
            auto [it, inserted] = sp.emplace(spkey, value);
            // Two full-pinyin keys mapping to one shuangpin key but different
            // values can't both be reached through a single value. kDrop covers
            // the known cases; a leftover collision means a scheme/dict bug.
            if (!inserted && it->second != value) ++collisions;
        });

    if (!missing_syls.empty()) {
        std::sort(missing_syls.begin(), missing_syls.end());
        missing_syls.erase(std::unique(missing_syls.begin(), missing_syls.end()),
                           missing_syls.end());
        std::cerr << "spbuild: skipped " << skipped
                  << " entries with " << missing_syls.size()
                  << " un-mappable syllable(s):";
        for (const auto& s : missing_syls) std::cerr << " " << s;
        std::cerr << "\n";
    }
    if (collisions) {
        std::cerr << "spbuild: WARNING " << collisions
                  << " shuangpin key(s) collide with differing values\n";
    }

    // Build the bare shuangpin DAT: key -> source value, verbatim.
    std::vector<std::string> keys;
    std::vector<uint32_t> values;
    keys.reserve(sp.size());
    values.reserve(sp.size());
    for (const auto& [k, v] : sp) { keys.push_back(k); values.push_back(v); }

    trie::DoubleArray sp_dat;
    sp_dat.Build(keys, values);

    std::vector<char> buf;
    sp_dat.Serialize(buf);

    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        std::cerr << "spbuild: cannot write " << out_path << "\n";
        return 1;
    }
    out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
    if (!out.good()) {
        std::cerr << "spbuild: write failed\n";
        return 1;
    }

    std::cerr << "spbuild: wrote " << out_path << " ("
              << keys.size() << " shuangpin keys, "
              << skipped << " skipped)\n";
    return 0;
}
