// A small AV1 encoder of the engine's own, for tests, placeholders and screen recordings: every
// frame is a key frame that decodes alone (a sequence header comes with each), coded lossless
// (base_q_idx 0) with screen content tools on and every block skipped and drawn from palettes,
// as AV1's palette mode allows, so a block of up to 8 colors per plane comes out exactly and
// bigger ones are reduced to 8. Symbol adaptation is off (disable_cdf_update), so the default
// CDFs below are all it needs. Integer math only, so the bytes are the same everywhere.
//
// The symbol coder follows libaom's od_ec_enc (BSD 2-Clause, Alliance for Open Media) and the
// default CDFs are the AV1 specification's, as dav1d's src/cdf.c (BSD 2-Clause, VideoLAN) holds
// them; each syntax element below is written in the order dav1d's decode_b and read_pal_plane
// read it.

#include "slash/video/video.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <string>
#include <utility>

namespace slash::video {
namespace {

constexpr int kMaxWidth = 4096;
constexpr int kMaxHeight = 2304;

// ---- Default CDFs, as cumulative counts out of 32768 for each symbol but the last.

// Partitions of 64x64, 32x32 and 16x16 blocks by context: NONE, H, V, SPLIT, T_TOP_SPLIT,
// T_BOTTOM_SPLIT, T_LEFT_SPLIT, T_RIGHT_SPLIT, H4 (V4 last).
constexpr std::uint16_t kPartitionCdf[3][4][9] = {
    {{20137, 21547, 23078, 29566, 29837, 30261, 30524, 30892, 31724},
     {6732, 7490, 9497, 27944, 28250, 28515, 28969, 29630, 30104},
     {5945, 7663, 8348, 28683, 29117, 29749, 30064, 30298, 32238},
     {870, 1212, 1487, 31198, 31394, 31574, 31743, 31881, 32332}},
    {{18462, 20920, 23124, 27647, 28227, 29049, 29519, 30178, 31544},
     {7689, 9060, 12056, 24992, 25660, 26182, 26951, 28041, 29052},
     {6015, 9009, 10062, 24544, 25409, 26545, 27071, 27526, 32047},
     {1394, 2208, 2796, 28614, 29061, 29466, 29840, 30185, 31899}},
    {{15597, 20929, 24571, 26706, 27664, 28821, 29601, 30571, 31902},
     {7925, 11043, 16785, 22470, 23971, 25043, 26651, 28701, 29834},
     {5414, 13269, 15111, 20488, 22360, 24500, 25537, 26336, 32117},
     {2662, 6362, 8614, 20860, 23053, 24778, 26436, 27829, 31171}},
};
// 8x8 blocks' partitions (NONE, H, V, SPLIT) by context: only NONE is written.
constexpr std::uint16_t kPartition8Cdf[4][3] = {
    {19132, 25510, 30392}, {13928, 19855, 28540}, {12522, 23679, 28629}, {9896, 18783, 25853}};
constexpr std::uint16_t kSkipCdf[3] = {31671, 16515, 4576};
// A key frame's y mode with DC_PRED above and to the left (13 modes), and the uv mode after a
// DC_PRED y mode with CfL allowed (14 modes) or not (13).
constexpr std::uint16_t kKfDcYModeCdf = 15588;
constexpr std::uint16_t kDcUvModeCflCdf = 10407;
constexpr std::uint16_t kDcUvModeCdf = 22631;
// has_palette_y by block size context and neighbors with palettes; has_palette_uv by whether
// the block has a luma palette.
constexpr std::uint16_t kPaletteYCdf[7][3] = {
    {31676, 3419, 1261}, {31912, 2859, 980},  {31823, 3400, 781}, {32030, 3561, 904},
    {32309, 7337, 1462}, {32265, 4015, 1521}, {32450, 7946, 129}};
constexpr std::uint16_t kPaletteUvCdf[2] = {32461, 21488};
// palette_size_y_minus_2 and palette_size_uv_minus_2 by block size context (7 sizes).
constexpr std::uint16_t kPaletteSizeCdf[2][7][6] = {
    {{7952, 13000, 18149, 21478, 25527, 29241},
     {7139, 11421, 16195, 19544, 23666, 28073},
     {7788, 12741, 17325, 20500, 24315, 28530},
     {8271, 14064, 18246, 21564, 25071, 28533},
     {12725, 19180, 21863, 24839, 27535, 30120},
     {9711, 14888, 16923, 21052, 25661, 27875},
     {14940, 20797, 21678, 24186, 27033, 28999}},
    {{8713, 19979, 27128, 29609, 31331, 32272},
     {5839, 15573, 23581, 26947, 29848, 31700},
     {4426, 11260, 17999, 21483, 25863, 29430},
     {3228, 9464, 14993, 18089, 22523, 27420},
     {3768, 8886, 13091, 17852, 22495, 27207},
     {2464, 8451, 12861, 21632, 25525, 28555},
     {1269, 5435, 10433, 18963, 21700, 25865}},
};
// palette_color_idx_y and _uv by palette size (2 to 8) and color context (5).
constexpr std::uint16_t kColorMapCdf[2][7][5][7] = {
    {
        {{28710}, {16384}, {10553}, {27036}, {31603}},
        {{27877, 30490}, {11532, 25697}, {6544, 30234}, {23018, 28072}, {31915, 32385}},
        {{25572, 28046, 30045},
         {9478, 21590, 27256},
         {7248, 26837, 29824},
         {19167, 24486, 28349},
         {31400, 31825, 32250}},
        {{24779, 26955, 28576, 30282},
         {8669, 20364, 24073, 28093},
         {4255, 27565, 29377, 31067},
         {19864, 23674, 26716, 29530},
         {31646, 31893, 32147, 32426}},
        {{23132, 25407, 26970, 28435, 30073},
         {7443, 17242, 20717, 24762, 27982},
         {6300, 24862, 26944, 28784, 30671},
         {18916, 22895, 25267, 27435, 29652},
         {31270, 31550, 31808, 32059, 32353}},
        {{23105, 25199, 26464, 27684, 28931, 30318},
         {6950, 15447, 18952, 22681, 25567, 28563},
         {7560, 23474, 25490, 27203, 28921, 30708},
         {18544, 22373, 24457, 26195, 28119, 30045},
         {31198, 31451, 31670, 31882, 32123, 32391}},
        {{21689, 23883, 25163, 26352, 27506, 28827, 30195},
         {6892, 15385, 17840, 21606, 24287, 26753, 29204},
         {5651, 23182, 25042, 26518, 27982, 29392, 30900},
         {19349, 22578, 24418, 25994, 27524, 29031, 30448},
         {31028, 31270, 31504, 31705, 31927, 32153, 32392}},
    },
    {
        {{29089}, {16384}, {8713}, {29257}, {31610}},
        {{25257, 29145}, {12287, 27293}, {7033, 27960}, {20145, 25405}, {30608, 31639}},
        {{24210, 27175, 29903},
         {9888, 22386, 27214},
         {5901, 26053, 29293},
         {18318, 22152, 28333},
         {30459, 31136, 31926}},
        {{22980, 25479, 27781, 29986},
         {8413, 21408, 24859, 28874},
         {2257, 29449, 30594, 31598},
         {19189, 21202, 25915, 28620},
         {31844, 32044, 32281, 32518}},
        {{22217, 24567, 26637, 28683, 30548},
         {7307, 16406, 19636, 24632, 28424},
         {4441, 25064, 26879, 28942, 30919},
         {17210, 20528, 23319, 26750, 29582},
         {30674, 30953, 31396, 31735, 32207}},
        {{21239, 23168, 25044, 26962, 28705, 30506},
         {6545, 15012, 18004, 21817, 25503, 28701},
         {3448, 26295, 27437, 28704, 30126, 31442},
         {15889, 18323, 21704, 24698, 26976, 29690},
         {30988, 31204, 31479, 31734, 31983, 32325}},
        {{21442, 23288, 24758, 26246, 27649, 28980, 30563},
         {5863, 14933, 17552, 20668, 23683, 26411, 29273},
         {3415, 25810, 26877, 27990, 29223, 30394, 31618},
         {17965, 20084, 22232, 23974, 26274, 28402, 30390},
         {31190, 31329, 31516, 31679, 31825, 32026, 32322}},
    },
};

// The partition context each block writes above and to the left, by level (64x64 to 8x8),
// for PARTITION_NONE: dav1d's dav1d_al_part_ctx.
constexpr std::uint8_t kPartitionContextAbove[4] = {0x10, 0x18, 0x1c, 0x1e};
constexpr std::uint8_t kPartitionContextLeft[4] = {0x10, 0x18, 0x1c, 0x1e};

int floor_log2(unsigned value) {
    return static_cast<int>(std::bit_width(value)) - 1;
}

// ---- Bits

// Header bits, most significant first.
class BitWriter {
public:
    void put(std::uint32_t value, int bits) {
        for (int i = bits - 1; i >= 0; --i) {
            bit(((value >> static_cast<unsigned>(i)) & 1U) != 0);
        }
    }
    void bit(bool value) {
        if (used_ == 0) {
            bytes_.push_back(0);
        }
        if (value) {
            bytes_.back() = static_cast<std::uint8_t>(bytes_.back() | (0x80U >> used_));
        }
        used_ = (used_ + 1) % 8;
    }
    // trailing_bits(): a one, then zeros to the byte.
    void trailing() {
        bit(true);
        align();
    }
    void align() { used_ = 0; }
    [[nodiscard]] std::vector<std::uint8_t> take() { return std::move(bytes_); }

private:
    std::vector<std::uint8_t> bytes_;
    int used_ = 0;
};

// AV1's symbol coder, as libaom's od_ec_enc with a 32-bit window.
class SymbolWriter {
public:
    // Writes symbol s of n by a CDF of n - 1 cumulative counts.
    void symbol(unsigned s, const std::uint16_t* cdf, unsigned n) {
        const unsigned fl = s > 0 ? 32768U - cdf[s - 1] : 32768U;
        const unsigned fh = s + 1 < n ? 32768U - cdf[s] : 0U;
        encode(s, fl, fh, n);
    }
    // A bool whose CDF gives 0 a count of cdf0.
    void boolean(bool value, std::uint16_t cdf0) { symbol(value ? 1U : 0U, &cdf0, 2); }
    // A bool by the inverse probability dav1d_msac_decode_bool reads, the chance of a one.
    void boolean_inverse(bool value, unsigned one) {
        encode(value ? 1U : 0U, value ? one : 32768U, value ? 0U : one, 2);
    }
    // L(n): n literal bits, most significant first.
    void literal(unsigned value, int bits) {
        for (int i = bits - 1; i >= 0; --i) {
            boolean(((value >> static_cast<unsigned>(i)) & 1U) != 0, 16384);
        }
    }
    // NS(n), as dav1d_msac_decode_uniform reads it.
    void uniform(unsigned value, unsigned n) {
        const int l = floor_log2(n) + 1;
        const unsigned m = (1U << static_cast<unsigned>(l)) - n;
        if (value < m) {
            literal(value, l - 1);
        } else {
            const unsigned w = value + m;
            literal(w >> 1U, l - 1);
            literal(w & 1U, 1);
        }
    }

