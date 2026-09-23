//
// disktest — proves stage 1 works.
//
// The point of stage 1 is that bytes written through PreadDevice actually reach
// the file and come back. A single-process test cannot prove that: the data
// could be sitting in a buffer somewhere. So the persistence check runs in two
// separate processes, one to write and one to read.
//
//   disktest                  run everything (this is what you want)
//   disktest write  <path>    create a disk, write the pattern, exit
//   disktest verify <path>    open that disk, check the pattern is there
//
// Run it under `make asan` as well as `make`.
//

#include "vfs/block_device.h"
#include "vfs/layout.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

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

// A pattern that depends on the block number, so a read of the wrong block
// fails the comparison instead of quietly passing.
void fill_pattern(vfs::Block& b, uint32_t block_no) {
    for (std::size_t i = 0; i < b.size(); i++)
        b[i] = static_cast<uint8_t>((block_no * 31u + i * 7u) & 0xFF);
}

bool has_pattern(const vfs::Block& b, uint32_t block_no) {
    vfs::Block want;
    fill_pattern(want, block_no);
    return b == want;
}

bool is_all_zero(const vfs::Block& b) {
    for (uint8_t byte : b)
        if (byte != 0) return false;
    return true;
}

constexpr uint32_t kBlocks    = 1024;   // a 4 MB disk
constexpr uint32_t kFirstUsed = 500;    // somewhere in the middle
constexpr uint32_t kLastUsed  = kBlocks - 1;

// ---------------------------------------------------------------- phase one

int phase_write(const char* path) {
    std::remove(path);

    vfs::PreadDevice dev = vfs::PreadDevice::create(path, kBlocks);

    vfs::Block b;
    fill_pattern(b, kFirstUsed);
    dev.write_block(kFirstUsed, b);

    fill_pattern(b, kLastUsed);
    dev.write_block(kLastUsed, b);

    dev.sync();
    std::printf("  wrote blocks %u and %u to %s\n", kFirstUsed, kLastUsed, path);
    return 0;
}

int phase_verify(const char* path) {
    vfs::PreadDevice dev = vfs::PreadDevice::open(path);

    check(dev.block_count() == kBlocks, "block count recovered from the file size");

    vfs::Block b;

    dev.read_block(kFirstUsed, b);
    check(has_pattern(b, kFirstUsed), "block 500 survived the process exiting");

    dev.read_block(kLastUsed, b);
    check(has_pattern(b, kLastUsed), "the last block survived too");

    dev.read_block(0, b);
    check(is_all_zero(b), "a block never written reads back as zeros");

    return failures ? 1 : 0;
}

// -------------------------------------------------------- in-process checks

void check_round_trip(const char* path) {
    std::printf("round trip within one process\n");
    std::remove(path);

    vfs::PreadDevice dev = vfs::PreadDevice::create(path, kBlocks);
    check(dev.block_count() == kBlocks, "block_count reports what we asked for");

    vfs::Block written, read_back;
    fill_pattern(written, 7);
    dev.write_block(7, written);
    dev.read_block(7, read_back);
    check(written == read_back, "what goes in comes out");
}

void check_bounds(const char* path) {
    std::printf("bounds\n");
    vfs::PreadDevice dev = vfs::PreadDevice::open(path);
    vfs::Block b{};

    check_throws([&] { dev.read_block(kBlocks, b); },
                 "reading one past the last block is rejected");
    check_throws([&] { dev.write_block(kBlocks, b); },
                 "writing one past the last block is rejected");
    check_throws([&] { dev.write_block(0xFFFFFFFFu, b); },
                 "a block number that would overflow the offset is rejected");

    // The last valid block must still work - an off-by-one in the bounds check
    // would make this throw.
    dev.read_block(kBlocks - 1, b);
    check(true, "the last valid block is still reachable");
}

