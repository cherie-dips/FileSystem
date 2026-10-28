#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

#include "vfs/bitmap.h"
#include "vfs/block_device.h"
#include "vfs/layout.h"
#include "vfs/storage.h"

namespace vfs {

    // Hands out free blocks and inodes, lowest number first. The bitmaps are
    // kept in memory for searching, and every change is made to the on-disk
    // bitmap block too, through the journal, so it commits with the operation.
    class Allocator {
        public:
            // Loads both bitmaps, through the journal - so they are exactly
            // what the cache holds, recovered or not. At mount, before any
            // operation runs.
            Allocator(Journal& j, const Superblock& sb);

            // Only the bit is marked: the caller sets up the block or inode.
            // FsError(VFS_ENOSPC) when nothing is free.
            uint32_t alloc_block(Journal& j);
            uint32_t alloc_inode(Journal& j);

            // Every block is checked before any is freed, so a bad list
            // (VFS_EIO) changes nothing.
            void free_blocks(Journal& j, std::vector<uint32_t> blocks);
            void free_inode(Journal& j, uint32_t id);

        private:
            const Superblock sb_;
            // Held across the in-memory change and the on-disk one. Otherwise
            // a free and a re-allocation of the same bit could reach the
            // bitmap block in the opposite order to the one they happened in.
            std::mutex mutex_;
            Bitmap inodes_;
            Bitmap blocks_;
    };

} // namespace vfs
