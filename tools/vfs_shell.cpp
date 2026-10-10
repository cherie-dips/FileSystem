// vfs_shell — the program you drive the file system with.
//
// Every command is listed with the stage that makes it work, so running --help
// tells you where the build is. A command that works has a function to run;
// the rest say which stage they are waiting for.


#include "vfs/bitmap.h"
#include "vfs/block_device.h"
#include "vfs/layout.h"
#include "vfs/mkfs.h"
#include "vfs/superblock.h"
#include "vfs/version.h"

#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

// Thrown for a malformed command line. main prints the command's usage line
// after the message.
struct UsageError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// ------------------------------------------------------------- argument parsing

// Reads the digits at p and moves p past them. Not strtoull: that accepts
// "-1" and quietly turns it into 18446744073709551615.
uint64_t parse_digits(const char*& p, const char* whole) {
    if (!std::isdigit(static_cast<unsigned char>(*p)))
        throw UsageError(std::string("not a number: ") + whole);

    uint64_t n = 0;
    while (std::isdigit(static_cast<unsigned char>(*p))) {
        uint64_t digit = static_cast<uint64_t>(*p - '0');
        if (n > (UINT64_MAX - digit) / 10)
            throw UsageError(std::string("too large: ") + whole);
        n = n * 10 + digit;
        p++;
    }
    return n;
}

uint32_t parse_u32(const char* text) {
    const char* p = text;
    uint64_t n = parse_digits(p, text);
    if (*p != '\0' || n > UINT32_MAX)
        throw UsageError(std::string("not a 32-bit number: ") + text);
    return static_cast<uint32_t>(n);
}

// "256M" -> 268435456. A bare number is bytes; K, M and G are powers of 1024.
uint64_t parse_size(const char* text) {
    const char* p = text;
    uint64_t n = parse_digits(p, text);

    int shift = 0;
    switch (*p) {
        case 'K': case 'k': shift = 10; p++; break;
        case 'M': case 'm': shift = 20; p++; break;
        case 'G': case 'g': shift = 30; p++; break;
        default: break;
    }

    if (*p != '\0')
        throw UsageError(std::string("not a size: ") + text + " (try 256M)");
    if (n > (UINT64_MAX >> shift))
        throw UsageError(std::string("too large: ") + text);
    return n << shift;
}

// ------------------------------------------------------------------- printing

// 268435456 -> "256 MB". Only exact multiples are scaled, so nothing is rounded.
std::string human_size(uint64_t bytes) {
    const char* units[] = {"bytes", "KB", "MB", "GB", "TB"};
    int u = 0;
    while (u < 4 && bytes >= 1024 && bytes % 1024 == 0) {
        bytes /= 1024;
        u++;
    }
    return std::to_string(bytes) + " " + units[u];
}

// The half-open range [first, end): "block 1", "blocks 4-131", or "none".
std::string block_range(uint32_t first, uint32_t end) {
    if (end <= first)
        return "none";
    if (end - first == 1)
        return "block " + std::to_string(first);
    return "blocks " + std::to_string(first) + "-" + std::to_string(end - 1)
         + " (" + std::to_string(end - first) + ")";
}

std::string format_time(uint64_t seconds) {
    if (seconds == 0)
        return "-";
    std::time_t t = static_cast<std::time_t>(seconds);
    std::tm local{};
    char text[32];
    if (localtime_r(&t, &local) == nullptr ||
        std::strftime(text, sizeof text, "%Y-%m-%d %H:%M:%S", &local) == 0)
        return std::to_string(seconds);
    return text;
}

const char* type_name(uint16_t mode) {
    switch (mode & vfs::MODE_TYPE_MASK) {
        case vfs::MODE_DIR:  return "directory";
        case vfs::MODE_FILE: return "file";
        case 0:              return "-";
        default:             return "unknown";
    }
}

