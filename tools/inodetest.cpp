//
// inodetest — proves stage 3 works: inodes and block mapping.
//
// Bytes are written through the API at the file blocks where the mapping
// changes gear - the last direct pointer, the first and last single-indirect
// ones, the first double-indirect ones - and then the image is read back raw,
// pointer by pointer, without going near the code under test.
//
// Run it under `make asan` as well as `make`.
//

#include "vfs/bitmap.h"
#include "vfs/block_device.h"
#include "vfs/fsck.h"
#include "vfs/layout.h"
#include "vfs/mkfs.h"
#include "vfs/superblock.h"
#include "vfs/vfs.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

std::string g_img;
std::string g_log;

void make_image(uint32_t blocks) {
    std::remove(g_img.c_str());
    std::remove(g_log.c_str());
    vfs::PreadDevice dev = vfs::PreadDevice::create(g_img.c_str(), blocks);
    vfs::mkfs(dev);
}

bool mount() {
    VfsOptions o;
    o.fsync = false;  // a test, not a durability test: skip the slow flushes
    return vfs_mount(g_img.c_str(), g_log.c_str(), &o) == 0;
}

bool write_at(int fd, uint64_t offset, const void* data, size_t n) {
    return vfs_lseek(fd, static_cast<off_t>(offset), VFS_SEEK_SET) == static_cast<off_t>(offset)
        && vfs_write(fd, data, n) == static_cast<ssize_t>(n);
}

int read_byte_at(int fd, uint64_t offset) {
    uint8_t b = 0;
    if (vfs_lseek(fd, static_cast<off_t>(offset), VFS_SEEK_SET) != static_cast<off_t>(offset))
        return -1;
    if (vfs_read(fd, &b, 1) != 1)
        return -1;
    return b;
}

// ---- raw access: reads the image without the file system's own code

struct Raw {
    vfs::PreadDevice dev;
    vfs::Superblock sb;

    explicit Raw(const std::string& path)
        : dev(vfs::PreadDevice::open(path.c_str())), sb(vfs::read_superblock(dev)) {}

    vfs::Inode inode(uint32_t id) {
        vfs::Block b;
        dev.read_block(vfs::inode_block(sb, id), b);
        vfs::Inode ino;
        std::memcpy(&ino, b.data() + vfs::inode_offset(id), sizeof ino);
        return ino;
    }

    uint32_t ptr(uint32_t block, uint32_t i) {
        vfs::Block b;
        dev.read_block(block, b);
        uint32_t p;
        std::memcpy(&p, b.data() + i * sizeof p, sizeof p);
        return p;
    }

    uint8_t first_byte(uint32_t block) {
        vfs::Block b;
        dev.read_block(block, b);
        return b[0];
    }

    uint32_t blocks_in_use() {
        vfs::Bitmap bm(sb.total_blocks);
        bm.load(dev, sb.data_bitmap_start);
        return bm.count_set();
    }

    bool block_in_use(uint32_t block) {
        vfs::Bitmap bm(sb.total_blocks);
        bm.load(dev, sb.data_bitmap_start);
        return bm.is_set(block);
    }
};

// fsck is someone else's stage; until it exists, its verdict is skipped.
void check_fsck(const char* what) {
    vfs::PreadDevice dev = vfs::PreadDevice::open(g_img.c_str());
    vfs::FsckReport r = vfs::fsck(dev);
    if (!r.problems.empty() && r.problems[0].find("not implemented") != std::string::npos) {
        std::printf("  skip  %s (fsck is not built yet)\n", what);
        return;
    }
    for (const std::string& p : r.problems)
        std::printf("        fsck: %s\n", p.c_str());
    check(r.ok(), what);
}

// ------------------------------------------------------------------- tests

constexpr uint64_t BS = vfs::BLOCK_SIZE;