    // od_ec_enc_done: the fewest bytes that decode the symbols written whatever follows, then
    // carries propagated.
    [[nodiscard]] std::vector<std::uint8_t> finish() {
        int c = cnt_;
        int s = 10;
        const std::uint64_t m = 0x3FFF;
        std::uint64_t e = ((low_ + m) & ~m) | (m + 1);
        s += c;
        if (s > 0) {
            std::uint64_t n = (std::uint64_t{1} << static_cast<unsigned>(c + 16)) - 1;
            do {
                precarry_.push_back(static_cast<std::uint16_t>(e >> static_cast<unsigned>(c + 16)));
                e &= n;
                s -= 8;
                c -= 8;
                n >>= 8U;
            } while (s > 0);
        }
        std::vector<std::uint8_t> out(precarry_.size());
        unsigned carry = 0;
        for (std::size_t i = precarry_.size(); i-- > 0;) {
            carry = precarry_[i] + carry;
            out[i] = static_cast<std::uint8_t>(carry & 0xFFU);
            carry >>= 8U;
        }
        return out;
    }

private:
    // od_ec_encode_q15: fl and fh are the inverse cumulative counts before and at s.
    void encode(unsigned s, unsigned fl, unsigned fh, unsigned n) {
        std::uint64_t l = low_;
        unsigned r = rng_;
        const unsigned last = n - 1;
        if (fl < 32768U) {
            const unsigned u = (((r >> 8U) * (fl >> 6U)) >> 1U) + (4U * (last - (s - 1)));
            const unsigned v = (((r >> 8U) * (fh >> 6U)) >> 1U) + (4U * (last - s));
            l += r - u;
            r = u - v;
        } else {
            r -= (((r >> 8U) * (fh >> 6U)) >> 1U) + (4U * (last - s));
        }
        normalize(l, r);
    }

