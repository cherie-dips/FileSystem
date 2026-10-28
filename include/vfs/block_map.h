#pragma once

#include <cstdint>

#include "vfs/alloc.h"
#include "vfs/layout.h"
#include "vfs/storage.h"

namespace vfs {

    // Which disk block holds file block `index` of ino: direct[index] for the
    // first 12, then the single-indirect block, then the double-indirect tree
    // (layout.h). 0 means a hole. FsError(VFS_EFBIG) past MAX_FILE_BLOCKS.
    uint32_t bmap_read(Journal& j, const Inode& ino, uint64_t index);

    // The same, but a hole is filled: the data block and any indirect blocks
    // missing above it are allocated, all or nothing - VFS_ENOSPC leaves ino
    // and the disk as they were. Updates ino's pointers and block_count; the
    // caller writes ino back. *fresh is true if the data block is new, so it
    // holds nothing yet and must be written with fresh = true.
    uint32_t bmap_alloc(Journal& j, Allocator& a, Inode& ino, uint64_t index, bool* fresh);

    // Frees every block ino owns, data and indirect, and zeroes its pointers
    // and block_count. Leaves size alone.
    void free_all_blocks(Journal& j, Allocator& a, Inode& ino);

} // namespace vfs
