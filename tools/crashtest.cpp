//
// crashtest — proves stage 8 works: killed at 500 different points, the file
// system comes back consistent every time, with nothing committed lost.
//
// One fixed workload of file system calls runs in a child process, which is
// killed with SIGKILL in the middle of its n-th write - to the image or to the
// log, cut off at a sector boundary - for 500 different n. After each crash
// the parent mounts the image, which replays the log, and checks that
//
//   1. fsck finds nothing wrong, and
//   2. the tree holds exactly what it held after some prefix of the
//      workload's calls: no call half-applied, and none lost that a
//      completed fsync had promised.
//
// Every fifth point also kills the recovery itself, then recovers again.
//
//   crashtest [--points N] [--seed S] [--no-journal]
//
// --no-journal runs the workload with the log turned off. Crashes should then
// leave damage behind, and this test should say so: it is how to check that
// the test can fail at all.
//

#include "vfs/block_device.h"
#include "vfs/fault.h"
#include "vfs/fsck.h"
#include "vfs/layout.h"
#include "vfs/mkfs.h"
#include "vfs/vfs.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr uint32_t kImageBlocks = 1024;                  // a 4 MB disk
constexpr size_t   kCalls       = 160;                   // calls that change something
constexpr uint32_t kMaxWrite    = 15 * vfs::BLOCK_SIZE;  // touches at most 16 blocks
constexpr uint64_t kSparseAt    = 4300000;               // past 4 MB: the double-indirect tree

struct Paths {
    std::string image, log, base;
};

bool journal_off = false;  // --no-journal: only the workload's mounts

VfsOptions options() {
    VfsOptions o;
    o.cache_frames     = 256;         // the smallest: evictions happen all the time
    o.fsync            = false;       // a killed process loses nothing the OS already has;
                                      // what is being tested is the order of the writes
    o.flusher          = false;       // no background thread: the same writes every run
    o.checkpoint_bytes = 256 * 1024;  // checkpoints happen all the time too
    return o;
}

// ------------------------------------------------------------------ model

struct Tree {
    std::map<std::string, std::string> files;
    std::set<std::string> dirs;  // every directory except "/"

    bool operator==(const Tree& o) const { return files == o.files && dirs == o.dirs; }
};

enum class Kind { create, write, append, truncate, mkdir, rmdir, unlink, fsync, sync };

struct Op {
    Kind kind;
    std::string path;
    uint64_t offset;
    uint32_t len;

    Op(Kind k, std::string p, uint64_t off = 0, uint32_t l = 0)
        : kind(k), path(std::move(p)), offset(off), len(l) {}
};

bool mutates(Kind k) { return k != Kind::fsync && k != Kind::sync; }

// What op number `op` writes. Different for every op, so a write that went
// to the wrong place, or came from the wrong op, cannot match by accident.
std::string data_for(size_t op, uint32_t len) {
    std::string s(len, '\0');
    for (uint32_t j = 0; j < len; j++)
        s[j] = static_cast<char>((op * 131 + j * 7 + j / 4096 + 1) & 0xFF);
    return s;
}

void apply(Tree& t, const Op& op, size_t index) {
    switch (op.kind) {
        case Kind::create:
        case Kind::truncate: t.files[op.path].clear(); break;
        case Kind::write: {
            std::string& f = t.files[op.path];
            if (f.size() < op.offset + op.len)
                f.resize(op.offset + op.len, '\0');
            f.replace(op.offset, op.len, data_for(index, op.len));
            break;
        }
        case Kind::append: t.files[op.path] += data_for(index, op.len); break;
        case Kind::mkdir:  t.dirs.insert(op.path); break;
        case Kind::rmdir:  t.dirs.erase(op.path); break;
        case Kind::unlink: t.files.erase(op.path); break;
        case Kind::fsync:
        case Kind::sync:   break;
    }
}

bool is_empty_dir(const Tree& t, const std::string& d) {
    std::string prefix = d + "/";
    for (const auto& [path, data] : t.files)
        if (path.compare(0, prefix.size(), prefix) == 0) return false;
    for (const std::string& other : t.dirs)
        if (other.compare(0, prefix.size(), prefix) == 0) return false;
    return true;
}

