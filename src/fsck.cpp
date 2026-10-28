#include "vfs/fsck.h"
#include "vfs/bitmap.h"
#include "vfs/layout.h"
#include "vfs/superblock.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

// The checklist is OSTEP 42.2 (pp. 529-530), made concrete by the rules in
// layout.h. Everything is read straight from the device and decoded here, with
// no code shared with the file system that wrote the image - a checker that
// trusted the writer's own block mapping would agree with the writer's bugs.

namespace vfs {

namespace {

    // Past this many, problems are counted rather than listed: a badly damaged
    // image would otherwise produce one line per block.
    constexpr size_t MAX_LISTED = 200;

    enum class Kind : uint8_t { none, file, dir, bad };

    // Which pointer of an inode a block number came from, for messages.
    enum class Ptr : uint8_t { data, single, dbl, dbl2 };

    using Pointers = std::array<uint32_t, PTRS_PER_BLOCK>;

    std::string str(uint64_t n) { return std::to_string(n); }

    class Checker {
        public:
            Checker(BlockDevice& dev, FsckReport& report)
                : dev_(dev), r_(report), inode_bm_(0), block_bm_(0) {}

            void run() {
                if (!check_superblock())
                    return;

                inode_bm_ = Bitmap(sb_.inode_count);
                inode_bm_.load(dev_, sb_.inode_bitmap_start);
                block_bm_ = Bitmap(sb_.total_blocks);
                block_bm_.load(dev_, sb_.data_bitmap_start);

                load_inodes();
                check_inodes();
                walk_tree();
                check_links();
                check_data_bitmap();

                r_.blocks_used = block_bm_.count_set();
                if (suppressed_ > 0)
                    r_.problems.push_back("... and " + str(suppressed_) + " more problems");
            }

        private:
            BlockDevice& dev_;
            FsckReport& r_;
            Superblock sb_{};
            Bitmap inode_bm_, block_bm_;
            size_t suppressed_ = 0;

            std::vector<Inode> inodes_;
            std::vector<Kind> kind_;                     // per inode
            std::vector<uint32_t> owner_;                // per block: the inode that owns it, or 0
            std::vector<std::vector<uint32_t>> dir_blocks_; // per directory: its data blocks, 0 for a hole
            std::vector<uint32_t> refs_;                 // entries naming the inode, "." and ".." included
            std::vector<uint32_t> names_;                // entries naming it, "." and ".." not included
            std::vector<uint8_t> reachable_;

            void problem(std::string what) {
                if (r_.problems.size() < MAX_LISTED)
                    r_.problems.push_back(std::move(what));
                else
                    suppressed_++;
            }

            void read(uint32_t block_no, Block& b) { dev_.read_block(block_no, b); }

            void read_pointers(uint32_t block_no, Pointers& p) {
                Block b;
                read(block_no, b);
                std::memcpy(p.data(), b.data(), BLOCK_SIZE);
            }

            // ------------------------------------------------------ superblock

            bool check_superblock() {
                Block b;
                read(0, b);
                std::memcpy(&sb_, b.data(), sizeof sb_);

                try {
                    validate_superblock(sb_, dev_.block_count());
                } catch (const std::exception& e) {
                    problem(std::string("superblock: ") + e.what());
                    return false;
                }

                for (size_t i = sizeof sb_; i < BLOCK_SIZE; i++) {
                    if (b[i] != 0) {
                        problem("superblock: block 0 has data after the superblock, at byte " + str(i));
                        break;
                    }
                }
                return true;
            }

            void load_inodes() {
                inodes_.resize(sb_.inode_count);
                Block b;
                for (uint32_t t = 0; t < sb_.inode_count / INODES_PER_BLOCK; t++) {
                    read(sb_.inode_table_start + t, b);
                    std::memcpy(&inodes_[static_cast<size_t>(t) * INODES_PER_BLOCK],
                                b.data(), BLOCK_SIZE);
                }

                kind_.assign(sb_.inode_count, Kind::none);
                owner_.assign(sb_.total_blocks, 0);
                dir_blocks_.assign(sb_.inode_count, {});
                refs_.assign(sb_.inode_count, 0);
                names_.assign(sb_.inode_count, 0);
                reachable_.assign(sb_.inode_count, 0);
            }

            // ---------------------------------------------------------- inodes

