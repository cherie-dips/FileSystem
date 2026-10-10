//
// mkfstest — proves stage 2 works.
//
// Three things are checked. The bitmap, at the places bit twiddling goes wrong:
// word and block boundaries, the last bit, a full map, a size that is not a
// multiple of 64. The layout mkfs picks for a given disk. And what a freshly
// formatted image actually holds, read back block by block - formatted over
// junk, so nothing passes just because a new file happens to be all zeros.
//
// Run it under `make asan` as well as `make`.
//

#include "vfs/bitmap.h"
#include "vfs/block_device.h"
#include "vfs/layout.h"
#include "vfs/mkfs.h"
#include "vfs/superblock.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

// Runs `body` and passes if it threw. Used for every rejection we expect.
template <typename F>
void check_throws(F body, const char* what) {
    try {
        body();
    } catch (const std::exception& e) {
        std::printf("  ok    %s\n           -> %s\n", what, e.what());
        return;
    }
    std::printf("  FAIL  %s (no exception)\n", what);
    failures++;
}

vfs::PreadDevice fresh_device(const char* path, uint32_t blocks) {
    std::remove(path);
    return vfs::PreadDevice::create(path, blocks);
}

vfs::Inode read_inode(vfs::BlockDevice& dev, const vfs::Superblock& sb, uint32_t id) {
    vfs::Block b;
    dev.read_block(vfs::inode_block(sb, id), b);
    vfs::Inode ino;
    std::memcpy(&ino, b.data() + vfs::inode_offset(id), sizeof ino);
    return ino;
}

bool is_all_zero(const void* p, size_t n) {
    const auto* bytes = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; i++)
        if (bytes[i] != 0) return false;
    return true;
}

const char* kDiskPath  = "/tmp/vfs_mkfstest.img";
const char* kOtherPath = "/tmp/vfs_mkfstest_other.img";

constexpr uint32_t kBlocks = 1024;   // a 4 MB disk

// ------------------------------------------------------------------- bitmap

void check_bitmap_bits() {
    std::printf("bitmap: single bits\n");

    vfs::Bitmap bm(1000);
    check(bm.count_set() == 0 && bm.find_first_free() == 0u,
          "a new bitmap has every bit clear");

    bool all = true;
    for (uint32_t i : {0u, 63u, 64u, 999u}) {
        bm.set(i);
        all = all && bm.is_set(i);
    }
    check(all, "set and is_set agree at bits 0, 63, 64 and the last one");
    check(!bm.is_set(1) && !bm.is_set(62) && !bm.is_set(65) && !bm.is_set(998),
          "setting a bit leaves its neighbours alone");
    check(bm.count_set() == 4, "count_set counts them");

    bm.clear(64);
    check(!bm.is_set(64) && bm.is_set(63), "clear undoes set, and only that bit");

    check_throws([&] { bm.is_set(1000); }, "reading one bit past the end is rejected");
    check_throws([&] { bm.set(1000); },    "setting one bit past the end is rejected");
    check_throws([&] { bm.clear(0xFFFFFFFFu); }, "clearing bit 2^32-1 is rejected");
}