std::string join(const std::string& dir, const std::string& name) {
    return dir == "/" ? "/" + name : dir + "/" + name;
}

// The workload: kCalls calls that change the tree, with fsync and sync
// sprinkled between them. Generated against the model, so every call is one
// that should succeed.
std::vector<Op> make_workload(uint64_t seed) {
    std::mt19937_64 rng(seed);
    auto below = [&](uint64_t n) { return rng() % n; };

    Tree t;
    std::vector<Op> ops;
    size_t calls = 0;
    int next_name = 0;

    auto push = [&](Op op) {
        if (mutates(op.kind)) {
            apply(t, op, ops.size());
            calls++;
        }
        ops.push_back(std::move(op));
    };

    while (calls < kCalls) {
        // A sparse file reaching into the double-indirect tree, which lives
        // for a while and is then removed.
        if (calls == 4)  { push({Kind::create, "/sparse"}); continue; }
        if (calls == 5)  { push({Kind::write, "/sparse", kSparseAt, 300}); continue; }
        if (calls == 40) { push({Kind::unlink, "/sparse"}); continue; }

        std::vector<std::string> files;
        uint64_t bytes = 0;
        for (const auto& [path, data] : t.files) {
            bytes += data.size();
            if (path != "/sparse") files.push_back(path);
        }
        std::vector<std::string> dirs{"/"};
        for (const std::string& d : t.dirs)
            if (std::count(d.begin(), d.end(), '/') < 3) dirs.push_back(d);

        const std::string dir = dirs[below(dirs.size())];
        const bool roomy = bytes < 1000 * 1024;
        auto any_file = [&] { return files[below(files.size())]; };

        Op op{Kind::sync, ""};
        unsigned r = static_cast<unsigned>(below(100));

        if (r < 10) {
            if (files.empty() || below(4) == 0) op = {Kind::sync, ""};
            else op = {Kind::fsync, any_file()};
        } else if (r < 18) {
            if (t.dirs.size() >= 10) continue;
            op = {Kind::mkdir, join(dir, "d" + std::to_string(next_name++))};
        } else if (r < 32) {
            if (files.size() >= 30) continue;
            op = {Kind::create, join(dir, "f" + std::to_string(next_name++))};
        } else if (r < 36) {
            if (files.empty()) continue;
            op = {Kind::create, any_file()};  // O_CREAT | O_TRUNC on a file that exists
        } else if (r < 62) {
            if (files.empty() || !roomy) continue;
            op = {Kind::write, any_file()};
            op.offset = below(t.files[op.path].size() + 6000);
            op.len = static_cast<uint32_t>(below(2) ? 1 + below(vfs::BLOCK_SIZE)
                                                    : 1 + below(kMaxWrite));
        } else if (r < 76) {
            if (files.empty() || !roomy) continue;
            op = {Kind::append, any_file()};
            op.len = static_cast<uint32_t>(1 + below(20000));
        } else if (r < 81) {
            if (files.empty()) continue;
            op = {Kind::truncate, any_file()};
        } else if (r < 92) {
            if (files.empty()) continue;
            op = {Kind::unlink, any_file()};
        } else {
            std::vector<std::string> empty;
            for (const std::string& d : t.dirs)
                if (is_empty_dir(t, d)) empty.push_back(d);
            if (empty.empty()) continue;
            op = {Kind::rmdir, empty[below(empty.size())]};
        }
        push(op);
    }
    return ops;
}

// ---------------------------------------------------------- talking to vfs

