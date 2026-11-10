#include "media/video/Perception.hpp"
// The search's costs with SSE2 where the target has it (x86-64 always), unless MEDIA_PERCEPTION_SCALAR asks for the scalar
// loop everywhere (the portable suite builds both). Integer arithmetic: the two give the same costs, bit for bit.
#if !defined(MEDIA_PERCEPTION_SCALAR) && (defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2))
#define MEDIA_PERCEPTION_SSE2 1
#include <immintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#define MEDIA_PERCEPTION_AVX2_TARGET
#else
#define MEDIA_PERCEPTION_AVX2_TARGET __attribute__((target("avx2")))
#endif
#endif
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <initializer_list>

#ifdef MEDIA_PERCEPTION_TRACE
// tools/flow_trace.cpp (2026-11-05, docs/benchmarks.md "Why the coarse level fails"): every main tile's search, stage by stage.
#include <cstdio>
std::FILE* g_perception_trace = nullptr;
namespace { struct CoarseWhy { int reason; long long best, costs, on; int bi, bj; } g_why{}; }
#define MEDIA_PT(...) do { if (g_perception_trace) std::fprintf(g_perception_trace, __VA_ARGS__); } while (0)
#define MEDIA_WHY(...) (g_why = CoarseWhy{__VA_ARGS__})
#else
#define MEDIA_PT(...) ((void)0)
#define MEDIA_WHY(...) ((void)0)
#endif
namespace media::video {
namespace {
// The search's costs: 0 the scalar loop, 1 SSE2, 2 AVX2 (Perception::ForceLevel; the best the processor has otherwise).
unsigned Available() noexcept {
#if MEDIA_PERCEPTION_SSE2
#if defined(_MSC_VER)
    int info[4]{}; __cpuid(info, 0);
    if (info[0] < 7) return 1;
    __cpuid(info, 1);
    const bool os = (info[2] & (1 << 27)) != 0 && (info[2] & (1 << 28)) != 0 && (_xgetbv(0) & 6) == 6; // OSXSAVE, AVX, the YMM state saved.
    __cpuidex(info, 7, 0);
    return os && (info[1] & (1 << 5)) ? 2u : 1u;
#else
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2") ? 2u : 1u;
#endif
#else
    return 0;
#endif
}
unsigned g_level = Available();
bool g_scalar = false; // Saliency's and the cells' loops follow ForceLevel(0) too.
constexpr unsigned kTileCount = Perception::kTiles * Perception::kTiles;
constexpr int kSide = 2 * Perception::kSearch + 1;
constexpr float kPi = 3.14159265358979323846f;
// A result with nothing in it, copied over result_ (GCC 13 refuses `result_ = {}` now that it holds the object table, and stops
// with an internal error on `result_ = PerceptionResult{}`).
const PerceptionResult kNoResult{};
float Clamp01(float v) noexcept { return v < 0 ? 0.0f : v > 1 ? 1.0f : v; }
// The minimum of a V through three absolute-difference costs (the middle one the least), -0.5..0.5: SAD rises linearly
// from its minimum, and a parabola through it is biased toward whole cells.
float Vertex(std::int64_t minus, std::int64_t centre, std::int64_t plus) noexcept {
    const auto d = 2 * ((std::max)(minus, plus) - centre);
    return d > 0 ? std::clamp(static_cast<float>(minus - plus) / static_cast<float>(d), -0.5f, 0.5f) : 0.0f;
}
}

bool Perception::Configure(core::LinearMemoryArena& arena, unsigned width, unsigned height, float fov) noexcept {
    history_ = nullptr;
    if (width < kMinSize || height < kMinSize || width > 8192 || height > 8192) return false;
    history_ = static_cast<std::uint16_t*>(arena.Allocate(std::size_t{kHistory} * kCells * kCells * 2, 64));
    coarse_ = nullptr; coarse_sums_ = nullptr;
    if (pyramid_) { // The coarse level's grids, only when it is asked for (off by default).
        coarse_ = static_cast<std::uint16_t*>(arena.Allocate(std::size_t{kHistory} * kCoarse * kCoarse * 2, 64));
        coarse_sums_ = static_cast<std::int32_t*>(arena.Allocate(std::size_t{kHistory} * (kCoarse + 1) * (kCoarse + 1) * 4, 64));
        if (!coarse_ || !coarse_sums_) { history_ = nullptr; return false; }
    }
    sums_ = static_cast<std::uint32_t*>(arena.Allocate(std::size_t{kCells} * kCells * 4, 64));
    column_ = static_cast<std::uint8_t*>(arena.Allocate(width, 64));
    if (!history_ || !sums_ || !column_) { history_ = nullptr; return false; }
    width_ = width; height_ = height;
    for (unsigned i = 0; i <= kCells; ++i) {
        column_bounds_[i] = static_cast<std::uint32_t>(std::uint64_t{i} * width / kCells);
        row_bounds_[i] = static_cast<std::uint32_t>(std::uint64_t{i} * height / kCells);
    }
    for (unsigned c = 0; c < kCells; ++c) for (unsigned x = column_bounds_[c]; x < column_bounds_[c + 1]; ++x) column_[x] = static_cast<std::uint8_t>(c);
    // A cell is cell_rows_ or one more lines tall, cell_columns_ or one more samples wide: four sample counts n. Each one's
    // reciprocal for MakeCells (Granlund and Montgomery): with l = ceil(log2 n) and s = 25 + l, m = ceil(2^s / n) gives
    // floor(x / n) = (x m) >> s for every x under 2^25 (16 x 255 x 4096 + 2048, a cell's largest numerator, is under it).
    cell_rows_ = height / kCells; cell_columns_ = width / kCells;
    for (unsigned t = 0; t < 2; ++t)
        for (unsigned w = 0; w < 2; ++w) {
            const std::uint64_t n = std::uint64_t{cell_rows_ + t} * (cell_columns_ + w);
            unsigned l = 0; while ((std::uint64_t{1} << l) < n) ++l;
            divide_[t][w] = {((std::uint64_t{1} << (25 + l)) + n - 1) / n, static_cast<std::uint32_t>(n / 2), 25 + l};
        }
    if (!(fov > 0.05f && fov < 3.1f)) fov = kPi / 2; // 90 degrees when not given.
    focal_ = static_cast<float>(width) * 0.5f / std::tan(fov * 0.5f);
    next_track_ = 1;
    Reset();
    result_.sequence = 0;
    return true;
}
void Perception::Reset() noexcept {
    count_ = 0; newest_ = 0;
    const auto sequence = result_.sequence;
    result_ = kNoResult; result_.sequence = sequence;
    std::memset(valid_, 0, sizeof(valid_)); std::memset(saliency_, 0, sizeof(saliency_));
    std::memset(flow_x_, 0, sizeof(flow_x_)); std::memset(flow_y_, 0, sizeof(flow_y_));
    std::memset(fine_valid_, 0, sizeof(fine_valid_)); ground_ = held_ = {}; candidate_count_ = 0; object_ = {};
    for (auto& t : tracks_) t = {}; // A cut: new objects (their ids keep counting).
}
void Perception::MakeCells(const std::uint8_t* luma, unsigned stride, std::uint16_t* out) noexcept {
    // Per cell row: its lines added into 16-bit column sums (at most 64 lines of 255: 16,320), 16 columns a step with SSE2, then
    // each cell's columns added up. The same sums as adding sample by sample into each cell (as before 2026-10-28): the same
    // cells. The column sums live in sums_ (64 KiB; at most 8192 columns of 2 bytes).
    auto* columns = reinterpret_cast<std::uint16_t*>(sums_);
    for (unsigned r = 0; r < kCells; ++r) {
        std::memset(columns, 0, std::size_t{width_} * 2);
        for (unsigned y = row_bounds_[r]; y < row_bounds_[r + 1]; ++y) {
            const std::uint8_t* line = luma + static_cast<std::size_t>(y) * stride;
            unsigned x = 0;
#if MEDIA_PERCEPTION_SSE2
            if (!g_scalar) {
                const __m128i zero = _mm_setzero_si128();
                for (; x + 16 <= width_; x += 16) {
                    const __m128i bytes = _mm_loadu_si128(reinterpret_cast<const __m128i*>(line + x));
                    auto* at = reinterpret_cast<__m128i*>(columns + x);
                    _mm_storeu_si128(at, _mm_add_epi16(_mm_loadu_si128(at), _mm_unpacklo_epi8(bytes, zero)));
                    _mm_storeu_si128(at + 1, _mm_add_epi16(_mm_loadu_si128(at + 1), _mm_unpackhi_epi8(bytes, zero)));
                }
            }
#endif
            for (; x < width_; ++x) columns[x] = static_cast<std::uint16_t>(columns[x] + line[x]);
        }
        const unsigned tall = row_bounds_[r + 1] - row_bounds_[r] > cell_rows_ ? 1u : 0u;
        for (unsigned c = 0; c < kCells; ++c) {
            std::uint32_t sum = 0;
            for (unsigned x = column_bounds_[c]; x < column_bounds_[c + 1]; ++x) sum += columns[x];
            // (16 sum + n / 2) / n, rounded, by the cell's reciprocal (Configure): exact for numerators under 2^25.
            const auto& d = divide_[tall][column_bounds_[c + 1] - column_bounds_[c] > cell_columns_ ? 1 : 0];
            out[r * kCells + c] = static_cast<std::uint16_t>(((std::uint64_t{sum} * 16 + d.half) * d.magic) >> d.shift);
        }
    }
}
// The cost of a position of a tile's search: the zero-mean SAD with the mean difference rounded to a whole cell unit
// (2026-10-28), sum |(a - b) - m| with m = round((sum a - sum b) / 64), sum a given. Every term fits 16 bits (a cell is at most
// 16 x 255 = 4080; the differences and m at most that each way): 1/64 of the exact zero-mean SAD's scale (sum |64 (a - b) -
// (sum a - sum b)|), within a cell unit a term. Three ways that compute the same integers: the scalar loop (the reference),
// SSE2 (a row of 8 cells a register) and AVX2 (two rows a register, where the processor and the system have it); their costs
// are equal bit for bit. Each evaluates a list of positions (the block's first cells in `then`, on the grid) at once.
namespace {
constexpr int kTile = static_cast<int>(Perception::kTileCells), kGrid = static_cast<int>(Perception::kCells);
void CostsScalar(const std::uint16_t* now, const std::uint16_t* then, int bx, int by, std::int32_t sum, const int* rx, const int* ry, unsigned n, std::int64_t* cost) noexcept {
    for (unsigned k = 0; k < n; ++k) {
        std::int32_t rsum = 0;
        for (int y = 0; y < kTile; ++y) for (int x = 0; x < kTile; ++x) rsum += then[(ry[k] + y) * kGrid + rx[k] + x];
        const std::int32_t mean = (sum - rsum + 32) >> 6;
        std::int64_t total = 0;
        for (int y = 0; y < kTile; ++y)
            for (int x = 0; x < kTile; ++x) {
                const std::int32_t e = now[(by + y) * kGrid + bx + x] - then[(ry[k] + y) * kGrid + rx[k] + x] - mean;
                total += e < 0 ? -e : e;
            }
        cost[k] = total;
    }
}
#if MEDIA_PERCEPTION_SSE2
void CostsSse2(const std::uint16_t* now, const std::uint16_t* then, int bx, int by, std::int32_t sum, const int* rx, const int* ry, unsigned n, std::int64_t* cost) noexcept {
    __m128i rows[kTile];
    for (int y = 0; y < kTile; ++y) rows[y] = _mm_loadu_si128(reinterpret_cast<const __m128i*>(now + (by + y) * kGrid + bx));
    const __m128i ones = _mm_set1_epi16(1);
    for (unsigned k = 0; k < n; ++k) {
        __m128i other[kTile], sums = _mm_setzero_si128();
        for (int y = 0; y < kTile; ++y) {
            other[y] = _mm_loadu_si128(reinterpret_cast<const __m128i*>(then + (ry[k] + y) * kGrid + rx[k]));
            sums = _mm_add_epi32(sums, _mm_madd_epi16(other[y], ones));
        }
        sums = _mm_add_epi32(sums, _mm_shuffle_epi32(sums, _MM_SHUFFLE(1, 0, 3, 2)));
        sums = _mm_add_epi32(sums, _mm_shuffle_epi32(sums, _MM_SHUFFLE(2, 3, 0, 1)));
        const __m128i mean = _mm_set1_epi16(static_cast<short>((sum - _mm_cvtsi128_si32(sums) + 32) >> 6)); // m.
        __m128i acc = _mm_setzero_si128();
        for (int y = 0; y < kTile; ++y) {
            const __m128i e = _mm_sub_epi16(_mm_sub_epi16(rows[y], other[y]), mean);
            acc = _mm_add_epi32(acc, _mm_madd_epi16(_mm_max_epi16(e, _mm_sub_epi16(_mm_setzero_si128(), e)), ones)); // |e|, paired.
        }
        acc = _mm_add_epi32(acc, _mm_shuffle_epi32(acc, _MM_SHUFFLE(1, 0, 3, 2)));
        acc = _mm_add_epi32(acc, _mm_shuffle_epi32(acc, _MM_SHUFFLE(2, 3, 0, 1)));
        cost[k] = static_cast<std::int64_t>(_mm_cvtsi128_si32(acc));
    }
}
// Plain loops (no lambda: GCC gives a lambda in a target("avx2") function none of its target).
MEDIA_PERCEPTION_AVX2_TARGET
void CostsAvx2(const std::uint16_t* now, const std::uint16_t* then, int bx, int by, std::int32_t sum, const int* rx, const int* ry, unsigned n, std::int64_t* cost) noexcept {
    // Rows y and y + 1 in one register: four registers a block.
    __m256i rows[kTile / 2];
    for (int y = 0; y < kTile / 2; ++y) {
        const std::uint16_t* first = now + (by + 2 * y) * kGrid + bx;
        rows[y] = _mm256_inserti128_si256(_mm256_castsi128_si256(_mm_loadu_si128(reinterpret_cast<const __m128i*>(first))),
                                          _mm_loadu_si128(reinterpret_cast<const __m128i*>(first + kGrid)), 1);
    }
    const __m256i ones = _mm256_set1_epi16(1);
    for (unsigned k = 0; k < n; ++k) {
        __m256i other[kTile / 2], sums = _mm256_setzero_si256();
        for (int y = 0; y < kTile / 2; ++y) {
            const std::uint16_t* first = then + (ry[k] + 2 * y) * kGrid + rx[k];
            other[y] = _mm256_inserti128_si256(_mm256_castsi128_si256(_mm_loadu_si128(reinterpret_cast<const __m128i*>(first))),
                                               _mm_loadu_si128(reinterpret_cast<const __m128i*>(first + kGrid)), 1);
            sums = _mm256_add_epi32(sums, _mm256_madd_epi16(other[y], ones));
        }
        __m128i s = _mm_add_epi32(_mm256_castsi256_si128(sums), _mm256_extracti128_si256(sums, 1));
        s = _mm_add_epi32(s, _mm_shuffle_epi32(s, _MM_SHUFFLE(1, 0, 3, 2)));
        s = _mm_add_epi32(s, _mm_shuffle_epi32(s, _MM_SHUFFLE(2, 3, 0, 1)));
        const __m256i mean = _mm256_set1_epi16(static_cast<short>((sum - _mm_cvtsi128_si32(s) + 32) >> 6)); // m.
        __m256i acc = _mm256_setzero_si256();
        for (int y = 0; y < kTile / 2; ++y)
            acc = _mm256_add_epi32(acc, _mm256_madd_epi16(_mm256_abs_epi16(_mm256_sub_epi16(_mm256_sub_epi16(rows[y], other[y]), mean)), ones));
        __m128i t = _mm_add_epi32(_mm256_castsi256_si128(acc), _mm256_extracti128_si256(acc, 1));
        t = _mm_add_epi32(t, _mm_shuffle_epi32(t, _MM_SHUFFLE(1, 0, 3, 2)));
        t = _mm_add_epi32(t, _mm_shuffle_epi32(t, _MM_SHUFFLE(2, 3, 0, 1)));
        cost[k] = _mm_cvtsi128_si32(t);
    }
    _mm256_zeroupper(); // Back to SSE code without the transition penalty.
}
#endif
}
// The coarse level's cost at one position (2026-11-05): sum |b - t - m| over the 4 x 4 support, b the tile's coarse cells, t the
// reference's at (x, y), m the rounded mean difference. Every term fits 16 bits (cells at most 4080, |b - t - m| at most 12,240).
// The scalar loop is the reference; SSE2 (two rows a register) and AVX2 (the four rows in one) give the same integers.
namespace {
constexpr int kCoarseGrid = static_cast<int>(Perception::kCoarse), kSupport = static_cast<int>(Perception::kCoarseSupport);
std::int32_t CoarseSadScalar(const std::int16_t* block, const std::uint16_t* then, int x, int y, std::int32_t mean) noexcept {
    std::int32_t sad = 0;
    for (int v = 0; v < kSupport; ++v)
        for (int u = 0; u < kSupport; ++u) { const auto d = block[v * kSupport + u] - static_cast<std::int32_t>(then[(y + v) * kCoarseGrid + x + u]) - mean; sad += d < 0 ? -d : d; }
    return sad;
}
#if MEDIA_PERCEPTION_SSE2
std::int32_t CoarseSadSse2(const std::int16_t* block, const std::uint16_t* then, int x, int y, std::int32_t mean) noexcept {
    const std::uint16_t* at = then + y * kCoarseGrid + x;
    const __m128i m = _mm_set1_epi16(static_cast<short>(mean)), ones = _mm_set1_epi16(1), zero = _mm_setzero_si128();
    const __m128i t01 = _mm_unpacklo_epi64(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(at)), _mm_loadl_epi64(reinterpret_cast<const __m128i*>(at + kCoarseGrid)));
    const __m128i t23 = _mm_unpacklo_epi64(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(at + 2 * kCoarseGrid)), _mm_loadl_epi64(reinterpret_cast<const __m128i*>(at + 3 * kCoarseGrid)));
    __m128i d0 = _mm_sub_epi16(_mm_sub_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(block)), t01), m);
    __m128i d1 = _mm_sub_epi16(_mm_sub_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(block + 8)), t23), m);
    d0 = _mm_max_epi16(d0, _mm_sub_epi16(zero, d0)); d1 = _mm_max_epi16(d1, _mm_sub_epi16(zero, d1));
    __m128i s = _mm_add_epi32(_mm_madd_epi16(d0, ones), _mm_madd_epi16(d1, ones));
    s = _mm_add_epi32(s, _mm_shuffle_epi32(s, _MM_SHUFFLE(1, 0, 3, 2)));
    s = _mm_add_epi32(s, _mm_shuffle_epi32(s, _MM_SHUFFLE(2, 3, 0, 1)));
    return _mm_cvtsi128_si32(s);
}
MEDIA_PERCEPTION_AVX2_TARGET std::int32_t CoarseSadAvx2(const std::int16_t* block, const std::uint16_t* then, int x, int y, std::int32_t mean) noexcept {
    const std::uint16_t* at = then + y * kCoarseGrid + x;
    const __m128i t01 = _mm_unpacklo_epi64(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(at)), _mm_loadl_epi64(reinterpret_cast<const __m128i*>(at + kCoarseGrid)));
    const __m128i t23 = _mm_unpacklo_epi64(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(at + 2 * kCoarseGrid)), _mm_loadl_epi64(reinterpret_cast<const __m128i*>(at + 3 * kCoarseGrid)));
    const __m256i t = _mm256_inserti128_si256(_mm256_castsi128_si256(t01), t23, 1);
    const __m256i d = _mm256_abs_epi16(_mm256_sub_epi16(_mm256_sub_epi16(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(block)), t), _mm256_set1_epi16(static_cast<short>(mean))));
    const __m256i s8 = _mm256_madd_epi16(d, _mm256_set1_epi16(1));
    __m128i s = _mm_add_epi32(_mm256_castsi256_si128(s8), _mm256_extracti128_si256(s8, 1));
    s = _mm_add_epi32(s, _mm_shuffle_epi32(s, _MM_SHUFFLE(1, 0, 3, 2)));
    s = _mm_add_epi32(s, _mm_shuffle_epi32(s, _MM_SHUFFLE(2, 3, 0, 1)));
    const std::int32_t out = _mm_cvtsi128_si32(s);
    _mm256_zeroupper();
    return out;
}
#endif
using CoarseSad = std::int32_t (*)(const std::int16_t*, const std::uint16_t*, int, int, std::int32_t) noexcept;
CoarseSad CoarseLevel() noexcept {
#if MEDIA_PERCEPTION_SSE2
    if (g_level >= 2) return CoarseSadAvx2;
    if (g_level == 1) return CoarseSadSse2;
#endif
    return CoarseSadScalar;
}
}
void Perception::Costs(const std::uint16_t* now, const std::uint16_t* then, int bx, int by, std::int32_t sum, const int* rx, const int* ry, unsigned n, std::int64_t* cost) noexcept {
    if (!n) return;
#if MEDIA_PERCEPTION_SSE2
    if (g_level >= 2) { CostsAvx2(now, then, bx, by, sum, rx, ry, n, cost); return; }
    if (g_level == 1) { CostsSse2(now, then, bx, by, sum, rx, ry, n, cost); return; }
#endif
    CostsScalar(now, then, bx, by, sum, rx, ry, n, cost);
}
bool Perception::CoarseMatch(const std::uint16_t* now, const unsigned age, unsigned bx, unsigned by, int cx, int cy, float& dx,
                             float& dy) const noexcept {
    // The tile's support on the coarse grid: its own coarse cells and a ring of context, moved inside the grid at its edges.
    constexpr int n = static_cast<int>(kCoarseSupport), grid = static_cast<int>(kCoarse), ratio = static_cast<int>(kCoarseRatio);
    constexpr int side = 2 * kCoarseSearch + 1;
    const int tile = static_cast<int>(kTileCells) / ratio; // Coarse cells a tile spans (2).
    const int x0 = std::clamp(static_cast<int>(bx) / ratio - (n - tile) / 2, 0, grid - n);
    const int y0 = std::clamp(static_cast<int>(by) / ratio - (n - tile) / 2, 0, grid - n);
    const std::uint16_t* then = CoarseGrid(age); const std::int32_t* table = CoarseSums(age);
    constexpr int w = grid + 1;
    alignas(32) std::int16_t block[kCoarseSupport * kCoarseSupport]; std::int32_t sum = 0;
    for (int j = 0; j < n; ++j) for (int i = 0; i < n; ++i) { block[j * n + i] = static_cast<std::int16_t>(now[(y0 + j) * grid + x0 + i]); sum += block[j * n + i]; }
    std::int64_t deviation = 0;
    for (const auto v : block) { const auto d = v * n * n - sum; deviation += d < 0 ? -d : d; }
    const CoarseSad sad_at = CoarseLevel();
    if (deviation < static_cast<std::int64_t>(kMinTexture * 16.0f * n * n * n * n)) { MEDIA_WHY(1, 0, 0, 0, 0, 0); return false; }
    // The window (its centre where the rotation moved the tile, in coarse cells), zero-mean SAD as the fine search: every second
    // position (corners and edges included: 36), whose mean says whether the best is a match, then the 3 x 3 around the cheapest.
    const int ccx = static_cast<int>(std::lround(static_cast<double>(cx) / ratio)), ccy = static_cast<int>(std::lround(static_cast<double>(cy) / ratio));
    std::int64_t cost[side * side]; bool known[side * side]{};
    const auto evaluate = [&](int i, int j) {
        const int index = j * side + i;
        if (known[index]) return cost[index];
        known[index] = true;
        const int x = x0 - (ccx + i - kCoarseSearch), y = y0 - (ccy + j - kCoarseSearch);
        if (x < 0 || y < 0 || x + n > grid || y + n > grid) return cost[index] = INT64_MAX;
        const std::int32_t other = table[(y + n) * w + x + n] - table[y * w + x + n] - table[(y + n) * w + x] + table[y * w + x];
        const std::int32_t mean = (sum - other + n * n / 2) >> 4; // n * n = 16.
        return cost[index] = sad_at(block, then, x, y, mean);
    };
    std::int64_t costs = 0, on = 0; int bi = -1, bj = -1;
    for (int j = 0; j < side; j += 2)
        for (int i = 0; i < side; i += 2) {
            const auto c = evaluate(i, j);
            if (c == INT64_MAX) continue;
            costs += c; ++on;
            if (bi < 0 || c < cost[bj * side + bi]) { bi = i; bj = j; }
        }
    if (bi < 0 || on < 9) { MEDIA_WHY(2, 0, 0, on, bi, bj); return false; }
    for (int moved = 1; moved;) { // To the cheapest of the 3 x 3 around, until the best is its centre (a local minimum).
        moved = 0; const int ci = bi, cj = bj;
        for (int j = (std::max)(cj - 1, 0); j <= (std::min)(cj + 1, side - 1); ++j)
            for (int i = (std::max)(ci - 1, 0); i <= (std::min)(ci + 1, side - 1); ++i)
                if (evaluate(i, j) < cost[bj * side + bi]) { bi = i; bj = j; moved = 1; }
    }
    const auto best = cost[bj * side + bi];
    // A match, not the least bad of many; inside the window (a motion beyond it is not known); the cost rising both ways.
    if (2 * best * on > costs) { MEDIA_WHY(3, best, costs, on, bi, bj); return false; }
    if (bi == 0 || bj == 0 || bi == side - 1 || bj == side - 1) { MEDIA_WHY(4, best, costs, on, bi, bj); return false; }
    const auto l = cost[bj * side + bi - 1], r = cost[bj * side + bi + 1], u = cost[(bj - 1) * side + bi], d = cost[(bj + 1) * side + bi];
    if (l == INT64_MAX || r == INT64_MAX || u == INT64_MAX || d == INT64_MAX || l <= best || r <= best || u <= best || d <= best) { MEDIA_WHY(5, best, costs, on, bi, bj); return false; }
    MEDIA_WHY(0, best, costs, on, bi, bj);
    dx = static_cast<float>(ratio) * (static_cast<float>(ccx + bi - kCoarseSearch) + Vertex(l, best, r));
    dy = static_cast<float>(ratio) * (static_cast<float>(ccy + bj - kCoarseSearch) + Vertex(u, best, d));
    return true;
}
bool Perception::MatchTile(const std::uint16_t* now, const std::uint16_t* then, unsigned first_x, unsigned first_y, int cx, int cy, const int* seeds, unsigned seed_count,
                           float& dx, float& dy, bool& edge, std::int64_t* best_cost, bool* settled) const noexcept {
    edge = false;
    if (settled) *settled = false;
    const int bx = static_cast<int>(first_x), by = static_cast<int>(first_y);
    constexpr int n = static_cast<int>(kTileCells);
    std::int32_t block[kTileCells * kTileCells]; std::int32_t sum = 0;
    for (int j = 0; j < n; ++j) for (int i = 0; i < n; ++i) { block[j * n + i] = now[(by + j) * static_cast<int>(kCells) + bx + i]; sum += block[j * n + i]; }
    // Texture: the mean absolute deviation, in cell units (16 per code), times 64.
    std::int64_t deviation = 0;
    for (auto& v : block) { v = v * n * n - sum; deviation += v < 0 ? -v : v; }
    if (deviation < static_cast<std::int64_t>(kMinTexture * 16.0f * n * n * n * n)) return false;
    // The search (2026-10-29): positions (i, j) of the window, displacement (cx + i - kSearch, cy + j - kSearch); each one's
    // cost computed once, when it is first asked for (INT64_MAX: off the grid).
    std::int64_t cost[kSide * kSide]; bool known[kSide * kSide]{};
    const auto evaluate = [&](const int* is, const int* js, unsigned count) {
        int rx[kSide * kSide], ry[kSide * kSide]; unsigned at[kSide * kSide]; unsigned m = 0;
        for (unsigned k = 0; k < count; ++k) {
            const unsigned index = static_cast<unsigned>(js[k] * kSide + is[k]);
            if (known[index]) continue;
            known[index] = true;
            // now(x) = then(x - d): the block came from (b - d).
            const int x = bx - (cx + is[k] - kSearch), y = by - (cy + js[k] - kSearch);
            if (x < 0 || y < 0 || x + n > static_cast<int>(kCells) || y + n > static_cast<int>(kCells)) { cost[index] = INT64_MAX; continue; }
            rx[m] = x; ry[m] = y; at[m++] = index;
        }
        std::int64_t out[kSide * kSide];
        Costs(now, then, bx, by, sum, rx, ry, m, out);
        for (unsigned k = 0; k < m; ++k) cost[at[k]] = out[k];
    };
    const auto at = [&](int i, int j) { return cost[j * kSide + i]; };
    // From (i, j), the small diamond: the four neighbours, to the cheapest one cheaper than here (left, right, up, down first
    // on ties), until none is: a local minimum whose neighbours are all known.
    const auto descend = [&](int& i, int& j) {
        for (unsigned step = 0; step < 4 * kSide; ++step) {
            int is[4], js[4]; unsigned count = 0;
            if (i > 0) { is[count] = i - 1; js[count++] = j; }
            if (i + 1 < kSide) { is[count] = i + 1; js[count++] = j; }
            if (j > 0) { is[count] = i; js[count++] = j - 1; }
            if (j + 1 < kSide) { is[count] = i; js[count++] = j + 1; }
            evaluate(is, js, count);
            int bi = i, bj = j; std::int64_t lowest = at(i, j);
            for (unsigned k = 0; k < count; ++k) if (at(is[k], js[k]) < lowest) { lowest = at(is[k], js[k]); bi = is[k]; bj = js[k]; }
            if (bi == i && bj == j) return;
            i = bi; j = bj;
        }
    };
    // Seeds: where the camera's rotation alone moved the tile (the window's centre), then the caller's predictions (the tile's
    // own flow in the previous picture, its neighbours' in this one), clamped to the window. Descend from the cheapest.
    int si[2 + kMaxSeeds], sj[2 + kMaxSeeds]; unsigned seeds_n = 0;
    si[seeds_n] = kSearch; sj[seeds_n++] = kSearch;
    for (unsigned k = 0; k < seed_count && k < kMaxSeeds + 1; ++k) {
        si[seeds_n] = std::clamp(seeds[2 * k] - cx + kSearch, 0, kSide - 1); sj[seeds_n++] = std::clamp(seeds[2 * k + 1] - cy + kSearch, 0, kSide - 1);
    }
    evaluate(si, sj, seeds_n);
    int best_i = si[0], best_j = sj[0];
    for (unsigned k = 1; k < seeds_n; ++k) if (at(si[k], sj[k]) < at(best_i, best_j)) { best_i = si[k]; best_j = sj[k]; }
    if (at(best_i, best_j) == INT64_MAX) {
        // Every seed off the grid (the tile near the picture's edge, the camera turned far): the window's centre row and column.
        int is[2 * kSide], js[2 * kSide];
        for (int k = 0; k < kSide; ++k) { is[k] = k; js[k] = kSearch; is[kSide + k] = kSearch; js[kSide + k] = k; }
        evaluate(is, js, 2 * kSide);
        for (int k = 0; k < 2 * kSide; ++k) if (at(is[k], js[k]) < at(best_i, best_j)) { best_i = is[k]; best_j = js[k]; }
        if (at(best_i, best_j) == INT64_MAX) return false;
    }
    descend(best_i, best_j);
    std::int64_t best = at(best_i, best_j);
    // Early exit: the prediction matched within the noise floor (its cost under kNoiseFloor of the tile's own texture, a
    // fraction of what an unrelated position costs: about the texture or more), so no other match can be much better. Else the
    // whole window on a lattice (every kLattice cells, its corners and edges included): its mean says whether the best is a
    // match, not the least bad of many (content entering the picture has none), and its cheapest point starts a second
    // descent, for a motion no seed predicted.
    const bool floor_met = static_cast<double>(best) * 64.0 < static_cast<double>(kNoiseFloor) * static_cast<double>(deviation);
    bool strong = floor_met;
    if (!floor_met) {
        int is[kLatticeSide * kLatticeSide], js[kLatticeSide * kLatticeSide]; unsigned count = 0;
        for (int j = 0; j < kSide; j += kLattice) for (int i = 0; i < kSide; i += kLattice) { is[count] = i; js[count++] = j; }
        evaluate(is, js, count);
        std::int64_t costs = 0, on = 0; int li = is[0], lj = js[0];
        for (unsigned k = 0; k < count; ++k) {
            const auto c = at(is[k], js[k]);
            if (c == INT64_MAX) continue;
            costs += c; ++on;
            if (at(li, lj) == INT64_MAX || c < at(li, lj)) { li = is[k]; lj = js[k]; }
        }
        if (!on) return false;
        if (at(li, lj) < best) { descend(li, lj); if (at(li, lj) < best) { best_i = li; best_j = lj; best = at(li, lj); } }
        if (2 * best * on > costs) return false;
        strong = 4 * best * on <= costs; // A quarter of the window's mean or less: no other window to look in.
    }
    if (best_i == 0 || best_j == 0 || best_i == kSide - 1 || best_j == kSide - 1) { edge = true; return false; }
    const auto l = at(best_i - 1, best_j), r = at(best_i + 1, best_j), u = at(best_i, best_j - 1), d = at(best_i, best_j + 1);
    if (l == INT64_MAX || r == INT64_MAX || u == INT64_MAX || d == INT64_MAX) return false;
    // The cost must rise both ways (an edge alone moves along itself unseen), by at least half a code per cell.
    const auto kx = l + r - 2 * best, ky = u + d - 2 * best;
    const std::int64_t floor = static_cast<std::int64_t>(0.5f * 16.0f) * n * n;
    if ((std::min)(kx, ky) < floor || 5 * (std::min)(kx, ky) < (std::max)(kx, ky)) return false;
    dx = static_cast<float>(cx + best_i - kSearch) + Vertex(l, best, r);
    dy = static_cast<float>(cy + best_j - kSearch) + Vertex(u, best, d);
    if (best_cost) *best_cost = best;
    if (settled) *settled = strong;
    return true;
}
const PerceptionResult& Perception::Push(const std::uint8_t* luma, unsigned stride, std::int64_t pts, const EgoMotion& ego) noexcept {
    if (!history_ || !luma) return result_;
    const auto sequence = result_.sequence + 1;
    result_ = kNoResult; result_.sequence = sequence; result_.pts_100ns = pts;
    if (ego.valid) { yaw_ += ego.yaw; pitch_ += ego.pitch; roll_ += ego.roll; }
    newest_ = (newest_ + 1) % kHistory; count_ = (std::min)(count_ + 1, kHistory);
    auto* now = history_ + static_cast<std::size_t>(newest_) * kCells * kCells;
    MakeCells(luma, stride, now);
    if (coarse_) {
        // The coarse grid: each coarse cell the rounded mean of kCoarseRatio x kCoarseRatio cells (the same 16x-code units).
        auto* coarse = coarse_ + static_cast<std::size_t>(newest_) * kCoarse * kCoarse;
        for (unsigned j = 0; j < kCoarse; ++j)
            for (unsigned i = 0; i < kCoarse; ++i) {
                std::uint32_t sum = 0;
                for (unsigned y = 0; y < kCoarseRatio; ++y) {
                    const std::uint16_t* row = now + (j * kCoarseRatio + y) * kCells + i * kCoarseRatio;
                    for (unsigned x = 0; x < kCoarseRatio; ++x) sum += row[x];
                }
                coarse[j * kCoarse + i] = static_cast<std::uint16_t>((sum + kCoarseRatio * kCoarseRatio / 2) / (kCoarseRatio * kCoarseRatio));
            }
        auto* table = coarse_sums_ + static_cast<std::size_t>(newest_) * (kCoarse + 1) * (kCoarse + 1);
        constexpr unsigned w = kCoarse + 1;
        for (unsigned i = 0; i < w; ++i) table[i] = 0;
        for (unsigned j = 0; j < kCoarse; ++j) {
            std::int32_t row = 0; table[(j + 1) * w] = 0;
            for (unsigned i = 0; i < kCoarse; ++i) { row += coarse[j * kCoarse + i]; table[(j + 1) * w + i + 1] = table[j * w + i + 1] + row; }
        }
    }
    frames_[newest_] = {pts, yaw_, pitch_, roll_};
    std::memcpy(last_valid_, valid_, sizeof(valid_)); std::memcpy(last_x_, flow_x_, sizeof(flow_x_)); std::memcpy(last_y_, flow_y_, sizeof(flow_y_));
    std::memcpy(last_fine_valid_, fine_valid_, sizeof(fine_valid_)); std::memcpy(last_fine_x_, fine_x_, sizeof(fine_x_)); std::memcpy(last_fine_y_, fine_y_, sizeof(fine_y_));
    std::memset(valid_, 0, sizeof(valid_)); std::memset(flow_x_, 0, sizeof(flow_x_)); std::memset(flow_y_, 0, sizeof(flow_y_));
    std::memset(fine_valid_, 0, sizeof(fine_valid_)); ground_ = {};
    ego_yaw_ = ego_pitch_ = ego_roll_ = 0;
    if (count_ < 2 || frames_[(newest_ + kHistory - 1) % kHistory].pts >= pts) { Saliency(now, nullptr); object_ = {}; return result_; }
    // The reference: the newest picture at least kBaseline old, else the oldest held; the fovea takes the oldest held first.
    unsigned age = count_ - 1;
    for (unsigned a = 1; a < count_; ++a) if (pts - frames_[(newest_ + kHistory - a) % kHistory].pts >= kBaseline) { age = a; break; }
    const unsigned coarse_ages[]{age, 1u}, fine_ages[]{count_ - 1, age, 1u};
    unsigned reliable = 0;
    float xs[kTileCount], ys[kTileCount];
    for (unsigned ty = 0; ty < kTiles; ++ty)
        for (unsigned tx = 0; tx < kTiles; ++tx) {
            const unsigned t = ty * kTiles + tx;
            const auto predicted = Predict(t, kTiles, last_valid_, last_x_, last_y_, valid_, flow_x_, flow_y_);
            if (!TileFlow(now, tx * kTileCells, ty * kTileCells, coarse_ages, 2, pts, predicted, flow_x_[t], flow_y_[t], flow_s_[t], flow_px_[t], flow_py_[t], flow_e_[t], true)) {
                flow_x_[t] = flow_y_[t] = 0; continue;
            }
            valid_[t] = 1;
            xs[reliable] = flow_x_[t]; ys[reliable] = flow_y_[t]; ++reliable;
        }
    for (unsigned j = 0; j < kFine; ++j)
        for (unsigned i = 0; i < kFine; ++i) {
            const unsigned t = j * kFine + i;
            const auto predicted = Predict(t, kFine, last_fine_valid_, last_fine_x_, last_fine_y_, fine_valid_, fine_x_, fine_y_);
            fine_valid_[t] = TileFlow(now, kFineOrigin + i * kFineStride, kFineOrigin + j * kFineStride, fine_ages, 3, pts, predicted, fine_x_[t], fine_y_[t], fine_s_[t], fine_px_[t],
                                      fine_py_[t], fine_e_[t]) ? 1 : 0;
            if (!fine_valid_[t]) fine_x_[t] = fine_y_[t] = 0;
        }
    result_.flow = true; result_.ego = ego.valid;
    if (ego.valid) {
        // Saliency's change is measured against where the camera's rotation alone moved each cell since the previous picture.
        const auto& previous = frames_[(newest_ + kHistory - 1) % kHistory];
        ego_yaw_ = static_cast<float>(yaw_ - previous.yaw); ego_pitch_ = static_cast<float>(pitch_ - previous.pitch); ego_roll_ = static_cast<float>(roll_ - previous.roll);
    }
    result_.coverage = static_cast<float>(reliable) / kTileCount;
    if (reliable) {
        std::nth_element(xs, xs + reliable / 2, xs + reliable); std::nth_element(ys, ys + reliable / 2, ys + reliable);
        result_.motion_x = xs[reliable / 2] / static_cast<float>(width_); result_.motion_y = ys[reliable / 2] / static_cast<float>(height_);
    }
    Saliency(now, Grid(1));
    Ground(pts, ego.valid ? ego.pitch : 0.0f);
    candidate_count_ = 0;
    Looming({kTiles, kTiles, flow_x_, flow_y_, flow_s_, flow_px_, flow_py_, flow_e_, valid_, false});
    Looming({kFine, kFine, fine_x_, fine_y_, fine_s_, fine_px_, fine_py_, fine_e_, fine_valid_, true});
    Approach(pts);
    Objects(pts);
    return result_;
}
Perception::Prediction Perception::Predict(unsigned t, unsigned columns, const std::uint8_t* last_valid, const float* last_x, const float* last_y, const std::uint8_t* valid,
                                           const float* vx, const float* vy) noexcept {
    Prediction p{}; // Velocities, not displacements: TileFlow scales them by each baseline it tries.
    const auto add = [&](float x, float y) { p.vx[p.count] = x; p.vy[p.count] = y; ++p.count; };
    if (last_valid[t]) add(last_x[t], last_y[t]);
    if (t % columns && valid[t - 1]) add(vx[t - 1], vy[t - 1]);
    if (t >= columns && valid[t - columns]) add(vx[t - columns], vy[t - columns]);
    return p;
}
bool Perception::TileFlow(const std::uint16_t* now, unsigned bx, unsigned by, const unsigned* ages, unsigned count, std::int64_t pts, const Prediction& predicted, float& vx,
                          float& vy, float& baseline, float& x, float& y, float& ego, const bool coarse) const noexcept {
    const float cell_w = static_cast<float>(width_) / kCells, cell_h = static_cast<float>(height_) / kCells;
    // The tile's centre about the picture's (pixels).
    const float rx = 0.5f * static_cast<float>(column_bounds_[bx] + column_bounds_[bx + kTileCells]) - 0.5f * static_cast<float>(width_);
    const float ry = 0.5f * static_cast<float>(row_bounds_[by] + row_bounds_[by + kTileCells]) - 0.5f * static_cast<float>(height_);
    // The coarse level's short baseline (2026-11-05, docs/benchmarks.md "Why the coarse level fails"): the long baseline (kBaseline,
    // three pictures at 24 fps) triples a motion, and 32+ px a frame lay beyond the coarse window on 76 % of Sintel's uniform fast
    // tiles. When it found nothing there, the picture before is searched, and its answer is kept only for a motion the long
    // baseline could not have reached (slow tiles keep their better-resolved long-baseline answer, or none).
    double reach_seconds = 0; // The long baseline that failed, when this baseline is the coarse level's retry.
    for (unsigned k = 0; k < count; ++k) {
        const unsigned a = ages[k];
        if (k && a == ages[k - 1]) continue;
        const auto& ref = frames_[(newest_ + kHistory - a) % kHistory];
        // Where the camera's rotation alone moved this tile since then (small angles).
        const auto yaw = static_cast<float>(yaw_ - ref.yaw), pitch = static_cast<float>(pitch_ - ref.pitch), roll = static_cast<float>(roll_ - ref.roll);
        const float px = -focal_ * yaw + roll * ry, py = focal_ * pitch - roll * rx;
        const int cx = static_cast<int>(std::lround(px / cell_w)), cy = static_cast<int>(std::lround(py / cell_h));
        const double seconds = static_cast<double>(pts - ref.pts) / 1e7;
        // The predictions over this baseline, with the rotation's shift (cells).
        int seeds[2 * (kMaxSeeds + 1)];
        const unsigned seed_count = predicted.count;
        for (unsigned s = 0; s < predicted.count; ++s) {
            seeds[2 * s] = static_cast<int>(std::lround((static_cast<double>(predicted.vx[s]) * seconds + px) / cell_w));
            seeds[2 * s + 1] = static_cast<int>(std::lround((static_cast<double>(predicted.vy[s]) * seconds + py) / cell_h));
        }
        // The coarse level (2026-11-05): when the window around the prediction holds no match within the noise floor, the tile's
        // motion on the coarse grids; beyond kCoarseRecentre cells of the prediction (a motion that window may not reach) the window
        // centred there is searched too, and its match kept when the first has none or one more than twice as costly.
        float dx = 0, dy = 0; bool edge = false, settled = false; std::int64_t cost = INT64_MAX;
        bool matched = MatchTile(now, Grid(a), bx, by, cx, cy, seeds, seed_count, dx, dy, edge, &cost, &settled);
        if (coarse) MEDIA_PT("A %u %u %u %.4f %d %d %d %.3f %.3f %d %lld %d", bx, by, a, seconds, cx, cy, matched ? 1 : 0, dx, dy, edge ? 1 : 0,
                             static_cast<long long>(cost == INT64_MAX ? -1 : cost), settled ? 1 : 0);
        float qx = 0, qy = 0;
        const bool tried = coarse && pyramid_ && coarse_ && !settled;
        const bool found = tried && CoarseMatch(CoarseGrid(0), a, bx, by, cx, cy, qx, qy);
        if (tried) MEDIA_PT(" C %d %.3f %.3f %d %lld %lld %lld %d %d", found ? 1 : 0, qx, qy, g_why.reason, g_why.best, g_why.costs, g_why.on, g_why.bi, g_why.bj);
        if (found) {
            const int wx = static_cast<int>(std::lround(qx)), wy = static_cast<int>(std::lround(qy));
            if (std::abs(wx - cx) > kCoarseRecentre || std::abs(wy - cy) > kCoarseRecentre) {
                seeds[2 * seed_count] = wx; seeds[2 * seed_count + 1] = wy;
                float fx = 0, fy = 0; bool far_edge = false; std::int64_t far_cost = INT64_MAX; // Its edge says nothing of the baseline.
                const bool far = MatchTile(now, Grid(a), bx, by, wx, wy, seeds, seed_count + 1, fx, fy, far_edge, &far_cost);
                const bool taken = far && (!matched || far_cost * 2 < cost) && std::fabs(fx - qx) <= kCoarseAgree && std::fabs(fy - qy) <= kCoarseAgree;
                MEDIA_PT(" F %d %.3f %.3f %lld %d", far ? 1 : 0, fx, fy, static_cast<long long>(far_cost == INT64_MAX ? -1 : far_cost), taken ? 1 : 0);
                if (taken) { matched = true; dx = fx; dy = fy; edge = false; }
            }
        }
        if (matched && reach_seconds > 0 && seconds > 0) {
            const double scale = reach_seconds / seconds; // The motion over the long baseline, in cells.
            if (std::fabs(dx - cx) * scale <= kSearch && std::fabs(dy - cy) * scale <= kSearch) matched = false;
        }
        if (coarse) MEDIA_PT(" R %d %.3f %.3f\n", matched ? 1 : 0, dx, dy);
        if (matched && seconds > 0) {
            vx = static_cast<float>((dx * cell_w - px) / seconds); vy = static_cast<float>((dy * cell_h - py) / seconds); baseline = static_cast<float>(seconds);
            ego = std::hypot(px, py);
            // Where the texture is: the cells' centres weighted by their gradient (central differences inside the tile), as
            // integer sums by row and column (twice the centres: exact).
            std::int64_t wsum = 0, sx = 0, sy = 0, columns[kTileCells]{};
            for (unsigned j = 0; j < kTileCells; ++j) {
                const std::uint16_t* row = now + (by + j) * kCells + bx;
                const std::uint16_t* above = now + (by + (j ? j - 1 : j)) * kCells + bx;
                const std::uint16_t* below = now + (by + (j + 1 < kTileCells ? j + 1 : j)) * kCells + bx;
                std::int64_t in_row = 0;
                for (unsigned i = 0; i < kTileCells; ++i) {
                    const int gx = row[i + 1 < kTileCells ? i + 1 : i] - row[i ? i - 1 : i], gy = below[i] - above[i];
                    const std::int64_t w = std::abs(gx) + std::abs(gy);
                    in_row += w; columns[i] += w;
                }
                wsum += in_row; sy += in_row * (row_bounds_[by + j] + row_bounds_[by + j + 1]);
            }
            for (unsigned i = 0; i < kTileCells; ++i) sx += columns[i] * (column_bounds_[bx + i] + column_bounds_[bx + i + 1]);
            x = wsum ? static_cast<float>(0.5 * static_cast<double>(sx) / static_cast<double>(wsum)) : rx + 0.5f * static_cast<float>(width_);
            y = wsum ? static_cast<float>(0.5 * static_cast<double>(sy) / static_cast<double>(wsum)) : ry + 0.5f * static_cast<float>(height_);
            return true;
        }
        if (matched || a == 1) break;
        if (!edge && !(tried && short_baseline_)) break; // Too fast for the long baseline (or, with the coarse level, nothing found): a shorter one.
        reach_seconds = edge ? 0 : seconds;
    }
    return false;
}
void Perception::Ground(std::int64_t pts, float pitch) noexcept {
    ground_ = {};
    // The camera's pitch moves the horizon with the scene (focal x pitch, as Push's model moves a tile): the held ground's too.
    const float shift = focal_ * pitch;
    if (held_.valid) held_.horizon += shift;
    // The ground's rows: the most rows from the bottom (kMaxGroundRows down to 3) that a ground explains, so the horizon may be
    // anywhere (a camera pitched up or down, a window cut from a larger picture); the rows below the held horizon (moved by the
    // pitch) are tried first.
    const double row = static_cast<double>(height_) / kTiles;
    bool found = false;
    if (held_.valid && pts > held_pts_ && pts - held_pts_ <= kGroundHold) {
        const auto first = static_cast<unsigned>(std::clamp(std::ceil(held_.horizon / row + 0.5), 0.0, static_cast<double>(kTiles - 3)));
        found = first + kMaxGroundRows >= kTiles && Fit(first);
    }
    for (unsigned rows = kMaxGroundRows; !found && rows >= 3; --rows) found = Fit(kTiles - rows);
    if (ground_.valid) { held_ = ground_; held_pts_ = pts; }
    else if (held_.valid && pts > held_pts_ && Confirm(held_)) { ground_ = held_; held_pts_ = pts; } // Too few rows to fit, the same ground.
    else if (held_.valid && pts > held_pts_ && pts - held_pts_ <= kGroundHold) ground_ = held_; // Steady travel: the rows are covered.
    else held_ = {};
    if (!ground_.valid) return;
    result_.ground = true;
    result_.horizon_y = Clamp01(ground_.horizon / static_cast<float>(height_)); result_.focus_x = Clamp01(ground_.focus / static_cast<float>(width_));
}
bool Perception::Confirm(GroundModel& model) const noexcept {
    // The last ground (its horizon moved by the pitch since) against the tiles below its horizon, however few rows they are: its
    // slope and focus kept, its two offsets (yaw and pitch rates the IMU did not take out) refitted, and kept when it explains
    // their flow as a fit must (residual under 0.75 of its own spread; 2026-10-29).
    constexpr unsigned kMax = kTiles * kTiles;
    float gu[kMax], gv[kMax], vx[kMax], vy[kMax]; unsigned n = 0, last_row = 0;
    float left = static_cast<float>(width_), right = 0;
    for (unsigned ty = 0; ty < kTiles; ++ty)
        for (unsigned tx = 0; tx < kTiles; ++tx) {
            const unsigned t = ty * kTiles + tx;
            if (!valid_[t] || flow_py_[t] <= model.horizon + 0.5f * static_cast<float>(height_) / kTiles) continue;
            const float s = flow_py_[t] - model.horizon;
            gu[n] = model.ku * s * (flow_px_[t] - model.focus); gv[n] = model.k * s * s; vx[n] = flow_x_[t]; vy[n] = flow_y_[t]; ++n;
            left = (std::min)(left, flow_px_[t]); right = (std::max)(right, flow_px_[t]); last_row = ty;
        }
    if (n < 6 || last_row + 3 < kTiles || right - left < 0.25f * static_cast<float>(width_)) return false;
    double ou = 0, ov = 0, mu = 0, mv = 0;
    for (unsigned i = 0; i < n; ++i) { ou += vx[i] - gu[i]; ov += vy[i] - gv[i]; mu += gu[i]; mv += gv[i]; }
    ou /= n; ov /= n; mu /= n; mv /= n;
    double residual = 0, spread = 0;
    for (unsigned i = 0; i < n; ++i) {
        const double eu = vx[i] - gu[i] - ou, ev = vy[i] - gv[i] - ov;
        residual += eu * eu + ev * ev; spread += (gu[i] - mu) * (gu[i] - mu) + (gv[i] - mv) * (gv[i] - mv);
    }
    if (!(spread > 0) || residual > 0.5625 * spread) return false; // 0.75^2.
    model.offset_u = static_cast<float>(ou); model.offset_v = static_cast<float>(ov);
    return true;
}
bool Perception::Fit(unsigned first) noexcept {
    // The coarse rows from `first` to the bottom as ground (see the header); ground_ and true when they are one.
    constexpr unsigned kMax = kTiles * kTiles;
    float px[kMax], py[kMax], vx[kMax], vy[kMax]; bool keep[kMax]; unsigned n = 0, rows = 0, first_row = kTiles, last_row = 0;
    float left = static_cast<float>(width_), right = 0;
    for (unsigned ty = first; ty < kTiles; ++ty) {
        unsigned in_row = 0;
        for (unsigned tx = 0; tx < kTiles; ++tx) {
            const unsigned t = ty * kTiles + tx;
            if (!valid_[t]) continue;
            px[n] = flow_px_[t]; py[n] = flow_py_[t]; vx[n] = flow_x_[t]; vy[n] = flow_y_[t]; keep[n] = true; ++n; ++in_row;
            left = (std::min)(left, flow_px_[t]); right = (std::max)(right, flow_px_[t]);
        }
        if (in_row) { ++rows; first_row = (std::min)(first_row, ty); last_row = ty; }
    }
    // The ground under a travelling camera reaches the bottom of the picture and spreads across it: tiles in the last three
    // rows and over 40 % of the width (2026-10-28: a still camera's obstacle over a flat background, its rows the only
    // textured ones, was fitted as a ground on 36 of 51 pictures; its fit even had a ground's shape).
    if (n < 12 || rows < 3 || last_row + 3 < kTiles || right - left < 0.4f * static_cast<float>(width_)) return false;
    const double top = 0.5 * (row_bounds_[first_row * kTileCells] + row_bounds_[(first_row + 1) * kTileCells]); // The first row's centre.
    const double scale = static_cast<double>(height_);
    double k = 0, ku = 0, horizon = 0, focus = 0, offset_u = 0, offset_v = 0, residual = 0, model = 0;
    for (unsigned pass = 0; pass < 2; ++pass) {
        // v = e t^2 + f t + g with t = y / height (conditioning), least squares.
        double s[5]{}, b[3]{}; unsigned m = 0;
        for (unsigned i = 0; i < n; ++i) {
            if (!keep[i]) continue;
            const double t = py[i] / scale, v = vy[i];
            s[0] += 1; s[1] += t; s[2] += t * t; s[3] += t * t * t; s[4] += t * t * t * t;
            b[0] += v; b[1] += v * t; b[2] += v * t * t; ++m;
        }
        if (m < 12) return false;
        // [s4 s3 s2; s3 s2 s1; s2 s1 s0] (e f g) = (b2 b1 b0), by Cramer's rule.
        const double a00 = s[4], a01 = s[3], a02 = s[2], a11 = s[2], a12 = s[1], a22 = s[0];
        const double det = a00 * (a11 * a22 - a12 * a12) - a01 * (a01 * a22 - a12 * a02) + a02 * (a01 * a12 - a11 * a02);
        if (!(std::fabs(det) > 1e-12)) return false;
        const double e = (b[2] * (a11 * a22 - a12 * a12) - a01 * (b[1] * a22 - a12 * b[0]) + a02 * (b[1] * a12 - a11 * b[0])) / det;
        const double f = (a00 * (b[1] * a22 - a12 * b[0]) - b[2] * (a01 * a22 - a12 * a02) + a02 * (a01 * b[0] - b[1] * a02)) / det;
        const double g = (a00 * (a11 * b[0] - b[1] * a12) - a01 * (a01 * b[0] - b[1] * a02) + b[2] * (a01 * a12 - a11 * a02)) / det;
        if (!(e > 0)) return false; // Not faster toward the bottom: no forward travel over a ground.
        k = e / (scale * scale); horizon = -f / (2 * e) * scale; offset_v = g - e * (horizon / scale) * (horizon / scale);
        // u = ku (y - yh)(x - x0) + r with its own ku: with s = y - yh, u = ku s x + c s + r (c = -ku x0), least squares. A
        // ground's ku is k (its spread across grows half as fast as its fall); an obstacle's expansion, isotropic, fitted by
        // this quadratic gives ku near 2k: the test below tells them apart.
        double m00 = 0, m01 = 0, m02 = 0, m11 = 0, m12 = 0, m22 = 0, r0 = 0, r1 = 0, r2 = 0;
        for (unsigned i = 0; i < n; ++i) {
            if (!keep[i]) continue;
            const double sl = (py[i] - horizon) / scale, z0 = sl * px[i] / scale, z1 = sl, u = vx[i];
            m00 += z0 * z0; m01 += z0 * z1; m02 += z0; m11 += z1 * z1; m12 += z1; m22 += 1; r0 += z0 * u; r1 += z1 * u; r2 += u;
        }
        const double d3 = m00 * (m11 * m22 - m12 * m12) - m01 * (m01 * m22 - m12 * m02) + m02 * (m01 * m12 - m11 * m02);
        if (!(std::fabs(d3) > 1e-12)) return false;
        const double q0 = (r0 * (m11 * m22 - m12 * m12) - m01 * (r1 * m22 - m12 * r2) + m02 * (r1 * m12 - m11 * r2)) / d3;
        const double q1 = (m00 * (r1 * m22 - m12 * r2) - r0 * (m01 * m22 - m12 * m02) + m02 * (m01 * r2 - r1 * m02)) / d3;
        const double q2 = (m00 * (m11 * r2 - r1 * m12) - m01 * (m01 * r2 - r1 * m02) + r0 * (m01 * m12 - m11 * m02)) / d3;
        ku = q0 / (scale * scale);
        if (!(std::fabs(ku) > 0)) return false;
        focus = -q1 / q0 * scale; offset_u = q2;
        float errors[kMax], sorted[kMax]; unsigned c = 0; residual = model = 0;
        double mu = 0, mv = 0, count = 0;
        for (unsigned i = 0; i < n; ++i) if (keep[i]) { mu += ku * (py[i] - horizon) * (px[i] - focus); mv += k * (py[i] - horizon) * (py[i] - horizon); count += 1; }
        mu /= count; mv /= count;
        for (unsigned i = 0; i < n; ++i) {
            const double gu = ku * (py[i] - horizon) * (px[i] - focus), gv = k * (py[i] - horizon) * (py[i] - horizon);
            errors[i] = static_cast<float>(std::hypot(vx[i] - gu - offset_u, vy[i] - gv - offset_v));
            if (keep[i]) { sorted[c++] = errors[i]; residual += static_cast<double>(errors[i]) * errors[i]; model += (gu - mu) * (gu - mu) + (gv - mv) * (gv - mv); }
        }
        residual = std::sqrt(residual / c); model = std::sqrt(model / c);
        if (pass) break;
        // Once more without the tiles that fit worst.
        std::nth_element(sorted, sorted + c / 2, sorted + c);
        const float limit = (std::max)(8.0f, 3.0f * sorted[c / 2]);
        bool dropped = false;
        for (unsigned i = 0; i < n; ++i) if (keep[i] && errors[i] > limit) { keep[i] = false; dropped = true; }
        if (!dropped) break;
    }
    // Forward travel over a ground: its flow grows toward the bottom from a horizon above these rows, its fall quickening down
    // them (v's slope goes as y - yh: at least 1.5x from the first row to the last; an obstacle's expansion, linear in y,
    // fitted this way puts the horizon far above and gives about 1x: 2026-10-28, a still camera's growing obstacle was taken
    // for ground on 36 of 51 pictures without it), with the ground's shape (ku about k, not an expansion's 2k), enough to be
    // travel, and the ground explains the rows' flow.
    const double last = 0.5 * (row_bounds_[(kTiles - 1) * kTileCells] + row_bounds_[kTiles * kTileCells]); // The last row's centre.
    const double bottom = k * (height_ - horizon) * (height_ - horizon), shape = ku / k;
    if (horizon >= top || last - horizon < 1.5 * (top - horizon) || shape < 0.5 || shape > 1.6 || bottom < kGroundFlow || residual > 0.75 * model) return false;
    ground_ = {true, static_cast<float>(k), static_cast<float>(horizon), static_cast<float>(focus), static_cast<float>(offset_u), static_cast<float>(offset_v),
               static_cast<float>(ku)};
    return true;
}
void Perception::Saliency(const std::uint16_t* now, const std::uint16_t* before) noexcept {
    const bool still = ego_yaw_ == 0 && ego_pitch_ == 0 && ego_roll_ == 0;
    float change[kTileCount], sorted[kTileCount];
    // Where each cell was before the camera turned (Push's model, per cell): the rotation moves a cell of row r across by
    // shift_x[r] cells and one of column c down by shift_y[c] (its centre about the picture's, in pixels, through the roll).
    const float cell_w = static_cast<float>(width_) / kCells, cell_h = static_cast<float>(height_) / kCells;
    float shift_x[kCells], shift_y[kCells];
    for (unsigned k = 0; k < kCells; ++k) {
        const float rx = (static_cast<float>(k) + 0.5f) * cell_w - 0.5f * static_cast<float>(width_);
        const float ry = (static_cast<float>(k) + 0.5f) * cell_h - 0.5f * static_cast<float>(height_);
        shift_x[k] = (-focal_ * ego_yaw_ + ego_roll_ * ry) / cell_w;
        shift_y[k] = (focal_ * ego_pitch_ - ego_roll_ * rx) / cell_h;
    }
    // Without roll every cell moved alike (2026-10-29): one whole-cell offset and one bilinear weight (in 256ths) for all, in
    // integers; with roll, each cell its own (below).
    const bool uniform = before && !still && ego_roll_ == 0;
    int ox = 0, oy = 0, wx = 0, wy = 0;
    if (uniform) {
        const float fx = -shift_x[0], fy = -shift_y[0];
        ox = static_cast<int>(std::floor(fx)); oy = static_cast<int>(std::floor(fy));
        wx = static_cast<int>((fx - static_cast<float>(ox)) * 256.0f + 0.5f); wy = static_cast<int>((fy - static_cast<float>(oy)) * 256.0f + 0.5f);
        if (wx == 256) { ++ox; wx = 0; }
        if (wy == 256) { ++oy; wy = 0; }
    }
    const int last = static_cast<int>(kCells) - 1;
    // Row by row of cells (2026-10-29; the same sums as tile by tile, in loops over contiguous cells): each cell's change and
    // edges, added to its tile's.
    std::uint32_t changes[kTileCount]{}, compared[kTileCount]{}, edges[kTileCount]{};
    std::uint32_t row_change[kCells], row_edges[kCells];
    for (unsigned r = 0; r < kCells; ++r) {
        const std::uint16_t* row = now + r * kCells;
        const std::uint16_t* down = r + 1 < kCells ? row + kCells : row;
        for (unsigned c = 0; c + 1 < kCells; ++c) row_edges[c] = static_cast<std::uint32_t>(std::abs(row[c + 1] - row[c]) + std::abs(down[c] - row[c]));
        row_edges[kCells - 1] = static_cast<std::uint32_t>(std::abs(down[kCells - 1] - row[kCells - 1]));
        // The cells compared, [from, to): those with something to compare with.
        unsigned from = 0, to = 0;
        if (before && still) { // No rotation: the registration lands on the cell itself (what the bilinear gives, exactly).
            const std::uint16_t* then = before + r * kCells;
            for (unsigned c = 0; c < kCells; ++c) row_change[c] = static_cast<std::uint32_t>(std::abs(row[c] - then[c]));
            to = kCells;
        } else if (uniform) {
            // Compared between cells (bilinear: a whole-cell shift left fine texture changing everywhere under shake); a cell
            // that came from outside the picture has nothing to compare with.
            const int y0 = static_cast<int>(r) + oy, dy = wy ? 1 : 0, dx = wx ? 1 : 0;
            if (y0 >= 0 && y0 + dy <= last) {
                const int lo = (std::max)(0, -ox), hi = (std::min)(last, last - ox - dx); // x0 = c + ox and x0 + dx within the grid.
                if (lo <= hi) {
                    const std::uint16_t* top = before + y0 * kCells;
                    const std::uint16_t* bottom = top + dy * kCells;
                    for (int c = lo; c <= hi; ++c) {
                        const int x0 = c + ox, x1 = x0 + dx; // A zero weight reads the cell itself.
                        const int above = top[x0] * (256 - wx) + top[x1] * wx, below = bottom[x0] * (256 - wx) + bottom[x1] * wx;
                        row_change[c] = static_cast<std::uint32_t>(std::abs(static_cast<int>(row[c]) - ((above * (256 - wy) + below * wy + 32768) >> 16)));
                    }
                    from = static_cast<unsigned>(lo); to = static_cast<unsigned>(hi) + 1;
                }
            }
        } else if (before) {
            // With roll, each cell its own registration; the cells compared need not be contiguous: the others count 0 and are
            // left out of `compared` below by the mask.
            from = kCells; to = 0;
            for (unsigned c = 0; c < kCells; ++c) {
                const float fx = static_cast<float>(c) - shift_x[r], fy = static_cast<float>(r) - shift_y[c];
                row_change[c] = UINT32_MAX;
                if (!(fx >= 0 && fy >= 0 && fx <= static_cast<float>(kCells - 1) && fy <= static_cast<float>(kCells - 1))) continue;
                const unsigned x0 = static_cast<unsigned>(fx), y0 = static_cast<unsigned>(fy);
                const unsigned x1 = (std::min)(x0 + 1, kCells - 1), y1 = (std::min)(y0 + 1, kCells - 1);
                const float ax = fx - static_cast<float>(x0), ay = fy - static_cast<float>(y0);
                const float above = before[y0 * kCells + x0] + ax * (before[y0 * kCells + x1] - before[y0 * kCells + x0]);
                const float below = before[y1 * kCells + x0] + ax * (before[y1 * kCells + x1] - before[y1 * kCells + x0]);
                // Rounded half up (not std::lround: a library call per cell).
                row_change[c] = static_cast<std::uint32_t>(std::fabs(static_cast<float>(row[c]) - (above + ay * (below - above))) + 0.5f);
                from = (std::min)(from, c); to = c + 1;
            }
        }
        const unsigned tiles = (r / kTileCells) * kTiles;
        for (unsigned tx = 0; tx < kTiles; ++tx) {
            const unsigned c0 = tx * kTileCells;
            std::uint32_t e = 0, d = 0, n = 0;
            for (unsigned c = c0; c < c0 + kTileCells; ++c) {
                e += row_edges[c];
                if (c >= from && c < to && row_change[c] != UINT32_MAX) { d += row_change[c]; ++n; }
            }
            edges[tiles + tx] += e; changes[tiles + tx] += d; compared[tiles + tx] += n;
        }
    }
    for (unsigned t = 0; t < kTileCount; ++t) {
        change[t] = sorted[t] = compared[t] * 2 >= kTileCells * kTileCells ? static_cast<float>(changes[t]) / static_cast<float>(compared[t]) : -1.0f; // -1: unknown.
        // Edge density, kept in the saliency array until the scores are made: 24 codes of gradient is a full score.
        saliency_[t] = static_cast<std::uint8_t>(std::lround(255.0f * Clamp01(static_cast<float>(edges[t]) / (2.0f * kTileCells * kTileCells) / (24.0f * 16.0f))));
    }
    std::nth_element(sorted, sorted + kTileCount / 2, sorted + kTileCount);
    const float floor = (std::max)(sorted[kTileCount / 2], 0.0f); // The picture's own change: sensor noise, or the camera moving.
    // The camera's own motion changes a tile in proportion to its texture (what is left of a moving camera after the IMU's
    // shift, a sub-cell registration, is too): each tile's floor is the picture's scaled by its edges against the median tile's.
    for (unsigned t = 0; t < kTileCount; ++t) sorted[t] = static_cast<float>(saliency_[t]);
    std::nth_element(sorted, sorted + kTileCount / 2, sorted + kTileCount);
    const float texture = (std::max)(sorted[kTileCount / 2], 1.0f);
    unsigned salient = 0;
    for (unsigned t = 0; t < kTileCount; ++t) {
        const float own = floor * std::clamp(static_cast<float>(saliency_[t]) / texture, 0.5f, 3.0f);
        // A change counts from 1.5x the tile's floor, on a scale that grows with it (6 codes over a still camera's ~0 floor): what a
        // moving camera leaves in texture is noise of that size. Mostly new to the picture: no change measured.
        const float temporal = change[t] < 0 ? 0.0f : Clamp01((change[t] - 1.5f * own) / (6.0f * 16.0f + own));
        const float speed = valid_[t] ? std::hypot(flow_x_[t] / static_cast<float>(width_), flow_y_[t] / static_cast<float>(height_)) : 0.0f;
        const float score = 0.55f * temporal + 0.30f * Clamp01(speed / 0.25f) + 0.15f * (static_cast<float>(saliency_[t]) / 255.0f);
        saliency_[t] = static_cast<std::uint8_t>(std::lround(255.0f * Clamp01(score)));
        if (Salient(t)) ++salient;
    }
    result_.salient = salient; result_.salient_fraction = static_cast<float>(salient) / kTileCount;
}
void Perception::Looming(const TileField& field) noexcept {
    const unsigned columns = field.columns, rows = field.rows;
    for (unsigned cy = 0; cy < rows; ++cy)
        for (unsigned cx = 0; cx < columns; ++cx) {
            const unsigned x0 = cx < 2 ? 0 : cx - 2, x1 = (std::min)(cx + 2, columns - 1), y0 = cy < 2 ? 0 : cy - 2, y1 = (std::min)(cy + 2, rows - 1);
            float px[25], py[25], vx[25], vy[25], dt[25], moved[25]; unsigned n = 0;
            for (unsigned ty = y0; ty <= y1; ++ty)
                for (unsigned tx = x0; tx <= x1; ++tx) {
                    const unsigned t = ty * columns + tx;
                    if (!field.valid[t]) continue;
                    px[n] = field.x[t]; py[n] = field.y[t];
                    vx[n] = field.vx[t]; vy[n] = field.vy[t]; dt[n] = field.seconds[t]; moved[n] = field.ego[t]; ++n;
                }
            if (n < 8) continue;
            // v = a0 + A (p - mean): least squares on centred positions, then once more without the tiles that fit worst
            // (a residual over 3 times the median and over 8 px/s: content entering at the picture's edge, a mismatch).
            bool keep[25]; for (unsigned k = 0; k < n; ++k) keep[k] = true;
            float mx = 0, my = 0, a0 = 0, b0 = 0, a1 = 0, a2 = 0, b1 = 0, b2 = 0, residual = 0, model = 0;
            bool fitted = false;
            for (unsigned pass = 0; pass < 2; ++pass) {
                unsigned m = 0; mx = my = a0 = b0 = 0;
                for (unsigned k = 0; k < n; ++k) if (keep[k]) { mx += px[k]; my += py[k]; a0 += vx[k]; b0 += vy[k]; ++m; }
                if (m < 8) { fitted = false; break; }
                mx /= static_cast<float>(m); my /= static_cast<float>(m); a0 /= static_cast<float>(m); b0 /= static_cast<float>(m);
                float sxx = 0, sxy = 0, syy = 0, sxu = 0, syu = 0, sxv = 0, syv = 0;
                for (unsigned k = 0; k < n; ++k) {
                    if (!keep[k]) continue;
                    const float x = px[k] - mx, y = py[k] - my;
                    sxx += x * x; sxy += x * y; syy += y * y; sxu += x * vx[k]; syu += y * vx[k]; sxv += x * vy[k]; syv += y * vy[k];
                }
                const float det = sxx * syy - sxy * sxy;
                if (!(det > 0.1f * sxx * syy)) { fitted = false; break; } // The tiles are (nearly) on a line.
                a1 = (sxu * syy - syu * sxy) / det; a2 = (syu * sxx - sxu * sxy) / det;
                b1 = (sxv * syy - syv * sxy) / det; b2 = (syv * sxx - sxv * sxy) / det;
                fitted = true;
                float errors[25], sorted[25]; unsigned e = 0; residual = model = 0;
                for (unsigned k = 0; k < n; ++k) {
                    const float x = px[k] - mx, y = py[k] - my, ex = a1 * x + a2 * y, ey = b1 * x + b2 * y;
                    const float ux = vx[k] - a0 - ex, uy = vy[k] - b0 - ey;
                    errors[k] = ux * ux + uy * uy; // Squared: compared with a squared limit, no root per tile.
                    if (keep[k]) { sorted[e++] = errors[k]; residual += errors[k]; model += ex * ex + ey * ey; }
                }
                residual = std::sqrt(residual / static_cast<float>(e)); model = std::sqrt(model / static_cast<float>(e));
                if (pass) break;
                // No error over the limit's least (8 px/s): nothing to leave out, whatever the median.
                float worst = 0; for (unsigned k = 0; k < e; ++k) worst = (std::max)(worst, sorted[k]);
                if (worst <= 64.0f) break;
                std::nth_element(sorted, sorted + e / 2, sorted + e);
                const float limit = (std::max)(64.0f, 9.0f * sorted[e / 2]); // (8 px/s and 3x the median error)^2.
                bool dropped = false;
                for (unsigned k = 0; k < n; ++k) if (keep[k] && errors[k] > limit) { keep[k] = false; dropped = true; }
                if (!dropped) break;
            }
            if (!fitted) continue;
            const float div = a1 + b2;
            // An approach expands both ways alike, without shear or rotation, and the expansion explains the flow (what is
            // left is small beside it); a translating edge or corner, or noise, does not.
            if (div <= kMinDivergence || a1 <= 0 || b2 <= 0 || (std::min)(a1, b2) < kIsotropy * (std::max)(a1, b2) || std::fabs(a2) + std::fabs(b1) > 0.5f * div ||
                residual > 0.5f * model) continue;
            // The fovea is for small, distant obstacles: over a ground they stand near the horizon. Below the ground's far third a
            // fine window is the near ground's (a near obstacle is large: the coarse windows' to find).
            if (field.fine && ground_.valid && my > ground_.horizon + (static_cast<float>(height_) - ground_.horizon) / 3) { ++result_.ground_windows; continue; }
            if (ground_.valid && my > ground_.horizon) {
                // Below the horizon of a ground the camera travels over: the ground's flow, unless the ground cannot explain it.
                float off = 0; unsigned e = 0;
                for (unsigned k = 0; k < n; ++k) {
                    if (!keep[k]) continue;
                    const float gu = ground_.ku * (py[k] - ground_.horizon) * (px[k] - ground_.focus) + ground_.offset_u;
                    const float gv = ground_.k * (py[k] - ground_.horizon) * (py[k] - ground_.horizon) + ground_.offset_v;
                    off += (vx[k] - gu) * (vx[k] - gu) + (vy[k] - gv) * (vy[k] - gv); ++e;
                }
                const float against = std::sqrt(off / static_cast<float>(e));
                if (against < 0.75f * model || against < 1.5f * residual) { ++result_.ground_windows; continue; }
            }
            // The focus of expansion: where A p + (a0, b0) = 0 (it may lie beyond the window: the field is the surface's).
            const float d2 = a1 * b2 - a2 * b1;
            float fx = mx, fy = my;
            if (d2 > 1e-6f) { fx = mx - (b2 * a0 - a2 * b0) / d2; fy = my - (a1 * b0 - b1 * a0) / d2; }
            // The expansion at the present positions over the window's mean baseline (see the header): 2 / (TTC + dt).
            float baseline = 0, ego = 0; unsigned kept = 0;
            for (unsigned k = 0; k < n; ++k) if (keep[k]) { baseline += dt[k]; ego += moved[k]; ++kept; }
            const float present = 1.0f - 0.5f * div * baseline / static_cast<float>(kept);
            if (present < 0.25f) continue; // Contact within the baseline: not a measurement.
            // The expansion must move the tiles further than the match can resolve, kMinShift of a cell over the baseline (the rms
            // of the expansion field), and than what is left of the camera's rotation once taken out: the IMU's gain, timing and
            // the lens leave errors in proportion to it (kEgoShare; a shaken window's long baseline made 1.5-2.4 px expansions).
            const float floor = kMinShift * 0.5f * static_cast<float>(width_ + height_) / kCells + kEgoShare * ego / static_cast<float>(kept);
            if (model * baseline / static_cast<float>(kept) < floor) continue;
            candidates_[candidate_count_++] = {div / present, fx, fy, field.fine};
        }
}
void Perception::Approach(std::int64_t pts) noexcept {
    const auto previous = object_; // The last object found (kept over a picture or two without one).
    if (!candidate_count_) return;
    unsigned strongest = 0;
    for (unsigned i = 1; i < candidate_count_; ++i) if (candidates_[i].divergence > candidates_[strongest].divergence) strongest = i;
    const auto& best = candidates_[strongest];
    // The same object: every window whose focus is within a tenth of the picture of the strongest's.
    float divergences[kTiles * kTiles + kFine * kFine], xs[kTiles * kTiles + kFine * kFine], ys[kTiles * kTiles + kFine * kFine]; unsigned n = 0;
    for (unsigned i = 0; i < candidate_count_; ++i) {
        const auto& c = candidates_[i];
        if (std::fabs(c.x - best.x) > 0.1f * static_cast<float>(width_) || std::fabs(c.y - best.y) > 0.1f * static_cast<float>(height_)) continue;
        divergences[n] = c.divergence; xs[n] = c.x; ys[n] = c.y; ++n;
    }
    // A rigid surface approaching expands about one centre, so many overlapping windows of both levels see it (an obstacle two
    // coarse tiles wide filled 42 or more on the beach footage); water washing in, reflections and foam pass the shape tests in
    // a window or three at a time (2026-10-29). And it is the same object from picture to picture: its centre where the last
    // object found was (within a tenth of the picture), at most kPersistence before.
    result_.support = n;
    if (n < kMinSupport) return;
    std::nth_element(divergences, divergences + n / 2, divergences + n);
    std::nth_element(xs, xs + n / 2, xs + n); std::nth_element(ys, ys + n / 2, ys + n);
    object_ = {true, pts, xs[n / 2], ys[n / 2]};
    if (!previous.valid || pts - previous.pts > kPersistence || std::fabs(object_.x - previous.x) > 0.1f * static_cast<float>(width_) ||
        std::fabs(object_.y - previous.y) > 0.1f * static_cast<float>(height_)) return;
    const float div = divergences[n / 2];
    result_.looming = true; result_.divergence = div; result_.ttc_seconds = 2.0f / div; result_.fovea = best.fine;
    result_.hazard_x = Clamp01(xs[n / 2] / static_cast<float>(width_)); result_.hazard_y = Clamp01(ys[n / 2] / static_cast<float>(height_));
}
void Perception::Objects(std::int64_t pts) noexcept {
    const float w = static_cast<float>(width_), h = static_cast<float>(height_), tile_w = w / kTiles, tile_h = h / kTiles;
    // What this picture holds (pixels): approaches (`divergence` > 0) and salient regions (`tiles` > 0), or one merged into the other.
    struct Found {
        float x, y, left, top, right, bottom, divergence, energy; unsigned support, tiles; bool fine; int track;
        float vx = 0, vy = 0, luma = 0; unsigned flows = 0; bool shared = false, occluded = false; // Its flow (px/s), its tiles' mean code; a part of a split region.
    };
    constexpr unsigned kMaxFound = 32;
    Found found[kMaxFound]; unsigned count = 0;
    // The approaches: the strongest window's object, its windows set aside, then the strongest left (non-maximum suppression).
    bool used[kTiles * kTiles + kFine * kFine]{};
    unsigned approaches = 0;
    while (approaches < kMaxPerceivedObjects) {
        int strongest = -1;
        for (unsigned i = 0; i < candidate_count_; ++i)
            if (!used[i] && (strongest < 0 || candidates_[i].divergence > candidates_[strongest].divergence)) strongest = static_cast<int>(i);
        if (strongest < 0) break;
        const auto best = candidates_[strongest];
        float divergences[kTiles * kTiles + kFine * kFine], xs[kTiles * kTiles + kFine * kFine], ys[kTiles * kTiles + kFine * kFine]; unsigned n = 0;
        float left = best.x, top = best.y, right = best.x, bottom = best.y;
        for (unsigned i = 0; i < candidate_count_; ++i) {
            const auto& c = candidates_[i];
            if (used[i] || std::fabs(c.x - best.x) > 0.1f * w || std::fabs(c.y - best.y) > 0.1f * h) continue;
            used[i] = true; divergences[n] = c.divergence; xs[n] = c.x; ys[n] = c.y; ++n;
            left = (std::min)(left, c.x); right = (std::max)(right, c.x); top = (std::min)(top, c.y); bottom = (std::max)(bottom, c.y);
        }
        if (n < kMinSupport) continue; // Surf, a reflection: set aside with its windows.
        std::nth_element(divergences, divergences + n / 2, divergences + n);
        std::nth_element(xs, xs + n / 2, xs + n); std::nth_element(ys, ys + n / 2, ys + n);
        const float x = xs[n / 2], y = ys[n / 2];
        // Without a salient region of its own: at least a tile around its focus.
        found[count++] = {x, y, (std::min)(left, x - tile_w / 2), (std::min)(top, y - tile_h / 2), (std::max)(right, x + tile_w / 2), (std::max)(bottom, y + tile_h / 2),
                          divergences[n / 2], 0.0f, n, 0u, best.fine, -1};
        ++approaches;
    }
    // The salient regions, breadth first: tiles at most kObjectGap tiles apart each way are one region (an approaching
    // surface's interior changes little while its edges do: its salient tiles come in pieces a tile apart). Labels 1, 2, ...;
    // 0xFF: salient tiles of no object (too few, or the scene).
    std::uint8_t region[kTileCount]{};
    unsigned queue[kTileCount], regions = 0;
    for (unsigned start = 0; start < kTileCount && regions < kMaxFound; ++start) {
        if (region[start] || !Salient(start)) continue;
        const auto mark = static_cast<std::uint8_t>(regions + 1);
        unsigned head = 0, tail = 0;
        queue[tail++] = start; region[start] = mark;
        while (head < tail) {
            const unsigned t = queue[head++], tx = t % kTiles, ty = t / kTiles;
            for (int dy = -kObjectGap; dy <= kObjectGap; ++dy)
                for (int dx = -kObjectGap; dx <= kObjectGap; ++dx) {
                    const int nx = static_cast<int>(tx) + dx, ny = static_cast<int>(ty) + dy;
                    if (nx < 0 || ny < 0 || nx >= static_cast<int>(kTiles) || ny >= static_cast<int>(kTiles)) continue;
                    const unsigned u = static_cast<unsigned>(ny) * kTiles + static_cast<unsigned>(nx);
                    if (!region[u] && Salient(u)) { region[u] = mark; queue[tail++] = u; }
                }
        }
        if (tail < kMinObjectTiles || static_cast<float>(tail) > kMaxObjectShare * kTileCount) { for (unsigned i = 0; i < tail; ++i) region[queue[i]] = 0xFF; continue; }
        ++regions;
    }
    // Each approach takes the region its focus is in, or next to (-1: none). A region taken by several (two obstacles whose
    // salient tiles touch) is split between them: each tile goes to the nearest focus.
    int taken[kMaxPerceivedObjects]{};
    for (unsigned a = 0; a < approaches; ++a) {
        taken[a] = -1;
        const int tx = std::clamp(static_cast<int>(found[a].x / tile_w), 0, static_cast<int>(kTiles) - 1), ty = std::clamp(static_cast<int>(found[a].y / tile_h), 0, static_cast<int>(kTiles) - 1);
        for (int ring = 0; ring <= 1 && taken[a] < 0; ++ring) // The focus's tile, then its neighbours.
            for (int j = ty - ring; j <= ty + ring && taken[a] < 0; ++j)
                for (int i = tx - ring; i <= tx + ring && taken[a] < 0; ++i) {
                    if (i < 0 || j < 0 || i >= static_cast<int>(kTiles) || j >= static_cast<int>(kTiles)) continue;
                    const unsigned r = region[static_cast<unsigned>(j) * kTiles + static_cast<unsigned>(i)];
                    if (r && r != 0xFF) taken[a] = static_cast<int>(r);
                }
    }
    // The tracks (2026-11-01): let go of those unseen for kTrackHold; where each one left is now at its velocity.
    for (auto& t : tracks_) if (t.id && pts - t.seen > kTrackHold) t = {};
    float predicted_x[kTracks]{}, predicted_y[kTracks]{}, line_vx[kTracks]{}, line_vy[kTracks]{}; bool live[kTracks]{};
    for (unsigned k = 0; k < kTracks; ++k) {
        const auto& t = tracks_[k];
        if (!t.id || pts <= t.pts) continue;
        (void)Extrapolate(t, pts, predicted_x[k], predicted_y[k], line_vx[k], line_vy[k]);
        live[k] = true;
    }
    // Which object each salient tile is part of (an index into `found`; -1: none).
    int owner[kTileCount];
    for (auto& o : owner) o = -1;
    for (unsigned t = 0; t < kTileCount; ++t) {
        const unsigned label = region[t];
        if (!label || label == 0xFF) continue;
        // An approach's region (or its part of one: the tile's nearest focus among the approaches that took it).
        const float px = (static_cast<float>(t % kTiles) + 0.5f) * tile_w, py = (static_cast<float>(t / kTiles) + 0.5f) * tile_h;
        int nearest = -1; float least = 0;
        for (unsigned a = 0; a < approaches; ++a) {
            if (taken[a] != static_cast<int>(label)) continue;
            const float d = std::hypot((px - found[a].x) / w, (py - found[a].y) / h);
            if (nearest < 0 || d < least) { nearest = static_cast<int>(a); least = d; }
        }
        owner[t] = nearest;
    }
    // An object's tiles (owner `index`) measured: its saliency-weighted centre, its tiles' bounds and energy, its own motion (the
    // saliency-weighted mean flow of its reliable tiles, px/s) and its tiles' mean code. False: no tile.
    const std::uint16_t* cells = Grid(0); // The newest picture's cells.
    const auto measure = [&](int index, Found& f) {
        unsigned tiles = 0, flows = 0, cx0 = kTiles, cy0 = kTiles, cx1 = 0, cy1 = 0; float energy = 0, sx = 0, sy = 0, fx = 0, fy = 0, fw = 0, luma = 0;
        for (unsigned t = 0; t < kTileCount; ++t) {
            if (owner[t] != index) continue;
            const unsigned tx = t % kTiles, ty = t / kTiles;
            const float px = (static_cast<float>(tx) + 0.5f) * tile_w, py = (static_cast<float>(ty) + 0.5f) * tile_h;
            const float s = static_cast<float>(saliency_[t]) / 255.0f;
            ++tiles; energy += s; sx += s * px; sy += s * py;
            if (valid_[t]) { ++flows; fx += s * flow_x_[t]; fy += s * flow_y_[t]; fw += s; }
            std::uint32_t sum = 0;
            for (unsigned j = 0; j < kTileCells; ++j) for (unsigned i = 0; i < kTileCells; ++i) sum += cells[(ty * kTileCells + j) * kCells + tx * kTileCells + i];
            luma += static_cast<float>(sum) / (16.0f * kTileCells * kTileCells);
            cx0 = (std::min)(cx0, tx); cx1 = (std::max)(cx1, tx); cy0 = (std::min)(cy0, ty); cy1 = (std::max)(cy1, ty);
        }
        if (!tiles || !(energy > 0)) return false;
        f.x = sx / energy; f.y = sy / energy; f.energy = energy; f.tiles = tiles; f.luma = luma / static_cast<float>(tiles);
        f.flows = flows; f.vx = fw > 0 ? fx / fw : 0.0f; f.vy = fw > 0 ? fy / fw : 0.0f;
        f.left = static_cast<float>(cx0) * tile_w; f.top = static_cast<float>(cy0) * tile_h;
        f.right = static_cast<float>(cx1 + 1) * tile_w; f.bottom = static_cast<float>(cy1 + 1) * tile_h;
        return true;
    };
    // The approaches with their regions (or their part of one).
    for (unsigned a = 0; a < approaches; ++a) if (taken[a] >= 0) (void)measure(static_cast<int>(a), found[a]);
    // The regions no approach took. One that holds the predictions of two tracks or more (within a tile of its bounds: objects
    // crossing, or passing side by side) is split between them: each tile to the track whose prediction is nearest, against its
    // size, and whose velocity its flow matches; a part too small to be an object leaves its track hidden (at its prediction).
    for (unsigned label = 1; label <= regions && count < kMaxFound; ++label) {
        bool free = true;
        for (unsigned a = 0; a < approaches; ++a) free = free && taken[a] != static_cast<int>(label);
        if (!free) continue;
        unsigned cx0 = kTiles, cy0 = kTiles, cx1 = 0, cy1 = 0;
        for (unsigned t = 0; t < kTileCount; ++t)
            if (region[t] == label) { cx0 = (std::min)(cx0, t % kTiles); cx1 = (std::max)(cx1, t % kTiles); cy0 = (std::min)(cy0, t / kTiles); cy1 = (std::max)(cy1, t / kTiles); }
        unsigned holders[kTracks]; unsigned held = 0;
        for (unsigned k = 0; k < kTracks; ++k) {
            if (!live[k]) continue;
            const float x = predicted_x[k] / tile_w, y = predicted_y[k] / tile_h;
            if (x >= static_cast<float>(cx0) - 1 && x <= static_cast<float>(cx1) + 2 && y >= static_cast<float>(cy0) - 1 && y <= static_cast<float>(cy1) + 2) holders[held++] = k;
        }
        if (held < 2) {
            const int index = static_cast<int>(count);
            for (unsigned t = 0; t < kTileCount; ++t) if (region[t] == label) owner[t] = index;
            Found f{0, 0, 0, 0, 0, 0, 0.0f, 0.0f, 0u, 0u, false, -1};
            if (measure(index, f)) found[count++] = f;
            else for (auto& o : owner) if (o == index) o = -1;
            continue;
        }
        held = (std::min)(held, kMaxFound - count);
        for (unsigned t = 0; t < kTileCount; ++t) {
            if (region[t] != label) continue;
            const float px = (static_cast<float>(t % kTiles) + 0.5f) * tile_w, py = (static_cast<float>(t / kTiles) + 0.5f) * tile_h;
            unsigned best = 0; float least = 0;
            for (unsigned i = 0; i < held; ++i) {
                const auto& track = tracks_[holders[i]];
                const float size = kTrackGate + 0.5f * (std::max)(track.width / w, track.height / h);
                float cost = std::hypot((px - predicted_x[holders[i]]) / w, (py - predicted_y[holders[i]]) / h) / size;
                if (valid_[t]) {
                    // The tile's flow against the track's velocity, in picture sizes per second, over the larger of the two speeds.
                    const float ux = flow_x_[t] / w, uy = flow_y_[t] / h, vx = line_vx[holders[i]] / w, vy = line_vy[holders[i]] / h;
                    const float scale = (std::max)({std::hypot(ux, uy), std::hypot(vx, vy), kMinSpeed});
                    cost += kFlowWeight * (std::min)(std::hypot(ux - vx, uy - vy) / scale, 2.0f);
                }
                if (i == 0 || cost < least) { best = i; least = cost; }
            }
            owner[t] = static_cast<int>(count + best);
        }
        for (unsigned i = 0; i < held; ++i) {
            const auto& track = tracks_[holders[i]];
            Found f{0, 0, 0, 0, 0, 0, 0.0f, 0.0f, 0u, 0u, false, static_cast<int>(holders[i])};
            f.shared = true;
            if (!measure(static_cast<int>(count + i), f) || f.tiles < kMinObjectTiles) {
                // Hidden behind the other: where its motion puts it, its size as last seen.
                const float x = predicted_x[holders[i]], y = predicted_y[holders[i]];
                f.x = x; f.y = y; f.left = x - 0.5f * track.width; f.right = x + 0.5f * track.width; f.top = y - 0.5f * track.height; f.bottom = y + 0.5f * track.height;
                f.tiles = 0; f.energy = 0; f.flows = 0; f.occluded = true;
            }
            found[count + i] = f;
        }
        count += held;
    }
    // Before the tracks: the most urgent kMaxPerceivedObjects (approaches by expansion, then by energy; hidden ones last).
    const auto earlier = [](const Found& a, const Found& b) {
        if ((a.divergence > 0) != (b.divergence > 0)) return a.divergence > 0;
        return a.divergence > 0 ? a.divergence > b.divergence : a.energy > b.energy;
    };
    std::sort(found, found + count, earlier);
    count = (std::min)(count, kMaxPerceivedObjects);
    // The other objects' tracks: the assignment of least total cost (see the header), over every way of giving each object one
    // of the tracks not split above, or none (kNewTrack).
    bool bound[kTracks]{};
    for (unsigned i = 0; i < count; ++i) if (found[i].track >= 0) bound[found[i].track] = true;
    constexpr float kNever = 1e30f;
    float cost[kMaxPerceivedObjects][kTracks];
    for (unsigned i = 0; i < count; ++i) {
        const auto& f = found[i];
        for (unsigned k = 0; k < kTracks; ++k) {
            cost[i][k] = kNever;
            const auto& t = tracks_[k];
            if (f.track >= 0 || !live[k] || bound[k]) continue;
            const float gate = kTrackGate + 0.5f * (std::max)((f.right - f.left) / w, (f.bottom - f.top) / h);
            const float d = std::hypot((f.x - predicted_x[k]) / w, (f.y - predicted_y[k]) / h);
            if (d > gate) continue;
            float c = d / gate;
            // Its direction: the object's own motion (its flow, else from where the track was) against the track's.
            const float dt = static_cast<float>(static_cast<double>(pts - t.pts) / 1e7);
            const float ux = f.flows >= 2 ? f.vx / w : (f.x - t.x) / dt / w, uy = f.flows >= 2 ? f.vy / h : (f.y - t.y) / dt / h;
            const float vx = line_vx[k] / w, vy = line_vy[k] / h, su = std::hypot(ux, uy), sv = std::hypot(vx, vy);
            if (su > kMinSpeed && sv > kMinSpeed) c += kTurnWeight * 0.5f * (1.0f - (ux * vx + uy * vy) / (su * sv));
            // Its look: how many tiles, its shape, its tiles' mean code.
            if (f.tiles && t.tiles > 0) c += kAreaWeight * (std::min)(std::fabs(std::log(static_cast<float>(f.tiles) / t.tiles)) / std::log(4.0f), 1.0f);
            if (f.right > f.left && f.bottom > f.top && t.width > 0 && t.height > 0)
                c += kShapeWeight * (std::min)(std::fabs(std::log((f.right - f.left) / (f.bottom - f.top) * t.height / t.width)), 1.0f);
            if (f.tiles && t.luma >= 0) c += kLumaWeight * (std::min)(std::fabs(f.luma - t.luma) / kLumaScale, 1.0f);
            cost[i][k] = c;
        }
    }
    {
        // Objects in order, tracks as a mask of those used: the least cost of the first i objects using exactly `mask`.
        constexpr unsigned kMasks = 1u << kTracks;
        float least[kMaxPerceivedObjects + 1][kMasks];
        signed char choice[kMaxPerceivedObjects][kMasks];
        for (auto& v : least[0]) v = kNever;
        least[0][0] = 0;
        for (unsigned i = 0; i < count; ++i)
            for (unsigned mask = 0; mask < kMasks; ++mask) {
                float v = least[i][mask] < kNever ? least[i][mask] + (found[i].track >= 0 ? 0.0f : kNewTrack) : kNever;
                signed char c = -1;
                for (unsigned k = 0; k < kTracks; ++k) {
                    if (!(mask & (1u << k)) || cost[i][k] >= kNever || least[i][mask ^ (1u << k)] >= kNever) continue;
                    const float candidate = least[i][mask ^ (1u << k)] + cost[i][k];
                    if (candidate < v) { v = candidate; c = static_cast<signed char>(k); }
                }
                least[i + 1][mask] = v; choice[i][mask] = c;
            }
        unsigned mask = 0;
        for (unsigned m = 1; m < kMasks; ++m) if (least[count][m] < least[count][mask]) mask = m;
        for (unsigned i = count; i-- > 0;) {
            const int k = choice[i][mask];
            if (k >= 0) { found[i].track = k; mask ^= 1u << k; }
        }
    }
    bool track_taken[kTracks]{};
    for (unsigned i = 0; i < count; ++i) if (found[i].track >= 0) track_taken[found[i].track] = true;
    unsigned out = 0;
    for (unsigned i = 0; i < count; ++i) {
        auto& f = found[i];
        bool confirmed = false;
        if (f.track >= 0) {
            auto& t = tracks_[f.track];
            const float dt = static_cast<float>(static_cast<double>(pts - t.pts) / 1e7);
            const float px = predicted_x[f.track], py = predicted_y[f.track];
            // A part of a shared region measures its track when its flow moves as the track's line does (see the header).
            bool agrees = !f.shared;
            if (f.shared && !f.occluded && f.flows >= 2) {
                const float su = std::hypot(f.vx / w, f.vy / h), sv = std::hypot(line_vx[f.track] / w, line_vy[f.track] / h);
                agrees = su > kMinSpeed && sv > kMinSpeed && (f.vx * line_vx[f.track] / (w * w) + f.vy * line_vy[f.track] / (h * h)) >= kAgreeCos * su * sv &&
                         su <= kAgreeSpeed * sv && sv <= kAgreeSpeed * su;
            }
            if (!agrees) {
                // Hidden, or its part is mostly the other's: carried along its line.
                t.x = px; t.y = py; t.pts = pts;
                if (!f.occluded) { t.seen = pts; ++t.age; }
            } else {
                // A measurement: on its path; its velocity the path's line, before that its own flow, else where its centre went.
                t.path_pts[t.path_head] = pts; t.path_x[t.path_head] = f.x; t.path_y[t.path_head] = f.y;
                t.path_head = (t.path_head + 1) % kPath; t.path_count = (std::min)(t.path_count + 1, kPath);
                float x = 0, y = 0, vx = 0, vy = 0;
                if (Extrapolate(t, pts, x, y, vx, vy)) { t.vx = vx; t.vy = vy; }
                else {
                    if (f.flows >= 2) { vx = f.vx; vy = f.vy; } else { vx = (f.x - t.x) / dt; vy = (f.y - t.y) / dt; }
                    t.vx = t.age > 1 ? 0.5f * (t.vx + vx) : vx; t.vy = t.age > 1 ? 0.5f * (t.vy + vy) : vy;
                }
                t.x = f.x; t.y = f.y;
                // Its look, while it is alone (half the new each picture).
                if (!f.shared && f.tiles) {
                    const auto blend = [&](float& v, float now) { v = v > 0 ? 0.5f * (v + now) : now; };
                    blend(t.tiles, static_cast<float>(f.tiles)); blend(t.width, f.right - f.left); blend(t.height, f.bottom - f.top);
                    t.luma = t.luma >= 0 ? 0.5f * (t.luma + f.luma) : f.luma;
                }
                t.pts = t.seen = pts; ++t.age;
            }
            confirmed = f.divergence > 0 && t.approach != INT64_MIN && pts - t.approach <= kPersistence;
        } else {
            // A new track: a free slot, else the one seen longest ago.
            unsigned k = 0;
            for (unsigned j = 0; j < kTracks; ++j) {
                if (track_taken[j]) continue;
                if (!tracks_[j].id) { k = j; break; }
                if (track_taken[k] || tracks_[j].seen < tracks_[k].seen) k = j;
            }
            if (track_taken[k]) continue; // Every slot taken this picture (cannot happen: kTracks > kMaxPerceivedObjects).
            Track t{};
            t.id = next_track_; t.age = 1; t.x = f.x; t.y = f.y; t.pts = t.seen = pts;
            t.path_pts[0] = pts; t.path_x[0] = f.x; t.path_y[0] = f.y; t.path_count = t.path_head = 1;
            if (f.flows >= 2) { t.vx = f.vx; t.vy = f.vy; }
            if (f.tiles) { t.tiles = static_cast<float>(f.tiles); t.width = f.right - f.left; t.height = f.bottom - f.top; t.luma = f.luma; }
            tracks_[k] = t;
            next_track_ = next_track_ == UINT32_MAX ? 1 : next_track_ + 1;
            track_taken[k] = true; f.track = static_cast<int>(k);
        }
        auto& t = tracks_[f.track];
        if (f.divergence > 0) t.approach = pts;
        auto& o = result_.objects[out++];
        o.track_id = t.id; o.age = t.age;
        o.x = Clamp01(f.x / w); o.y = Clamp01(f.y / h);
        o.width = Clamp01((f.right - f.left) / w); o.height = Clamp01((f.bottom - f.top) / h);
        o.vx = t.vx / w; o.vy = t.vy / h;
        o.approaching = confirmed; o.divergence = f.divergence; o.ttc_seconds = confirmed ? 2.0f / f.divergence : -1.0f;
        o.support = f.support; o.tiles = f.tiles; o.energy = f.energy; o.saliency = f.tiles ? f.energy / static_cast<float>(f.tiles) : 0.0f;
        o.fovea = f.fine; o.occluded = f.occluded;
    }
    result_.object_count = out;
    // The table: confirmed approaches by time to contact, then the rest by energy.
    std::sort(result_.objects, result_.objects + out, [](const PerceivedObject& a, const PerceivedObject& b) {
        if (a.approaching != b.approaching) return a.approaching;
        return a.approaching ? a.ttc_seconds < b.ttc_seconds : a.energy > b.energy;
    });
}
bool Perception::Extrapolate(const Track& t, std::int64_t pts, float& x, float& y, float& vx, float& vy) noexcept {
    // The centres of the last second, about their mean time (seconds from `pts`).
    double n = 0, mt = 0, mx = 0, my = 0, first = 0, last = -1e9;
    for (unsigned i = 0; i < t.path_count; ++i) {
        if (pts - t.path_pts[i] > 10000000) continue;
        const double s = static_cast<double>(t.path_pts[i] - pts) / 1e7;
        n += 1; mt += s; mx += t.path_x[i]; my += t.path_y[i];
        first = n == 1 ? s : (std::min)(first, s); last = (std::max)(last, s);
    }
    if (n >= 3 && last - first >= 0.08) {
        mt /= n; mx /= n; my /= n;
        double stt = 0, stx = 0, sty = 0;
        for (unsigned i = 0; i < t.path_count; ++i) {
            if (pts - t.path_pts[i] > 10000000) continue;
            const double s = static_cast<double>(t.path_pts[i] - pts) / 1e7 - mt;
            stt += s * s; stx += s * (t.path_x[i] - mx); sty += s * (t.path_y[i] - my);
        }
        vx = static_cast<float>(stx / stt); vy = static_cast<float>(sty / stt);
        x = static_cast<float>(mx - stx / stt * mt); y = static_cast<float>(my - sty / stt * mt); // The line at s = 0 (now).
        return true;
    }
    const float dt = static_cast<float>(static_cast<double>(pts - t.pts) / 1e7);
    vx = t.vx; vy = t.vy; x = t.x + t.vx * dt; y = t.y + t.vy * dt;
    return false;
}
unsigned Perception::ForceLevel(unsigned level) noexcept {
    g_level = (std::min)(level, Available()); g_scalar = g_level == 0;
    return g_level;
}
unsigned Perception::Level() noexcept { return g_level; }
}