void check_boundaries() {
    std::printf("block mapping at the boundaries\n");
    make_image(2048);
    if (!mount()) { check(false, "mount"); return; }

    // 11: last direct. 12, 13: first single-indirect. 1035: last single-
    // indirect. 1036, 1037: first double-indirect.
    const uint64_t blocks[] = {11, 12, 13, 1035, 1036, 1037};
    int fd = vfs_open("/f", VFS_O_RDWR | VFS_O_CREAT);
    bool wrote = fd >= 0;
    for (uint64_t b : blocks) {
        uint8_t v = static_cast<uint8_t>(b * 7 + 1);
        wrote = wrote && write_at(fd, b * BS, &v, 1);
    }
    check(wrote, "one byte written into each of file blocks 11, 12, 13, 1035, 1036, 1037");

    bool reads = true;
    for (uint64_t b : blocks)
        reads = reads && read_byte_at(fd, b * BS) == static_cast<uint8_t>(b * 7 + 1);
    check(reads, "each reads back");

    bool holes = read_byte_at(fd, 0) == 0 && read_byte_at(fd, 10 * BS + 99) == 0
              && read_byte_at(fd, 500 * BS) == 0;
    check(holes, "the blocks in between are holes, and read as zeros");

    VfsStat st{};
    vfs_stat("/f", &st);
    check(st.size == 1037 * BS + 1, "size is one past the last byte written");
    check(st.blocks == 9, "block_count is 9: six data blocks and three indirect ones");
    vfs_close(fd);
    uint32_t id = st.inode;
    vfs_unmount();

    // Now walk the pointers by hand, straight off the image.
    Raw raw(g_img);
    vfs::Inode ino = raw.inode(id);
    bool direct_ok = ino.direct[11] != 0;
    for (int i = 0; i < 11; i++)
        direct_ok = direct_ok && ino.direct[i] == 0;
    check(direct_ok, "raw: direct[11] is set and direct[0..10] are holes");

    uint32_t single = ino.single_indirect;
    bool single_ok = single != 0 && raw.ptr(single, 0) != 0 && raw.ptr(single, 1) != 0
                  && raw.ptr(single, 1023) != 0 && raw.ptr(single, 2) == 0;
    check(single_ok, "raw: file blocks 12, 13, 1035 are entries 0, 1, 1023 of the single-indirect block");

    uint32_t top = ino.double_indirect;
    uint32_t second = top ? raw.ptr(top, 0) : 0;
    bool double_ok = top != 0 && second != 0 && raw.ptr(top, 1) == 0
                  && raw.ptr(second, 0) != 0 && raw.ptr(second, 1) != 0 && raw.ptr(second, 2) == 0;
    check(double_ok, "raw: file blocks 1036, 1037 are entries 0, 1 under the double-indirect block's entry 0");

    bool contents = single_ok && double_ok
        && raw.first_byte(ino.direct[11]) == static_cast<uint8_t>(11 * 7 + 1)
        && raw.first_byte(raw.ptr(single, 0)) == static_cast<uint8_t>(12 * 7 + 1)
        && raw.first_byte(raw.ptr(single, 1023)) == static_cast<uint8_t>(1035 * 7 + 1)
        && raw.first_byte(raw.ptr(second, 1)) == static_cast<uint8_t>(1037 * 7 + 1);
    check(contents, "raw: the bytes are in the blocks those pointers name");

    bool marked = single_ok && double_ok && raw.block_in_use(single) && raw.block_in_use(top)
               && raw.block_in_use(second) && raw.block_in_use(raw.ptr(second, 1));
    check(marked, "raw: the data and indirect blocks are all marked in the data bitmap");
}

void check_five_million() {
    std::printf("a byte at offset 5,000,000 (a double-indirect block)\n");
    make_image(2048);
    if (!mount()) { check(false, "mount"); return; }

    const uint64_t off = 5000000;
    int fd = vfs_open("/big", VFS_O_RDWR | VFS_O_CREAT);
    uint8_t v = 0x5A;
    check(fd >= 0 && write_at(fd, off, &v, 1), "written");

    VfsStat st{};
    vfs_stat("/big", &st);
    check(st.size == off + 1, "size is 5,000,001");
    check(st.blocks == 3, "3 blocks: the data, the double-indirect block, one second-level block");

    std::vector<uint8_t> all(off + 1, 0xEE);
    bool read_ok = vfs_lseek(fd, 0, VFS_SEEK_SET) == 0
                && vfs_read(fd, all.data(), all.size()) == static_cast<ssize_t>(all.size());
    bool zeros = true;
    for (size_t i = 0; i < off && zeros; i++)
        zeros = all[i] == 0;
    check(read_ok && zeros && all[off] == 0x5A,
          "read back: 5,000,000 zeros from the hole, then the byte");
    vfs_close(fd);
    vfs_unmount();

    bool again = mount();
    fd = again ? vfs_open("/big", VFS_O_RDONLY) : -1;
    check(fd >= 0 && read_byte_at(fd, off) == 0x5A, "and again after unmounting and mounting");
    vfs_close(fd);
    vfs_unmount();
    check_fsck("fsck is happy with it");
}

