#pragma once

#include <cstdint>

#include "vfs/layout.h"
#include "vfs/storage.h"

namespace vfs {

    // Inode `id` is 128 bytes in block inode_block(sb, id). Reads copy it out;
    // writes change only those 128 bytes, in place, so writers of different
    // inodes that share a block never undo each other.
    //
    // All three throw FsError(VFS_EIO) for id 0 or an id past the table: an
    // inode number like that can only have come from a damaged directory.
    Inode read_inode(Journal& j, const Superblock& sb, uint32_t id);
    void write_inode(Journal& j, const Superblock& sb, const Inode& ino);

    // Writes inode id back as all zeros, as layout.h wants a free inode.
    void clear_inode(Journal& j, const Superblock& sb, uint32_t id);

    inline bool is_dir(const Inode& ino) {
        return (ino.mode & MODE_TYPE_MASK) == MODE_DIR;
    }

} // namespace vfs
