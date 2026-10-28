#include "vfs/crc32.h"

#include <array>
#include <cstring>

#if defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#endif

namespace vfs {

namespace {

    // One entry per byte value: what dividing that byte by the polynomial
    // leaves behind. Built at compile time, so there is no start-up cost and
    // no question of two threads building it at once.
    constexpr std::array<uint32_t, 256> make_table() {
        std::array<uint32_t, 256> table{};
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int bit = 0; bit < 8; bit++)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        return table;
    }

    constexpr std::array<uint32_t, 256> TABLE = make_table();

} // anonymous namespace

    uint32_t crc32(const void* data, size_t n, uint32_t crc) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        uint32_t c = ~crc;

    #if defined(__ARM_FEATURE_CRC32)
        // ARMv8's CRC32 instructions divide by this same polynomial, eight
        // bytes at a time - over ten times the table's speed, and a commit
        // checksums every byte it logs. The bytes are taken lowest address
        // first, as the table loop below takes them, so the result is the
        // same either way.
        while (n >= 8) {
            uint64_t word;
            std::memcpy(&word, bytes, sizeof word);
            c = __crc32d(c, word);
            bytes += 8;
            n -= 8;
        }
    #endif

        for (size_t i = 0; i < n; i++)
            c = TABLE[(c ^ bytes[i]) & 0xFF] ^ (c >> 8);
        return ~c;
    }

} // namespace vfs
