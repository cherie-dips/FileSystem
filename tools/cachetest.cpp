//
// cachetest — proves the block cache (stage 6) keeps its promises.
//
// The cache runs over a real disk image, with a plain counter standing in for
// the log's durable lsn, so the write-ahead rule can be tested without a log.
// Blocks that are multiples of 16 all land in shard 0, which in a 256-frame
// cache holds 16 frames - so one shard's LRU order can be watched directly.
//
// Run it under `make asan` and `make tsan` as well as `make`.
//

#include "vfs/block_device.h"
#include "vfs/buffer_cache.h"
#include "vfs/layout.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

const std::string kPath = "/tmp/vfs_cachetest_" + std::to_string(::getpid()) + ".img";

constexpr uint32_t kBlocks = 4096;     // a 16 MB image, sparse
constexpr uint32_t kFrames = 256;      // so 16 frames per shard
constexpr uint32_t kShare  = kFrames / vfs::BufferCache::SHARDS;

// The k-th block of shard 0.
uint32_t s0(uint32_t k) { return k * vfs::BufferCache::SHARDS; }

bool all_bytes(const vfs::Block& b, uint8_t v) {
    for (uint8_t x : b)
        if (x != v) return false;
    return true;
}

auto filler(uint8_t v) {
    return [v](vfs::Block& b) { b.fill(v); };
}

// A fresh image, a counting device over it, and a cache over that.
struct Rig {
    std::atomic<uint64_t> durable{UINT64_MAX};
    vfs::PreadDevice raw;
    vfs::InstrumentedDevice dev;
    vfs::BufferCache cache;

    Rig()
        : raw(fresh()),
          dev(raw, 0, false),
          cache(dev, kFrames, [this] { return durable.load(); }) {
    }

    static vfs::PreadDevice fresh() {
        std::remove(kPath.c_str());
        return vfs::PreadDevice::create(kPath.c_str(), kBlocks);
    }

    // What the disk itself holds, past the cache.
    vfs::Block on_disk(uint32_t block_no) {
        vfs::Block b;
        raw.read_block(block_no, b);
        return b;
    }
};

// --------------------------------------------------------------------------

void check_hits() {
    std::printf("hits and misses\n");
    Rig r;
    vfs::Block b;

    r.cache.read(5, b);
    r.cache.read(5, b);
    r.cache.read(6, b);
    vfs::CacheStats st = r.cache.stats();
    check(st.misses == 2 && st.hits == 1, "two first reads miss, a second read of one hits");
    check(r.dev.reads() == 2, "and only the misses reach the disk");

    r.cache.modify(s0(50), true, false, filler(9));
    check(r.dev.reads() == 2, "a fresh block is zeroed in memory, never read from disk");
    r.cache.read(s0(50), b);
    check(all_bytes(b, 9), "and reads back as written");
}

void check_lru() {
    std::printf("LRU order\n");
    Rig r;
    vfs::Block b;

    for (uint32_t k = 1; k <= kShare; k++)
        r.cache.read(s0(k), b);
    check(r.cache.stats().evictions == 0, "16 blocks fill shard 0 without evicting");

    r.cache.read(s0(1), b);           // now the most recently used
    r.cache.read(s0(kShare + 1), b);  // one too many
    check(r.cache.stats().evictions == 1, "the 17th evicts exactly one");
    check(!r.cache.contains(s0(2)), "the least recently used one, block 32");
    check(r.cache.contains(s0(1)) && r.cache.contains(s0(3)),
          "not block 16, which was just touched, nor the newer ones");

    r.cache.read(s0(kShare + 2), b);
    check(!r.cache.contains(s0(3)) && r.cache.contains(s0(1)), "the next eviction takes the next oldest");

    uint64_t misses = r.cache.stats().misses;
    r.cache.read(s0(2), b);
    check(r.cache.stats().misses == misses + 1, "an evicted block misses when it comes back");
}

void check_dirty_victim() {
    std::printf("dirty frames\n");
    Rig r;
    vfs::Block b;

    r.cache.modify(s0(1), false, false, filler(0xAB));
    check(r.cache.dirty_count() == 1, "a change outside a transaction is dirty at once");
    check(all_bytes(r.on_disk(s0(1)), 0), "and is not on disk yet");

    for (uint32_t k = 2; k <= kShare + 1; k++)
        r.cache.read(s0(k), b);
    check(r.cache.contains(s0(1)) && !r.cache.contains(s0(2)),
          "eviction passes over the oldest frame when it is dirty, for the oldest clean one");
    check(r.dev.writes() == 0, "so it costs no write");

    // Now nothing in the shard is clean.
    for (uint32_t k = 3; k <= kShare + 1; k++)
        r.cache.modify(s0(k), false, false, filler(static_cast<uint8_t>(k)));
    r.cache.modify(s0(kShare + 2), true, false, filler(0x77));
    check(!r.cache.contains(s0(1)), "with no clean frame left, the oldest dirty one goes");
    check(all_bytes(r.on_disk(s0(1)), 0xAB), "written back on the way out");
    check(r.dev.writes() == 1 && r.cache.dirty_count() == kShare,
          "with exactly one write, and the rest still dirty");

    r.cache.read(s0(1), b);
    check(all_bytes(b, 0xAB), "and it reads back from disk intact");
}

