#pragma once

#include <cstdint>

#include "vfs/block_device.h"
#include "vfs/layout.h"

namespace vfs {
    // Works out where every region goes on a disk of total_blocks, and returns
    // the superblock that describes it. Nothing is written.
    //
    // inode_count is rounded up to fill whole inode table blocks; 0 picks one
    // inode per BLOCKS_PER_INODE blocks. Throws std::invalid_argument if the
    // metadata, plus one block for the root directory, does not fit.
    //
    // The layout depends on nothing but these two numbers, which is what lets
    // validate_superblock() check every region of a superblock it is handed.
    Superblock plan_layout(uint32_t total_blocks, uint32_t inode_count = 0);

    // Throws std::runtime_error naming the first problem if sb is not a
    // superblock this code can use on a device of device_blocks blocks.
    void validate_superblock(const Superblock& sb, uint32_t device_blocks);

    // Reads block 0 and validates it. Anything that opens an existing image
    // goes through here, so a file that is not ours is turned away at the door.
    Superblock read_superblock(BlockDevice& dev);

    void write_superblock(BlockDevice& dev, const Superblock& sb);
}