void check_rejections(const char* path) {
    std::printf("bad input\n");

    check_throws([&] { vfs::PreadDevice::open("/tmp/vfs_no_such_file_xyz.img"); },
                 "opening a file that does not exist");

    check_throws([&] { vfs::PreadDevice::create(path, kBlocks); },
                 "creating over a disk that already exists");

    check_throws([&] { vfs::PreadDevice::create("/tmp/vfs_zero.img", 0); },
                 "creating a disk with zero blocks");

    const char* odd = "/tmp/vfs_odd_size.img";
    std::remove(odd);
    if (FILE* f = std::fopen(odd, "wb")) {
        std::fwrite("not a whole block", 1, 17, f);
        std::fclose(f);
    }
    check_throws([&] { vfs::PreadDevice::open(odd); },
                 "opening a file whose size is not a whole number of blocks");
    std::remove(odd);

    const char* empty = "/tmp/vfs_empty.img";
    std::remove(empty);
    if (FILE* f = std::fopen(empty, "wb")) std::fclose(f);
    check_throws([&] { vfs::PreadDevice::open(empty); },
                 "opening an empty file");
    std::remove(empty);
}

void check_moves(const char* path) {
    std::printf("move semantics\n");
    vfs::Block b;

    vfs::PreadDevice a = vfs::PreadDevice::open(path);
    vfs::PreadDevice moved = std::move(a);
    check(moved.block_count() == kBlocks, "a moved-to device still knows its size");
    moved.read_block(7, b);
    check(has_pattern(b, 7), "a moved-to device still reads");

    vfs::PreadDevice other = vfs::PreadDevice::open(path);
    other = std::move(moved);
    check(other.block_count() == kBlocks, "move assignment works");
    other.read_block(7, b);
    check(has_pattern(b, 7), "the move-assigned device reads");
    // Leaving this scope closes `other` once and the two moved-from husks not
    // at all. Run under asan: a double close or a leaked fd shows up here.
}

void check_sync(const char* path) {
    std::printf("sync\n");
    vfs::PreadDevice dev = vfs::PreadDevice::open(path);
    try {
        dev.sync();
        check(true, "sync() returns without error");
    } catch (const std::exception& e) {
        std::printf("  FAIL  sync() threw: %s\n", e.what());
        failures++;
    }
}

// Re-runs this same binary twice, so the write and the read happen in two
// different processes. That is the only way to prove the bytes reached the file.
int check_persistence_across_processes(const char* self, const char* path) {
    std::printf("persistence across two processes\n");

    std::string write_cmd  = std::string("\"") + self + "\" write \""  + path + "\"";
    std::string verify_cmd = std::string("\"") + self + "\" verify \"" + path + "\"";

    // When stdout is a pipe rather than a terminal it is fully buffered, so
    // without this flush the child's output appears before our own.
    std::fflush(stdout);

    if (std::system(write_cmd.c_str()) != 0) {
        std::printf("  FAIL  the write process failed\n");
        return 1;
    }
    if (std::system(verify_cmd.c_str()) != 0) {
        std::printf("  FAIL  the verify process failed\n");
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const char* default_path = "/tmp/vfs_disktest.img";

    try {
        if (argc >= 2 && std::strcmp(argv[1], "write") == 0)
            return phase_write(argc >= 3 ? argv[2] : default_path);

        if (argc >= 2 && std::strcmp(argv[1], "verify") == 0)
            return phase_verify(argc >= 3 ? argv[2] : default_path);

        if (argc >= 2) {
            std::fprintf(stderr, "usage: disktest [write|verify] [path]\n");
            return 2;
        }

        const char* path = "/tmp/vfs_disktest_local.img";

        check_round_trip(path);
        check_bounds(path);
        check_rejections(path);
        check_moves(path);
        check_sync(path);
        std::remove(path);

        int sub = check_persistence_across_processes(argv[0], default_path);
        std::remove(default_path);

        std::printf("\n%s\n", (failures == 0 && sub == 0)
                                  ? "stage 1: all checks passed"
                                  : "stage 1: FAILURES");
        return (failures == 0 && sub == 0) ? 0 : 1;

    } catch (const std::exception& e) {
        std::fprintf(stderr, "disktest: unexpected exception: %s\n", e.what());
        return 1;
    }
}
