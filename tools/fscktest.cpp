//
// fscktest — proves fsck finds what it is meant to find.
//
// A checker that never complains looks exactly like a file system that never
// breaks, so this builds a small tree by hand - raw inodes, raw directory
// entries, raw bitmap bits - checks that fsck calls it consistent, and then
// breaks copies of it one way at a time and checks fsck names each break.
//
// Run it under `make asan` as well as `make`.
//

#include "vfs/bitmap.h"
#include "vfs/block_device.h"
#include "vfs/fsck.h"
#include "vfs/layout.h"
#include "vfs/mkfs.h"
#include "vfs/superblock.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>

#include <unistd.h>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

const std::string kBase = "/tmp/vfs_fscktest_" + std::to_string(getpid()) + "_base.img";
const std::string kCopy = "/tmp/vfs_fscktest_" + std::to_string(getpid()) + ".img";

constexpr uint32_t kBlocks = 1024; // a 4 MB disk

// ------------------------------------------------------------ raw editing

vfs::Inode get_inode(vfs::BlockDevice& dev, const vfs::Superblock& sb, uint32_t id) {
    vfs::Block b;
    dev.read_block(vfs::inode_block(sb, id), b);
    vfs::Inode ino;
    std::memcpy(&ino, b.data() + vfs::inode_offset(id), sizeof ino);
    return ino;
}

void put_inode(vfs::BlockDevice& dev, const vfs::Superblock& sb, uint32_t id, const vfs::Inode& ino) {
    vfs::Block b;
    dev.read_block(vfs::inode_block(sb, id), b);
    std::memcpy(b.data() + vfs::inode_offset(id), &ino, sizeof ino);
    dev.write_block(vfs::inode_block(sb, id), b);
}

void set_bit(vfs::BlockDevice& dev, uint32_t bitmap_start, uint32_t bit, bool value) {
    uint32_t block = bitmap_start + bit / vfs::BITS_PER_BLOCK;
    uint32_t i = bit % vfs::BITS_PER_BLOCK;
    vfs::Block b;
    dev.read_block(block, b);
    if (value)
        b[i / 8] = static_cast<uint8_t>(b[i / 8] | (1u << (i % 8)));
    else
        b[i / 8] = static_cast<uint8_t>(b[i / 8] & ~(1u << (i % 8)));
    dev.write_block(block, b);
}

vfs::DirEntry entry(const char* name, uint32_t id, uint8_t type) {
    vfs::DirEntry e{};
    e.inode_id = id;
    e.name_len = static_cast<uint8_t>(std::strlen(name));
    e.type = type;
    std::memcpy(e.name, name, e.name_len);
    return e;
}

void put_entry(vfs::BlockDevice& dev, uint32_t block, uint32_t slot, const vfs::DirEntry& e) {
    vfs::Block b;
    dev.read_block(block, b);
    std::memcpy(b.data() + slot * vfs::DIRENT_SIZE, &e, sizeof e);
    dev.write_block(block, b);
}

vfs::DirEntry get_entry(vfs::BlockDevice& dev, uint32_t block, uint32_t slot) {
    vfs::Block b;
    dev.read_block(block, b);
    vfs::DirEntry e;
    std::memcpy(&e, b.data() + slot * vfs::DIRENT_SIZE, sizeof e);
    return e;
}

void put_pointer(vfs::BlockDevice& dev, uint32_t block, uint32_t index, uint32_t value) {
    vfs::Block b;
    dev.read_block(block, b);
    std::memcpy(b.data() + index * sizeof(uint32_t), &value, sizeof value);
    dev.write_block(block, b);
}

vfs::Inode new_inode(uint32_t id, bool dir) {
    vfs::Inode ino{};
    ino.id = id;
    ino.mode = dir ? (vfs::MODE_DIR | vfs::DIR_PERM) : (vfs::MODE_FILE | vfs::FILE_PERM);
    ino.link_count = dir ? 2 : 1;
    ino.mtime = 1700000000;
    return ino;
}

// ------------------------------------------------------- the base tree
//
//   /            inode 1, block R         (made by mkfs)
//   /hello       inode 2, block R+1       100 bytes
//   /big         inode 3                  file blocks 0, 12 and 1036: one
//                                         direct, one through the single-
//                                         indirect block, one through the
//                                         double-indirect tree
//   /sub         inode 4, block R+8
//   /sub/inner   inode 5                  empty