void check_limits() {
    std::printf("the largest file\n");
    make_image(2048);
    if (!mount()) { check(false, "mount"); return; }

    const uint64_t max = vfs::MAX_FILE_BLOCKS * BS;
    int fd = vfs_open("/edge", VFS_O_RDWR | VFS_O_CREAT);
    uint8_t v = 7;
    check(write_at(fd, max - 1, &v, 1), "the last byte the pointers can reach can be written");
    check(read_byte_at(fd, max - 1) == 7, "and read");

    vfs_lseek(fd, static_cast<off_t>(max), VFS_SEEK_SET);
    check(vfs_write(fd, &v, 1) == -VFS_EFBIG, "one byte further is VFS_EFBIG");

    uint8_t four[4] = {1, 2, 3, 4};
    vfs_lseek(fd, static_cast<off_t>(max - 2), VFS_SEEK_SET);
    check(vfs_write(fd, four, 4) == 2, "a write across the limit stops at it: a short write of 2");
    vfs_close(fd);
    vfs_unmount();
    check_fsck("fsck is happy with it");
}

void check_freeing() {
    std::printf("giving blocks back\n");
    make_image(2048);
    uint32_t baseline;
    {
        Raw raw(g_img);
        baseline = raw.blocks_in_use();
    }
    if (!mount()) { check(false, "mount"); return; }

    std::vector<uint8_t> data(3 * 1024 * 1024, 0x42); // reaches the single-indirect range and past it
    int fd = vfs_open("/a", VFS_O_WRONLY | VFS_O_CREAT);
    check(vfs_write(fd, data.data(), data.size()) == static_cast<ssize_t>(data.size()),
          "a 3 MB file is written");
    vfs_close(fd);

    fd = vfs_open("/b", VFS_O_WRONLY | VFS_O_CREAT);
    uint8_t v = 1;
    write_at(fd, 5000000, &v, 1);
    vfs_close(fd);

    fd = vfs_open("/a", VFS_O_WRONLY | VFS_O_TRUNC);
    VfsStat st{};
    vfs_stat("/a", &st);
    check(fd >= 0 && st.size == 0 && st.blocks == 0, "O_TRUNC leaves it with no size and no blocks");
    vfs_close(fd);

    check(vfs_unlink("/a") == 0 && vfs_unlink("/b") == 0, "both files are unlinked");
    vfs_unmount();

    Raw raw(g_img);
    check(raw.blocks_in_use() == baseline, "every block they had is free again");
}

void check_disk_full() {
    std::printf("running out of space\n");
    // 64 blocks: 4 of metadata and the root's block leave 59 to fill.
    make_image(64);
    uint32_t baseline;
    {
        Raw raw(g_img);
        baseline = raw.blocks_in_use();
    }
    if (!mount()) { check(false, "mount"); return; }

    std::vector<uint8_t> data(1024 * 1024, 0x33);
    int fd = vfs_open("/fill", VFS_O_WRONLY | VFS_O_CREAT);
    ssize_t n = vfs_write(fd, data.data(), data.size());
    check(n > 0 && n < static_cast<ssize_t>(data.size()) && n % static_cast<ssize_t>(BS) == 0,
          "a 1 MB write to a 256 KB disk is cut short at a block boundary");

    VfsStat st{};
    vfs_stat("/fill", &st);
    check(st.size == static_cast<uint64_t>(n), "the size says exactly what was written");
    check(vfs_write(fd, data.data(), 10) == -VFS_ENOSPC, "the next write is VFS_ENOSPC");
    vfs_close(fd);

    check(vfs_mkdir("/d") == -VFS_ENOSPC, "so is a mkdir");
    check(vfs_open("/g", VFS_O_WRONLY | VFS_O_CREAT) >= 0, "but an empty file still fits: it needs no block");
    vfs_unmount();
    check_fsck("fsck is happy after the failures");

    mount();
    check(vfs_unlink("/fill") == 0 && vfs_unlink("/g") == 0, "the files go");
    vfs_unmount();
    Raw raw(g_img);
    check(raw.blocks_in_use() == baseline, "and every block is free again");
}

} // namespace

int main() {
    g_img = "/tmp/vfs_inodetest_" + std::to_string(getpid()) + ".img";
    g_log = g_img + ".log";

    try {
        check_boundaries();
        check_five_million();
        check_limits();
        check_freeing();
        check_disk_full();
    } catch (const std::exception& e) {
        std::printf("  FAIL  unexpected exception: %s\n", e.what());
        failures++;
    }
    vfs_unmount();
    std::remove(g_img.c_str());
    std::remove(g_log.c_str());

    std::printf("\n%s\n", failures == 0 ? "stage 3: all checks passed" : "stage 3: FAILURES");
    return failures == 0 ? 0 : 1;
}
