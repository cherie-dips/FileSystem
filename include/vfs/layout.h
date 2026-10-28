#pragma once

// layout.h — the bytes on the disk.
//
// Every struct here is copied to and from disk blocks with memcpy, so each one
// must be plain data: fixed-size integers and arrays, nothing else. The
// static_asserts at the bottom are what hold that line.

#include <array>
#include <cstdint> // for uint32_t
#include <type_traits>

// On-disk integers are stored in the machine's own byte order, and every
// machine this targets is little-endian. Refusing to build anywhere else is
// better than quietly writing images that no other build can read.
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
              "the on-disk format assumes a little-endian machine");

namespace vfs {
    constexpr uint32_t BLOCK_SIZE = 4096;  // 4 KB blocks

    // One block's worth of bytes.
    //
    // Passing this around instead of a bare void* means the size is part of the
    // type, so the compiler rejects a buffer that is too small. read_block used
    // to take a void* with an unwritten promise that the caller had allocated
    // at least BLOCK_SIZE bytes; breaking that promise was a 4 KB stack smash
    // that nothing would have caught.
    using Block = std::array<uint8_t, BLOCK_SIZE>;

    // Stored little-endian, so the first four bytes of a disk image read "CFS1"
    // in a hex dump.
    constexpr uint32_t MAGIC = 0x31534643;

    constexpr uint32_t BITS_PER_BLOCK   = BLOCK_SIZE * 8;   // 32768 per bitmap block
    constexpr uint32_t INODE_SIZE       = 128;
    constexpr uint32_t INODES_PER_BLOCK = BLOCK_SIZE / INODE_SIZE;   // 32
    constexpr uint32_t DIRENT_SIZE      = 64;
    constexpr uint32_t DIRENTS_PER_BLOCK = BLOCK_SIZE / DIRENT_SIZE; // 64
    constexpr uint32_t DIRECT_PTRS      = 12;

    // mkfs gives a disk one inode for every this many blocks unless told
    // otherwise: 4096 inodes on a 256 MB disk.
    constexpr uint32_t BLOCKS_PER_INODE = 16;

    // Inode 0 is never handed out, because a directory entry whose inode_id is
    // 0 is a free slot. If the root were inode 0, its own "." and ".." would
    // read as empty. So inode 0 is marked used and left alone, and the root
    // directory is inode 1.
    constexpr uint32_t ROOT_INODE = 1;

    // Inode::mode. The type bits use the same values as S_IFREG and S_IFDIR,
    // so they can be handed to FUSE unchanged later.
    constexpr uint16_t MODE_TYPE_MASK = 0xF000;
    constexpr uint16_t MODE_FILE      = 0x8000;
    constexpr uint16_t MODE_DIR       = 0x4000;
    constexpr uint16_t MODE_PERM_MASK = 0x0FFF;

    // DirEntry::type
    constexpr uint8_t FT_FILE = 1;
    constexpr uint8_t FT_DIR  = 2;

    // Block 0. Everything else on the disk is found from here.
    //
    //   0               1..             ..              ..             ..
    //   +------------+--------------+-------------+-------------+-------------+
    //   | superblock | inode bitmap | data bitmap | inode table | data blocks |
    //   +------------+--------------+-------------+-------------+-------------+
    //
    // Each region is as many blocks as it needs for this disk's size; the
    // superblock records where each one starts. Only the first 48 bytes of
    // block 0 are used; the rest stays zero.
    struct Superblock {
        uint32_t magic;               // MAGIC, or this is not our disk
        uint32_t block_size;          // BLOCK_SIZE
        uint32_t total_blocks;
        uint32_t inode_count;         // always a whole number of inode table blocks
        uint32_t inode_bitmap_start;  // block numbers
        uint32_t data_bitmap_start;
        uint32_t inode_table_start;
        uint32_t data_start;
        uint32_t root_inode;          // ROOT_INODE
        uint32_t clean_unmount;       // 0 means we crashed: run recovery (stage 8)
        uint64_t last_checkpoint_lsn; // where recovery starts reading the log (stage 7)
    };

    struct Inode {
        uint32_t id;
        uint16_t mode;                // MODE_FILE or MODE_DIR, plus permission bits
        uint16_t link_count;          // directory entries that point here
        uint64_t size;                // bytes. 64-bit: the pointers below reach past 4 GB
        uint64_t mtime;               // seconds since the epoch
        uint32_t block_count;         // data blocks allocated to this file
        uint32_t direct[DIRECT_PTRS]; // 0 means no block: block 0 is the superblock
        uint32_t single_indirect;
        uint32_t double_indirect;
        uint32_t pad[11];             // up to INODE_SIZE
    };

