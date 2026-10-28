//
// concurrenttest — proves stage 9 works: many threads hammering one mount.
//
// Each thread mostly works in its own directory, on files whose contents only
// it knows, and checks every byte it reads back. It also takes turns at things
// every thread shares: record files that some threads rewrite while others read
// them, and one directory where they all create and remove the same names.
// There, losing a race (EEXIST, ENOENT, EBUSY) is fine; a torn or mixed-up
// record is not. At the end the image is unmounted, checked with fsck, mounted
// again, and every thread's files are checked once more.
//
//   concurrenttest [--seconds N] [--threads N] [--seed S]
//
// The deliverable is a clean run under `make MODE=tsan`: zero reports.
//

#include "vfs/block_device.h"
#include "vfs/fsck.h"
#include "vfs/layout.h"
#include "vfs/mkfs.h"
#include "vfs/vfs.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

namespace {

constexpr uint32_t kImageBlocks  = 16384;                  // a 64 MB disk
constexpr int      kSharedFiles  = 4;
constexpr uint32_t kRecords      = 32;
constexpr uint32_t kRecordSize   = 2 * vfs::BLOCK_SIZE;    // spans two blocks
constexpr uint32_t kSharedNames  = 32;
constexpr size_t   kMaxOwnFiles  = 16;
constexpr size_t   kMaxFileSize  = 64 * 1024;
constexpr uint32_t kRecordMagic  = 0x31434552;             // "REC1"

// ---------------------------------------------------------------- failures

std::atomic<long> failures{0};
std::mutex report_mutex;
std::vector<std::string> reports;

void fail(const std::string& what) {
    failures++;
    std::lock_guard<std::mutex> lock(report_mutex);
    if (reports.size() < 10)
        reports.push_back(what);
}

std::string err(long rc) { return vfs_strerror(static_cast<int>(rc)); }

// ------------------------------------------------------------------- files

// Reads a whole file. False, with the error in rc, if a call failed.
bool read_all(const std::string& path, std::string& out, long& rc) {
    int fd = vfs_open(path.c_str(), VFS_O_RDONLY);
    if (fd < 0) {
        rc = fd;
        return false;
    }
    out.clear();
    char buf[16384];
    while (true) {
        ssize_t n = vfs_read(fd, buf, sizeof buf);
        if (n < 0) {
            rc = n;
            vfs_close(fd);
            return false;
        }
        if (n == 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    vfs_close(fd);
    rc = 0;
    return true;
}

// Writes data at offset (or appends, or replaces everything), in one call.
long write_at(const std::string& path, int flags, off_t offset, const std::string& data) {
    int fd = vfs_open(path.c_str(), flags);
    if (fd < 0) return fd;
    if (offset > 0) {
        off_t at = vfs_lseek(fd, offset, VFS_SEEK_SET);
        if (at < 0) {
            vfs_close(fd);
            return at;
        }
    }
    ssize_t n = data.empty() ? 0 : vfs_write(fd, data.data(), data.size());
    int rc = vfs_close(fd);
    if (n < 0) return n;
    if (static_cast<size_t>(n) != data.size()) return -VFS_EIO;
    return rc;
}

std::vector<std::string> list(const std::string& dir, long& rc) {
    std::vector<VfsDirEnt> ents(64);
    int n = vfs_readdir(dir.c_str(), ents.data(), ents.size());
    if (n >= 0 && static_cast<size_t>(n) > ents.size()) {
        ents.resize(static_cast<size_t>(n) + 16);
        n = vfs_readdir(dir.c_str(), ents.data(), ents.size());
    }
    rc = n < 0 ? n : 0;
    std::vector<std::string> names;
    for (int i = 0; i < n && static_cast<size_t>(i) < ents.size(); i++)
        names.push_back(ents[static_cast<size_t>(i)].name);
    return names;
}

// --------------------------------------------------------- shared records

struct RecordHeader {
    uint32_t magic, slot, thread, pad;
    uint64_t seq;
};

std::string make_record(uint32_t slot, uint32_t thread, uint64_t seq) {
    std::string rec(kRecordSize, '\0');
    RecordHeader h{kRecordMagic, slot, thread, 0, seq};
    std::memcpy(&rec[0], &h, sizeof h);
    for (size_t j = sizeof h; j < kRecordSize; j++)
        rec[j] = static_cast<char>((thread * 131 + seq * 7 + j * 13 + slot) & 0xFF);
    return rec;
}

// "" if rec is one whole record for slot, as one writer wrote it in one call.
std::string check_record(const char* rec, uint32_t slot) {
    RecordHeader h;
    std::memcpy(&h, rec, sizeof h);
    if (h.magic != kRecordMagic)
        return "record " + std::to_string(slot) + " has no header";
    if (h.slot != slot)
        return "record for slot " + std::to_string(h.slot) + " found in slot " + std::to_string(slot);
    if (std::memcmp(make_record(slot, h.thread, h.seq).data(), rec, kRecordSize) != 0)
        return "record " + std::to_string(slot) + " is torn: its header says thread "
             + std::to_string(h.thread) + ", write " + std::to_string(h.seq)
             + ", and its body is not that write";
    return "";
}

std::string shared_file(int i) { return "/shared/s" + std::to_string(i); }

// What every file in the shared directory holds: decided by its name, so any
// thread that creates it writes the same bytes.
std::string shared_name(uint32_t k) { return "/shared/dir/n" + std::to_string(k); }
std::string shared_content(uint32_t k) {
    std::string s(500 + (k * 997) % 12000, '\0');
    for (size_t j = 0; j < s.size(); j++)
        s[j] = static_cast<char>((k * 31 + j * 5 + 3) & 0xFF);
    return s;
}

// ----------------------------------------------------------------- workers

struct Worker {
    uint32_t id = 0;
    std::mt19937_64 rng;
    std::string dir;
    std::map<std::string, std::string> files;  // what this thread's files hold
    std::set<std::string> subdirs;
    uint64_t ops = 0, next = 0, seq = 0;

    uint64_t below(uint64_t n) { return rng() % n; }

    std::string random_bytes(size_t n) {
        std::string s(n, '\0');
        uint64_t x = rng();
        for (size_t j = 0; j < n; j++) {
            if (j % 8 == 0) x = x * 6364136223846793005ull + 1442695040888963407ull;
            s[j] = static_cast<char>(x >> ((j % 8) * 8));
        }
        return s;
    }

    std::string any_file() {
        auto it = files.begin();
        std::advance(it, static_cast<long>(below(files.size())));
        return it->first;
    }

    void expect(long rc, const char* what, const std::string& path) {
        if (rc < 0)
            fail("thread " + std::to_string(id) + ": " + what + " " + path + ": " + err(rc));
    }

    // ------------------------------------------------- its own directory

    void own_op() {
        unsigned r = static_cast<unsigned>(below(100));

        if (files.empty() || (r < 15 && files.size() < kMaxOwnFiles)) {
            if (files.size() >= kMaxOwnFiles) return;
            std::string path = dir + "/f" + std::to_string(next++);
            std::string data = random_bytes(1 + below(40000));
            long rc = write_at(path, VFS_O_CREAT | VFS_O_EXCL | VFS_O_WRONLY, 0, data);
            expect(rc, "create", path);
            if (rc >= 0) files[path] = data;
            return;
        }

        std::string path = any_file();
        std::string& model = files[path];

        if (r < 45) {
            std::string got;
            long rc = 0;
            if (!read_all(path, got, rc)) {
                expect(rc, "read", path);
            } else if (got != model) {
                fail("thread " + std::to_string(id) + ": " + path + " reads back "
                     + std::to_string(got.size()) + " bytes that differ from the "
                     + std::to_string(model.size()) + " it wrote");
            }
        } else if (r < 67 && model.size() > kMaxFileSize) {
            // Keeps the files - and the disk - from filling up on a long run.
            long rc = write_at(path, VFS_O_WRONLY | VFS_O_TRUNC, 0, "");
            expect(rc, "truncate", path);
            if (rc >= 0) model.clear();
        } else if (r < 55) {
            std::string data = random_bytes(1 + below(8000));
            long rc = write_at(path, VFS_O_WRONLY | VFS_O_APPEND, 0, data);
            expect(rc, "append to", path);
            if (rc >= 0) model += data;
        } else if (r < 67) {
            uint64_t offset = below(model.size() + 1);
            std::string data = random_bytes(1 + below(16000));
            long rc = write_at(path, VFS_O_RDWR, static_cast<off_t>(offset), data);
            expect(rc, "overwrite", path);
            if (rc >= 0) {
                if (model.size() < offset + data.size()) model.resize(offset + data.size(), '\0');
                model.replace(offset, data.size(), data);
            }
        } else if (r < 72) {
            long rc = write_at(path, VFS_O_WRONLY | VFS_O_TRUNC, 0, "");
            expect(rc, "truncate", path);
            if (rc >= 0) model.clear();
        } else if (r < 82) {
            long rc = vfs_unlink(path.c_str());
            expect(rc, "unlink", path);
            if (rc >= 0) files.erase(path);
        } else if (r < 88) {
            if (subdirs.size() < 3) {
                std::string sub = dir + "/s" + std::to_string(next++);
                long rc = vfs_mkdir(sub.c_str());
                expect(rc, "mkdir", sub);
                if (rc >= 0) subdirs.insert(sub);
            } else {
                std::string sub = *subdirs.begin();
                long rc = vfs_rmdir(sub.c_str());
                expect(rc, "rmdir", sub);
                if (rc >= 0) subdirs.erase(sub);
            }
        } else if (r < 94) {
            VfsStat st{};
            long rc = vfs_stat(path.c_str(), &st);
            expect(rc, "stat", path);
            if (rc >= 0 && st.size != model.size())
                fail("thread " + std::to_string(id) + ": stat says " + path + " is "
                     + std::to_string(st.size) + " bytes, not " + std::to_string(model.size()));
        } else {
            check_listing();
        }
    }

    void check_listing() {
        long rc = 0;
        std::vector<std::string> names = list(dir, rc);
        expect(rc, "readdir", dir);
        if (rc < 0) return;

        std::set<std::string> want{".", ".."};
        for (const auto& [p, data] : files) want.insert(p.substr(dir.size() + 1));
        for (const std::string& s : subdirs) want.insert(s.substr(dir.size() + 1));
        std::set<std::string> got(names.begin(), names.end());
        if (got != want || got.size() != names.size())
            fail("thread " + std::to_string(id) + ": " + dir + " lists " + std::to_string(names.size())
                 + " entries, expected " + std::to_string(want.size()));
    }

    // ------------------------------------------------ what everyone shares

    void shared_op() {
        unsigned r = static_cast<unsigned>(below(100));
        int s = static_cast<int>(below(kSharedFiles));
        uint32_t slot = static_cast<uint32_t>(below(kRecords));
        uint32_t k = static_cast<uint32_t>(below(kSharedNames));

        if (r < 25) {
            std::string rec = make_record(slot, id, ++seq);
            expect(write_at(shared_file(s), VFS_O_WRONLY, static_cast<off_t>(slot) * kRecordSize, rec),
                   "write a record to", shared_file(s));
        } else if (r < 45) {
            int fd = vfs_open(shared_file(s).c_str(), VFS_O_RDONLY);
            if (fd < 0) return expect(fd, "open", shared_file(s));
            std::string rec(kRecordSize, '\0');
            off_t at = vfs_lseek(fd, static_cast<off_t>(slot) * kRecordSize, VFS_SEEK_SET);
            ssize_t n = at < 0 ? at : vfs_read(fd, &rec[0], rec.size());
            vfs_close(fd);
            if (n < 0) return expect(n, "read a record from", shared_file(s));
            if (static_cast<size_t>(n) != rec.size())
                return fail("short read of a record: " + std::to_string(n) + " bytes");
            std::string bad = check_record(rec.data(), slot);
            if (!bad.empty()) fail(shared_file(s) + ": " + bad);
        } else if (r < 50) {
            // All 32 records in one read: every one must be whole.
            int fd = vfs_open(shared_file(s).c_str(), VFS_O_RDONLY);
            if (fd < 0) return expect(fd, "open", shared_file(s));
            std::string all(kRecords * kRecordSize, '\0');
            ssize_t n = vfs_read(fd, &all[0], all.size());
            vfs_close(fd);
            if (n < 0) return expect(n, "read", shared_file(s));
            if (static_cast<size_t>(n) != all.size())
                return fail("short read of " + shared_file(s));
            for (uint32_t i = 0; i < kRecords; i++) {
                std::string bad = check_record(all.data() + i * kRecordSize, i);
                if (!bad.empty()) return fail(shared_file(s) + ": " + bad);
            }
        } else if (r < 70) {
            int fd = vfs_open(shared_name(k).c_str(), VFS_O_CREAT | VFS_O_EXCL | VFS_O_WRONLY);
            if (fd == -VFS_EEXIST) return;
            if (fd < 0) return expect(fd, "create", shared_name(k));
            std::string data = shared_content(k);
            ssize_t n = vfs_write(fd, data.data(), data.size());
            vfs_close(fd);
            if (n < 0) return expect(n, "write", shared_name(k));
            if (static_cast<size_t>(n) != data.size()) fail("short write to " + shared_name(k));
        } else if (r < 85) {
            std::string got;
            long rc = 0;
            if (!read_all(shared_name(k), got, rc)) {
                if (rc != -VFS_ENOENT) expect(rc, "read", shared_name(k));
                return;
            }
            // Its creator writes it in one call, so a reader sees all of it or
            // none of it - nothing in between, and never someone else's bytes.
            if (!got.empty() && got != shared_content(k))
                fail(shared_name(k) + " holds " + std::to_string(got.size())
                     + " bytes that are not what any thread wrote");
        } else if (r < 95) {
            long rc = vfs_unlink(shared_name(k).c_str());
            if (rc != 0 && rc != -VFS_ENOENT && rc != -VFS_EBUSY)
                expect(rc, "unlink", shared_name(k));
        } else {
            long rc = 0;
            std::vector<std::string> names = list("/shared/dir", rc);
            if (rc < 0) return expect(rc, "readdir", "/shared/dir");
            std::set<std::string> seen;
            for (const std::string& name : names) {
                bool known = name == "." || name == "..";
                for (uint32_t i = 0; i < kSharedNames && !known; i++)
                    known = name == "n" + std::to_string(i);
                if (!known || !seen.insert(name).second)
                    return fail("/shared/dir lists a bad or repeated name: \"" + name + "\"");
            }
        }
    }

    void run(std::chrono::steady_clock::time_point deadline) {
        while (std::chrono::steady_clock::now() < deadline) {
            if (below(100) < 65) own_op();
            else shared_op();
            ops++;
        }
    }

    // Every file this thread owns holds what it wrote. Used at the end.
    long verify_files() {
        long bad = 0;
        for (const auto& [path, model] : files) {
            std::string got;
            long rc = 0;
            if (!read_all(path, got, rc)) {
                fail(path + ": " + err(rc));
                bad++;
            } else if (got != model) {
                fail(path + " lost its contents: " + std::to_string(got.size())
                     + " bytes, expected " + std::to_string(model.size()));
                bad++;
            }
        }
        check_listing();
        return bad;
    }
};

int checks_failed = 0;

void check(bool ok, const std::string& what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) checks_failed++;
}

void print_reports() {
    std::lock_guard<std::mutex> lock(report_mutex);
    for (const std::string& r : reports)
        std::printf("           -> %s\n", r.c_str());
    reports.clear();
}

bool verify_shared_records() {
    for (int s = 0; s < kSharedFiles; s++) {
        std::string all;
        long rc = 0;
        if (!read_all(shared_file(s), all, rc) || all.size() != kRecords * kRecordSize) {
            fail(shared_file(s) + " cannot be read back whole");
            return false;
        }
        for (uint32_t i = 0; i < kRecords; i++) {
            std::string bad = check_record(all.data() + i * kRecordSize, i);
            if (!bad.empty()) {
                fail(shared_file(s) + ": " + bad);
                return false;
            }
        }
    }
    return true;
}

VfsOptions options() {
    VfsOptions o;
    o.fsync              = false;
    o.flusher            = true;
    o.commit_interval_ms = 50;          // the flusher commits while the threads work
    o.checkpoint_bytes   = 4u << 20;    // and checkpoints happen during the run too
    return o;
}

} // namespace

int main(int argc, char** argv) {
    double seconds = 5;
    unsigned threads = 16;
    uint64_t seed = 1;

    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) seconds = std::atof(argv[++i]);
        else if (std::strcmp(argv[i], "--threads") == 0 && i + 1 < argc) threads = static_cast<unsigned>(std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 10);
        else {
            std::fprintf(stderr, "usage: concurrenttest [--seconds N] [--threads N] [--seed S]\n");
            return 2;
        }
    }
    if (threads == 0) threads = 1;

    const std::string image = "/tmp/vfs_concurrenttest_" + std::to_string(getpid()) + ".img";
    const std::string log = image + ".log";
    auto cleanup = [&] {
        std::remove(image.c_str());
        std::remove(log.c_str());
    };

    try {
        cleanup();
        {
            vfs::PreadDevice dev = vfs::PreadDevice::create(image.c_str(), kImageBlocks);
            vfs::mkfs(dev);
        }

        VfsOptions o = options();
        int rc = vfs_mount(image.c_str(), log.c_str(), &o);
        if (rc != 0) {
            std::printf("concurrenttest: mount failed: %s\n", vfs_strerror(rc));
            cleanup();
            return 1;
        }

        // Set up what the threads share, and a directory each.
        bool setup = vfs_mkdir("/shared") == 0 && vfs_mkdir("/shared/dir") == 0;
        for (int s = 0; s < kSharedFiles && setup; s++) {
            std::string all;
            for (uint32_t i = 0; i < kRecords; i++)
                all += make_record(i, 9999, 0);
            setup = write_at(shared_file(s), VFS_O_CREAT | VFS_O_WRONLY, 0, all) == 0;
        }
        std::vector<Worker> workers(threads);
        for (unsigned t = 0; t < threads && setup; t++) {
            workers[t].id = t;
            workers[t].rng.seed(seed * 7919 + t);
            workers[t].dir = "/t" + std::to_string(t);
            setup = vfs_mkdir(workers[t].dir.c_str()) == 0;
        }
        if (!setup) {
            std::printf("concurrenttest: setting up the shared files failed\n");
            vfs_unmount();
            cleanup();
            return 1;
        }

        auto start = std::chrono::steady_clock::now();
        auto deadline = start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                    std::chrono::duration<double>(seconds));
        std::vector<std::thread> pool;
        for (Worker& w : workers)
            pool.emplace_back([&w, deadline] { w.run(deadline); });
        for (std::thread& t : pool)
            t.join();
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

        uint64_t total = 0;
        for (const Worker& w : workers) total += w.ops;
        std::printf("%u threads for %.1f s: %llu operations (%.0f per second)\n",
                    threads, secs, static_cast<unsigned long long>(total), total / secs);

        check(failures == 0, "no thread read back anything but what was written");
        print_reports();

        long before = failures;
        for (Worker& w : workers) w.verify_files();
        check(failures == before && verify_shared_records(),
              "after the run, every file holds what its thread wrote");
        print_reports();

        rc = vfs_unmount();
        check(rc == 0, "unmount");

        {
            vfs::PreadDevice dev = vfs::PreadDevice::open(image.c_str());
            vfs::FsckReport report = vfs::fsck(dev);
            check(report.ok(), "fsck after unmount: " + std::to_string(report.files) + " files, "
                               + std::to_string(report.directories) + " directories");
            for (size_t i = 0; i < report.problems.size() && i < 10; i++)
                std::printf("           -> %s\n", report.problems[i].c_str());
        }

        rc = vfs_mount(image.c_str(), log.c_str(), &o);
        check(rc == 0, "mount again");
        if (rc == 0) {
            before = failures;
            for (Worker& w : workers) w.verify_files();
            check(failures == before && verify_shared_records(),
                  "and every file still holds what its thread wrote");
            print_reports();
            check(vfs_unmount() == 0, "unmount again");
        }

        cleanup();
        std::printf("\n%s\n", checks_failed == 0 ? "stage 9: all checks passed" : "stage 9: FAILURES");
        return checks_failed == 0 ? 0 : 1;

    } catch (const std::exception& e) {
        std::fprintf(stderr, "concurrenttest: unexpected exception: %s\n", e.what());
        cleanup();
        return 1;
    }
}