void check_no_steal() {
    std::printf("no-steal: frames in a transaction\n");
    Rig r;
    vfs::Block b;

    bool joined = false;
    r.cache.modify(s0(1), true, true, filler(0xCD), &joined);
    check(joined, "the first change brings a frame into the transaction");
    r.cache.modify(s0(1), false, true, [](vfs::Block& x) { x[0] = 0xCE; }, &joined);
    check(!joined, "the second finds it already there");

    for (uint32_t k = 2; k <= 3 * kShare; k++)
        r.cache.read(s0(k), b);
    check(r.cache.contains(s0(1)), "it survives 47 other blocks passing through its shard");
    check(r.cache.write_back_all() == 0 && r.dev.writes() == 0,
          "write_back_all() leaves it alone");
    check(all_bytes(r.on_disk(s0(1)), 0), "nothing of it is on disk");

    r.cache.commit_frame(s0(1), 7);
    check(r.cache.dirty_count() == 1, "once committed it is dirty");
    check(r.cache.write_back_all() == 1, "and may be written back");
    vfs::Block disk = r.on_disk(s0(1));
    check(disk[0] == 0xCE && disk[1] == 0xCD, "with both changes");
    check(r.cache.dirty_count() == 0, "after which it is clean");
}

void check_stretch() {
    std::printf("a shard with nothing to evict\n");
    Rig r;
    vfs::Block b;

    for (uint32_t k = 1; k <= kShare; k++)
        r.cache.modify(s0(k), true, true, filler(static_cast<uint8_t>(k)));
    r.cache.read(s0(kShare + 1), b);
    check(true, "all 16 frames in a transaction: a 17th block still loads");

    bool all_there = true;
    for (uint32_t k = 1; k <= kShare; k++) {
        r.cache.read(s0(k), b);
        all_there = all_there && r.cache.contains(s0(k)) && all_bytes(b, static_cast<uint8_t>(k));
    }
    check(all_there, "and none of the 16 was given up for it");
}

void check_write_ahead() {
    std::printf("write-ahead rule\n");
    Rig r;
    vfs::Block b;

    r.durable = 5;
    r.cache.modify(s0(1), true, true, filler(0x11));
    r.cache.commit_frame(s0(1), 10);   // committed at lsn 10, log durable to 5
    check(r.cache.write_back_all() == 0, "a frame from lsn 10 is not written while the log is durable to 5");

    for (uint32_t k = 2; k <= 3 * kShare; k++)
        r.cache.read(s0(k), b);
    check(r.cache.contains(s0(1)), "nor evicted, which would mean writing it");
    check(all_bytes(r.on_disk(s0(1)), 0), "so the disk still has the old block");

    r.durable = 10;
    check(r.cache.write_back_all() == 1, "once the log reaches lsn 10 it goes");
    check(all_bytes(r.on_disk(s0(1)), 0x11), "and the disk has it");
}

void check_threads() {
    std::printf("many threads\n");
    Rig r;

    // Four writers bump their own counter in one shared block, and fill
    // blocks of their own, while a fifth thread keeps writing frames back.
    // A lost update or a dirty bit cleared too early shows up in the totals.
    constexpr int kThreads = 4;
    constexpr uint32_t kRounds = 2000;
    constexpr uint32_t kShared = 777;
    std::atomic<bool> done{false};

    std::thread flusher([&] {
        while (!done.load())
            r.cache.write_back(8);
    });

    std::vector<std::thread> writers;
    for (int t = 0; t < kThreads; t++) {
        writers.emplace_back([&r, t] {
            for (uint32_t i = 1; i <= kRounds; i++) {
                r.cache.modify(kShared, false, false, [t](vfs::Block& b) {
                    uint32_t v;
                    std::memcpy(&v, b.data() + 4 * t, 4);
                    v++;
                    std::memcpy(b.data() + 4 * t, &v, 4);
                });
                uint32_t own = 1000 + static_cast<uint32_t>(t) * 64 + i % 64;
                r.cache.modify(own, false, false, [i](vfs::Block& b) {
                    std::memcpy(b.data(), &i, 4);
                });
                vfs::Block b;
                r.cache.read(kShared, b);
            }
        });
    }
    for (std::thread& w : writers)
        w.join();
    done = true;
    flusher.join();

    r.cache.write_back_all();
    check(r.cache.dirty_count() == 0, "everything written back at the end");

    vfs::Block disk = r.on_disk(kShared);
    bool counts = true;
    for (int t = 0; t < kThreads; t++) {
        uint32_t v;
        std::memcpy(&v, disk.data() + 4 * t, 4);
        counts = counts && v == kRounds;
    }
    check(counts, "the shared block has every one of the 8000 increments, on disk");

    bool own_blocks = true;
    for (int t = 0; t < kThreads; t++) {
        for (uint32_t k = 0; k < 64; k++) {
            vfs::Block b = r.on_disk(1000 + static_cast<uint32_t>(t) * 64 + k);
            uint32_t v;
            std::memcpy(&v, b.data(), 4);
            // The last round that wrote this block: the largest i <= kRounds with i % 64 == k.
            uint32_t want = kRounds - ((kRounds - k) % 64);
            own_blocks = own_blocks && v == want;
        }
    }
    check(own_blocks, "and each thread's own blocks hold their last values");
}

} // namespace

int main() {
    try {
        check_hits();
        check_lru();
        check_dirty_victim();
        check_no_steal();
        check_stretch();
        check_write_ahead();
        check_threads();
        std::remove(kPath.c_str());

        std::printf("\n%s\n", failures == 0
                                  ? "stage 6: all checks passed"
                                  : "stage 6: FAILURES");
        return failures == 0 ? 0 : 1;

    } catch (const std::exception& e) {
        std::fprintf(stderr, "cachetest: unexpected exception: %s\n", e.what());
        std::remove(kPath.c_str());
        return 1;
    }
}