            void check_inodes() {
                static const Inode zero{};

                for (uint32_t id = 0; id < sb_.inode_count; id++) {
                    const Inode& ino = inodes_[id];
                    bool marked = inode_bm_.is_set(id);
                    bool is_zero = std::memcmp(&ino, &zero, sizeof ino) == 0;

                    if (id == 0) {
                        if (!marked)
                            problem("inode 0 is reserved but not marked in the inode bitmap");
                        if (!is_zero)
                            problem("inode 0 is reserved and should be all zeros");
                        continue;
                    }

                    if (!marked) {
                        if (!is_zero)
                            problem("inode " + str(id) + " is free but not zeroed");
                        continue;
                    }

                    r_.inodes_used++;

                    if (ino.id != id)
                        problem("inode " + str(id) + ": its id field says " + str(ino.id));

                    if (ino.mode == (MODE_FILE | FILE_PERM)) {
                        kind_[id] = Kind::file;
                    } else if (ino.mode == (MODE_DIR | DIR_PERM)) {
                        kind_[id] = Kind::dir;
                    } else {
                        char mode[16];
                        std::snprintf(mode, sizeof mode, "0%o", static_cast<unsigned>(ino.mode));
                        problem("inode " + str(id) + ": mode " + mode
                                + " is neither a file (0100644) nor a directory (040755)");
                        kind_[id] = Kind::bad;
                    }

                    if (ino.link_count == 0)
                        problem("inode " + str(id) + " is in use but has link_count 0");

                    check_blocks(id);
                }

                if (sb_.inode_count <= ROOT_INODE || kind_[ROOT_INODE] != Kind::dir)
                    problem("the root, inode " + str(ROOT_INODE) + ", is not a directory in use");
            }

            std::string describe(Ptr p, uint64_t index) {
                switch (p) {
                    case Ptr::data:   return "file block " + str(index);
                    case Ptr::single: return "its single-indirect block";
                    case Ptr::dbl:    return "its double-indirect block";
                    case Ptr::dbl2:   return "entry " + str(index) + " of its double-indirect block";
                }
                return "?";
            }

            // Records that inode id owns block b. False if b is out of range or
            // already owned, in which case it must not be read as this inode's.
            bool claim(uint32_t id, uint32_t b, Ptr p, uint64_t index) {
                if (b < sb_.data_start || b >= sb_.total_blocks) {
                    problem("inode " + str(id) + ": " + describe(p, index) + " points to block "
                            + str(b) + ", outside the data region " + str(sb_.data_start)
                            + "-" + str(sb_.total_blocks - 1));
                    return false;
                }
                if (owner_[b] == id) {
                    problem("inode " + str(id) + " points to block " + str(b) + " twice ("
                            + describe(p, index) + ")");
                    return false;
                }
                if (owner_[b] != 0) {
                    problem("block " + str(b) + " is owned twice: by inode " + str(owner_[b])
                            + " and by inode " + str(id) + " (" + describe(p, index) + ")");
                    return false;
                }
                owner_[b] = id;
                return true;
            }