    void normalize(std::uint64_t low, unsigned rng) {
        const int d = 16 - static_cast<int>(std::bit_width(rng));
        int c = cnt_;
        int s = c + d;
        if (s >= 0) {
            c += 16;
            std::uint64_t m = (std::uint64_t{1} << static_cast<unsigned>(c)) - 1;
            if (s >= 8) {
                precarry_.push_back(static_cast<std::uint16_t>(low >> static_cast<unsigned>(c)));
                low &= m;
                c -= 8;
                m >>= 8U;
            }
            precarry_.push_back(static_cast<std::uint16_t>(low >> static_cast<unsigned>(c)));
            s = c + d - 24;
            low &= m;
        }
        low_ = low << static_cast<unsigned>(d);
        rng_ = rng << static_cast<unsigned>(d);
        cnt_ = s;
    }

    std::vector<std::uint16_t> precarry_;
    std::uint64_t low_ = 0;
    unsigned rng_ = 0x8000;
    int cnt_ = -9;
};

// ---- Palettes

struct Pair {
    int u = 0;
    int v = 0;
    friend bool operator==(const Pair&, const Pair&) = default;
    friend auto operator<=>(const Pair&, const Pair&) = default;
};

int distance(int a, int b) {
    return (a - b) * (a - b);
}
int distance(Pair a, Pair b) {
    return distance(a.u, b.u) + distance(a.v, b.v);
}
int mean(long long sum, long long count) {
    return static_cast<int>((sum + (count / 2)) / count);
}
int nearest(std::span<const int> palette, int value) {
    int best = 0;
    for (std::size_t i = 1; i < palette.size(); ++i) {
        if (distance(palette[i], value) <
            distance(palette[static_cast<std::size_t>(best)], value)) {
            best = static_cast<int>(i);
        }
    }
    return best;
}
int nearest(std::span<const Pair> palette, Pair value) {
    int best = 0;
    for (std::size_t i = 1; i < palette.size(); ++i) {
        if (distance(palette[i], value) <
            distance(palette[static_cast<std::size_t>(best)], value)) {
            best = static_cast<int>(i);
        }
    }
    return best;
}

// Up to 8 colors for values, sorted: the values themselves when there are that few, else
// k-means from colors spread over them, a few rounds.
template <typename T>
std::vector<T> choose_palette(std::vector<T> values) {
    std::sort(values.begin(), values.end());
    std::vector<T> unique = values;
    unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
    if (unique.size() <= 8) {
        return unique;
    }
    std::vector<T> centers(8);
    for (std::size_t k = 0; k < 8; ++k) {
        centers[k] = unique[(k * (unique.size() - 1)) / 7];
    }
    for (int round = 0; round < 6; ++round) {
        // Each center's sums (u and v for pairs) and count.
        std::array<std::array<long long, 3>, 8> sums{};
        for (const T& value : values) {
            auto& sum = sums.at(static_cast<std::size_t>(nearest(centers, value)));
            if constexpr (std::is_same_v<T, Pair>) {
                sum[0] += value.u;
                sum[1] += value.v;
            } else {
                sum[0] += value;
            }
            ++sum[2];
        }
        for (std::size_t k = 0; k < 8; ++k) {
            const auto& sum = sums.at(k);
            if (sum[2] == 0) {
                continue;
            }
            if constexpr (std::is_same_v<T, Pair>) {
                centers[k] = {mean(sum[0], sum[2]), mean(sum[1], sum[2])};
            } else {
                centers[k] = mean(sum[0], sum[2]);
            }
        }
    }
    std::sort(centers.begin(), centers.end());
    centers.erase(std::unique(centers.begin(), centers.end()), centers.end());
    return centers;
}

// ---- The frame

class FrameWriter {
public:
    explicit FrameWriter(const Yuv420& picture)
        : picture_(picture), width4_(static_cast<int>(picture.width) / 4),
          height4_(static_cast<int>(picture.height) / 4) {
        above_.resize(static_cast<std::size_t>(width4_));
        left_.resize(16);
        above_partition_.resize(static_cast<std::size_t>(width4_ / 2));
        left_partition_.resize(8);
    }