// Runs ops[i]. 0, or the negative error of the call that failed.
long run_op(const std::vector<Op>& ops, size_t i) {
    const Op& op = ops[i];
    const char* path = op.path.c_str();

    switch (op.kind) {
        case Kind::create:
        case Kind::truncate: {
            int flags = VFS_O_WRONLY | VFS_O_TRUNC | (op.kind == Kind::create ? VFS_O_CREAT : 0);
            int fd = vfs_open(path, flags);
            if (fd < 0) return fd;
            return vfs_close(fd);
        }
        case Kind::write:
        case Kind::append: {
            int fd = vfs_open(path, VFS_O_WRONLY | (op.kind == Kind::append ? VFS_O_APPEND : 0));
            if (fd < 0) return fd;
            if (op.kind == Kind::write) {
                off_t at = vfs_lseek(fd, static_cast<off_t>(op.offset), VFS_SEEK_SET);
                if (at < 0) {
                    vfs_close(fd);
                    return at;
                }
            }
            std::string data = data_for(i, op.len);
            ssize_t n = vfs_write(fd, data.data(), data.size());
            int rc = vfs_close(fd);
            if (n < 0) return n;
            if (static_cast<size_t>(n) != data.size()) return -VFS_EIO;
            return rc;
        }
        case Kind::mkdir:  return vfs_mkdir(path);
        case Kind::rmdir:  return vfs_rmdir(path);
        case Kind::unlink: return vfs_unlink(path);
        case Kind::fsync: {
            int fd = vfs_open(path, VFS_O_RDONLY);
            if (fd < 0) return fd;
            int rc = vfs_fsync(fd);
            vfs_close(fd);
            return rc;
        }
        case Kind::sync: return vfs_sync();
    }
    return -VFS_EINVAL;
}

bool read_file(const std::string& path, std::string& out, std::string& err) {
    int fd = vfs_open(path.c_str(), VFS_O_RDONLY);
    if (fd < 0) {
        err = "open " + path + ": " + vfs_strerror(fd);
        return false;
    }
    out.clear();
    std::vector<char> buf(1 << 16);
    while (true) {
        ssize_t n = vfs_read(fd, buf.data(), buf.size());
        if (n < 0) {
            err = "read " + path + ": " + vfs_strerror(static_cast<int>(n));
            vfs_close(fd);
            return false;
        }
        if (n == 0) break;
        out.append(buf.data(), static_cast<size_t>(n));
    }
    vfs_close(fd);
    return true;
}

// Reads the whole mounted tree back through the API.
bool snapshot(Tree& out, std::string& err) {
    std::vector<std::string> stack{"/"};
    std::vector<VfsDirEnt> ents(128);

    while (!stack.empty()) {
        std::string dir = stack.back();
        stack.pop_back();

        int n = vfs_readdir(dir.c_str(), ents.data(), ents.size());
        if (n >= 0 && static_cast<size_t>(n) > ents.size()) {
            ents.resize(static_cast<size_t>(n));
            n = vfs_readdir(dir.c_str(), ents.data(), ents.size());
        }
        if (n < 0) {
            err = "readdir " + dir + ": " + vfs_strerror(n);
            return false;
        }

        for (int i = 0; i < n; i++) {
            std::string name = ents[static_cast<size_t>(i)].name;
            if (name == "." || name == "..") continue;
            std::string path = join(dir, name);
            if (ents[static_cast<size_t>(i)].type == vfs::FT_DIR) {
                out.dirs.insert(path);
                stack.push_back(path);
            } else if (!read_file(path, out.files[path], err)) {
                return false;
            }
        }
    }
    return true;
}

// The first difference between two trees, in words.
std::string diff(const Tree& want, const Tree& got) {
    for (const std::string& d : want.dirs)
        if (!got.dirs.count(d)) return "directory " + d + " is missing";
    for (const std::string& d : got.dirs)
        if (!want.dirs.count(d)) return "directory " + d + " should not exist";
    for (const auto& [path, data] : want.files) {
        auto it = got.files.find(path);
        if (it == got.files.end()) return "file " + path + " is missing";
        if (it->second.size() != data.size())
            return "file " + path + " is " + std::to_string(it->second.size())
                 + " bytes, should be " + std::to_string(data.size());
        if (it->second != data) {
            size_t at = 0;
            while (it->second[at] == data[at]) at++;
            return "file " + path + " differs at byte " + std::to_string(at);
        }
    }
    for (const auto& [path, data] : got.files)
        if (!want.files.count(path)) return "file " + path + " should not exist";
    return "no difference";
}

// --------------------------------------------------------------- processes

// What the child tells the parent through a pipe, one event per call.
struct Event {
    uint32_t kind;
    uint32_t calls;  // calls that changed something, completed so far
    int32_t  error;
    uint32_t op;
};