            void check_blocks(uint32_t id) {
                const Inode& ino = inodes_[id];
                const bool is_dir = kind_[id] == Kind::dir;
                std::vector<uint32_t>& dir_map = dir_blocks_[id];

                uint64_t owned = 0;
                uint64_t end = 0;  // one past the last file block that has a block

                auto data = [&](uint64_t index, uint32_t b) {
                    owned++;
                    bool mine = claim(id, b, Ptr::data, index);
                    end = std::max(end, index + 1);
                    if (is_dir && mine && index < (uint64_t{1} << 20)) {
                        if (dir_map.size() <= index)
                            dir_map.resize(index + 1, 0);
                        dir_map[index] = b;
                    }
                };

                for (uint32_t i = 0; i < DIRECT_PTRS; i++)
                    if (ino.direct[i] != 0)
                        data(i, ino.direct[i]);

                Pointers p;
                if (ino.single_indirect != 0) {
                    owned++;
                    if (claim(id, ino.single_indirect, Ptr::single, 0)) {
                        read_pointers(ino.single_indirect, p);
                        for (uint32_t j = 0; j < PTRS_PER_BLOCK; j++)
                            if (p[j] != 0)
                                data(DIRECT_PTRS + j, p[j]);
                    }
                }

                if (ino.double_indirect != 0) {
                    owned++;
                    if (claim(id, ino.double_indirect, Ptr::dbl, 0)) {
                        Pointers top;
                        read_pointers(ino.double_indirect, top);
                        for (uint32_t j = 0; j < PTRS_PER_BLOCK; j++) {
                            if (top[j] == 0)
                                continue;
                            owned++;
                            if (!claim(id, top[j], Ptr::dbl2, j))
                                continue;
                            read_pointers(top[j], p);
                            for (uint32_t k = 0; k < PTRS_PER_BLOCK; k++)
                                if (p[k] != 0)
                                    data(DIRECT_PTRS + PTRS_PER_BLOCK
                                         + uint64_t{j} * PTRS_PER_BLOCK + k, p[k]);
                        }
                    }
                }

                if (owned != ino.block_count)
                    problem("inode " + str(id) + ": block_count is " + str(ino.block_count)
                            + ", but it owns " + str(owned) + " blocks");

                if (ino.size > MAX_FILE_BLOCKS * BLOCK_SIZE)
                    problem("inode " + str(id) + ": size " + str(ino.size)
                            + " is larger than any file can be");

                if (!is_dir) {
                    uint64_t size_blocks = (ino.size + BLOCK_SIZE - 1) / BLOCK_SIZE;
                    if (end > size_blocks)
                        problem("inode " + str(id) + " owns file block " + str(end - 1)
                                + ", past the end of its " + str(ino.size) + "-byte size");
                    return;
                }

                std::string who = "directory inode " + str(id);
                if (ino.size % BLOCK_SIZE != 0)
                    problem(who + ": size " + str(ino.size) + " is not a whole number of blocks");

                uint64_t nblocks = ino.size / BLOCK_SIZE;
                if (nblocks == 0)
                    problem(who + " has no blocks: it needs one for \".\" and \"..\"");
                if (end > nblocks)
                    problem(who + " owns file block " + str(end - 1)
                            + ", past the end of its " + str(ino.size) + "-byte size");

                for (uint64_t i = 0; i < nblocks; i++) {
                    if (i >= dir_map.size() || dir_map[i] == 0) {
                        problem(who + " has a hole at file block " + str(i));
                        break;
                    }
                }
                if (dir_map.size() > nblocks)
                    dir_map.resize(nblocks);
            }

            // ------------------------------------------------------------ tree

            void walk_tree() {
                if (kind_.size() <= ROOT_INODE || kind_[ROOT_INODE] != Kind::dir)
                    return;

                // An explicit stack, not recursion: a deep tree must not be
                // able to run the checker out of stack.
                std::vector<std::pair<uint32_t, uint32_t>> stack{{ROOT_INODE, ROOT_INODE}};
                reachable_[ROOT_INODE] = 1;

                while (!stack.empty()) {
                    auto [dir, parent] = stack.back();
                    stack.pop_back();
                    r_.directories++;
                    check_directory(dir, parent, stack);
                }
            }

            void check_directory(uint32_t dir, uint32_t parent,
                                 std::vector<std::pair<uint32_t, uint32_t>>& stack) {
                static const DirEntry zero{};
                const std::string who = "directory inode " + str(dir);
                std::unordered_set<std::string> seen;
                Block b;

                const std::vector<uint32_t>& blocks = dir_blocks_[dir];
                for (size_t fb = 0; fb < blocks.size(); fb++) {
                    if (blocks[fb] == 0)
                        continue;  // a hole, already reported
                    read(blocks[fb], b);

                    for (uint32_t s = 0; s < DIRENTS_PER_BLOCK; s++) {
                        DirEntry e;
                        std::memcpy(&e, b.data() + s * DIRENT_SIZE, sizeof e);
                        uint64_t slot = fb * DIRENTS_PER_BLOCK + s;

                        if (slot < 2) {
                            check_dot(who, e, slot == 0 ? "." : "..",
                                      slot == 0 ? dir : parent);
                            if (e.inode_id != 0 && e.inode_id < sb_.inode_count)
                                refs_[e.inode_id]++;
                            continue;
                        }

                        if (e.inode_id == 0) {
                            if (std::memcmp(&e, &zero, sizeof e) != 0)
                                problem(who + ": free slot " + str(slot) + " is not zeroed");
                            continue;
                        }

                        size_t len = std::min<size_t>(e.name_len, MAX_NAME_LEN);
                        std::string name(e.name, len);
                        std::string entry = "entry \"" + name + "\" in " + who;

                        if (e.name_len == 0 || e.name_len > MAX_NAME_LEN)
                            problem(who + ": slot " + str(slot) + " has a name "
                                    + str(e.name_len) + " bytes long");
                        else if (name.find('/') != std::string::npos ||
                                 name.find('\0') != std::string::npos)
                            problem(entry + " has a '/' or NUL in its name");
                        else if (name == "." || name == "..")
                            problem(who + ": an extra \"" + name + "\" entry in slot " + str(slot));

                        if (!seen.insert(name).second)
                            problem(who + " has two entries named \"" + name + "\"");

                        uint32_t x = e.inode_id;
                        if (x >= sb_.inode_count) {
                            problem(entry + " points to inode " + str(x) + ", which does not exist");
                            continue;
                        }
                        refs_[x]++;

                        if (kind_[x] == Kind::none) {
                            problem(entry + " points to inode " + str(x) + ", which is not in use");
                            continue;
                        }
                        if (kind_[x] == Kind::bad) {
                            // Its mode is already reported. It is named, so it
                            // is reachable, but it cannot be walked or typed.
                            names_[x]++;
                            reachable_[x] = 1;
                            continue;
                        }

                        bool x_is_dir = kind_[x] == Kind::dir;
                        uint8_t want = x_is_dir ? FT_DIR : FT_FILE;
                        if (e.type != want)
                            problem(entry + " has type " + str(e.type) + ", but inode " + str(x)
                                    + " is a " + (x_is_dir ? "directory" : "file"));

                        names_[x]++;
                        if (names_[x] > 1 || x == ROOT_INODE)
                            continue;  // reported in check_links; never walk anything twice

                        reachable_[x] = 1;
                        if (x_is_dir)
                            stack.push_back({x, dir});
                        else
                            r_.files++;
                    }
                }
            }