struct Tree {
    vfs::Superblock sb;
    uint32_t R;          // the root directory's block
    uint32_t hello_block, sub_block, big_single;
};

Tree build_base() {
    std::remove(kBase.c_str());
    vfs::PreadDevice dev = vfs::PreadDevice::create(kBase.c_str(), kBlocks);
    Tree t;
    t.sb = vfs::mkfs(dev);
    const vfs::Superblock& sb = t.sb;
    t.R = sb.data_start;
    const uint32_t R = t.R;

    // /hello
    vfs::Inode hello = new_inode(2, false);
    hello.size = 100;
    hello.block_count = 1;
    hello.direct[0] = R + 1;
    t.hello_block = R + 1;
    put_inode(dev, sb, 2, hello);
    vfs::Block data{};
    std::memcpy(data.data(), "hello, world", 12);
    dev.write_block(R + 1, data);

    // /big
    vfs::Inode big = new_inode(3, false);
    big.size = uint64_t{1037} * vfs::BLOCK_SIZE;
    big.direct[0] = R + 2;
    big.single_indirect = R + 3;
    big.double_indirect = R + 5;
    big.block_count = 6;
    t.big_single = R + 3;
    put_inode(dev, sb, 3, big);
    vfs::Block zero{};
    for (uint32_t b = R + 2; b <= R + 7; b++)
        dev.write_block(b, zero);
    put_pointer(dev, R + 3, 0, R + 4);  // file block 12
    put_pointer(dev, R + 5, 0, R + 6);  // the double tree's first second-level block
    put_pointer(dev, R + 6, 0, R + 7);  // file block 1036

    // /sub and /sub/inner
    vfs::Inode sub = new_inode(4, true);
    sub.size = vfs::BLOCK_SIZE;
    sub.block_count = 1;
    sub.direct[0] = R + 8;
    t.sub_block = R + 8;
    put_inode(dev, sb, 4, sub);
    dev.write_block(R + 8, zero);
    put_entry(dev, R + 8, 0, entry(".", 4, vfs::FT_DIR));
    put_entry(dev, R + 8, 1, entry("..", 1, vfs::FT_DIR));
    put_entry(dev, R + 8, 2, entry("inner", 5, vfs::FT_FILE));
    put_inode(dev, sb, 5, new_inode(5, false));

    // The root: three entries, and one more link for /sub's "..".
    put_entry(dev, R, 2, entry("hello", 2, vfs::FT_FILE));
    put_entry(dev, R, 3, entry("big", 3, vfs::FT_FILE));
    put_entry(dev, R, 4, entry("sub", 4, vfs::FT_DIR));
    vfs::Inode root = get_inode(dev, sb, vfs::ROOT_INODE);
    root.link_count = 3;
    put_inode(dev, sb, vfs::ROOT_INODE, root);

    for (uint32_t id = 2; id <= 5; id++)
        set_bit(dev, sb.inode_bitmap_start, id, true);
    for (uint32_t b = R + 1; b <= R + 8; b++)
        set_bit(dev, sb.data_bitmap_start, b, true);

    dev.sync();
    return t;
}

// A fresh copy of the base image, open for editing.
vfs::PreadDevice copy_base() {
    std::remove(kCopy.c_str());
    vfs::PreadDevice from = vfs::PreadDevice::open(kBase.c_str());
    vfs::PreadDevice to = vfs::PreadDevice::create(kCopy.c_str(), from.block_count());
    vfs::Block b;
    for (uint32_t i = 0; i < from.block_count(); i++) {
        from.read_block(i, b);
        to.write_block(i, b);
    }
    return to;
}

void print_problems(const vfs::FsckReport& r) {
    for (const std::string& p : r.problems)
        std::printf("           -> %s\n", p.c_str());
}

// Breaks a copy of the base tree with `edit`, runs fsck, and passes if one of
// the problems it reports contains `expect`.
void check_finds(const char* what, const char* expect,
                 const std::function<void(vfs::BlockDevice&)>& edit) {
    vfs::PreadDevice dev = copy_base();
    edit(dev);
    vfs::FsckReport r = vfs::fsck(dev);

    bool found = false;
    for (const std::string& p : r.problems)
        found = found || p.find(expect) != std::string::npos;

    check(found, what);
    print_problems(r);
    if (!found)
        std::printf("           (expected a problem containing \"%s\")\n", expect);
}

// ------------------------------------------------------------------ checks