// Every set bit, as runs: "0-132, 140, 200-201". Wrapped to fit a terminal.
void print_runs(const vfs::Bitmap& bm) {
    std::string line = " ";
    bool any = false;
    uint32_t i = 0;

    while (i < bm.size()) {
        if (!bm.is_set(i)) {
            i++;
            continue;
        }
        uint32_t start = i;
        while (i < bm.size() && bm.is_set(i))
            i++;

        std::string run = std::to_string(start);
        if (i - start > 1)
            run += "-" + std::to_string(i - 1);

        if (any)
            line += ",";
        if (line.size() + run.size() + 1 > 78) {
            std::printf("%s\n", line.c_str());
            line = " ";
        }
        line += " " + run;
        any = true;
    }

    std::printf("%s\n", any ? line.c_str() : "  (none)");
}

// ---------------------------------------------------------------------- mkfs

int cmd_mkfs(int argc, char** argv) {
    const char* path = nullptr;
    const char* size_text = nullptr;
    uint32_t inodes = 0;

    for (int i = 0; i < argc; i++) {
        if (std::strcmp(argv[i], "--inodes") == 0) {
            if (++i == argc)
                throw UsageError("--inodes needs a number");
            inodes = parse_u32(argv[i]);
            if (inodes == 0)
                throw UsageError("--inodes must be at least 1");
        } else if (path == nullptr) {
            path = argv[i];
        } else if (size_text == nullptr) {
            size_text = argv[i];
        } else {
            throw UsageError(std::string("unexpected argument: ") + argv[i]);
        }
    }

    if (path == nullptr || size_text == nullptr)
        throw UsageError("needs an image path and a size");

    uint64_t bytes = parse_size(size_text);
    if (bytes == 0 || bytes % vfs::BLOCK_SIZE != 0)
        throw UsageError("the size must be a whole number of 4096-byte blocks");
    if (bytes / vfs::BLOCK_SIZE > UINT32_MAX)
        throw UsageError("too large: block numbers are 32 bits");
    uint32_t blocks = static_cast<uint32_t>(bytes / vfs::BLOCK_SIZE);

    // Checked before the file exists, so a layout that does not fit leaves
    // nothing behind.
    vfs::plan_layout(blocks, inodes);

    vfs::PreadDevice dev = vfs::PreadDevice::create(path, blocks);
    vfs::Superblock sb;
    try {
        sb = vfs::mkfs(dev, inodes);
    } catch (...) {
        // create() made this file a moment ago, so removing it cannot touch
        // anything that was there before. Better no image than half of one.
        std::remove(path);
        throw;
    }

    std::printf("formatted %s\n", path);
    std::printf("  size         %s, %u blocks of %u bytes\n",
                human_size(bytes).c_str(), sb.total_blocks, vfs::BLOCK_SIZE);
    std::printf("  inodes       %u\n", sb.inode_count);
    std::printf("  data blocks  %u, from block %u\n",
                sb.total_blocks - sb.data_start, sb.data_start);
    std::printf("  root         inode %u, its directory in block %u\n",
                sb.root_inode, sb.data_start);
    return 0;
}

// ---------------------------------------------------------------------- dump

// Prints block 0 as it is, before checking it - a broken superblock is exactly
// the one you want to look at.
int dump_super(vfs::BlockDevice& dev) {
    vfs::Block b;
    dev.read_block(0, b);
    vfs::Superblock sb;
    std::memcpy(&sb, b.data(), sizeof sb);

    std::string magic_text;
    for (int i = 0; i < 4; i++)
        magic_text += std::isprint(b[i]) ? static_cast<char>(b[i]) : '.';

    std::printf("superblock, block 0\n");
    std::printf("  magic                0x%08x \"%s\"\n", sb.magic, magic_text.c_str());
    std::printf("  block size           %u\n", sb.block_size);
    std::printf("  total blocks         %u (%s)\n", sb.total_blocks,
                human_size(uint64_t{sb.total_blocks} * vfs::BLOCK_SIZE).c_str());
    std::printf("  inodes               %u\n", sb.inode_count);
    std::printf("  inode bitmap         %s\n", block_range(sb.inode_bitmap_start, sb.data_bitmap_start).c_str());
    std::printf("  data bitmap          %s\n", block_range(sb.data_bitmap_start, sb.inode_table_start).c_str());
    std::printf("  inode table          %s\n", block_range(sb.inode_table_start, sb.data_start).c_str());
    std::printf("  data                 %s\n", block_range(sb.data_start, sb.total_blocks).c_str());
    std::printf("  root inode           %u\n", sb.root_inode);
    std::printf("  clean unmount        %s\n", sb.clean_unmount ? "yes" : "no");
    std::printf("  last checkpoint lsn  %" PRIu64 "\n", sb.last_checkpoint_lsn);

    try {
        vfs::validate_superblock(sb, dev.block_count());
    } catch (const std::exception& e) {
        std::printf("  INVALID: %s\n", e.what());
        return 1;
    }
    std::printf("  valid\n");
    return 0;
}