    std::vector<std::uint8_t> tile() {
        for (int y = 0; y < height4_; y += 16) {
            std::fill(left_.begin(), left_.end(), Edge{});
            std::fill(left_partition_.begin(), left_partition_.end(), 0);
            for (int x = 0; x < width4_; x += 16) {
                partition(x, y, 1);
            }
        }
        return symbols_.finish();
    }

private:
    // What a block leaves above and to the left of it for the blocks after: skip, palette sizes
    // and the colors the next blocks' palette caches read.
    struct Edge {
        bool skip = false;
        int y_size = 0;
        int uv_size = 0;
        std::array<int, 8> y{};
        std::array<int, 8> u{};
    };

    // decode_sb, at level (1 for 64x64 to 4 for 8x8), in 4x4 units.
    void partition(int x, int y, int level) {
        const int half = 16 >> level;
        const bool h_split = width4_ > x + half;
        const bool v_split = height4_ > y + half;
        if (!h_split && !v_split) {
            partition(x, y, level + 1);
            return;
        }
        const int x8 = x / 2;
        const int y8 = (y % 16) / 2;
        const int ctx = ((above_partition_[static_cast<std::size_t>(x8)] >> (4 - level)) & 1) +
                        (((left_partition_[static_cast<std::size_t>(y8)] >> (4 - level)) & 1) << 1);
        const int size4 = half * 2;
        if (level == 4) {
            symbols_.symbol(0, kPartition8Cdf[ctx], 4);
            block(x, y, size4);
            set_partition(x8, y8, size4 / 2, level);
            return;
        }
        const std::uint16_t* cdf = kPartitionCdf[level - 1][ctx];
        if (h_split && v_split) {
            const bool inside = x + size4 <= width4_ && y + size4 <= height4_;
            if (inside && fits(x, y, size4)) {
                symbols_.symbol(0, cdf, 10);
                block(x, y, size4);
                set_partition(x8, y8, size4 / 2, level);
                return;
            }
            symbols_.symbol(3, cdf, 10);
            partition(x, y, level + 1);
            partition(x + half, y, level + 1);
            partition(x, y + half, level + 1);
            partition(x + half, y + half, level + 1);
            return;
        }
        // Past the frame's bottom or right edge: a split, as a bool by gather_top_partition_prob
        // or gather_left_partition_prob.
        std::array<unsigned, 10> icdf{};
        for (std::size_t i = 0; i < 9; ++i) {
            icdf.at(i) = 32768U - cdf[i];
        }
        if (h_split) {
            const unsigned one = icdf[1] - icdf[4] + icdf[5] + icdf[8] - icdf[7];
            symbols_.boolean_inverse(true, one);
            partition(x, y, level + 1);
            partition(x + half, y, level + 1);
        } else {
            const unsigned one = icdf[0] - icdf[1] + icdf[2] - icdf[6] + icdf[7] - icdf[8];
            symbols_.boolean_inverse(true, one);
            partition(x, y, level + 1);
            partition(x, y + half, level + 1);
        }
    }