void check_clean_images(const Tree& t) {
    std::printf("consistent images\n");

    {
        const std::string fresh = "/tmp/vfs_fscktest_" + std::to_string(getpid()) + "_fresh.img";
        std::remove(fresh.c_str());
        vfs::PreadDevice dev = vfs::PreadDevice::create(fresh.c_str(), 65536);
        vfs::mkfs(dev);

        auto start = std::chrono::steady_clock::now();
        vfs::FsckReport r = vfs::fsck(dev);
        double ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - start).count();

        check(r.ok(), "a freshly formatted 256 MB image");
        print_problems(r);
        check(r.directories == 1 && r.files == 0 && r.inodes_used == 1 && r.blocks_used == 133,
              "and its counts: the root, no files, 133 blocks in use");
        std::printf("           (fsck took %.1f ms)\n", ms);
        std::remove(fresh.c_str());
    }

    vfs::PreadDevice dev = vfs::PreadDevice::open(kBase.c_str());
    vfs::FsckReport r = vfs::fsck(dev);
    check(r.ok(), "a hand-built tree: files, a subdirectory, single and double indirect blocks");
    print_problems(r);
    check(r.files == 3 && r.directories == 2 && r.inodes_used == 5 &&
          r.blocks_used == t.sb.data_start + 9,
          "and its counts: 3 files, 2 directories, 5 inodes");

    const std::string blank = "/tmp/vfs_fscktest_" + std::to_string(getpid()) + "_blank.img";
    std::remove(blank.c_str());
    vfs::PreadDevice raw = vfs::PreadDevice::create(blank.c_str(), kBlocks);
    vfs::FsckReport rb = vfs::fsck(raw);
    bool says_superblock = !rb.ok() && rb.problems[0].find("superblock") != std::string::npos;
    check(says_superblock, "an image that was never formatted is reported, not crashed on");
    print_problems(rb);
    std::remove(blank.c_str());
}