void dump_bitmaps(vfs::BlockDevice& dev, const vfs::Superblock& sb) {
    vfs::Bitmap inodes(sb.inode_count);
    inodes.load(dev, sb.inode_bitmap_start);
    std::printf("inode bitmap, %s\n",
                block_range(sb.inode_bitmap_start, sb.data_bitmap_start).c_str());
    std::printf("  %u of %u inodes in use:\n", inodes.count_set(), inodes.size());
    print_runs(inodes);

    vfs::Bitmap blocks(sb.total_blocks);
    blocks.load(dev, sb.data_bitmap_start);
    std::printf("data bitmap, %s\n",
                block_range(sb.data_bitmap_start, sb.inode_table_start).c_str());
    std::printf("  %u of %u blocks in use:\n", blocks.count_set(), blocks.size());
    print_runs(blocks);
}

void dump_inode(vfs::BlockDevice& dev, const vfs::Superblock& sb, uint32_t id) {
    if (id >= sb.inode_count)
        throw std::out_of_range("there is no inode " + std::to_string(id)
                                + ": this disk has " + std::to_string(sb.inode_count));

    vfs::Bitmap used(sb.inode_count);
    used.load(dev, sb.inode_bitmap_start);

    uint32_t block  = vfs::inode_block(sb, id);
    uint32_t offset = vfs::inode_offset(id);
    vfs::Block b;
    dev.read_block(block, b);
    vfs::Inode ino;
    std::memcpy(&ino, b.data() + offset, sizeof ino);

    const char* state = id == 0 ? "reserved" : used.is_set(id) ? "in use" : "free";
    std::printf("inode %u, %s - block %u, bytes %u-%u\n",
                id, state, block, offset, offset + vfs::INODE_SIZE - 1);

    std::printf("  id           %u", ino.id);
    if (used.is_set(id) && id != 0 && ino.id != id)
        std::printf("  (WRONG: should be %u)", id);
    std::printf("\n");

    std::printf("  type         %s\n", type_name(ino.mode));
    std::printf("  permissions  %04o\n", static_cast<unsigned>(ino.mode & vfs::MODE_PERM_MASK));
    std::printf("  links        %u\n", static_cast<unsigned>(ino.link_count));
    std::printf("  size         %" PRIu64 " bytes\n", ino.size);
    std::printf("  blocks       %u\n", ino.block_count);
    std::printf("  modified     %s\n", format_time(ino.mtime).c_str());

    std::string direct;
    for (uint32_t i = 0; i < vfs::DIRECT_PTRS; i++) {
        if (ino.direct[i] != 0)
            direct += " [" + std::to_string(i) + "] " + std::to_string(ino.direct[i]);
    }
    std::printf("  direct      %s\n", direct.empty() ? " -" : direct.c_str());
    std::printf("  indirect     %s\n", ino.single_indirect ? std::to_string(ino.single_indirect).c_str() : "-");
    std::printf("  double ind.  %s\n", ino.double_indirect ? std::to_string(ino.double_indirect).c_str() : "-");
}

