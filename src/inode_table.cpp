#include "vfs/inode_table.h"
#include "vfs/errors.h"

#include <cstring>

namespace vfs {

namespace {

    void check_id(const Superblock& sb, uint32_t id) {
        if (id == 0 || id >= sb.inode_count)
            throw FsError(VFS_EIO);
    }

} // anonymous namespace

    Inode read_inode(Journal& j, const Superblock& sb, uint32_t id) {
        check_id(sb, id);
        Block b;
        j.read(inode_block(sb, id), b);
        Inode ino;
        std::memcpy(&ino, b.data() + inode_offset(id), sizeof ino);
        return ino;
    }

    void write_inode(Journal& j, const Superblock& sb, const Inode& ino) {
        check_id(sb, ino.id);
        j.write(inode_block(sb, ino.id), false, [&](Block& b) {
            std::memcpy(b.data() + inode_offset(ino.id), &ino, sizeof ino);
        });
    }

    void clear_inode(Journal& j, const Superblock& sb, uint32_t id) {
        check_id(sb, id);
        j.write(inode_block(sb, id), false, [&](Block& b) {
            std::memset(b.data() + inode_offset(id), 0, INODE_SIZE);
        });
    }

} // namespace vfs