void check_bitmap_find() {
    std::printf("bitmap: find_first_free\n");

    vfs::Bitmap bm(1000);
    for (uint32_t i = 0; i < 64; i++) bm.set(i);
    check(bm.find_first_free() == 64u, "skips a full first word and finds bit 64");

    for (uint32_t i = 0; i < 1000; i++) bm.set(i);
    bm.clear(777);
    check(bm.find_first_free() == 777u, "finds a lone hole in the middle");

    bm.set(777);
    check(!bm.find_first_free(), "a full 1000-bit map has nothing free");

    // 100 bits is one and a half words. The bits past 100 in the second word
    // are clear but do not exist - returning one of them would hand out an
    // inode or block that is not there.
    vfs::Bitmap odd(100);
    for (uint32_t i = 0; i < 100; i++) odd.set(i);
    check(!odd.find_first_free(), "a full 100-bit map does not return bit 100");

    vfs::Bitmap even(128);
    for (uint32_t i = 0; i < 128; i++) even.set(i);
    check(!even.find_first_free(), "a full 128-bit map has nothing free");

    // One bit more than a block holds, so the last bit is on a second block.
    vfs::Bitmap two(vfs::BITS_PER_BLOCK + 1);
    for (uint32_t i = 0; i < two.size(); i++) two.set(i);
    check(!two.find_first_free(), "a full two-block map has nothing free");
    two.clear(vfs::BITS_PER_BLOCK);
    check(two.find_first_free() == vfs::BITS_PER_BLOCK,
          "and the only free bit, the first on the second block, is found");

    check(!vfs::Bitmap(0).find_first_free(), "an empty map has nothing free");
}

void check_bitmap_disk() {
    std::printf("bitmap: on disk\n");

    check(vfs::Bitmap::blocks_for(0) == 0 &&
          vfs::Bitmap::blocks_for(1) == 1 &&
          vfs::Bitmap::blocks_for(vfs::BITS_PER_BLOCK) == 1 &&
          vfs::Bitmap::blocks_for(vfs::BITS_PER_BLOCK + 1) == 2,
          "blocks_for: 0, 1, 32768 and 32769 bits take 0, 1, 1 and 2 blocks");
    check(vfs::Bitmap::blocks_for(0xFFFFFFFFu) == 131072,
          "blocks_for does not wrap around near 2^32");

    vfs::PreadDevice dev = fresh_device(kDiskPath, 16);

    vfs::Bitmap out(40000);
    for (uint32_t i : {0u, 9u, 32767u, 32768u, 39999u})
        out.set(i);
    out.store(dev, 3);

    // Pins the on-disk format: bit 9 is byte 1, bit 1.
    vfs::Block raw;
    dev.read_block(3, raw);
    check(raw[0] == 0x01 && raw[1] == 0x02, "bit i is stored in byte i/8, at bit i%8");

    vfs::Bitmap in(40000);
    in.load(dev, 3);
    check(in.is_set(0) && in.is_set(9) && in.is_set(32767) && in.is_set(32768) &&
          in.is_set(39999) && in.count_set() == 5,
          "store then load round-trips bits on both sides of a block boundary");

    // A block of all ones, loaded as a 100-bit map: the 32668 bits past the
    // end must not count, and must not be found.
    raw.fill(0xFF);
    dev.write_block(10, raw);
    vfs::Bitmap ones(100);
    ones.load(dev, 10);
    check(ones.count_set() == 100 && !ones.find_first_free(),
          "load ignores bits past the end of the map");

    std::remove(kDiskPath);
}

// ------------------------------------------------------------------- layout

void check_layout() {
    std::printf("layout\n");

    // 256 MB: 65536 blocks. Two blocks of data bitmap (32768 bits each), and
    // 4096 inodes at 32 per block is 128 blocks of inode table.
    vfs::Superblock sb = vfs::plan_layout(65536);
    check(sb.inode_count == 4096, "256 MB gets 4096 inodes");
    check(sb.inode_bitmap_start == 1 && sb.data_bitmap_start == 2 &&
          sb.inode_table_start == 4 && sb.data_start == 132,
          "256 MB: inode bitmap at 1, data bitmap at 2-3, inode table at 4-131, data from 132");

    check(vfs::plan_layout(kBlocks, 100).inode_count == 128,
          "the inode count rounds up to a whole block of inodes");
    check(vfs::plan_layout(kBlocks, 1).inode_count == 32,
          "and never goes below one block of them");

    vfs::Superblock tiny = vfs::plan_layout(5);
    check(tiny.data_start == 4, "5 blocks is just enough: 4 of metadata, 1 for the root");
    check_throws([] { vfs::plan_layout(4); }, "4 blocks is not");

    check_throws([] { vfs::plan_layout(kBlocks, 0xFFFFFFFFu); },
                 "an inode count that rounds past 2^32 is rejected, not wrapped");

    vfs::Superblock huge = vfs::plan_layout(0xFFFFFFFFu);
    check(huge.data_bitmap_start + 131072 == huge.inode_table_start &&
          huge.data_start < huge.total_blocks,
          "the largest possible disk lays out without overflowing");
}