int cmd_dump(int argc, char** argv) {
    if (argc < 1)
        throw UsageError("needs an image path");

    // Read the whole command line before printing anything, so a typo at the
    // end does not arrive after a screenful of output.
    enum class What { super, bitmaps, inode };
    std::vector<std::pair<What, uint32_t>> wanted;

    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--super") == 0) {
            wanted.push_back({What::super, 0});
        } else if (std::strcmp(argv[i], "--bitmaps") == 0) {
            wanted.push_back({What::bitmaps, 0});
        } else if (std::strcmp(argv[i], "--inode") == 0) {
            if (++i == argc)
                throw UsageError("--inode needs an inode number");
            wanted.push_back({What::inode, parse_u32(argv[i])});
        } else {
            throw UsageError(std::string("unknown option: ") + argv[i]);
        }
    }
    if (wanted.empty())
        wanted.push_back({What::super, 0});

    vfs::PreadDevice dev = vfs::PreadDevice::open(argv[0]);
    int status = 0;

    for (size_t i = 0; i < wanted.size(); i++) {
        if (i > 0)
            std::printf("\n");

        if (wanted[i].first == What::super) {
            status |= dump_super(dev);
            continue;
        }

        // Everything past block 0 is found through the superblock, so it has
        // to be trustworthy first.
        vfs::Superblock sb = vfs::read_superblock(dev);
        if (wanted[i].first == What::bitmaps)
            dump_bitmaps(dev, sb);
        else
            dump_inode(dev, sb, wanted[i].second);
    }
    return status;
}

// ------------------------------------------------------------------ commands

struct Command {
    const char* name;
    const char* help;
    const char* stage;                  // the stage that makes it work
    int (*run)(int argc, char** argv);  // gets the arguments after the name;
                                        // nullptr until its stage lands
    const char* args;                   // for the usage line
};

// Kept in the order the build plan implements them.
constexpr Command kCommands[] = {
    {"mkfs",  "format a new disk image",             "stage 2", cmd_mkfs,
              "<image> <size> [--inodes N]"},
    {"dump",  "print a superblock, bitmap or inode", "stage 2", cmd_dump,
              "<image> [--super] [--bitmaps] [--inode N]..."},
    {"mkdir", "create a directory",                  "stage 4", nullptr, nullptr},
    {"ls",    "list a directory",                    "stage 4", nullptr, nullptr},
    {"cat",   "print a file",                        "stage 5", nullptr, nullptr},
    {"write", "write text into a file",              "stage 5", nullptr, nullptr},
    {"rm",    "remove a file",                       "stage 5", nullptr, nullptr},
    {"sync",  "flush everything to the disk image",  "stage 6", nullptr, nullptr},
    {"fsck",  "check the disk image for damage",     "stage 8", nullptr, nullptr},
};

void print_usage() {
    std::printf("%s\n\n", vfs::version_string().c_str());
    std::printf("usage: vfs_shell [--version] [--help] <command> [args]\n\n");
    std::printf("commands:\n");
    for (const Command& c : kCommands) {
        std::printf("  %-8s %-36s %s\n",
                    c.name, c.help, c.run ? "ready" : c.stage);
    }

    std::printf("\nthe ones that are ready:\n");
    for (const Command& c : kCommands) {
        if (c.run)
            std::printf("  vfs_shell %s %s\n", c.name, c.args);
    }
    std::printf("\nsizes are in bytes, or end in K, M or G: 256M\n");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 0;
    }

    const char* arg = argv[1];

    if (std::strcmp(arg, "--version") == 0 || std::strcmp(arg, "-v") == 0) {
        std::printf("%s\n", vfs::version_string().c_str());
        return 0;
    }

    if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
        print_usage();
        return 0;
    }

    for (const Command& c : kCommands) {
        if (std::strcmp(arg, c.name) != 0)
            continue;

        if (c.run == nullptr) {
            std::fprintf(stderr,
                         "vfs_shell: '%s' is not built yet - it arrives at %s\n",
                         c.name, c.stage);
            return 1;
        }

        try {
            return c.run(argc - 2, argv + 2);
        } catch (const UsageError& e) {
            std::fprintf(stderr, "vfs_shell %s: %s\n", c.name, e.what());
            std::fprintf(stderr, "usage: vfs_shell %s %s\n", c.name, c.args);
            return 2;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "vfs_shell %s: %s\n", c.name, e.what());
            return 1;
        }
    }

    std::fprintf(stderr, "vfs_shell: unknown command '%s'\n", arg);
    std::fprintf(stderr, "try: vfs_shell --help\n");
    return 1;
}
