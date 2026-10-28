#pragma once

// crc32.h — the checksum on every log record.

#include <cstddef>
#include <cstdint>

namespace vfs {
    // CRC-32 as zlib and PNG compute it (reflected polynomial 0xEDB88320), so
    // crc32("123456789", 9) == 0xCBF43926. Pass an earlier result as crc to
    // carry on over more bytes: crc32(b, nb, crc32(a, na)) is the CRC of a
    // followed by b.
    uint32_t crc32(const void* data, size_t n, uint32_t crc = 0);
}
