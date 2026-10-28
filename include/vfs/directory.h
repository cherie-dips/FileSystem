#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "vfs/alloc.h"
#include "vfs/layout.h"
#include "vfs/storage.h"

namespace vfs {

    // A directory is a file of 64-byte DirEntry slots (layout.h). Every
    // function here expects the caller to hold the directory's inode lock:
    // shared to look, exclusive to change.

    struct DirItem {
        std::string name;
        uint32_t inode;
        uint8_t type;
    };

    // The inode a name points at, or 0 if there is no such entry. *type gets
    // the entry's type when found.
    uint32_t dir_lookup(Journal& j, const Inode& dir, const std::string& name,
                        uint8_t* type = nullptr);

    // Puts the entry in the first free slot. If there is none, the directory
    // grows by one zeroed block - which changes dir (size, pointers), so the
    // caller writes dir back either way. All or nothing: VFS_ENOSPC changes
    // nothing. The caller has checked the name is not there already.
    void dir_add(Journal& j, Allocator& a, Inode& dir, const std::string& name,
                 uint32_t inode_id, uint8_t type);

    // Zeroes the slot holding name. False if there is none.
    bool dir_remove(Journal& j, const Inode& dir, const std::string& name);

    // Every entry in slot order, so "." and ".." come first.
    std::vector<DirItem> dir_list(Journal& j, const Inode& dir);

    // True if nothing but "." and "..".
    bool dir_is_empty(Journal& j, const Inode& dir);

    // Writes a new directory's first block - "." then "..", the rest free -
    // into a block just allocated.
    void dir_init(Journal& j, uint32_t block, uint32_t self, uint32_t parent);

} // namespace vfs
