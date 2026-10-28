#pragma once

// fsck.h — checks an image against every rule in layout.h.

#include <cstdint>
#include <string>
#include <vector>

#include "vfs/block_device.h"

namespace vfs {

    struct FsckReport {
        std::vector<std::string> problems;  // empty means consistent
        uint32_t files = 0, directories = 0;
        uint32_t inodes_used = 0, blocks_used = 0;
        bool ok() const { return problems.empty(); }
    };

    // Reads the image straight from dev - not mounted, and not through the
    // log. If the last unmount was not clean, mount and unmount it first so the
    // log is replayed; otherwise this checks the image as the crash left it.
    FsckReport fsck(BlockDevice& dev);

} // namespace vfs