            void check_dot(const std::string& who, const DirEntry& e,
                           const char* want_name, uint32_t want_id) {
                size_t want_len = std::strlen(want_name);
                bool name_ok = e.name_len == want_len &&
                               std::memcmp(e.name, want_name, want_len) == 0;
                std::string slot = std::string("\"") + want_name + "\"";

                if (!name_ok || e.type != FT_DIR) {
                    problem(who + ": slot " + (want_len == 1 ? "0" : "1") + " should be "
                            + slot + ", a directory entry");
                    return;
                }
                if (e.inode_id != want_id)
                    problem(who + ": " + slot + " points to inode " + str(e.inode_id)
                            + ", but should point to inode " + str(want_id));
            }

            // ----------------------------------------------------------- links

            void check_links() {
                for (uint32_t id = 1; id < sb_.inode_count; id++) {
                    Kind k = kind_[id];
                    if (k == Kind::none)
                        continue;

                    const char* what = k == Kind::dir  ? "directory"
                                     : k == Kind::file ? "file" : "bad";
                    std::string who = std::string(what) + " inode " + str(id);

                    if (id == ROOT_INODE) {
                        if (names_[id] > 0)
                            problem("the root directory is named by " + str(names_[id])
                                    + " entries; only its own \".\" and \"..\" may name it");
                    } else if (!reachable_[id]) {
                        problem(who + " is in use but not reachable from the root");
                        continue;
                    } else if (names_[id] > 1) {
                        problem(who + " is named by " + str(names_[id])
                                + " entries; it must have exactly one name");
                    }

                    if (k != Kind::bad && inodes_[id].link_count != refs_[id])
                        problem(who + " has link_count " + str(inodes_[id].link_count) + ", but "
                                + str(refs_[id]) + (refs_[id] == 1 ? " directory entry names it"
                                                                   : " directory entries name it"));
                }
            }

            // ------------------------------------------------------ data bitmap

            void check_data_bitmap() {
                for (uint32_t b = 0; b < sb_.total_blocks; b++) {
                    bool bit = block_bm_.is_set(b);
                    if (b < sb_.data_start) {
                        if (!bit)
                            problem("metadata block " + str(b)
                                    + " is not marked in use in the data bitmap");
                    } else if (owner_[b] != 0 && !bit) {
                        problem("block " + str(b) + " is owned by inode " + str(owner_[b])
                                + " but not marked in use in the data bitmap");
                    } else if (owner_[b] == 0 && bit) {
                        problem("block " + str(b)
                                + " is marked in use, but no inode owns it (leaked)");
                    }
                }
            }
    };

} // anonymous namespace

    FsckReport fsck(BlockDevice& dev) {
        FsckReport report;
        Checker(dev, report).run();
        return report;
    }

} // namespace vfs
