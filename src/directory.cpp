#include "vfs/directory.h"
#include "vfs/block_map.h"
#include "vfs/errors.h"

#include <cstring>

namespace vfs {

namespace {

    DirEntry slot(const Block& b, uint32_t i) {
        DirEntry e;
        std::memcpy(&e, b.data() + i * DIRENT_SIZE, sizeof e);
        return e;
    }

    bool is_named(const DirEntry& e, const std::string& name) {
        return e.inode_id != 0 && e.name_len == name.size()
            && std::memcmp(e.name, name.data(), name.size()) == 0;
    }

    DirEntry make_entry(const std::string& name, uint32_t inode_id, uint8_t type) {
        DirEntry e{};
        e.inode_id = inode_id;
        e.name_len = static_cast<uint8_t>(name.size());
        e.type     = type;
        std::memcpy(e.name, name.data(), name.size());
        return e;
    }

    // Reads directory block k. Directories never have holes, so a missing
    // block means the image is damaged.
    uint32_t read_dir_block(Journal& j, const Inode& dir, uint64_t k, Block& b) {
        uint32_t block = bmap_read(j, dir, k);
        if (block == 0)
            throw FsError(VFS_EIO);
        j.read(block, b);
        return block;
    }

    uint64_t block_count_of(const Inode& dir) {
        return dir.size / BLOCK_SIZE;
    }

} // anonymous namespace

    uint32_t dir_lookup(Journal& j, const Inode& dir, const std::string& name, uint8_t* type) {
        Block b;
        for (uint64_t k = 0; k < block_count_of(dir); k++) {
            read_dir_block(j, dir, k, b);
            for (uint32_t s = 0; s < DIRENTS_PER_BLOCK; s++) {
                DirEntry e = slot(b, s);
                if (is_named(e, name)) {
                    if (type != nullptr)
                        *type = e.type;
                    return e.inode_id;
                }
            }
        }
        return 0;
    }

    void dir_add(Journal& j, Allocator& a, Inode& dir, const std::string& name,
                 uint32_t inode_id, uint8_t type) {
        const DirEntry e = make_entry(name, inode_id, type);
        Block b;
        uint64_t blocks = block_count_of(dir);

        for (uint64_t k = 0; k < blocks; k++) {
            uint32_t block = read_dir_block(j, dir, k, b);
            for (uint32_t s = 0; s < DIRENTS_PER_BLOCK; s++) {
                if (slot(b, s).inode_id != 0)
                    continue;
                j.write(block, false, [&](Block& w) {
                    std::memcpy(w.data() + s * DIRENT_SIZE, &e, sizeof e);
                });
                return;
            }
        }

        // Every slot is taken: grow by one block, with this entry first.
        bool fresh = false;
        uint32_t block = bmap_alloc(j, a, dir, blocks, &fresh);
        j.write(block, true, [&](Block& w) { std::memcpy(w.data(), &e, sizeof e); });
        dir.size += BLOCK_SIZE;
    }

    bool dir_remove(Journal& j, const Inode& dir, const std::string& name) {
        Block b;
        for (uint64_t k = 0; k < block_count_of(dir); k++) {
            uint32_t block = read_dir_block(j, dir, k, b);
            for (uint32_t s = 0; s < DIRENTS_PER_BLOCK; s++) {
                if (!is_named(slot(b, s), name))
                    continue;
                j.write(block, false, [&](Block& w) {
                    std::memset(w.data() + s * DIRENT_SIZE, 0, DIRENT_SIZE);
                });
                return true;
            }
        }
        return false;
    }

    std::vector<DirItem> dir_list(Journal& j, const Inode& dir) {
        std::vector<DirItem> items;
        Block b;
        for (uint64_t k = 0; k < block_count_of(dir); k++) {
            read_dir_block(j, dir, k, b);
            for (uint32_t s = 0; s < DIRENTS_PER_BLOCK; s++) {
                DirEntry e = slot(b, s);
                if (e.inode_id == 0)
                    continue;
                size_t len = e.name_len <= MAX_NAME_LEN ? e.name_len : MAX_NAME_LEN;
                items.push_back({std::string(e.name, len), e.inode_id, e.type});
            }
        }
        return items;
    }

    bool dir_is_empty(Journal& j, const Inode& dir) {
        Block b;
        for (uint64_t k = 0; k < block_count_of(dir); k++) {
            read_dir_block(j, dir, k, b);
            // Slots 0 and 1 of the first block are always "." and "..".
            for (uint32_t s = (k == 0 ? 2 : 0); s < DIRENTS_PER_BLOCK; s++)
                if (slot(b, s).inode_id != 0)
                    return false;
        }
        return true;
    }

    void dir_init(Journal& j, uint32_t block, uint32_t self, uint32_t parent) {
        const DirEntry dot    = make_entry(".", self, FT_DIR);
        const DirEntry dotdot = make_entry("..", parent, FT_DIR);
        j.write(block, true, [&](Block& w) {
            std::memcpy(w.data(), &dot, sizeof dot);
            std::memcpy(w.data() + DIRENT_SIZE, &dotdot, sizeof dotdot);
        });
    }

} // namespace vfs