    struct DirEntry {
        uint32_t inode_id;            // 0 means this slot is free
        uint8_t  name_len;
        uint8_t  type;                // FT_FILE or FT_DIR
        char     name[58];            // not NUL-terminated; name_len says how long
    };

    // has_unique_object_representations is the check that matters: it fails if
    // the compiler slipped padding between fields. Padding is never
    // initialised, so it would put whatever was on the stack onto the disk.
    static_assert(sizeof(Superblock) == 48, "Superblock changed size");
    static_assert(sizeof(Inode) == INODE_SIZE, "Inode must be exactly INODE_SIZE");
    static_assert(sizeof(DirEntry) == DIRENT_SIZE, "DirEntry must be exactly DIRENT_SIZE");
    static_assert(std::is_trivially_copyable_v<Superblock> &&
                  std::is_trivially_copyable_v<Inode> &&
                  std::is_trivially_copyable_v<DirEntry>,
                  "on-disk structs are copied with memcpy");
    static_assert(std::has_unique_object_representations_v<Superblock> &&
                  std::has_unique_object_representations_v<Inode> &&
                  std::has_unique_object_representations_v<DirEntry>,
                  "on-disk structs must not contain padding");

    constexpr uint32_t MAX_NAME_LEN   = sizeof(DirEntry::name);          // 58
    constexpr uint32_t PTRS_PER_BLOCK = BLOCK_SIZE / sizeof(uint32_t);   // 1024
    constexpr uint64_t MAX_FILE_BLOCKS =
        DIRECT_PTRS + PTRS_PER_BLOCK + uint64_t{PTRS_PER_BLOCK} * PTRS_PER_BLOCK;

    constexpr uint16_t FILE_PERM = 0644;
    constexpr uint16_t DIR_PERM  = 0755;

    // What a consistent image looks like. src/vfs.cpp keeps every rule true
    // after every operation; src/fsck.cpp checks every one of them.
    //
    //  - Inode N is in use iff bit N of the inode bitmap is set. Inode 0 is
    //    always set and never used. An inode not in use is all zeros. One in
    //    use has id == N, mode MODE_FILE | FILE_PERM or MODE_DIR | DIR_PERM,
    //    and link_count >= 1.
    //  - link_count is the number of directory entries, anywhere, holding that
    //    inode number, "." and ".." included. A file has 1 (no hard links); a
    //    directory has 2 + its number of subdirectories - the root too, since
    //    its ".." points at itself.
    //  - block_count is every block the inode owns: data blocks plus indirect
    //    blocks (single, double, and the double's second level).
    //  - A block pointer is 0 (a hole) or in [data_start, total_blocks). File
    //    block i is direct[i] for i < 12, entry i - 12 of the single-indirect
    //    block for i < 12 + 1024, and in the double-indirect tree after that.
    //    An indirect block is allocated when first needed and freed only with
    //    the rest of the file (unlink, or O_TRUNC).
    //  - A file owns no block at or past file block ceil(size / BLOCK_SIZE).
    //    Holes before that are allowed and read as zeros.
    //  - A directory's size is a whole number of blocks, with no holes. Its
    //    first block starts with "." (itself) then ".." (its parent; the
    //    root's is itself). Entries with inode_id 0 are free and all zeros.
    //    Every other entry has 1 <= name_len <= MAX_NAME_LEN, no '/' or NUL in
    //    the name, a type (FT_FILE / FT_DIR) matching the inode it names, and
    //    a name no other entry in that directory has.
    //  - Every inode in use is reachable from the root. A directory is named
    //    by exactly one entry besides its own "." and its children's "..".
    //  - Bit B of the data bitmap is set iff B < data_start or exactly one
    //    inode in use owns block B. No block is owned twice.

    // Where inode `id` lives: which block of the inode table, and how far into it.
    inline uint32_t inode_block(const Superblock& sb, uint32_t id) {
        return sb.inode_table_start + id / INODES_PER_BLOCK;
    }

    inline uint32_t inode_offset(uint32_t id) {
        return (id % INODES_PER_BLOCK) * INODE_SIZE;
    }
}
