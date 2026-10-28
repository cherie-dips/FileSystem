#include "vfs/block_map.h"
#include "vfs/errors.h"

#include <cstring>
#include <vector>

namespace vfs {

namespace {

    constexpr uint64_t DOUBLE_SPAN = uint64_t{PTRS_PER_BLOCK} * PTRS_PER_BLOCK;

    uint32_t get_ptr(Journal& j, uint32_t block, uint64_t i) {
        Block b;
        j.read(block, b);
        uint32_t p;
        std::memcpy(&p, b.data() + i * sizeof p, sizeof p);
        return p;
    }

    // fresh: the indirect block is brand new, so start it from zeros rather
    // than reading whatever the disk last held there.
    void set_ptr(Journal& j, uint32_t block, bool fresh, uint64_t i, uint32_t value) {
        j.write(block, fresh, [&](Block& b) {
            std::memcpy(b.data() + i * sizeof value, &value, sizeof value);
        });
    }

    // n blocks or none: if the disk runs out part way, the ones already taken
    // go back before the error leaves.
    std::vector<uint32_t> alloc_all(Journal& j, Allocator& a, size_t n) {
        std::vector<uint32_t> got;
        try {
            while (got.size() < n)
                got.push_back(a.alloc_block(j));
        } catch (...) {
            a.free_blocks(j, got);
            throw;
        }
        return got;
    }

    // Every pointer in an indirect block that is not a hole.
    void collect(Journal& j, uint32_t block, std::vector<uint32_t>& out) {
        Block b;
        j.read(block, b);
        for (uint32_t k = 0; k < PTRS_PER_BLOCK; k++) {
            uint32_t p;
            std::memcpy(&p, b.data() + k * sizeof p, sizeof p);
            if (p != 0)
                out.push_back(p);
        }
    }

} // anonymous namespace

    uint32_t bmap_read(Journal& j, const Inode& ino, uint64_t index) {
        if (index < DIRECT_PTRS)
            return ino.direct[index];

        index -= DIRECT_PTRS;
        if (index < PTRS_PER_BLOCK)
            return ino.single_indirect ? get_ptr(j, ino.single_indirect, index) : 0;

        index -= PTRS_PER_BLOCK;
        if (index >= DOUBLE_SPAN)
            throw FsError(VFS_EFBIG);
        if (ino.double_indirect == 0)
            return 0;

        uint32_t second = get_ptr(j, ino.double_indirect, index / PTRS_PER_BLOCK);
        return second ? get_ptr(j, second, index % PTRS_PER_BLOCK) : 0;
    }

    uint32_t bmap_alloc(Journal& j, Allocator& a, Inode& ino, uint64_t index, bool* fresh) {
        *fresh = false;

        if (index < DIRECT_PTRS) {
            if (ino.direct[index] != 0)
                return ino.direct[index];
            uint32_t data = a.alloc_block(j);
            ino.direct[index] = data;
            ino.block_count += 1;
            *fresh = true;
            return data;
        }

        index -= DIRECT_PTRS;
        if (index < PTRS_PER_BLOCK) {
            if (ino.single_indirect != 0) {
                uint32_t data = get_ptr(j, ino.single_indirect, index);
                if (data != 0)
                    return data;
                data = a.alloc_block(j);
                set_ptr(j, ino.single_indirect, false, index, data);
                ino.block_count += 1;
                *fresh = true;
                return data;
            }

            std::vector<uint32_t> got = alloc_all(j, a, 2); // indirect, data
            set_ptr(j, got[0], true, index, got[1]);
            ino.single_indirect = got[0];
            ino.block_count += 2;
            *fresh = true;
            return got[1];
        }

        index -= PTRS_PER_BLOCK;
        if (index >= DOUBLE_SPAN)
            throw FsError(VFS_EFBIG);

        uint64_t i1 = index / PTRS_PER_BLOCK;
        uint64_t i2 = index % PTRS_PER_BLOCK;
        uint32_t top    = ino.double_indirect;
        uint32_t second = top ? get_ptr(j, top, i1) : 0;
        uint32_t data   = second ? get_ptr(j, second, i2) : 0;
        if (data != 0)
            return data;

        // Up to three new blocks: the top of the tree, one second-level
        // block, and the data block. Taken together, so running out of space
        // half way leaves nothing behind.
        size_t need = 1 + (second ? 0 : 1) + (top ? 0 : 1);
        std::vector<uint32_t> got = alloc_all(j, a, need);
        size_t k = 0;
        bool new_top = top == 0;
        bool new_second = second == 0;
        if (new_top)
            top = got[k++];
        if (new_second)
            second = got[k++];
        data = got[k];

        set_ptr(j, second, new_second, i2, data);
        if (new_second)
            set_ptr(j, top, new_top, i1, second);

        ino.double_indirect = top;
        ino.block_count += static_cast<uint32_t>(need);
        *fresh = true;
        return data;
    }

    void free_all_blocks(Journal& j, Allocator& a, Inode& ino) {
        std::vector<uint32_t> owned;

        for (uint32_t p : ino.direct)
            if (p != 0)
                owned.push_back(p);

        if (ino.single_indirect != 0) {
            collect(j, ino.single_indirect, owned);
            owned.push_back(ino.single_indirect);
        }

        if (ino.double_indirect != 0) {
            std::vector<uint32_t> seconds;
            collect(j, ino.double_indirect, seconds);
            for (uint32_t second : seconds) {
                collect(j, second, owned);
                owned.push_back(second);
            }
            owned.push_back(ino.double_indirect);
        }

        a.free_blocks(j, std::move(owned));

        std::memset(ino.direct, 0, sizeof ino.direct);
        ino.single_indirect = 0;
        ino.double_indirect = 0;
        ino.block_count = 0;
    }

} // namespace vfs