    void set_partition(int x8, int y8, int size8, int level) {
        for (int i = 0; i < size8; ++i) {
            above_partition_[static_cast<std::size_t>(x8) + static_cast<std::size_t>(i)] =
                kPartitionContextAbove[level - 1];
            left_partition_[static_cast<std::size_t>(y8) + static_cast<std::size_t>(i)] =
                kPartitionContextLeft[level - 1];
        }
    }

    [[nodiscard]] int luma(int x, int y) const {
        return picture_
            .y[(static_cast<std::size_t>(y) * picture_.width) + static_cast<std::size_t>(x)];
    }
    [[nodiscard]] Pair chroma(int x, int y) const {
        const std::size_t at =
            (static_cast<std::size_t>(y) * (picture_.width / 2)) + static_cast<std::size_t>(x);
        return {picture_.u[at], picture_.v[at]};
    }

    // Whether a block of size4 has few enough colors to come out exactly.
    [[nodiscard]] bool fits(int x, int y, int size4) const {
        const int size = size4 * 4;
        std::vector<int> ys;
        for (int py = 0; py < size; ++py) {
            for (int px = 0; px < size; ++px) {
                const int value = luma((x * 4) + px, (y * 4) + py);
                if (std::find(ys.begin(), ys.end(), value) == ys.end()) {
                    ys.push_back(value);
                    if (ys.size() > 8) {
                        return false;
                    }
                }
            }
        }
        std::vector<Pair> uvs;
        for (int py = 0; py < size / 2; ++py) {
            for (int px = 0; px < size / 2; ++px) {
                const Pair value = chroma((x * 2) + px, (y * 2) + py);
                if (std::find(uvs.begin(), uvs.end(), value) == uvs.end()) {
                    uvs.push_back(value);
                    if (uvs.size() > 8) {
                        return false;
                    }
                }
            }
        }
        return true;
    }

    // decode_b for a skipped DC_PRED block with luma and chroma palettes.
    void block(int x, int y, int size4) {
        const int size = size4 * 4;
        const auto ax = static_cast<std::size_t>(x);
        const auto ly = static_cast<std::size_t>(y % 16);
        // skip
        symbols_.boolean(
            true, kSkipCdf[static_cast<int>(above_[ax].skip) + static_cast<int>(left_[ly].skip)]);
        // y mode and uv mode: DC_PRED, CfL allowed (lossless) only for 4x4 chroma.
        symbols_.symbol(0, &kKfDcYModeCdf, 13);
        if (size4 == 2) {
            symbols_.symbol(0, &kDcUvModeCflCdf, 14);
        } else {
            symbols_.symbol(0, &kDcUvModeCdf, 13);
        }
        // Palettes.
        std::vector<int> ys;
        for (int py = 0; py < size; ++py) {
            for (int px = 0; px < size; ++px) {
                ys.push_back(luma((x * 4) + px, (y * 4) + py));
            }
        }
        std::vector<Pair> uvs;
        for (int py = 0; py < size / 2; ++py) {
            for (int px = 0; px < size / 2; ++px) {
                uvs.push_back(chroma((x * 2) + px, (y * 2) + py));
            }
        }
        std::vector<int> y_palette = choose_palette(ys);
        if (y_palette.size() == 1) {
            const int other = y_palette[0] == 255 ? 254 : y_palette[0] + 1;
            y_palette.push_back(other);
            std::sort(y_palette.begin(), y_palette.end());
        }
        std::vector<Pair> uv_palette = choose_palette(uvs);
        if (uv_palette.size() == 1) {
            uv_palette.push_back(uv_palette[0]);
        }
        const int size_ctx = (2 * floor_log2(static_cast<unsigned>(size4))) - 2;
        const int palette_ctx =
            static_cast<int>(above_[ax].y_size > 0) + static_cast<int>(left_[ly].y_size > 0);
        symbols_.boolean(true, kPaletteYCdf[size_ctx][palette_ctx]);
        write_palette(0, size_ctx, x, y, y_palette);
        symbols_.boolean(true, kPaletteUvCdf[1]);
        std::vector<int> u_palette;
        u_palette.reserve(uv_palette.size());
        for (const Pair& pair : uv_palette) {
            u_palette.push_back(pair.u);
        }
        write_palette(1, size_ctx, x, y, u_palette);
        // V: each color as a literal.
        symbols_.literal(0, 1);
        for (const Pair& pair : uv_palette) {
            symbols_.literal(static_cast<unsigned>(pair.v), 8);
        }
        // The color maps.
        std::vector<int> y_map;
        y_map.reserve(ys.size());
        for (const int value : ys) {
            y_map.push_back(nearest(y_palette, value));
        }
        write_map(0, static_cast<int>(y_palette.size()), size, y_map);
        std::vector<int> uv_map;
        uv_map.reserve(uvs.size());
        for (const Pair& value : uvs) {
            uv_map.push_back(nearest(uv_palette, value));
        }
        write_map(1, static_cast<int>(uv_palette.size()), size / 2, uv_map);
        // What the next blocks see.
        Edge edge;
        edge.skip = true;
        edge.y_size = static_cast<int>(y_palette.size());
        edge.uv_size = static_cast<int>(uv_palette.size());
        std::copy(y_palette.begin(), y_palette.end(), edge.y.begin());
        std::copy(u_palette.begin(), u_palette.end(), edge.u.begin());
        for (int i = 0; i < size4; ++i) {
            above_[ax + static_cast<std::size_t>(i)] = edge;
            left_[ly + static_cast<std::size_t>(i)] = edge;
        }
    }