enum : uint32_t { EV_CALL = 1, EV_SYNCED = 2, EV_ERROR = 3 };

void send(int fd, Event e) {
    const char* p = reinterpret_cast<const char*>(&e);
    size_t done = 0;
    while (done < sizeof e) {
        ssize_t n = ::write(fd, p + done, sizeof e - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) _exit(4);
        done += static_cast<size_t>(n);
    }
}

bool receive(int fd, Event& e) {
    char* p = reinterpret_cast<char*>(&e);
    size_t done = 0;
    while (done < sizeof e) {
        ssize_t n = ::read(fd, p + done, sizeof e - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        done += static_cast<size_t>(n);
    }
    return true;
}

// Runs the workload, armed to die at write n. Never returns.
[[noreturn]] void child(int fd, const Paths& p, const std::vector<Op>& ops,
                        uint64_t n, uint64_t seed) {
    VfsOptions o = options();
    o.journal = !journal_off;
    int rc = vfs_mount(p.image.c_str(), p.log.c_str(), &o);
    if (rc != 0) {
        send(fd, {EV_ERROR, 0, rc, UINT32_MAX});
        _exit(3);
    }

    vfs::fault::arm_crash(n, seed);
    uint32_t calls = 0;
    for (size_t i = 0; i < ops.size(); i++) {
        long r = run_op(ops, i);
        if (r < 0) {
            send(fd, {EV_ERROR, calls, static_cast<int32_t>(r), static_cast<uint32_t>(i)});
            _exit(3);
        }
        if (mutates(ops[i].kind))
            send(fd, {EV_CALL, ++calls, 0, static_cast<uint32_t>(i)});
        else
            send(fd, {EV_SYNCED, calls, 0, static_cast<uint32_t>(i)});
    }

    // Unmounting makes everything durable, like a final fsync.
    rc = vfs_unmount();
    send(fd, {rc == 0 ? EV_SYNCED : EV_ERROR, calls, rc, UINT32_MAX});
    _exit(rc == 0 ? 0 : 3);
}

std::string describe_status(int status) {
    if (WIFSIGNALED(status))
        return std::string("killed by signal ") + std::to_string(WTERMSIG(status))
             + " (" + strsignal(WTERMSIG(status)) + ")";
    if (WIFEXITED(status))
        return "exited with status " + std::to_string(WEXITSTATUS(status));
    return "status " + std::to_string(status);
}

bool died_as_planned(int status) {
    return (WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL) ||
           (WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

void copy_image(const std::string& from_path, const std::string& to_path) {
    std::remove(to_path.c_str());
    vfs::PreadDevice from = vfs::PreadDevice::open(from_path.c_str());
    vfs::PreadDevice to = vfs::PreadDevice::create(to_path.c_str(), from.block_count());
    vfs::Block b;
    for (uint32_t i = 0; i < from.block_count(); i++) {
        from.read_block(i, b);
        to.write_block(i, b);
    }
}

// ------------------------------------------------------------------- checks

struct Workload {
    std::vector<Op> ops;
    std::vector<size_t> call_op;  // call_op[k - 1]: the op that is call k
};

// Mounts (replaying the log), reads the tree back, unmounts, runs fsck, and
// checks the tree is the model after k calls for some k in [lo, hi].
std::string verify(const Paths& p, const Workload& w, size_t lo, size_t hi) {
    VfsOptions o = options();
    int rc = vfs_mount(p.image.c_str(), p.log.c_str(), &o);
    if (rc != 0)
        return std::string("mounting after the crash failed: ") + vfs_strerror(rc);

    Tree got;
    std::string err;
    bool read_ok = snapshot(got, err);
    rc = vfs_unmount();
    if (!read_ok)
        return "reading the tree back failed: " + err;
    if (rc != 0)
        return std::string("unmounting after recovery failed: ") + vfs_strerror(rc);

    vfs::PreadDevice dev = vfs::PreadDevice::open(p.image.c_str());
    vfs::FsckReport report = vfs::fsck(dev);
    if (!report.ok()) {
        std::string all = "fsck found " + std::to_string(report.problems.size()) + " problems:";
        for (size_t i = 0; i < report.problems.size() && i < 5; i++)
            all += "\n      " + report.problems[i];
        return all;
    }

    Tree model;
    size_t k = 0;
    for (; k < lo; k++)
        apply(model, w.ops[w.call_op[k]], w.call_op[k]);
    const Tree at_lo = model;
    for (; k <= hi; k++) {
        if (model == got)
            return "";
        if (k < hi)
            apply(model, w.ops[w.call_op[k]], w.call_op[k]);
    }

    return "the tree matches no state between call " + std::to_string(lo) + " and call "
         + std::to_string(hi) + "; against call " + std::to_string(lo) + ": "
         + diff(at_lo, got);
}

// One crash: run the workload to write n, kill it, maybe kill the recovery
// too, then verify. Empty string if all is well.
std::string crash_once(const Paths& p, const Workload& w, uint64_t n, uint64_t seed,
                       bool crash_recovery, uint32_t& calls_done) {
    copy_image(p.base, p.image);
    std::remove(p.log.c_str());

    int fds[2];
    if (pipe(fds) != 0)
        return std::string("pipe: ") + std::strerror(errno);

    std::fflush(stdout);
    std::fflush(stderr);
    pid_t pid = fork();
    if (pid < 0)
        return std::string("fork: ") + std::strerror(errno);
    if (pid == 0) {
        close(fds[0]);
        child(fds[1], p, w.ops, n, seed);
    }
    close(fds[1]);

    uint32_t completed = 0, synced = 0;
    Event e{}, error{};
    bool failed_call = false;
    while (receive(fds[0], e)) {
        if (e.kind == EV_CALL) completed = e.calls;
        if (e.kind == EV_SYNCED) completed = synced = e.calls;
        if (e.kind == EV_ERROR) { failed_call = true; error = e; }
    }
    close(fds[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    calls_done = completed;

    if (failed_call)
        return "the workload's op " + std::to_string(error.op) + " failed before the crash: "
             + vfs_strerror(error.error);
    if (!died_as_planned(status))
        return "the workload " + describe_status(status);

    if (crash_recovery) {
        std::fflush(stdout);
        pid_t rpid = fork();
        if (rpid < 0)
            return std::string("fork: ") + std::strerror(errno);
        if (rpid == 0) {
            vfs::fault::arm_crash(1 + seed % 40, seed / 3 + 1);
            VfsOptions o = options();
            if (vfs_mount(p.image.c_str(), p.log.c_str(), &o) != 0) _exit(3);
            if (vfs_unmount() != 0) _exit(3);
            _exit(0);
        }
        waitpid(rpid, &status, 0);
        if (!died_as_planned(status))
            return "the recovery that was meant to crash " + describe_status(status);
    }

    size_t hi = std::min<size_t>(completed + 1, w.call_op.size());
    return verify(p, w, synced, hi);
}

} // namespace

int main(int argc, char** argv) {
    size_t points = 500;
    uint64_t seed = 1;

    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--points") == 0 && i + 1 < argc) {
            points = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            seed = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--no-journal") == 0) {
            journal_off = true;
        } else {
            std::fprintf(stderr, "usage: crashtest [--points N] [--seed S] [--no-journal]\n");
            return 2;
        }
    }
    if (points == 0) points = 1;

    const std::string stem = "/tmp/vfs_crashtest_" + std::to_string(getpid());
    Paths p{stem + ".img", stem + ".img.log", stem + "_base.img"};
    auto cleanup = [&] {
        std::remove(p.image.c_str());
        std::remove(p.log.c_str());
        std::remove(p.base.c_str());
    };

    try {
        {
            std::remove(p.base.c_str());
            vfs::PreadDevice dev = vfs::PreadDevice::create(p.base.c_str(), kImageBlocks);
            vfs::mkfs(dev);
        }

        Workload w;
        w.ops = make_workload(seed);
        for (size_t i = 0; i < w.ops.size(); i++)
            if (mutates(w.ops[i].kind)) w.call_op.push_back(i);

        // One run to the end, uncrashed: every call must succeed, the result
        // must be the model's final state, and it says how many writes the
        // workload makes - the range the crash points are spread over.
        copy_image(p.base, p.image);
        std::remove(p.log.c_str());
        VfsOptions o = options();
        o.journal = !journal_off;
        int rc = vfs_mount(p.image.c_str(), p.log.c_str(), &o);
        if (rc != 0) {
            std::printf("crashtest: mount failed: %s\n", vfs_strerror(rc));
            cleanup();
            return 1;
        }
        vfs::fault::arm_crash(0, 0);
        for (size_t i = 0; i < w.ops.size(); i++) {
            long r = run_op(w.ops, i);
            if (r < 0) {
                std::printf("crashtest: op %zu of the uncrashed run failed: %s\n",
                            i, vfs_strerror(static_cast<int>(r)));
                vfs_unmount();
                cleanup();
                return 1;
            }
        }
        rc = vfs_unmount();
        uint64_t writes = vfs::fault::writes_counted();
        std::string clean = rc != 0 ? std::string("unmount failed: ") + vfs_strerror(rc)
                                    : verify(p, w, w.call_op.size(), w.call_op.size());

        std::printf("workload: %zu calls that change the tree, %zu calls in all, "
                    "%llu writes to the image and the log\n",
                    w.call_op.size(), w.ops.size(), static_cast<unsigned long long>(writes));
        std::printf("uncrashed run\n  %s  every call succeeds, and the tree comes out as the model says\n",
                    clean.empty() ? "ok  " : "FAIL");
        if (!clean.empty()) {
            std::printf("      %s\n", clean.c_str());
            cleanup();
            return 1;
        }
        if (writes == 0) {
            std::printf("crashtest: no writes were counted - is vfs::fault wired in?\n");
            cleanup();
            return 1;
        }

        // Spread the points evenly over [1, writes]. With fewer writes than
        // points, every write is a crash point, several times over, each
        // time torn at a different sector.
        std::vector<uint64_t> at;
        for (size_t i = 0; i < points; i++) {
            if (writes >= points)
                at.push_back(1 + (points == 1 ? 0 : i * (writes - 1) / (points - 1)));
            else
                at.push_back(1 + i % writes);
        }

        std::printf("crashing at %zu points\n", at.size());
        auto start = std::chrono::steady_clock::now();
        size_t failures = 0, recovery_crashes = 0;
        std::set<uint32_t> landed;  // distinct numbers of calls done when killed

        for (size_t i = 0; i < at.size(); i++) {
            bool crash_recovery = i % 5 == 4;
            recovery_crashes += crash_recovery;
            uint32_t calls_done = 0;
            uint64_t point_seed = seed * 1000003 + i;
            std::string why = crash_once(p, w, at[i], point_seed, crash_recovery, calls_done);
            landed.insert(calls_done);

            if (!why.empty()) {
                failures++;
                if (failures <= 3)
                    std::printf("  FAIL  crash at write %llu (point %zu, seed %llu, %u calls done%s)\n"
                                "      %s\n",
                                static_cast<unsigned long long>(at[i]), i,
                                static_cast<unsigned long long>(point_seed), calls_done,
                                crash_recovery ? ", recovery crashed too" : "", why.c_str());
            }
            if ((i + 1) % 50 == 0 || i + 1 == at.size())
                std::printf("  %4zu/%zu points  %s  (last: crash at write %llu, %u calls done)\n",
                            i + 1, at.size(), failures == 0 ? "ok  " : "FAIL",
                            static_cast<unsigned long long>(at[i]), calls_done);
        }

        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        cleanup();

        std::printf("\n%zu crashes (%zu of them also mid-recovery), landing after %zu different "
                    "numbers of completed calls, in %.1f s: %zu bad\n",
                    at.size(), recovery_crashes, landed.size(), secs, failures);
        std::printf("%s\n", failures == 0 ? "stage 8: all checks passed" : "stage 8: FAILURES");
        return failures == 0 ? 0 : 1;

    } catch (const std::exception& e) {
        std::fprintf(stderr, "crashtest: unexpected exception: %s\n", e.what());
        cleanup();
        return 1;
    }
}