// --------------------------------------------------------------------- mkfs

void check_mkfs() {
    std::printf("mkfs, on a disk full of junk\n");

    vfs::Superblock made;
    {
        vfs::PreadDevice dev = fresh_device(kDiskPath, kBlocks);
        vfs::Block junk;
        junk.fill(0xAB);
        for (uint32_t b = 0; b < kBlocks; b++)
            dev.write_block(b, junk);

        made = vfs::mkfs(dev);
    }

    // Everything below reads a reopened image, so it checks what reached the
    // file rather than anything mkfs still held.
    vfs::PreadDevice dev = vfs::PreadDevice::open(kDiskPath);
    vfs::Superblock sb = vfs::read_superblock(dev);
    check(std::memcmp(&sb, &made, sizeof sb) == 0,
          "the superblock reads back as mkfs wrote it");
    check(sb.clean_unmount == 1 && sb.last_checkpoint_lsn == 0,
          "a new file system is marked cleanly unmounted");

    vfs::Bitmap inodes(sb.inode_count);
    inodes.load(dev, sb.inode_bitmap_start);
    check(inodes.is_set(0) && inodes.is_set(vfs::ROOT_INODE) && inodes.count_set() == 2,
          "inode bitmap: only inode 0 (reserved) and the root are in use");

    vfs::Bitmap blocks(sb.total_blocks);
    blocks.load(dev, sb.data_bitmap_start);
    bool metadata_used = true;
    for (uint32_t b = 0; b < sb.data_start; b++)
        metadata_used = metadata_used && blocks.is_set(b);
    check(metadata_used, "data bitmap: every metadata block is marked in use");
    check(blocks.is_set(sb.data_start) && blocks.count_set() == sb.data_start + 1,
          "data bitmap: plus the root directory's block, and nothing else");
    check(blocks.find_first_free() == sb.data_start + 1,
          "so the first free block is the one after it");

    vfs::Inode root = read_inode(dev, sb, vfs::ROOT_INODE);
    check(root.id == vfs::ROOT_INODE &&
          (root.mode & vfs::MODE_TYPE_MASK) == vfs::MODE_DIR &&
          (root.mode & vfs::MODE_PERM_MASK) == 0755,
          "the root inode is a directory, mode 0755");
    check(root.link_count == 2 && root.size == vfs::BLOCK_SIZE && root.block_count == 1,
          "with two links, one block, and a size of one block");
    bool other_ptrs_zero = root.single_indirect == 0 && root.double_indirect == 0;
    for (uint32_t i = 1; i < vfs::DIRECT_PTRS; i++)
        other_ptrs_zero = other_ptrs_zero && root.direct[i] == 0;
    check(root.direct[0] == sb.data_start && other_ptrs_zero,
          "and its only block pointer is to the first data block");

    vfs::Block dir;
    dev.read_block(root.direct[0], dir);
    vfs::DirEntry dot, dotdot;
    std::memcpy(&dot,    dir.data(),                    sizeof dot);
    std::memcpy(&dotdot, dir.data() + vfs::DIRENT_SIZE, sizeof dotdot);
    check(dot.inode_id == vfs::ROOT_INODE && dot.type == vfs::FT_DIR &&
          dot.name_len == 1 && dot.name[0] == '.',
          "the root directory's first entry is \".\", pointing at itself");
    check(dotdot.inode_id == vfs::ROOT_INODE && dotdot.type == vfs::FT_DIR &&
          dotdot.name_len == 2 && std::memcmp(dotdot.name, "..", 2) == 0,
          "its second is \"..\", also pointing at itself");
    check(is_all_zero(dir.data() + 2 * vfs::DIRENT_SIZE, vfs::BLOCK_SIZE - 2 * vfs::DIRENT_SIZE),
          "and the other 62 slots are free");

    bool table_clean = true;
    for (uint32_t id = 0; id < sb.inode_count; id++) {
        if (id == vfs::ROOT_INODE) continue;
        vfs::Inode ino = read_inode(dev, sb, id);
        table_clean = table_clean && is_all_zero(&ino, sizeof ino);
    }
    check(table_clean, "every other inode is zeroed, junk and all");

    vfs::Block block0;
    dev.read_block(0, block0);
    check(is_all_zero(block0.data() + sizeof(vfs::Superblock),
                      vfs::BLOCK_SIZE - sizeof(vfs::Superblock)),
          "block 0 holds the superblock and zeros, no junk");
}