    // read_pal_plane, for Y (plane 0) or U (1): the size, which colors of the neighbors'
    // palettes it reuses, then the rest in ascending order as a first color and deltas.
    void write_palette(int plane, int size_ctx, int x, int y, const std::vector<int>& palette) {
        const auto count = static_cast<unsigned>(palette.size());
        symbols_.symbol(count - 2, kPaletteSizeCdf[plane][size_ctx], 7);
        const Edge& left = left_[static_cast<std::size_t>(y % 16)];
        const Edge& above = above_[static_cast<std::size_t>(x)];
        int l_count = plane == 0 ? left.y_size : left.uv_size;
        // Not the palette above outside this 64x64 superblock.
        int a_count = 0;
        if ((y & 15) != 0) {
            a_count = plane == 0 ? above.y_size : above.uv_size;
        }
        const int* l = plane == 0 ? left.y.data() : left.u.data();
        const int* a = plane == 0 ? above.y.data() : above.u.data();
        std::vector<int> cache;
        while (l_count > 0 && a_count > 0) {
            if (*l < *a) {
                if (cache.empty() || cache.back() != *l) {
                    cache.push_back(*l);
                }
                ++l;
                --l_count;
            } else {
                if (*a == *l) {
                    ++l;
                    --l_count;
                }
                if (cache.empty() || cache.back() != *a) {
                    cache.push_back(*a);
                }
                ++a;
                --a_count;
            }
        }
        for (; l_count > 0; --l_count, ++l) {
            if (cache.empty() || cache.back() != *l) {
                cache.push_back(*l);
            }
        }
        for (; a_count > 0; --a_count, ++a) {
            if (cache.empty() || cache.back() != *a) {
                cache.push_back(*a);
            }
        }
        std::vector<int> rest = palette;
        std::size_t used = 0;
        for (std::size_t n = 0; n < cache.size() && used < palette.size(); ++n) {
            const auto found = std::find(rest.begin(), rest.end(), cache[n]);
            const bool reuse = found != rest.end();
            symbols_.literal(reuse ? 1U : 0U, 1);
            if (reuse) {
                rest.erase(found);
                ++used;
            }
        }
        if (rest.empty()) {
            return;
        }
        const int step = plane == 0 ? 1 : 0;
        constexpr int kMax = 255;
        symbols_.literal(static_cast<unsigned>(rest[0]), 8);
        if (rest.size() == 1) {
            return;
        }
        int widest = 0;
        for (std::size_t i = 1; i < rest.size(); ++i) {
            widest = std::max(widest, rest[i] - rest[i - 1] - step);
        }
        int extra = 0;
        while (extra < 3 && widest >= (1 << (5 + extra))) {
            ++extra;
        }
        symbols_.literal(static_cast<unsigned>(extra), 2);
        int bits = 5 + extra;
        int prev = rest[0];
        for (std::size_t i = 1; i < rest.size(); ++i) {
            symbols_.literal(static_cast<unsigned>(rest[i] - prev - step), bits);
            prev = rest[i];
            if (prev + step >= kMax) {
                break;
            }
            bits = std::min(bits, 1 + floor_log2(static_cast<unsigned>(kMax - prev - step)));
        }
    }

