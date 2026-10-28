#pragma once

// fault.h — simulated crashes, for the crash test.

#include <cstddef>
#include <cstdint>
#include <sys/types.h>

namespace vfs::fault {

    // Arms a crash. Counting from now, the n-th write - every block written
    // to disk.img and every record written to the log counts - is cut short at
    // a 512-byte sector boundary picked from seed, and the process is killed
    // with SIGKILL before the write returns. n = 0 disarms.
    void arm_crash(uint64_t n, uint64_t seed);

    // Writes counted since the last arm_crash(), or since the program started.
    uint64_t writes_counted();

    // For the two write paths: call just before writing len bytes of buf at
    // offset in fd. Returns at once unless this is the fatal write; if it is,
    // writes a sector-aligned prefix of buf and never returns.
    void before_write(int fd, const void* buf, size_t len, off_t offset);

} // namespace vfs::fault