void check_mkfs_refuses() {
    std::printf("mkfs, on a disk too small\n");

    vfs::PreadDevice dev = fresh_device(kOtherPath, 4);
    vfs::Block marker;
    marker.fill(0x5A);
    dev.write_block(0, marker);

    check_throws([&] { vfs::mkfs(dev); }, "a 4-block disk is refused");

    vfs::Block after;
    dev.read_block(0, after);
    check(after == marker, "and nothing on it was touched");

    std::remove(kOtherPath);
}

// --------------------------------------------------------------- superblock

// Writes a modified copy of the superblock in kDiskPath to a scratch image of
// `blocks` blocks, then tries to read it back.
template <typename Edit>
void check_rejected(uint32_t blocks, Edit edit, const char* what) {
    vfs::Superblock sb;
    {
        vfs::PreadDevice good = vfs::PreadDevice::open(kDiskPath);
        sb = vfs::read_superblock(good);
    }
    edit(sb);

    vfs::PreadDevice dev = fresh_device(kOtherPath, blocks);
    vfs::write_superblock(dev, sb);
    check_throws([&] { vfs::read_superblock(dev); }, what);
    std::remove(kOtherPath);
}

void check_superblock_rejections() {
    std::printf("read_superblock turns away\n");

    {
        vfs::PreadDevice blank = fresh_device(kOtherPath, kBlocks);
        check_throws([&] { vfs::read_superblock(blank); }, "an image that was never formatted");
        std::remove(kOtherPath);
    }

    check_rejected(kBlocks, [](vfs::Superblock& sb) { sb.magic ^= 1; },
                   "a magic number one bit off");
    check_rejected(kBlocks / 2, [](vfs::Superblock&) {},
                   "a superblock from a 4 MB disk on a 2 MB image");
    check_rejected(kBlocks, [](vfs::Superblock& sb) { sb.block_size = 1024; },
                   "a different block size");
    check_rejected(kBlocks, [](vfs::Superblock& sb) { sb.data_start++; },
                   "a region that has moved by one block");
    check_rejected(kBlocks, [](vfs::Superblock& sb) { sb.inode_count = 100; },
                   "an inode count that is not whole inode table blocks");
    check_rejected(kBlocks, [](vfs::Superblock& sb) { sb.root_inode = 0; },
                   "a root inode other than 1");
}

} // namespace

int main() {
    try {
        check_bitmap_bits();
        check_bitmap_find();
        check_bitmap_disk();
        check_layout();
        check_mkfs();
        check_mkfs_refuses();
        check_superblock_rejections();
        std::remove(kDiskPath);

        std::printf("\n%s\n", failures == 0
                                  ? "stage 2: all checks passed"
                                  : "stage 2: FAILURES");
        return failures == 0 ? 0 : 1;

    } catch (const std::exception& e) {
        std::fprintf(stderr, "mkfstest: unexpected exception: %s\n", e.what());
        return 1;
    }
}
