#include "gru_reranker.h"

#ifdef SIME_ENABLE_NCNN
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#endif
#include <mat.h>
#include <net.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#endif

#include <cstdint>
#include <cstring>
#include <string>

#ifdef SIME_ENABLE_NCNN
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace sime {

namespace {

#ifdef SIME_ENABLE_NCNN
constexpr std::uint32_t kEmbeddingVersion = 1;
constexpr std::uint32_t kEmbeddingDimension = 32;

std::uint32_t ReadU32LE(const unsigned char* bytes) {
    return static_cast<std::uint32_t>(bytes[0])
        | (static_cast<std::uint32_t>(bytes[1]) << 8)
        | (static_cast<std::uint32_t>(bytes[2]) << 16)
        | (static_cast<std::uint32_t>(bytes[3]) << 24);
}
#endif

} // namespace

class GruReranker::Impl {
public:
#ifdef SIME_ENABLE_NCNN
    bool Load(const std::filesystem::path& directory) {
        // mmap the quantized embedding read-only rather than reading it into a
        // heap vector: 8.5 MB of dirty, non-evictable memory becomes clean
        // pageable memory, which matters under the keyboard extension's tight
        // per-process limit. The GRU only touches a handful of rows per
        // rerank, so demand paging keeps the resident set small.
        if (!MapEmbedding(directory / "gru.embedding.i8")) {
            Clear();
            return false;
        }

        pinyin_.opt.num_threads = 1;
        t9_.opt.num_threads = 1;
        if (pinyin_.load_param((directory / "gru.pinyin.ncnn.param").c_str()) != 0
            || pinyin_.load_model((directory / "gru.pinyin.ncnn.bin").c_str()) != 0
            || t9_.load_param((directory / "gru.t9.ncnn.param").c_str()) != 0
            || t9_.load_model((directory / "gru.t9.ncnn.bin").c_str()) != 0) {
            Clear();
            return false;
        }
        ready_ = true;
        return true;
    }

    bool MapEmbedding(const std::filesystem::path& path) {
        const int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) return false;
        struct stat st{};
        if (::fstat(fd, &st) != 0 || st.st_size <= 0) {
            ::close(fd);
            return false;
        }
        const std::size_t size = static_cast<std::size_t>(st.st_size);
        void* addr = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        ::close(fd);
        if (addr == MAP_FAILED) return false;
        // Rerank reads scattered rows, so suppress read-ahead that would
        // otherwise fault in (and keep resident) the whole table.
        ::madvise(addr, size, MADV_RANDOM);

        const auto* base = static_cast<const unsigned char*>(addr);
        constexpr std::size_t kHeaderBytes = 16;  // "STI8" + version + rows + dim
        if (size < kHeaderBytes || std::memcmp(base, "STI8", 4) != 0) {
            ::munmap(addr, size);
            return false;
        }
        const std::uint32_t version = ReadU32LE(base + 4);
        const std::uint32_t rows = ReadU32LE(base + 8);
        const std::uint32_t dimension = ReadU32LE(base + 12);
        if (version != kEmbeddingVersion || rows < 2
            || dimension != kEmbeddingDimension) {
            ::munmap(addr, size);
            return false;
        }
        const std::size_t scales_bytes =
            static_cast<std::size_t>(rows) * sizeof(std::uint16_t);
        const std::size_t values_bytes =
            static_cast<std::size_t>(rows) * dimension;
        if (size < kHeaderBytes + scales_bytes + values_bytes) {
            ::munmap(addr, size);
            return false;
        }
        embed_map_ = addr;
        embed_len_ = size;
        rows_ = rows;
        dimension_ = dimension;
        // base + kHeaderBytes is 2-byte aligned (mmap is page aligned), so the
        // uint16 scale table can be addressed directly.
        scales_ = reinterpret_cast<const std::uint16_t*>(base + kHeaderBytes);
        values_ = reinterpret_cast<const std::int8_t*>(
            base + kHeaderBytes + scales_bytes);
        return true;
    }

    float_t Score(const std::vector<TokenID>& tokens, bool t9) const {
        if (!ready_ || tokens.empty()) return 0.0;

        ncnn::Mat embedded(static_cast<int>(dimension_),
                           static_cast<int>(tokens.size()));
        for (std::size_t row = 0; row < tokens.size(); ++row) {
            const auto index = 1U + static_cast<std::uint32_t>(tokens[row])
                % (rows_ - 1U);
            const auto scale = ncnn::float16_to_float32(scales_[index]);
            const auto* source = values_
                + static_cast<std::size_t>(index) * dimension_;
            auto* destination = embedded.row(static_cast<int>(row));
            for (std::uint32_t column = 0; column < dimension_; ++column) {
                destination[column] = static_cast<float>(source[column]) * scale;
            }
        }

        auto extractor = (t9 ? t9_ : pinyin_).create_extractor();
        if (extractor.input("in0", embedded) != 0) return 0.0;
        ncnn::Mat output;
        if (extractor.extract("out0", output) != 0 || output.empty()) return 0.0;
        return output[0];
    }

    void Clear() {
        ready_ = false;
        rows_ = 0;
        dimension_ = 0;
        scales_ = nullptr;
        values_ = nullptr;
        if (embed_map_ && embed_map_ != MAP_FAILED) {
            ::munmap(embed_map_, embed_len_);
        }
        embed_map_ = nullptr;
        embed_len_ = 0;
        pinyin_.clear();
        t9_.clear();
    }

    ncnn::Net pinyin_;
    ncnn::Net t9_;
    void* embed_map_ = nullptr;
    std::size_t embed_len_ = 0;
    const std::uint16_t* scales_ = nullptr;
    const std::int8_t* values_ = nullptr;
    std::uint32_t rows_ = 0;
    std::uint32_t dimension_ = 0;
    bool ready_ = false;
#else
    bool Load(const std::filesystem::path&) { return false; }
    float_t Score(const std::vector<TokenID>&, bool) const { return 0.0; }
#endif
};

GruReranker::GruReranker() : impl_(std::make_unique<Impl>()) {}
GruReranker::~GruReranker() = default;

bool GruReranker::Load(const std::filesystem::path& directory) {
    return impl_->Load(directory);
}

bool GruReranker::Ready() const {
#ifdef SIME_ENABLE_NCNN
    return impl_->ready_;
#else
    return false;
#endif
}

float_t GruReranker::Score(const std::vector<TokenID>& tokens, bool t9) const {
    return impl_->Score(tokens, t9);
}

} // namespace sime