void check_breaks(const Tree& t) {
    std::printf("broken images\n");
    const vfs::Superblock& sb = t.sb;
    const uint32_t R = t.R;

    check_finds("a leaked block: marked in use, owned by nobody", "leaked",
        [&](vfs::BlockDevice& d) { set_bit(d, sb.data_bitmap_start, R + 20, true); });

    check_finds("a missing bit: owned, but not marked in use", "not marked in use in the data bitmap",
        [&](vfs::BlockDevice& d) { set_bit(d, sb.data_bitmap_start, t.hello_block, false); });

    check_finds("a block owned by two files", "owned twice",
        [&](vfs::BlockDevice& d) {
            vfs::Inode inner = get_inode(d, sb, 5);
            inner.direct[0] = t.hello_block;
            inner.size = 10;
            inner.block_count = 1;
            put_inode(d, sb, 5, inner);
        });

    check_finds("an indirect block that is also a file's data block", "owned twice",
        [&](vfs::BlockDevice& d) {
            vfs::Inode hello = get_inode(d, sb, 2);
            hello.direct[1] = t.big_single;
            hello.size = 2 * vfs::BLOCK_SIZE;
            hello.block_count = 2;
            put_inode(d, sb, 2, hello);
        });

    check_finds("a wrong link_count", "has link_count 2, but 1 directory entry names it",
        [&](vfs::BlockDevice& d) {
            vfs::Inode hello = get_inode(d, sb, 2);
            hello.link_count = 2;
            put_inode(d, sb, 2, hello);
        });

    check_finds("a root whose link_count forgot a subdirectory", "has link_count 2, but 3",
        [&](vfs::BlockDevice& d) {
            vfs::Inode root = get_inode(d, sb, vfs::ROOT_INODE);
            root.link_count = 2;
            put_inode(d, sb, vfs::ROOT_INODE, root);
        });

    check_finds("an inode in use that no directory names", "not reachable from the root",
        [&](vfs::BlockDevice& d) {
            put_inode(d, sb, 6, new_inode(6, false));
            set_bit(d, sb.inode_bitmap_start, 6, true);
        });

    check_finds("a \"..\" that points at the wrong directory", "\"..\" points to inode 4",
        [&](vfs::BlockDevice& d) { put_entry(d, t.sub_block, 1, entry("..", 4, vfs::FT_DIR)); });

    check_finds("a missing \".\"", "slot 0 should be \".\"",
        [&](vfs::BlockDevice& d) { put_entry(d, t.sub_block, 0, entry("x", 4, vfs::FT_DIR)); });

    check_finds("two entries with the same name", "two entries named \"hello\"",
        [&](vfs::BlockDevice& d) { put_entry(d, R, 4, entry("hello", 4, vfs::FT_DIR)); });

    check_finds("an entry whose type does not match its inode", "has type 2, but inode 2 is a file",
        [&](vfs::BlockDevice& d) { put_entry(d, R, 2, entry("hello", 2, vfs::FT_DIR)); });

    check_finds("an entry that names a free inode", "which is not in use",
        [&](vfs::BlockDevice& d) { put_entry(d, R, 5, entry("ghost", 9, vfs::FT_FILE)); });

    check_finds("an entry with a '/' in its name", "'/' or NUL",
        [&](vfs::BlockDevice& d) {
            vfs::DirEntry e = get_entry(d, R, 2);
            e.name[1] = '/';
            put_entry(d, R, 2, e);
        });

    check_finds("a directory named twice", "it must have exactly one name",
        [&](vfs::BlockDevice& d) {
            put_entry(d, R, 5, entry("sub2", 4, vfs::FT_DIR));
            vfs::Inode sub = get_inode(d, sb, 4);
            sub.link_count = 3;
            put_inode(d, sb, 4, sub);
        });

    check_finds("a block pointer past the end of the disk", "outside the data region",
        [&](vfs::BlockDevice& d) {
            vfs::Inode hello = get_inode(d, sb, 2);
            hello.direct[1] = 5000;
            hello.size = 2 * vfs::BLOCK_SIZE;
            hello.block_count = 2;
            put_inode(d, sb, 2, hello);
        });

    check_finds("a block pointer into the inode table", "outside the data region",
        [&](vfs::BlockDevice& d) { put_pointer(d, t.big_single, 1, sb.inode_table_start); });

    check_finds("a wrong block_count", "block_count is 5, but it owns 6 blocks",
        [&](vfs::BlockDevice& d) {
            vfs::Inode big = get_inode(d, sb, 3);
            big.block_count = 5;
            put_inode(d, sb, 3, big);
        });

    check_finds("a file block past the end of the file", "past the end of its",
        [&](vfs::BlockDevice& d) {
            vfs::Inode big = get_inode(d, sb, 3);
            big.size = 13 * vfs::BLOCK_SIZE;
            put_inode(d, sb, 3, big);
        });

    check_finds("a free inode that is not zeroed", "inode 7 is free but not zeroed",
        [&](vfs::BlockDevice& d) {
            vfs::Inode junk{};
            junk.mtime = 123;
            put_inode(d, sb, 7, junk);
        });

    check_finds("an inode in use with a nonsense mode", "is neither a file",
        [&](vfs::BlockDevice& d) {
            vfs::Inode inner = get_inode(d, sb, 5);
            inner.mode = 0x1234;
            put_inode(d, sb, 5, inner);
        });

    check_finds("a directory size that is not whole blocks", "not a whole number of blocks",
        [&](vfs::BlockDevice& d) {
            vfs::Inode sub = get_inode(d, sb, 4);
            sub.size = vfs::BLOCK_SIZE + 10;
            put_inode(d, sb, 4, sub);
        });

    check_finds("a hole in a directory", "has a hole at file block 1",
        [&](vfs::BlockDevice& d) {
            vfs::Inode sub = get_inode(d, sb, 4);
            sub.size = 2 * vfs::BLOCK_SIZE;
            put_inode(d, sb, 4, sub);
        });

    check_finds("a free directory slot with junk in it", "free slot 9 is not zeroed",
        [&](vfs::BlockDevice& d) {
            vfs::DirEntry e{};
            e.name[0] = 'z';
            put_entry(d, R, 9, e);
        });

    check_finds("a metadata block marked free", "metadata block 0 is not marked",
        [&](vfs::BlockDevice& d) { set_bit(d, sb.data_bitmap_start, 0, false); });

    check_finds("inode 0 handed out", "inode 0 is reserved",
        [&](vfs::BlockDevice& d) { put_inode(d, sb, 0, new_inode(0, false)); });
}

} // namespace

int main() {
    try {
        Tree t = build_base();
        check_clean_images(t);
        check_breaks(t);
        std::remove(kBase.c_str());
        std::remove(kCopy.c_str());

        std::printf("\n%s\n", failures == 0 ? "fsck: all checks passed" : "fsck: FAILURES");
        return failures == 0 ? 0 : 1;

    } catch (const std::exception& e) {
        std::fprintf(stderr, "fscktest: unexpected exception: %s\n", e.what());
        return 1;
    }
}
