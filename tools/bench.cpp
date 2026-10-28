//
// bench — the numbers behind the README: what the cache buys, what the
// write-ahead log costs, and how far the locking lets threads scale.
//
//   bench [--quick] [sequential] [reads] [threads] [wal]   (default: all)
//
// Prints markdown tables. Unless a table says otherwise, fsync is off: writes
// reach the operating system, not the drive, which keeps the numbers about
// this code rather than about the SSD. The artificial disk latency is a sleep
// before every block read or write of disk.img (not of the log).
//

#include "vfs/block_device.h"
#include "vfs/mkfs.h"
#include "vfs/vfs.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;

double since(Clock::time_point t) {
    return std::chrono::duration<double>(Clock::now() - t).count();
}

constexpr double MB = 1024.0 * 1024.0;

bool quick = false;
std::string image, wal;

long must(long rc, const char* what) {
    if (rc < 0)
        throw std::runtime_error(std::string(what) + ": " + vfs_strerror(static_cast<int>(rc)));
    return rc;
}

VfsOptions opts(uint32_t cache_frames = 4096, uint32_t latency_us = 0,
                bool journal = true, bool fsync = false) {
    VfsOptions o;
    o.cache_frames = cache_frames;
    o.disk_latency_us = latency_us;
    o.journal = journal;
    o.fsync = fsync;
    return o;
}

// Formats a fresh image of `mb` megabytes and mounts it.
void fresh(uint32_t mb, const VfsOptions& o) {
    std::remove(image.c_str());
    std::remove(wal.c_str());
    {
        vfs::PreadDevice dev = vfs::PreadDevice::create(image.c_str(), mb * 256);
        vfs::mkfs(dev);
    }
    must(vfs_mount(image.c_str(), wal.c_str(), &o), "mount");
}

void write_all(int fd, const std::string& data) {
    if (must(vfs_write(fd, data.data(), data.size()), "write") != static_cast<long>(data.size()))
        throw std::runtime_error("short write");
}

// Writes `bytes` to path in 64 KB calls.
void write_file(const char* path, uint64_t bytes) {
    std::string chunk(64 * 1024, 'x');
    int fd = static_cast<int>(must(vfs_open(path, VFS_O_CREAT | VFS_O_TRUNC | VFS_O_WRONLY), "open"));
    for (uint64_t done = 0; done < bytes; done += chunk.size())
        write_all(fd, chunk);
    must(vfs_close(fd), "close");
}

VfsStats stats() {
    VfsStats s{};
    vfs_get_stats(&s);
    return s;
}

// ------------------------------------------------- (a) sequential writes

void bench_sequential() {
    const uint64_t bytes = quick ? (4u << 20) : (16u << 20);
    const uint32_t latencies[] = {0, 50, 200};
    const uint32_t caches[] = {256, 16384};  // 1 MB and 64 MB

    std::printf("### Sequential write: what the cache buys\n\n");
    std::printf("%.0f MB in 64 KB calls, then vfs_sync. fsync off. \"write\" stops the clock when "
                "the last vfs_write returns; \"write + sync\" when vfs_sync has put everything "
                "in disk.img. Best of three runs.\n\n", bytes / MB);
    std::printf("| disk latency | cache | write | write + sync | disk writes |\n");
    std::printf("|---|---|---|---|---|\n");

    struct Run {
        double write_secs, total_secs;
        uint64_t writes;
    };
    auto run = [&](uint32_t frames, uint32_t lat) {
        fresh(64, opts(frames, lat));
        vfs_reset_stats();
        auto t = Clock::now();
        write_file("/seq", bytes);
        Run r{since(t), 0, 0};
        must(vfs_sync(), "sync");
        r.total_secs = since(t);
        r.writes = stats().disk_writes;
        must(vfs_unmount(), "unmount");
        return r;
    };

    for (uint32_t lat : latencies) {
        for (uint32_t frames : caches) {
            // Short runs are at the mercy of whatever else the machine is
            // doing, so each row is the best of three.
            Run best = run(frames, lat);
            for (int i = 0; i < 2; i++) {
                Run again = run(frames, lat);
                if (again.total_secs < best.total_secs)
                    best = again;
            }
            std::printf("| %u µs | %u MB | %.1f MB/s | %.1f MB/s | %llu |\n", lat, frames / 256,
                        bytes / MB / best.write_secs, bytes / MB / best.total_secs,
                        static_cast<unsigned long long>(best.writes));
        }
    }
    std::printf("\n");
}

// --------------------------------------------------- (b) random reads

