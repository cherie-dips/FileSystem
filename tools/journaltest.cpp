//
// journaltest — proves the journal makes changes all-or-nothing (stages 7-9).
//
// The storage engine is driven directly. Operations fill whole blocks with a
// byte that names them, so after a crash every block shows exactly which
// operation last wrote it. The crash test kills a child process at every
// single write the workload makes, one run per write, tearing that write at a
// sector boundary; after each, recovery must leave the image exactly as some
// whole number of committed transactions left it - never part of one, and
// never missing one that commit() had returned for.
//
// Run it under `make asan` and `make tsan` as well as `make`.
//

#include "vfs/block_device.h"
#include "vfs/fault.h"
#include "vfs/layout.h"
#include "vfs/mkfs.h"
#include "vfs/storage.h"
#include "vfs/superblock.h"
#include "vfs/wal.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

const std::string kPrefix = "/tmp/vfs_journaltest_" + std::to_string(::getpid());
const std::string kBase   = kPrefix + "_base.img";
const std::string kBig    = kPrefix + "_big.img";
const std::string kImg    = kPrefix + ".img";
const std::string kLog    = kImg + ".log";

// fsync off: a killed process loses nothing it handed to the OS, so for these
// tests only the order of writes matters - and that does not need fsync. The
// flusher off: every run then makes exactly the same writes.
vfs::StorageOptions quiet() {
    vfs::StorageOptions o;
    o.cache_frames = 1024;
    o.fsync = false;
    o.flusher = false;
    return o;
}

void make_image(const std::string& path, uint32_t blocks) {
    std::remove(path.c_str());
    vfs::PreadDevice dev = vfs::PreadDevice::create(path.c_str(), blocks);
    vfs::mkfs(dev);
}

