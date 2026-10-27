// Adapted from Crypto++ 8.9 chacha_simd.cpp, written and placed in the public
// domain by Jack Lloyd and Jeffrey Walton.

#include "chacha_internal.h"

#ifdef YSM_X64

#include <emmintrin.h>

#include <array>

namespace ysm::legacy::v3::codec::internal {
namespace {
struct Rows {
    __m128i row0;
    __m128i row1;
    __m128i row2;
    __m128i row3;
};

template <unsigned int kBits>
inline __m128i RotateLeft(__m128i value) noexcept {
    return _mm_or_si128(_mm_slli_epi32(value, kBits),
                        _mm_srli_epi32(value, 32 - kBits));
}

inline void DoubleRound(Rows& rows) noexcept {
    rows.row0 = _mm_add_epi32(rows.row0, rows.row1);
    rows.row3 = RotateLeft<16>(_mm_xor_si128(rows.row3, rows.row0));
    rows.row2 = _mm_add_epi32(rows.row2, rows.row3);
    rows.row1 = RotateLeft<12>(_mm_xor_si128(rows.row1, rows.row2));
    rows.row0 = _mm_add_epi32(rows.row0, rows.row1);
    rows.row3 = RotateLeft<8>(_mm_xor_si128(rows.row3, rows.row0));
    rows.row2 = _mm_add_epi32(rows.row2, rows.row3);
    rows.row1 = RotateLeft<7>(_mm_xor_si128(rows.row1, rows.row2));

    rows.row1 = _mm_shuffle_epi32(rows.row1, _MM_SHUFFLE(0, 3, 2, 1));
    rows.row2 = _mm_shuffle_epi32(rows.row2, _MM_SHUFFLE(1, 0, 3, 2));
    rows.row3 = _mm_shuffle_epi32(rows.row3, _MM_SHUFFLE(2, 1, 0, 3));

    rows.row0 = _mm_add_epi32(rows.row0, rows.row1);
    rows.row3 = RotateLeft<16>(_mm_xor_si128(rows.row3, rows.row0));
    rows.row2 = _mm_add_epi32(rows.row2, rows.row3);
    rows.row1 = RotateLeft<12>(_mm_xor_si128(rows.row1, rows.row2));
    rows.row0 = _mm_add_epi32(rows.row0, rows.row1);
    rows.row3 = RotateLeft<8>(_mm_xor_si128(rows.row3, rows.row0));
    rows.row2 = _mm_add_epi32(rows.row2, rows.row3);
    rows.row1 = RotateLeft<7>(_mm_xor_si128(rows.row1, rows.row2));

    rows.row1 = _mm_shuffle_epi32(rows.row1, _MM_SHUFFLE(2, 1, 0, 3));
    rows.row2 = _mm_shuffle_epi32(rows.row2, _MM_SHUFFLE(1, 0, 3, 2));
    rows.row3 = _mm_shuffle_epi32(rows.row3, _MM_SHUFFLE(0, 3, 2, 1));
}

inline __m128i AddBlockCounter(__m128i state3,
                               std::uint32_t increment) noexcept {
    return _mm_add_epi64(state3,
                         _mm_set_epi32(0, 0, 0, static_cast<int>(increment)));
}
}  // namespace

void ChaChaXor4Sse(const ChaChaState& state, Byte* bytes,
                   std::uint32_t rounds) noexcept {
    const std::array<__m128i, 4> base{
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(state.data())),
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(state.data() + 4)),
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(state.data() + 8)),
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(state.data() + 12))};
    std::array<Rows, 4> blocks{};
    for (std::size_t block = 0; block < blocks.size(); ++block) {
        blocks[block] = {
            base[0], base[1], base[2],
            AddBlockCounter(base[3], static_cast<std::uint32_t>(block))};
    }

    for (std::uint32_t round = 0; round < rounds; round += 2) {
        for (auto& block : blocks) {
            DoubleRound(block);
        }
    }

    for (std::size_t block = 0; block < blocks.size(); ++block) {
        auto* output = bytes + block * 64;
        const std::array<__m128i, 4> initial{
            base[0], base[1], base[2],
            AddBlockCounter(base[3], static_cast<std::uint32_t>(block))};
        const std::array<__m128i, 4> result{
            _mm_add_epi32(blocks[block].row0, initial[0]),
            _mm_add_epi32(blocks[block].row1, initial[1]),
            _mm_add_epi32(blocks[block].row2, initial[2]),
            _mm_add_epi32(blocks[block].row3, initial[3])};
        for (std::size_t row = 0; row < result.size(); ++row) {
            const auto input = _mm_loadu_si128(
                reinterpret_cast<const __m128i*>(output + row * 16));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(output + row * 16),
                             _mm_xor_si128(input, result[row]));
        }
    }
}
}  // namespace ysm::legacy::v3::codec::internal

#elif defined(YSM_ARM64)

#include <arm_neon.h>

#include <array>