    // read_pal_indices: the first index, then the rest along anti-diagonals, each as its place
    // in an order made from the indices above, left and above-left (order_palette).
    void write_map(int plane, int count, int size, const std::vector<int>& map) {
        const auto at = [&](int x, int y) {
            return map[(static_cast<std::size_t>(y) * static_cast<std::size_t>(size)) +
                       static_cast<std::size_t>(x)];
        };
        symbols_.uniform(static_cast<unsigned>(at(0, 0)), static_cast<unsigned>(count));
        for (int i = 1; i < (2 * size) - 1; ++i) {
            const int first = std::min(i, size - 1);
            const int last = std::max(0, i - size + 1);
            for (int j = first; j >= last; --j) {
                const int x = j;
                const int y = i - j;
                std::array<int, 8> order{};
                int filled = 0;
                unsigned mask = 0;
                const auto add = [&](int index) {
                    order.at(static_cast<std::size_t>(filled++)) = index;
                    mask |= 1U << static_cast<unsigned>(index);
                };
                int ctx = 0;
                if (x == 0) {
                    add(at(x, y - 1));
                } else if (y == 0) {
                    add(at(x - 1, y));
                } else {
                    const int l = at(x - 1, y);
                    const int t = at(x, y - 1);
                    const int tl = at(x - 1, y - 1);
                    const bool same_t_l = t == l;
                    const bool same_t_tl = t == tl;
                    const bool same_l_tl = l == tl;
                    if (same_t_l && same_t_tl && same_l_tl) {
                        ctx = 4;
                        add(t);
                    } else if (same_t_l) {
                        ctx = 3;
                        add(t);
                        add(tl);
                    } else if (same_t_tl || same_l_tl) {
                        ctx = 2;
                        add(tl);
                        add(same_t_tl ? l : t);
                    } else {
                        ctx = 1;
                        add(std::min(t, l));
                        add(std::max(t, l));
                        add(tl);
                    }
                }
                for (int index = 0; index < 8; ++index) {
                    if ((mask & (1U << static_cast<unsigned>(index))) == 0) {
                        order.at(static_cast<std::size_t>(filled++)) = index;
                    }
                }
                const int value = at(x, y);
                const auto place = static_cast<unsigned>(
                    std::find(order.begin(), order.end(), value) - order.begin());
                symbols_.symbol(place, kColorMapCdf[plane][count - 2][ctx],
                                static_cast<unsigned>(count));
            }
        }
    }

