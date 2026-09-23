#pragma once

#include <array>
#include <cstdint> // for uint32_t

namespace vfs {
    constexpr uint32_t BLOCK_SIZE = 4096;  // 4 KB blocks

    // One block's worth of bytes.
    //
    // Passing this around instead of a bare void* means the size is part of the
    // type, so the compiler rejects a buffer that is too small. read_block used
    // to take a void* with an unwritten promise that the caller had allocated
    // at least BLOCK_SIZE bytes; breaking that promise was a 4 KB stack smash
    // that nothing would have caught.
    using Block = std::array<uint8_t, BLOCK_SIZE>;
}
