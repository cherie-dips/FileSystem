#include "vfs/fault.h"

#include <atomic>
#include <cerrno>
#include <csignal>

#include <unistd.h>

namespace vfs::fault {

namespace {

    constexpr size_t SECTOR = 512;

    std::atomic<uint64_t> counted{0};
    std::atomic<uint64_t> fatal{0};       // which write dies, counting from 1; 0 = none
    std::atomic<uint64_t> crash_seed{0};

    // splitmix64, so the torn length changes from one crash point to the next.
    uint64_t mix(uint64_t x) {
        x += 0x9E3779B97F4A7C15ull;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
        return x ^ (x >> 31);
    }

} // anonymous namespace

    void arm_crash(uint64_t n, uint64_t seed) {
        fatal.store(0);
        counted.store(0);
        crash_seed.store(seed);
        fatal.store(n);
    }

    uint64_t writes_counted() {
        return counted.load();
    }

    void before_write(int fd, const void* buf, size_t len, off_t offset) {
        uint64_t index = counted.fetch_add(1) + 1;
        uint64_t n = fatal.load();
        if (n == 0 || index != n)
            return;

        // A disk persists whole sectors, so the write is cut at a sector
        // boundary: anywhere from nothing to all but the last sector.
        size_t sectors = (len + SECTOR - 1) / SECTOR;
        size_t keep = static_cast<size_t>(mix(crash_seed.load() ^ index) % sectors) * SECTOR;

        const char* bytes = static_cast<const char*>(buf);
        size_t done = 0;
        while (done < keep) {
            ssize_t w = ::pwrite(fd, bytes + done, keep - done,
                                 offset + static_cast<off_t>(done));
            if (w < 0 && errno == EINTR)
                continue;
            if (w <= 0)
                break;
            done += static_cast<size_t>(w);
        }

        // No cleanup, no flush, no destructors: the process just stops, the
        // way it would if the power went.
        ::raise(SIGKILL);
        ::_exit(137);
    }

} // namespace vfs::fault