void copy_file(const std::string& from, const std::string& to) {
    int in = ::open(from.c_str(), O_RDONLY);
    int out = ::open(to.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (in < 0 || out < 0)
        throw std::runtime_error("copy_file: open failed");
    std::vector<char> buf(1 << 16);
    ssize_t n;
    while ((n = ::read(in, buf.data(), buf.size())) > 0) {
        if (::write(out, buf.data(), static_cast<size_t>(n)) != n)
            throw std::runtime_error("copy_file: write failed");
    }
    ::close(in);
    ::close(out);
}

// A fresh, clean copy of the base image, with no log beside it.
void reset_image() {
    copy_file(kBase, kImg);
    std::remove(kLog.c_str());
}

uint64_t file_size(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 ? static_cast<uint64_t>(st.st_size) : 0;
}

auto fill(uint8_t v) {
    return [v](vfs::Block& b) { b.fill(v); };
}

// Reads a block straight from the image file, past any cache.
vfs::Block on_disk(const std::string& path, uint32_t block_no) {
    vfs::PreadDevice dev = vfs::PreadDevice::open(path.c_str());
    vfs::Block b;
    dev.read_block(block_no, b);
    return b;
}

bool all_bytes(const vfs::Block& b, uint8_t v) {
    for (uint8_t x : b)
        if (x != v) return false;
    return true;
}

// ---------------------------------------------------------------- basics

void check_reopen() {
    std::printf("committed changes survive\n");
    reset_image();

    uint32_t base;
    {
        vfs::Storage s(kImg.c_str(), kLog.c_str(), quiet());
        base = s.superblock().data_start;
        vfs::Journal& j = s.journal();

        { vfs::OpScope op(j); j.write(base + 1, true, fill(0x5A)); }
        j.commit();
        check(file_size(kLog) > 0, "commit() puts the transaction in the log");
        check(all_bytes(on_disk(kImg, base + 1), 0), "and leaves disk.img alone for now");

        { vfs::OpScope op(j); j.write(base + 2, true, fill(0x5B)); }
        s.close();
        check(file_size(kLog) == 0, "close() checkpoints: the log ends empty");
    }
    check(all_bytes(on_disk(kImg, base + 1), 0x5A) && all_bytes(on_disk(kImg, base + 2), 0x5B),
          "both blocks are in disk.img, the second committed by close() itself");

    {
        vfs::PreadDevice dev = vfs::PreadDevice::open(kImg.c_str());
        check(vfs::read_superblock(dev).clean_unmount == 1, "the image is marked clean");
    }

    vfs::Storage s(kImg.c_str(), kLog.c_str(), quiet());
    check(!s.recovery().ran, "so the next mount needs no recovery");
    vfs::Block b;
    s.journal().read(base + 1, b);
    check(all_bytes(b, 0x5A), "and reads the block back");

    bool refused = false;
    try {
        vfs::OpScope op(s.journal());
        s.journal().write(0, false, fill(1));
    } catch (const std::out_of_range&) {
        refused = true;
    }
    check(refused, "block 0 is not the journal's to write");
    s.close();
}

void check_stale_log() {
    std::printf("a stale log\n");
    reset_image();

    // A perfectly good committed transaction - but beside an image that was
    // unmounted cleanly, so it belongs to some other history.
    uint32_t base;
    {
        vfs::PreadDevice dev = vfs::PreadDevice::open(kImg.c_str());
        base = vfs::read_superblock(dev).data_start;
    }
    {
        vfs::Wal wal(kLog.c_str(), false);
        wal.reset(1);
        vfs::Block b;
        b.fill(0x99);
        wal.append(vfs::WAL_BEGIN, 1, 0, nullptr);
        wal.append(vfs::WAL_BLOCK, 1, base + 1, &b);
        wal.append(vfs::WAL_COMMIT, 1, 0, nullptr);
        wal.sync();
    }

    vfs::Storage s(kImg.c_str(), kLog.c_str(), quiet());
    vfs::Block b;
    s.journal().read(base + 1, b);
    check(!s.recovery().ran && all_bytes(b, 0), "is never replayed onto a clean image");
    check(file_size(kLog) == 0, "it is thrown away at mount");
    s.close();
}

void check_checkpoint() {
    std::printf("checkpoint\n");
    reset_image();

    vfs::Storage s(kImg.c_str(), kLog.c_str(), quiet());
    const uint32_t base = s.superblock().data_start;
    vfs::Journal& j = s.journal();
    for (uint32_t i = 1; i <= 3; i++) {
        vfs::OpScope op(j);
        j.write(base + i, true, fill(static_cast<uint8_t>(0x60 + i)));
    }
    j.commit();
    check(file_size(kLog) > 0 && all_bytes(on_disk(kImg, base + 1), 0),
          "committed blocks wait in the cache; the log has them");

    s.checkpoint();
    check(file_size(kLog) == 0, "a checkpoint empties the log");
    check(all_bytes(on_disk(kImg, base + 1), 0x61) && all_bytes(on_disk(kImg, base + 3), 0x63),
          "because every block is now in disk.img");
    check(s.stats().checkpoints == 1 && s.stats().commits == 1, "one commit, one checkpoint");
    s.close();
}

void check_no_journal() {
    std::printf("journal off\n");
    reset_image();

    vfs::StorageOptions o = quiet();
    o.journal = false;
    uint32_t base;
    {
        vfs::Storage s(kImg.c_str(), kLog.c_str(), o);
        base = s.superblock().data_start;
        { vfs::OpScope op(s.journal()); s.journal().write(base + 1, true, fill(0x77)); }
        s.journal().commit();
        check(all_bytes(on_disk(kImg, base + 1), 0x77), "commit() writes the block itself back");
        check(file_size(kLog) == 0 && s.stats().log_bytes == 0, "and nothing goes to a log");
        s.close();
    }
    vfs::Storage s(kImg.c_str(), kLog.c_str(), quiet());
    vfs::Block b;
    s.journal().read(base + 1, b);
    check(all_bytes(b, 0x77), "a journaled mount reads it back");
    s.close();
}

void check_big_transactions() {
    std::printf("more blocks than one transaction holds\n");

    // A 256-frame cache lets a transaction hold 64 blocks. 40 operations of
    // 40 blocks each cannot share one.
    vfs::StorageOptions o = quiet();
    o.cache_frames = 256;
    std::remove((kBig + ".log").c_str());

    uint32_t base;
    {
        vfs::Storage s(kBig.c_str(), (kBig + ".log").c_str(), o);
        base = s.superblock().data_start;
        vfs::Journal& j = s.journal();
        for (uint32_t i = 0; i < 40; i++) {
            vfs::OpScope op(j);
            for (uint32_t k = 0; k < 40; k++)
                j.write(base + 1 + i * 40 + k, true, fill(static_cast<uint8_t>(i + 1)));
        }
        check(s.stats().commits >= 39, "the journal commits on its own when a transaction fills");
        s.close();
    }

    vfs::Storage s(kBig.c_str(), (kBig + ".log").c_str(), o);
    bool all = true;
    vfs::Block b;
    for (uint32_t i = 0; i < 40 && all; i++) {
        for (uint32_t k = 0; k < 40 && all; k++) {
            s.journal().read(base + 1 + i * 40 + k, b);
            all = all_bytes(b, static_cast<uint8_t>(i + 1));
        }
    }
    check(all, "all 1600 blocks are there after a remount, through a 256-block cache");
    s.close();
}

// ------------------------------------------------------------------ crashes

// The five blocks the workload writes, past data_start.
constexpr uint32_t X[5] = {10, 11, 12, 13, 14};

struct Write {
    int x;          // which of X
    uint8_t value;
};

// Five operations. Operations 2 and 3 run back to back with no commit between
// them, so they share a transaction and must land together.
const std::vector<std::vector<Write>> kOps = {
    {{0, 0x11}, {1, 0x12}},   // op 1
    {{1, 0x22}},              // op 2  \ one transaction
    {{2, 0x23}},              // op 3  /
    {{0, 0x31}, {3, 0x34}},   // op 4
    {{4, 0x45}, {2, 0x43}},   // op 5: committed only by close(), if at all
};

// What the five blocks hold after the first k operations.
std::vector<uint8_t> state_after(size_t k) {
    std::vector<uint8_t> s(5, 0);
    for (size_t i = 0; i < k; i++)
        for (const Write& w : kOps[i])
            s[static_cast<size_t>(w.x)] = w.value;
    return s;
}

enum class Ending { leave_open, close };

constexpr uint8_t kCountMarker = 0xFF;

// The child's last act: report how many writes it made, then stop dead.
// _exit, not return: running the Storage's destructor would close it cleanly
// and commit the transaction the workload means to leave open.
[[noreturn]] void finish(int report_fd) {
    uint64_t n = vfs::fault::writes_counted();
    uint8_t marker = kCountMarker;
    if (::write(report_fd, &marker, 1) != 1 || ::write(report_fd, &n, sizeof n) != sizeof n)
        ::_exit(3);
    ::_exit(0);
}

// Runs in the child. Reports, after each durable point returns, how many
// operations are now durable.
void workload(Ending ending, int report_fd) {
    vfs::Storage s(kImg.c_str(), kLog.c_str(), quiet());
    vfs::Journal& j = s.journal();
    const uint32_t base = s.superblock().data_start;

    auto run_op = [&](size_t i) {
        vfs::OpScope op(j);
        for (const Write& w : kOps[i])
            j.write(base + X[w.x], true, fill(w.value));
    };
    auto report = [&](uint8_t durable_ops) {
        if (::write(report_fd, &durable_ops, 1) != 1)
            ::_exit(3);
    };

    run_op(0);
    j.commit();
    report(1);

    run_op(1);
    run_op(2);
    j.commit();
    report(3);

    s.checkpoint();
    report(3);

    run_op(3);
    j.commit();
    report(4);

    run_op(4);
    if (ending == Ending::close) {
        s.close();
        report(5);
    }
    finish(report_fd);
}

struct ChildResult {
    bool killed = false;            // died of SIGKILL
    bool exited_ok = false;
    int durable_ops = 0;            // the last report
    uint64_t writes = 0;            // only if it finished
};

ChildResult run_child(uint64_t crash_at, Ending ending) {
    int fds[2];
    if (::pipe(fds) != 0)
        throw std::runtime_error("pipe failed");

    std::fflush(stdout);
    pid_t pid = ::fork();
    if (pid < 0)
        throw std::runtime_error("fork failed");

    if (pid == 0) {
        ::close(fds[0]);
        try {
            vfs::fault::arm_crash(crash_at, crash_at * 7919 + 1);
            workload(ending, fds[1]);
        } catch (...) {
            ::_exit(2);
        }
        ::_exit(4);
    }

    ::close(fds[1]);
    ChildResult r;
    std::vector<uint8_t> bytes;
    uint8_t buf[64];
    ssize_t n;
    while ((n = ::read(fds[0], buf, sizeof buf)) > 0)
        bytes.insert(bytes.end(), buf, buf + n);
    ::close(fds[0]);

    for (size_t i = 0; i < bytes.size(); i++) {
        if (bytes[i] == kCountMarker && i + 1 + sizeof(uint64_t) <= bytes.size()) {
            std::memcpy(&r.writes, &bytes[i + 1], sizeof(uint64_t));
            break;
        }
        r.durable_ops = bytes[i];
    }

    int status = 0;
    ::waitpid(pid, &status, 0);
    r.killed = WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL;
    r.exited_ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    return r;
}

// Runs one workload once per write it makes, crashing at that write, and
// checks what recovery leaves behind each time.
void crash_everywhere(Ending ending, const char* name) {
    // The operation counts at which a commit can finish.
    std::vector<int> commit_points = {0, 1, 3, 4};
    if (ending == Ending::close)
        commit_points.push_back(5);

    reset_image();
    ChildResult dry = run_child(0, ending);
    if (!dry.exited_ok || dry.writes == 0) {
        check(false, "the workload runs to the end when nothing crashes");
        return;
    }

    int wrong_state = 0, torn = 0, lost_commit = 0, not_killed = 0, replays = 0;
    for (uint64_t n = 1; n <= dry.writes + 1; n++) {
        reset_image();
        ChildResult r = run_child(n, ending);
        const bool should_die = n <= dry.writes;
        if (should_die != r.killed || (!should_die && !r.exited_ok))
            not_killed++;

        vfs::Storage s(kImg.c_str(), kLog.c_str(), quiet());
        if (s.recovery().txns_replayed > 0)
            replays++;

        const uint32_t base = s.superblock().data_start;
        std::vector<uint8_t> got(5);
        bool whole = true;
        for (size_t i = 0; i < 5; i++) {
            vfs::Block b;
            s.journal().read(base + X[i], b);
            got[i] = b[0];
            whole = whole && all_bytes(b, b[0]);
        }
        s.close();
        if (!whole) {
            torn++;
            continue;
        }

        // Allowed: what was durable at the last report, or the next commit
        // point, if the crash came after its COMMIT record but before
        // commit() could return.
        int next = -1;
        for (int p : commit_points) {
            if (p > r.durable_ops) {
                next = p;
                break;
            }
        }
        if (got == state_after(static_cast<size_t>(r.durable_ops)))
            continue;
        if (next >= 0 && got == state_after(static_cast<size_t>(next)))
            continue;

        bool some_prefix = false;
        for (int p : commit_points)
            some_prefix = some_prefix || got == state_after(static_cast<size_t>(p));
        (some_prefix ? lost_commit : wrong_state)++;
    }

    std::printf("  ..    %s: %llu writes, a crash at each one, %d runs replayed the log\n",
                name, static_cast<unsigned long long>(dry.writes), replays);
    check(not_killed == 0, "every run died exactly at its crash point");
    check(torn == 0, "no block is ever left half-written");
    check(wrong_state == 0, "the image always matches a whole number of committed transactions");
    check(lost_commit == 0, "and never loses one that commit() had returned for");
}

void check_crashes() {
    std::printf("a crash at every write\n");
    crash_everywhere(Ending::leave_open, "workload ending with a transaction left open");
    crash_everywhere(Ending::close, "workload ending with close()");
}

// ------------------------------------------------------------------ threads

void check_threads() {
    std::printf("many threads\n");
    reset_image();

    vfs::StorageOptions o = quiet();
    o.cache_frames = 2048;        // room for 8 operations in one transaction
    o.flusher = true;
    o.commit_interval_ms = 5;

    constexpr int kThreads = 8;
    constexpr uint32_t kRounds = 300;

    uint32_t base;
    {
        vfs::Storage s(kImg.c_str(), kLog.c_str(), o);
        base = s.superblock().data_start;
        const uint32_t shared = base + 1;
        vfs::Journal& j = s.journal();

        std::atomic<bool> done{false};
        std::atomic<int> went_backwards{0};
        std::thread reader([&] {
            uint32_t last[kThreads] = {};
            while (!done.load()) {
                vfs::Block b;
                j.read(shared, b);
                for (int t = 0; t < kThreads; t++) {
                    uint32_t v;
                    std::memcpy(&v, b.data() + 4 * t, 4);
                    if (v < last[t])
                        went_backwards++;
                    last[t] = v;
                }
            }
        });

        std::vector<std::thread> workers;
        for (int t = 0; t < kThreads; t++) {
            workers.emplace_back([&j, base, shared, t] {
                for (uint32_t i = 0; i < kRounds; i++) {
                    {
                        vfs::OpScope op(j);
                        j.write(base + 10 + static_cast<uint32_t>(t), true,
                                fill(static_cast<uint8_t>(i)));
                        j.write(shared, false, [t](vfs::Block& b) {
                            uint32_t v;
                            std::memcpy(&v, b.data() + 4 * t, 4);
                            v++;
                            std::memcpy(b.data() + 4 * t, &v, 4);
                        });
                    }
                    if (i % 50 == 49)
                        j.commit();
                }
            });
        }
        for (std::thread& w : workers)
            w.join();
        done = true;
        reader.join();

        check(went_backwards.load() == 0, "a reader never sees a counter go backwards");
        check(s.stats().commits > 0, "the threads' operations were committed along the way");

        vfs::Block b;
        j.read(shared, b);
        bool counts = true;
        for (int t = 0; t < kThreads; t++) {
            uint32_t v;
            std::memcpy(&v, b.data() + 4 * t, 4);
            counts = counts && v == kRounds;
        }
        check(counts, "8 threads x 300 increments of one shared block: none lost");
        s.close();
    }

    vfs::Storage s(kImg.c_str(), kLog.c_str(), quiet());
    vfs::Block b;
    s.journal().read(base + 1, b);
    bool counts = true;
    for (int t = 0; t < kThreads; t++) {
        uint32_t v;
        std::memcpy(&v, b.data() + 4 * t, 4);
        counts = counts && v == kRounds;
    }
    bool own = true;
    for (int t = 0; t < kThreads; t++) {
        vfs::Block mine;
        s.journal().read(base + 10 + static_cast<uint32_t>(t), mine);
        own = own && all_bytes(mine, static_cast<uint8_t>(kRounds - 1));
    }
    check(counts && own, "and all of it is still there after a remount");
    s.close();
}

void check_flusher_commits() {
    std::printf("the flusher\n");
    reset_image();

    vfs::StorageOptions o = quiet();
    o.flusher = true;
    o.commit_interval_ms = 20;

    vfs::Storage s(kImg.c_str(), kLog.c_str(), o);
    const uint32_t base = s.superblock().data_start;
    { vfs::OpScope op(s.journal()); s.journal().write(base + 1, true, fill(0x42)); }

    bool committed = false;
    for (int i = 0; i < 100 && !committed; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        committed = s.stats().commits > 0;
    }
    check(committed, "commits an open transaction on its own, within its interval");
    s.close();
}

} // namespace

int main() {
    try {
        make_image(kBase, 512);
        make_image(kBig, 2048);

        check_reopen();
        check_stale_log();
        check_checkpoint();
        check_no_journal();
        check_big_transactions();
        check_crashes();
        check_threads();
        check_flusher_commits();

        for (const std::string& p : {kBase, kBig, kBig + ".log", kImg, kLog})
            std::remove(p.c_str());

        std::printf("\n%s\n", failures == 0
                                  ? "stage 8: all checks passed"
                                  : "stage 8: FAILURES");
        return failures == 0 ? 0 : 1;

    } catch (const std::exception& e) {
        std::fprintf(stderr, "journaltest: unexpected exception: %s\n", e.what());
        for (const std::string& p : {kBase, kBig, kBig + ".log", kImg, kLog})
            std::remove(p.c_str());
        return 1;
    }
}
