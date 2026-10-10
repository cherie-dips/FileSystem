#include "vfs/mkfs.h"
#include "vfs/bitmap.h"
#include "vfs/superblock.h"

#include <cstring>
#include <ctime>

namespace vfs {

namespace {

    DirEntry make_entry(const char* name, uint32_t inode_id, uint8_t type) {
        DirEntry e{};
        e.inode_id = inode_id;
        e.name_len = static_cast<uint8_t>(std::strlen(name));
        e.type     = type;
        std::memcpy(e.name, name, e.name_len);
        return e;
    }

} // anonymous namespace

    Superblock mkfs(BlockDevice& dev, uint32_t inode_count) {
        const Superblock sb = plan_layout(dev.block_count(), inode_count);
        const uint32_t root_block = sb.data_start;
        const Block zero{};

        // The new superblock goes on last (see the end). Wipe any old one
        // first, and sync, so that while the rest is being rewritten block 0
        // never vouches for a disk that is half old and half new.
        dev.write_block(0, zero);
        dev.sync();

        // The inode table has to be zeroed, not assumed zero: on an image that
        // held a file system before, an old inode left here would look live.
        // Data blocks can keep their old bytes - nothing points at them.
        for (uint32_t b = sb.inode_table_start; b < sb.data_start; b++)
            dev.write_block(b, zero);

        // Inode 0 is reserved (see ROOT_INODE in layout.h).
        Bitmap inodes(sb.inode_count);
        inodes.set(0);
        inodes.set(ROOT_INODE);
        inodes.store(dev, sb.inode_bitmap_start);

        // The data bitmap has a bit for every block on the disk, metadata
        // included. Marking the metadata used means no allocation can ever
        // hand out the superblock or a piece of the inode table.
        Bitmap blocks(sb.total_blocks);
        for (uint32_t b = 0; b < sb.data_start; b++)
            blocks.set(b);
        blocks.set(root_block);
        blocks.store(dev, sb.data_bitmap_start);

        // The root directory is its own parent, so "." and ".." both point
        // back at it.
        Block dir{};
        const DirEntry dot    = make_entry(".",  ROOT_INODE, FT_DIR);
        const DirEntry dotdot = make_entry("..", ROOT_INODE, FT_DIR);
        std::memcpy(dir.data(),               &dot,    sizeof dot);
        std::memcpy(dir.data() + DIRENT_SIZE, &dotdot, sizeof dotdot);
        dev.write_block(root_block, dir);

        Inode root{};
        root.id          = ROOT_INODE;
        root.mode        = MODE_DIR | 0755;
        root.link_count  = 2;           // its own "." and ".."
        root.size        = BLOCK_SIZE;  // a directory always owns whole blocks
        root.mtime       = static_cast<uint64_t>(std::time(nullptr));
        root.block_count = 1;
        root.direct[0]   = root_block;

        Block table{};  // this block of the table was zeroed just above
        std::memcpy(table.data() + inode_offset(ROOT_INODE), &root, sizeof root);
        dev.write_block(inode_block(sb, ROOT_INODE), table);

        // The superblock goes last, behind a sync. If mkfs dies part way, the
        // image has no magic number and nothing will mistake it for a file
        // system: it is either complete or obviously unformatted.
        dev.sync();
        write_superblock(dev, sb);
        dev.sync();

        return sb;
    }

} // namespace vfs