void bench_random_reads() {
    const uint64_t file_bytes = 32u << 20;
    const uint64_t blocks = file_bytes / 4096;
    const int reads = quick ? 4000 : 20000;
    const uint32_t cache_mb[] = {1, 4, 16, 64};
    const uint32_t latency = 50;

    fresh(64, opts());
    write_file("/big", file_bytes);
    must(vfs_unmount(), "unmount");

    std::printf("### Random 4 KB reads: hit rate against cache size\n\n");
    std::printf("A %.0f MB file, %d reads after a warm-up, disk latency %u µs. "
                "Predicted disk reads per read, for uniform reads: 1 - cache / file.\n\n",
                file_bytes / MB, reads, latency);
    std::printf("| cache | reads/s | disk reads per read | predicted |\n");
    std::printf("|---|---|---|---|\n");

    std::vector<char> buf(4096);
    for (uint32_t mb : cache_mb) {
        VfsOptions o = opts(mb * 256, latency);
        must(vfs_mount(image.c_str(), wal.c_str(), &o), "mount");
        int fd = static_cast<int>(must(vfs_open("/big", VFS_O_RDONLY), "open"));
        std::mt19937_64 rng(42);

        auto read_at = [&](uint64_t block) {
            must(vfs_lseek(fd, static_cast<off_t>(block * 4096), VFS_SEEK_SET), "lseek");
            must(vfs_read(fd, buf.data(), buf.size()), "read");
        };

        // Warm up: once through the file, then random reads, so the cache
        // holds what it would in a steady state.
        for (uint64_t b = 0; b < blocks; b++) read_at(b);
        for (int i = 0; i < reads / 2; i++) read_at(rng() % blocks);

        vfs_reset_stats();
        auto t = Clock::now();
        for (int i = 0; i < reads; i++) read_at(rng() % blocks);
        double secs = since(t);
        VfsStats s = stats();

        must(vfs_close(fd), "close");
        must(vfs_unmount(), "unmount");

        double predicted = 1.0 - std::min(1.0, mb / (file_bytes / MB));
        std::printf("| %u MB | %.0f | %.3f | %.3f |\n", mb, reads / secs,
                    static_cast<double>(s.disk_reads) / reads, predicted);
    }
    std::printf("\n");
}

// --------------------------------------------------------- (c) threads

// Each thread rewrites random 4 KB blocks of a 1 MB file for `secs`, either
// its own file or the one they all share. durable: fsync on, and every write
// is followed by vfs_fsync. Returns operations per second.
double thread_run(unsigned threads, bool same_file, double secs, bool durable = false) {
    // 64 MB of cache, so that even 16 threads' files (16 MB) fit inside one
    // transaction - half the cache - and this measures the locking. With the
    // default 16 MB, past about 7 threads every commit has to log the whole
    // working set again, and the log's bandwidth sets the pace instead.
    fresh(128, opts(16384, 0, true, durable));
    std::string block(4096, 'b');
    for (unsigned t = 0; t < (same_file ? 1u : threads); t++)
        write_file(("/w" + std::to_string(t)).c_str(), 1u << 20);
    must(vfs_sync(), "sync");

    std::atomic<uint64_t> total{0};
    std::atomic<bool> failed{false};
    auto deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(
                                       std::chrono::duration<double>(secs));
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; t++) {
        pool.emplace_back([&, t] {
            std::string path = "/w" + std::to_string(same_file ? 0 : t);
            int fd = vfs_open(path.c_str(), VFS_O_WRONLY);
            if (fd < 0) { failed = true; return; }
            std::mt19937_64 rng(t + 1);
            uint64_t n = 0;
            while (Clock::now() < deadline) {
                off_t at = static_cast<off_t>((rng() % 256) * 4096);
                if (vfs_lseek(fd, at, VFS_SEEK_SET) < 0 ||
                    vfs_write(fd, block.data(), block.size()) != static_cast<ssize_t>(block.size()) ||
                    (durable && vfs_fsync(fd) != 0)) {
                    failed = true;
                    break;
                }
                n++;
            }
            vfs_close(fd);
            total += n;
        });
    }
    for (std::thread& t : pool) t.join();
    must(vfs_unmount(), "unmount");
    if (failed) throw std::runtime_error("a writer thread failed");
    return total / secs;
}

void bench_threads() {
    const double secs = quick ? 0.3 : 1.0;
    const unsigned counts[] = {1, 2, 4, 8, 16};

    std::printf("### Threads: 4 KB writes to different files, and to one shared file\n\n");
    std::printf("Each thread rewrites random blocks of a 1 MB file for %.1f s, no disk latency, "
                "64 MB cache. The first two columns have fsync off. The last has fsync on and "
                "calls vfs_fsync after every write.\n\n", secs);
    std::printf("| threads | different files, ops/s | same file, ops/s | different files + fsync each, ops/s |\n");
    std::printf("|---|---|---|---|\n");

    for (unsigned n : counts) {
        double diff = thread_run(n, false, secs);
        double same = thread_run(n, true, secs);
        double durable = thread_run(n, false, secs, true);
        std::printf("| %u | %.0f | %.0f | %.0f |\n", n, diff, same, durable);
    }
    std::printf("\n");
}

