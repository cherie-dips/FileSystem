#include "vfs/alloc.h"
#include "vfs/errors.h"

#include <algorithm>
#include <stdexcept>

namespace vfs {

namespace {

    void flip(Block& b, uint32_t bit, bool value) {
        bit %= BITS_PER_BLOCK;
        uint8_t mask = static_cast<uint8_t>(1u << (bit % 8));
        if (value)
            b[bit / 8] |= mask;
        else
            b[bit / 8] &= static_cast<uint8_t>(~mask);
    }

    void set_bit(Journal& j, uint32_t region_start, uint32_t bit, bool value) {
        j.write(region_start + bit / BITS_PER_BLOCK, false,
                [&](Block& b) { flip(b, bit, value); });
    }

    // Lets Bitmap::load read through the journal.
    class JournalReader : public BlockDevice {
        public:
            JournalReader(Journal& j, uint32_t blocks) : j_(j), blocks_(blocks) {}
            void read_block(uint32_t block_no, Block& dst) override { j_.read(block_no, dst); }
            void write_block(uint32_t, const Block&) override {
                throw std::logic_error("JournalReader is read-only");
            }
            void sync() override {}
            uint32_t block_count() const override { return blocks_; }
        private:
            Journal& j_;
            uint32_t blocks_;
    };

} // anonymous namespace

    Allocator::Allocator(Journal& j, const Superblock& sb)
        : sb_(sb), inodes_(sb.inode_count), blocks_(sb.total_blocks) {
        JournalReader reader(j, sb.total_blocks);
        inodes_.load(reader, sb.inode_bitmap_start);
        blocks_.load(reader, sb.data_bitmap_start);
    }

    uint32_t Allocator::alloc_block(Journal& j) {
        std::lock_guard<std::mutex> lk(mutex_);
        std::optional<uint32_t> b = blocks_.find_first_free();
        if (!b)
            throw FsError(VFS_ENOSPC);
        // mkfs marks every metadata block used, so this means a damaged bitmap.
        if (*b < sb_.data_start)
            throw FsError(VFS_EIO);

        blocks_.set(*b);
        set_bit(j, sb_.data_bitmap_start, *b, true);
        return *b;
    }

    uint32_t Allocator::alloc_inode(Journal& j) {
        std::lock_guard<std::mutex> lk(mutex_);
        std::optional<uint32_t> id = inodes_.find_first_free();
        if (!id)
            throw FsError(VFS_ENOSPC);
        if (*id == 0)
            throw FsError(VFS_EIO); // inode 0 is reserved and always marked

        inodes_.set(*id);
        set_bit(j, sb_.inode_bitmap_start, *id, true);
        return *id;
    }

    void Allocator::free_blocks(Journal& j, std::vector<uint32_t> blocks) {
        if (blocks.empty())
            return;
        std::sort(blocks.begin(), blocks.end());

        std::lock_guard<std::mutex> lk(mutex_);
        for (size_t k = 0; k < blocks.size(); k++) {
            uint32_t b = blocks[k];
            bool duplicate = k > 0 && blocks[k - 1] == b;
            if (duplicate || b < sb_.data_start || b >= sb_.total_blocks || !blocks_.is_set(b))
                throw FsError(VFS_EIO);
        }

        // One journal write per bitmap block, however many bits it loses.
        size_t first = 0;
        while (first < blocks.size()) {
            uint32_t map_block = blocks[first] / BITS_PER_BLOCK;
            size_t end = first;
            while (end < blocks.size() && blocks[end] / BITS_PER_BLOCK == map_block)
                end++;

            j.write(sb_.data_bitmap_start + map_block, false,
                    [&](Block& b) {
                        for (size_t k = first; k < end; k++)
                            flip(b, blocks[k], false);
                    });
            for (size_t k = first; k < end; k++)
                blocks_.clear(blocks[k]);
            first = end;
        }
    }

    void Allocator::free_inode(Journal& j, uint32_t id) {
        std::lock_guard<std::mutex> lk(mutex_);
        if (id == 0 || id >= sb_.inode_count || !inodes_.is_set(id))
            throw FsError(VFS_EIO);
        inodes_.clear(id);
        set_bit(j, sb_.inode_bitmap_start, id, false);
    }

} // namespace vfs
