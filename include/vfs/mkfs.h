#pragma once

#include <cstdint>

#include "vfs/block_device.h"
#include "vfs/layout.h"

namespace vfs {
    // Formats dev as an empty file system: both bitmaps, a zeroed inode table,
    // a root directory holding "." and "..", and - last - the superblock.
    //
    // inode_count works as it does for plan_layout(). Throws before writing
    // anything if the layout does not fit. Returns the superblock it wrote.
    Superblock mkfs(BlockDevice& dev, uint32_t inode_count = 0);
}