// --------------------------------------------------------- (d) the log

struct Result {
    double rate;
    uint64_t log_bytes, commits;
};

Result run_sequential(bool journal, bool fsync, uint64_t bytes) {
    fresh(64, opts(4096, 0, journal, fsync));
    vfs_reset_stats();
    auto t = Clock::now();
    write_file("/seq", bytes);
    must(vfs_sync(), "sync");
    VfsStats st = stats();
    Result r{bytes / MB / since(t), st.log_bytes, st.commits};
    must(vfs_unmount(), "unmount");
    return r;
}

// Creates `files` 4 KB files; fsync_each makes every one durable before the
// next, the way a careful program saves a document.
Result run_creates(bool journal, bool fsync, int files, bool fsync_each) {
    fresh(64, opts(4096, 0, journal, fsync));
    must(vfs_mkdir("/d"), "mkdir");
    std::string data(4096, 'c');
    vfs_reset_stats();
    auto t = Clock::now();
    for (int i = 0; i < files; i++) {
        std::string path = "/d/f" + std::to_string(i);
        int fd = static_cast<int>(must(vfs_open(path.c_str(), VFS_O_CREAT | VFS_O_WRONLY), "open"));
        write_all(fd, data);
        if (fsync_each) must(vfs_fsync(fd), "fsync");
        must(vfs_close(fd), "close");
    }
    must(vfs_sync(), "sync");
    VfsStats st = stats();
    Result r{files / since(t), st.log_bytes, st.commits};
    must(vfs_unmount(), "unmount");
    return r;
}

void bench_wal() {
    const uint64_t bytes = quick ? (4u << 20) : (16u << 20);
    const int creates = quick ? 200 : 1000;
    const int durable = quick ? 20 : 100;

    std::printf("### The cost of the write-ahead log\n\n");
    std::printf("Each run ends with vfs_sync, so everything is in disk.img when the clock stops. "
                "Best of three runs.\n\n");
    std::printf("| workload | log off | log on | on / off | bytes logged | commits |\n");
    std::printf("|---|---|---|---|---|---|\n");

    auto row = [](const char* what, const char* unit, Result off, Result on) {
        std::printf("| %s | %.1f %s | %.1f %s | %.2f | %.1f MB | %llu |\n", what, off.rate, unit,
                    on.rate, unit, on.rate / off.rate, on.log_bytes / MB,
                    static_cast<unsigned long long>(on.commits));
    };

    // Short runs are at the mercy of whatever else the machine is doing, so
    // each figure is the best of three.
    auto best = [](auto run) {
        Result r = run();
        for (int i = 0; i < 2; i++) {
            Result again = run();
            if (again.rate > r.rate) r = again;
        }
        return r;
    };

    std::string seq = std::to_string(bytes >> 20) + " MB sequential write, fsync off";
    row(seq.c_str(), "MB/s",
        best([&] { return run_sequential(false, false, bytes); }),
        best([&] { return run_sequential(true, false, bytes); }));

    std::string many = std::to_string(creates) + " x 4 KB file creates, fsync off";
    row(many.c_str(), "files/s",
        best([&] { return run_creates(false, false, creates, false); }),
        best([&] { return run_creates(true, false, creates, false); }));

    std::string each = std::to_string(durable) + " x 4 KB creates, fsync after each, fsync on";
    row(each.c_str(), "files/s",
        best([&] { return run_creates(false, true, durable, true); }),
        best([&] { return run_creates(true, true, durable, true); }));
    std::printf("\n");
}

} // namespace

int main(int argc, char** argv) {
    struct Section {
        const char* name;
        void (*run)();
        bool wanted;
    };
    Section sections[] = {
        {"sequential", bench_sequential, false},
        {"reads", bench_random_reads, false},
        {"threads", bench_threads, false},
        {"wal", bench_wal, false},
    };

    bool any = false;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--quick") == 0) {
            quick = true;
            continue;
        }
        bool known = false;
        for (Section& s : sections) {
            if (std::strcmp(argv[i], s.name) == 0) {
                s.wanted = known = any = true;
            }
        }
        if (!known) {
            std::fprintf(stderr, "usage: bench [--quick] [sequential] [reads] [threads] [wal]\n");
            return 2;
        }
    }

    image = "/tmp/vfs_bench_" + std::to_string(getpid()) + ".img";
    wal = image + ".log";

    int status = 0;
    try {
        for (Section& s : sections) {
            if (!any || s.wanted)
                s.run();
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "bench: %s\n", e.what());
        vfs_unmount();
        status = 1;
    }

    std::remove(image.c_str());
    std::remove(wal.c_str());
    return status;
}
