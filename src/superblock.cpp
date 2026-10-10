#include "vfs/superblock.h"
#include "vfs/bitmap.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace vfs {

namespace {

    [[noreturn]] void corrupt(const std::string& what) {
        throw std::runtime_error("bad superblock: " + what);
    }

    void expect_field(const char* name, uint32_t got, uint32_t want) {
        if (got != want)
            corrupt(std::string(name) + " is " + std::to_string(got)
                    + ", expected " + std::to_string(want));
    }

} // anonymous namespace

    Superblock plan_layout(uint32_t total_blocks, uint32_t inode_count) {
        // 64-bit throughout: an absurd inode_count must be rejected, not wrap
        // around into a small one that happens to fit.
        uint64_t inodes = inode_count != 0
                        ? inode_count
                        : total_blocks / BLOCKS_PER_INODE;
        inodes = (inodes + INODES_PER_BLOCK - 1) / INODES_PER_BLOCK * INODES_PER_BLOCK;
        if (inodes < INODES_PER_BLOCK)
            inodes = INODES_PER_BLOCK;
        if (inodes > UINT32_MAX)
            throw std::invalid_argument("too many inodes: " + std::to_string(inodes));

        // Sized by Bitmap itself, so the region on disk and the bitmap that
        // loads it can never disagree about how many blocks it spans.
        uint64_t inode_bitmap_blocks = Bitmap::blocks_for(static_cast<uint32_t>(inodes));
        uint64_t data_bitmap_blocks  = Bitmap::blocks_for(total_blocks);
        uint64_t inode_table_blocks  = inodes / INODES_PER_BLOCK;

        uint64_t inode_bitmap_start = 1;
        uint64_t data_bitmap_start  = inode_bitmap_start + inode_bitmap_blocks;
        uint64_t inode_table_start  = data_bitmap_start + data_bitmap_blocks;
        uint64_t data_start         = inode_table_start + inode_table_blocks;

        // At least one data block, for the root directory.
        if (data_start >= total_blocks) {
            throw std::invalid_argument(
                std::to_string(total_blocks) + " blocks is too small for "
                + std::to_string(inodes) + " inodes: the metadata needs "
                + std::to_string(data_start) + " blocks, and the root directory one more"
            );
        }

        Superblock sb{};
        sb.magic               = MAGIC;
        sb.block_size          = BLOCK_SIZE;
        sb.total_blocks        = total_blocks;
        sb.inode_count         = static_cast<uint32_t>(inodes);
        sb.inode_bitmap_start  = static_cast<uint32_t>(inode_bitmap_start);
        sb.data_bitmap_start   = static_cast<uint32_t>(data_bitmap_start);
        sb.inode_table_start   = static_cast<uint32_t>(inode_table_start);
        sb.data_start          = static_cast<uint32_t>(data_start);
        sb.root_inode          = ROOT_INODE;
        sb.clean_unmount       = 1;
        sb.last_checkpoint_lsn = 0;
        return sb;
    }

    void validate_superblock(const Superblock& sb, uint32_t device_blocks) {
        if (sb.magic != MAGIC) {
            char hex[16];
            std::snprintf(hex, sizeof hex, "0x%08x", sb.magic);
            corrupt(std::string("magic is ") + hex + " - this is not a ConcurrentFS image");
        }

        expect_field("block_size", sb.block_size, BLOCK_SIZE);

        if (sb.total_blocks != device_blocks)
            corrupt("it says the disk has " + std::to_string(sb.total_blocks)
                    + " blocks, but the image holds " + std::to_string(device_blocks));

        expect_field("root_inode", sb.root_inode, ROOT_INODE);

        // The layout is a function of total_blocks and inode_count alone, so
        // rebuild it and compare region by region.
        Superblock want{};
        try {
            want = plan_layout(sb.total_blocks, sb.inode_count);
        } catch (const std::invalid_argument& e) {
            corrupt(e.what());
        }

        expect_field("inode_count",        sb.inode_count,        want.inode_count);
        expect_field("inode_bitmap_start", sb.inode_bitmap_start, want.inode_bitmap_start);
        expect_field("data_bitmap_start",  sb.data_bitmap_start,  want.data_bitmap_start);
        expect_field("inode_table_start",  sb.inode_table_start,  want.inode_table_start);
        expect_field("data_start",         sb.data_start,         want.data_start);
    }

    Superblock read_superblock(BlockDevice& dev) {
        Block b;
        dev.read_block(0, b);

        Superblock sb;
        std::memcpy(&sb, b.data(), sizeof sb);

        validate_superblock(sb, dev.block_count());
        return sb;
    }

    void write_superblock(BlockDevice& dev, const Superblock& sb) {
        Block b{};
        std::memcpy(b.data(), &sb, sizeof sb);
        dev.write_block(0, b);
    }

} // namespace vfs