namespace ysm::legacy::v3::codec::internal {
namespace {
struct Rows {
    uint32x4_t row0;
    uint32x4_t row1;
    uint32x4_t row2;
    uint32x4_t row3;
};

template <unsigned int kBits>
inline uint32x4_t RotateLeft(uint32x4_t value) noexcept {
    return vorrq_u32(vshlq_n_u32(value, kBits), vshrq_n_u32(value, 32 - kBits));
}

template <>
inline uint32x4_t RotateLeft<8>(uint32x4_t value) noexcept {
    constexpr std::array<std::uint8_t, 16> kMask{3,  0, 1, 2,  7,  4,  5,  6,
                                                 11, 8, 9, 10, 15, 12, 13, 14};
    return vreinterpretq_u32_u8(
        vqtbl1q_u8(vreinterpretq_u8_u32(value), vld1q_u8(kMask.data())));
}

template <>
inline uint32x4_t RotateLeft<16>(uint32x4_t value) noexcept {
    return vreinterpretq_u32_u16(vrev32q_u16(vreinterpretq_u16_u32(value)));
}

inline void DoubleRound(Rows& rows) noexcept {
    rows.row0 = vaddq_u32(rows.row0, rows.row1);
    rows.row3 = RotateLeft<16>(veorq_u32(rows.row3, rows.row0));
    rows.row2 = vaddq_u32(rows.row2, rows.row3);
    rows.row1 = RotateLeft<12>(veorq_u32(rows.row1, rows.row2));
    rows.row0 = vaddq_u32(rows.row0, rows.row1);
    rows.row3 = RotateLeft<8>(veorq_u32(rows.row3, rows.row0));
    rows.row2 = vaddq_u32(rows.row2, rows.row3);
    rows.row1 = RotateLeft<7>(veorq_u32(rows.row1, rows.row2));

    rows.row1 = vextq_u32(rows.row1, rows.row1, 1);
    rows.row2 = vextq_u32(rows.row2, rows.row2, 2);
    rows.row3 = vextq_u32(rows.row3, rows.row3, 3);

    rows.row0 = vaddq_u32(rows.row0, rows.row1);
    rows.row3 = RotateLeft<16>(veorq_u32(rows.row3, rows.row0));
    rows.row2 = vaddq_u32(rows.row2, rows.row3);
    rows.row1 = RotateLeft<12>(veorq_u32(rows.row1, rows.row2));
    rows.row0 = vaddq_u32(rows.row0, rows.row1);
    rows.row3 = RotateLeft<8>(veorq_u32(rows.row3, rows.row0));
    rows.row2 = vaddq_u32(rows.row2, rows.row3);
    rows.row1 = RotateLeft<7>(veorq_u32(rows.row1, rows.row2));

    rows.row1 = vextq_u32(rows.row1, rows.row1, 3);
    rows.row2 = vextq_u32(rows.row2, rows.row2, 2);
    rows.row3 = vextq_u32(rows.row3, rows.row3, 1);
}

inline uint32x4_t AddBlockCounter(uint32x4_t state3,
                                  std::uint64_t increment) noexcept {
    const uint64x2_t counter{increment, 0};
    return vreinterpretq_u32_u64(
        vaddq_u64(vreinterpretq_u64_u32(state3), counter));
}
}  // namespace

void ChaChaXor4Neon(const ChaChaState& state, Byte* bytes,
                    std::uint32_t rounds) noexcept {
    const std::array<uint32x4_t, 4> base{
        vld1q_u32(state.data()), vld1q_u32(state.data() + 4),
        vld1q_u32(state.data() + 8), vld1q_u32(state.data() + 12)};
    std::array<Rows, 4> blocks{};
    for (std::size_t block = 0; block < blocks.size(); ++block) {
        blocks[block] = {base[0], base[1], base[2],
                         AddBlockCounter(base[3], block)};
    }

    for (std::uint32_t round = 0; round < rounds; round += 2) {
        for (auto& block : blocks) {
            DoubleRound(block);
        }
    }

    for (std::size_t block = 0; block < blocks.size(); ++block) {
        auto* output = bytes + block * 64;
        const std::array<uint32x4_t, 4> initial{
            base[0], base[1], base[2], AddBlockCounter(base[3], block)};
        const std::array<uint32x4_t, 4> result{
            vaddq_u32(blocks[block].row0, initial[0]),
            vaddq_u32(blocks[block].row1, initial[1]),
            vaddq_u32(blocks[block].row2, initial[2]),
            vaddq_u32(blocks[block].row3, initial[3])};
        for (std::size_t row = 0; row < result.size(); ++row) {
            const auto input =
                vreinterpretq_u32_u8(vld1q_u8(output + row * 16));
            vst1q_u8(output + row * 16,
                     vreinterpretq_u8_u32(veorq_u32(input, result[row])));
        }
    }
}
}  // namespace ysm::legacy::v3::codec::internal

#endif