    const Yuv420& picture_;
    int width4_;
    int height4_;
    SymbolWriter symbols_;
    std::vector<Edge> above_;
    std::vector<Edge> left_;
    std::vector<std::uint8_t> above_partition_;
    std::vector<std::uint8_t> left_partition_;
};

void put_leb128(std::vector<std::uint8_t>& out, std::size_t value) {
    do {
        auto byte = static_cast<std::uint8_t>(value & 0x7FU);
        value >>= 7U;
        if (value != 0) {
            byte |= 0x80U;
        }
        out.push_back(byte);
    } while (value != 0);
}

void put_obu(std::vector<std::uint8_t>& out, int type, std::span<const std::uint8_t> payload) {
    // obu_type, obu_has_size_field.
    out.push_back(static_cast<std::uint8_t>((static_cast<unsigned>(type) << 3U) | 0x2U));
    put_leb128(out, payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
}

std::vector<std::uint8_t> sequence_header(std::uint32_t width, std::uint32_t height) {
    BitWriter bits;
    bits.put(0, 3);  // seq_profile: 8-bit 4:2:0
    bits.put(0, 1);  // still_picture
    bits.put(0, 1);  // reduced_still_picture_header
    bits.put(0, 1);  // timing_info_present_flag
    bits.put(0, 1);  // initial_display_delay_present_flag
    bits.put(0, 5);  // operating_points_cnt_minus_1
    bits.put(0, 12); // operating_point_idc
    bits.put(31, 5); // seq_level_idx: no level's limits
    bits.put(0, 1);  // seq_tier
    const int width_bits = std::max(1, static_cast<int>(std::bit_width(width - 1)));
    const int height_bits = std::max(1, static_cast<int>(std::bit_width(height - 1)));
    bits.put(static_cast<std::uint32_t>(width_bits - 1), 4);
    bits.put(static_cast<std::uint32_t>(height_bits - 1), 4);
    bits.put(width - 1, width_bits);
    bits.put(height - 1, height_bits);
    bits.put(0, 1); // frame_id_numbers_present_flag
    bits.put(0, 1); // use_128x128_superblock
    bits.put(0, 1); // enable_filter_intra
    bits.put(0, 1); // enable_intra_edge_filter
    bits.put(0, 1); // enable_interintra_compound
    bits.put(0, 1); // enable_masked_compound
    bits.put(0, 1); // enable_warped_motion
    bits.put(0, 1); // enable_dual_filter
    bits.put(0, 1); // enable_order_hint
    bits.put(0, 1); // seq_choose_screen_content_tools
    bits.put(1, 1); // seq_force_screen_content_tools
    bits.put(0, 1); // seq_choose_integer_mv
    bits.put(0, 1); // seq_force_integer_mv
    bits.put(0, 1); // enable_superres
    bits.put(0, 1); // enable_cdef
    bits.put(0, 1); // enable_restoration
    bits.put(0, 1); // high_bitdepth
    bits.put(0, 1); // mono_chrome
    bits.put(1, 1); // color_description_present_flag
    bits.put(1, 8); // color_primaries: BT.709
    bits.put(1, 8); // transfer_characteristics: BT.709
    bits.put(1, 8); // matrix_coefficients: BT.709
    bits.put(0, 1); // color_range: limited
    bits.put(0, 2); // chroma_sample_position: unknown
    bits.put(0, 1); // separate_uv_delta_q
    bits.put(0, 1); // film_grain_params_present
    bits.trailing();
    return bits.take();
}

std::vector<std::uint8_t> frame(const Yuv420& picture) {
    BitWriter bits;
    bits.put(0, 1); // show_existing_frame
    bits.put(0, 2); // frame_type: KEY_FRAME
    bits.put(1, 1); // show_frame
    bits.put(1, 1); // disable_cdf_update
    bits.put(0, 1); // frame_size_override_flag
    bits.put(0, 1); // render_and_frame_size_different
    bits.put(0, 1); // allow_intrabc
    // tile_info: uniform spacing, one tile.
    bits.put(1, 1);
    const unsigned sb_cols = (picture.width + 63) / 64;
    const unsigned sb_rows = (picture.height + 63) / 64;
    if (sb_cols > 1) {
        bits.put(0, 1); // increment_tile_cols_log2
    }
    if (sb_rows > 1) {
        bits.put(0, 1); // increment_tile_rows_log2
    }
    bits.put(0, 8); // base_q_idx: lossless
    bits.put(0, 1); // DeltaQYDc
    bits.put(0, 1); // DeltaQUDc
    bits.put(0, 1); // DeltaQUAc
    bits.put(0, 1); // using_qmatrix
    bits.put(0, 1); // segmentation_enabled
    bits.put(0, 1); // reduced_tx_set
    bits.align();
    std::vector<std::uint8_t> out = bits.take();
    FrameWriter writer(picture);
    const std::vector<std::uint8_t> tile = writer.tile();
    out.insert(out.end(), tile.begin(), tile.end());
    return out;
}

} // namespace

Result<std::vector<std::uint8_t>> encode_frame(const Yuv420& picture) {
    if (picture.width == 0 || picture.height == 0 || picture.width % 8 != 0 ||
        picture.height % 8 != 0) {
        return make_error(ErrorCode::InvalidArgument,
                          "video frames must be a multiple of 8 pixels wide and high, not " +
                              std::to_string(picture.width) + "x" + std::to_string(picture.height));
    }
    if (picture.width > static_cast<std::uint32_t>(kMaxWidth) ||
        picture.height > static_cast<std::uint32_t>(kMaxHeight)) {
        return make_error(ErrorCode::InvalidArgument,
                          "video frames can be at most 4096x2304, not " +
                              std::to_string(picture.width) + "x" + std::to_string(picture.height));
    }
    std::vector<std::uint8_t> out;
    put_obu(out, 2, {}); // temporal delimiter
    put_obu(out, 1, sequence_header(picture.width, picture.height));
    put_obu(out, 6, frame(picture));
    return out;
}

Result<std::vector<std::uint8_t>> encode_ivf(std::span<const render::Image> frames,
                                             std::uint32_t rate, std::uint32_t scale) {
    if (frames.empty()) {
        return make_error(ErrorCode::InvalidArgument, "a video needs at least one frame");
    }
    if (rate == 0 || scale == 0) {
        return make_error(ErrorCode::InvalidArgument, "a video's frame rate can't be 0");
    }
    std::vector<std::vector<std::uint8_t>> units;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        if (frames[i].width != frames[0].width || frames[i].height != frames[0].height) {
            return make_error(
                ErrorCode::InvalidArgument,
                "frame " + std::to_string(i) + " is " + std::to_string(frames[i].width) + "x" +
                    std::to_string(frames[i].height) + ", not " + std::to_string(frames[0].width) +
                    "x" + std::to_string(frames[0].height) + " as the first is");
        }
        auto unit = encode_frame(to_yuv420(frames[i]));
        if (!unit) {
            return unit.error();
        }
        units.push_back(std::move(unit).value());
    }
    return write_ivf(frames[0].width, frames[0].height, rate, scale, units);
}

} // namespace slash::video
