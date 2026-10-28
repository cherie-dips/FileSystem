//
// vfstest — proves stage 5 works: the public API.
//
// A 10 MB file written, closed, reopened and read back byte for byte; every
// open flag, seek and bad descriptor; and a reference model - a std::map of
// paths to contents - put through thousands of random operations alongside
// the real thing (fixed seed, so a failure replays), compared after each one
// and in full after every remount.
//
// Run it under `make asan` and `make tsan` as well as `make`.
//

#include "vfs/block_device.h"
#include "vfs/fsck.h"
#include "vfs/layout.h"
#include "vfs/mkfs.h"
#include "vfs/vfs.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

std::string g_img;
std::string g_log;

void make_image(uint32_t blocks) {
    std::remove(g_img.c_str());
    std::remove(g_log.c_str());
    vfs::PreadDevice dev = vfs::PreadDevice::create(g_img.c_str(), blocks);
    vfs::mkfs(dev);
}

bool mount() {
    VfsOptions o;
    o.fsync = false;
    return vfs_mount(g_img.c_str(), g_log.c_str(), &o) == 0;
}

void check_fsck(const char* what) {
    vfs::PreadDevice dev = vfs::PreadDevice::open(g_img.c_str());
    vfs::FsckReport r = vfs::fsck(dev);
    if (!r.problems.empty() && r.problems[0].find("not implemented") != std::string::npos) {
        std::printf("  skip  %s (fsck is not built yet)\n", what);
        return;
    }
    for (const std::string& p : r.problems)
        std::printf("        fsck: %s\n", p.c_str());
    check(r.ok(), what);
}

// Reads a whole file through the API, or reports the error.
int read_all(const std::string& path, std::string* out) {
    int fd = vfs_open(path.c_str(), VFS_O_RDONLY);
    if (fd < 0)
        return fd;
    out->clear();
    char buf[8192];
    while (true) {
        ssize_t n = vfs_read(fd, buf, sizeof buf);
        if (n < 0) {
            vfs_close(fd);
            return static_cast<int>(n);
        }
        if (n == 0)
            break;
        out->append(buf, static_cast<size_t>(n));
    }
    return vfs_close(fd);
}

// ------------------------------------------------------------ a big file

void check_big_file() {
    std::printf("a 10 MB file\n");
    const size_t size = 10 * 1024 * 1024;
    std::vector<uint8_t> data(size);
    std::mt19937 rng(1234);
    for (uint8_t& b : data)
        b = static_cast<uint8_t>(rng());

    int fd = vfs_open("/ten", VFS_O_WRONLY | VFS_O_CREAT | VFS_O_EXCL);
    bool wrote = fd >= 0;
    for (size_t at = 0; wrote && at < size; at += 1024 * 1024)
        wrote = vfs_write(fd, data.data() + at, 1024 * 1024) == 1024 * 1024;
    check(wrote && vfs_close(fd) == 0, "written in 1 MB pieces, and closed");

    auto read_back = [&]() {
        std::vector<uint8_t> got(size + 100, 0);
        int rfd = vfs_open("/ten", VFS_O_RDONLY);
        size_t have = 0;
        while (rfd >= 0) {
            ssize_t n = vfs_read(rfd, got.data() + have, got.size() - have);
            if (n <= 0)
                break;
            have += static_cast<size_t>(n);
        }
        vfs_close(rfd);
        return have == size && std::memcmp(got.data(), data.data(), size) == 0;
    };
    check(read_back(), "reopened and read back byte for byte");

    VfsStat st{};
    vfs_stat("/ten", &st);
    // 2560 data blocks, a single-indirect block, and the double-indirect
    // block with two second-level blocks under it.
    check(st.size == size && st.blocks == 2560 + 1 + 1 + 2, "size 10 MB, 2564 blocks");

    vfs_unmount();
    check(mount() && read_back(), "and again after unmounting and mounting");
}

// ----------------------------------------------------------------- flags

void check_flags() {
    std::printf("open flags\n");
    check(vfs_open("/nope", VFS_O_RDONLY) == -VFS_ENOENT, "a missing file without O_CREAT: VFS_ENOENT");
    check(vfs_open("/nodir/f", VFS_O_WRONLY | VFS_O_CREAT) == -VFS_ENOENT,
          "O_CREAT in a missing directory: VFS_ENOENT");
    check(vfs_open("/ten", VFS_O_WRONLY | VFS_O_CREAT | VFS_O_EXCL) == -VFS_EEXIST,
          "O_CREAT | O_EXCL on a file that exists: VFS_EEXIST");
    check(vfs_open("/f", VFS_O_ACCMODE) == -VFS_EINVAL, "access mode 3: VFS_EINVAL");
    check(vfs_open("/ten", VFS_O_RDONLY | VFS_O_TRUNC) == -VFS_EINVAL,
          "O_TRUNC without write access: VFS_EINVAL");

    int fd = vfs_open("/f", VFS_O_RDWR | VFS_O_CREAT | VFS_O_EXCL);
    check(fd >= 0 && vfs_write(fd, "hello world", 11) == 11, "a new file, written");
    vfs_close(fd);

    fd = vfs_open("/f", VFS_O_WRONLY | VFS_O_APPEND);
    vfs_lseek(fd, 0, VFS_SEEK_SET);
    check(vfs_write(fd, "!", 1) == 1 && vfs_lseek(fd, 0, VFS_SEEK_CUR) == 12,
          "O_APPEND writes at the end even after a seek to 0, and leaves the offset there");
    vfs_close(fd);
    std::string s;
    read_all("/f", &s);
    check(s == "hello world!", "the file reads \"hello world!\"");

    fd = vfs_open("/f", VFS_O_RDONLY);
    check(vfs_write(fd, "x", 1) == -VFS_EBADF, "write to a read-only descriptor: VFS_EBADF");
    vfs_close(fd);
    fd = vfs_open("/f", VFS_O_WRONLY);
    char c;
    check(vfs_read(fd, &c, 1) == -VFS_EBADF, "read from a write-only descriptor: VFS_EBADF");
    vfs_close(fd);

    fd = vfs_open("/f", VFS_O_WRONLY | VFS_O_TRUNC);
    VfsStat st{};
    vfs_stat("/f", &st);
    check(fd >= 0 && st.size == 0 && st.blocks == 0, "O_TRUNC empties it");
    vfs_close(fd);
}

void check_seeks() {
    std::printf("seeking\n");
    int fd = vfs_open("/s", VFS_O_RDWR | VFS_O_CREAT);
    vfs_write(fd, "0123456789", 10);
    check(vfs_lseek(fd, 3, VFS_SEEK_SET) == 3, "SEEK_SET");
    check(vfs_lseek(fd, 2, VFS_SEEK_CUR) == 5, "SEEK_CUR");
    check(vfs_lseek(fd, -1, VFS_SEEK_END) == 9, "SEEK_END");
    char c = 0;
    check(vfs_read(fd, &c, 1) == 1 && c == '9', "reads from where it was put");
    check(vfs_read(fd, &c, 1) == 0, "at the end, a read returns 0");
    check(vfs_lseek(fd, -11, VFS_SEEK_END) == -VFS_EINVAL, "before the start: VFS_EINVAL");
    check(vfs_lseek(fd, 0, 7) == -VFS_EINVAL, "an unknown whence: VFS_EINVAL");

    check(vfs_lseek(fd, 100000, VFS_SEEK_SET) == 100000 && vfs_write(fd, "X", 1) == 1,
          "past the end, and a write there");
    std::string s;
    read_all("/s", &s);
    bool gap = s.size() == 100001 && s.compare(0, 10, "0123456789") == 0 && s[100000] == 'X';
    for (size_t i = 10; gap && i < 100000; i++)
        gap = s[i] == '\0';
    check(gap, "the gap reads as zeros");
    vfs_close(fd);
}

void check_descriptors() {
    std::printf("descriptors\n");
    char c;
    check(vfs_read(-1, &c, 1) == -VFS_EBADF && vfs_write(1024, &c, 1) == -VFS_EBADF,
          "out of range: VFS_EBADF");
    check(vfs_close(77) == -VFS_EBADF && vfs_lseek(77, 0, VFS_SEEK_SET) == -VFS_EBADF
          && vfs_fsync(77) == -VFS_EBADF, "never opened: VFS_EBADF");

    int fd = vfs_open("/s", VFS_O_RDONLY);
    vfs_close(fd);
    check(vfs_read(fd, &c, 1) == -VFS_EBADF && vfs_close(fd) == -VFS_EBADF,
          "closed: VFS_EBADF, and closing twice too");

    std::vector<int> fds;
    for (int i = 0; i < 1024; i++) {
        int f = vfs_open("/s", VFS_O_RDONLY);
        if (f < 0)
            break;
        fds.push_back(f);
    }
    check(fds.size() == 1024, "1024 files open at once");
    check(vfs_open("/s", VFS_O_RDONLY) == -VFS_EMFILE, "the 1025th: VFS_EMFILE");
    VfsStat st;
    check(vfs_open("/new", VFS_O_WRONLY | VFS_O_CREAT) == -VFS_EMFILE
          && vfs_stat("/new", &st) == -VFS_ENOENT, "and a full table creates nothing");
    for (int f : fds)
        vfs_close(f);
    fd = vfs_open("/s", VFS_O_RDONLY);
    check(fd >= 0, "after closing them, opening works again");
    check(vfs_fsync(fd) == 0, "fsync on an open file");
    vfs_close(fd);
    check(vfs_sync() == 0, "vfs_sync");
}

void check_not_mounted() {
    std::printf("mounting\n");
    check(vfs_mount(g_img.c_str(), g_log.c_str(), nullptr) == -VFS_EBUSY,
          "mounting twice: VFS_EBUSY");
    vfs_unmount();
    check(vfs_unmount() == -VFS_EINVAL, "unmounting twice: VFS_EINVAL");
    check(vfs_open("/s", VFS_O_RDONLY) == -VFS_EINVAL && vfs_mkdir("/x") == -VFS_EINVAL,
          "calls with nothing mounted: VFS_EINVAL");
    check(vfs_mount("/tmp/vfs_no_such_image.img", "/tmp/vfs_no_such_image.log", nullptr) == -VFS_ENOENT,
          "mounting an image that does not exist: VFS_ENOENT");

    std::string junk = g_img + ".junk";
    std::remove(junk.c_str());
    { vfs::PreadDevice::create(junk.c_str(), 64); }
    check(vfs_mount(junk.c_str(), (junk + ".log").c_str(), nullptr) == -VFS_EINVAL,
          "mounting a file that was never formatted: VFS_EINVAL");
    std::remove(junk.c_str());
    std::remove((junk + ".log").c_str());

    VfsOptions tiny;
    tiny.cache_frames = 10;
    check(vfs_mount(g_img.c_str(), g_log.c_str(), &tiny) == -VFS_EINVAL,
          "a cache under 256 frames: VFS_EINVAL");
    check(mount(), "and a normal mount still works");
}

// ----------------------------------------------------------------- model

// What the file system should hold: every directory, and every file with its
// contents. Paths are normalised: "/", "/a", "/a/b".
struct Model {
    std::set<std::string> dirs{"/"};
    std::map<std::string, std::string> files;

    bool is_dir(const std::string& p) const { return dirs.count(p) != 0; }
    bool is_file(const std::string& p) const { return files.count(p) != 0; }
    bool exists(const std::string& p) const { return is_dir(p) || is_file(p); }

    static std::string parent(const std::string& p) {
        size_t slash = p.rfind('/');
        return slash == 0 ? "/" : p.substr(0, slash);
    }

    // The error a lookup of p's parent would give, or 0: a missing
    // component is VFS_ENOENT, a file where a directory should be VFS_ENOTDIR,
    // whichever comes first walking down.
    int parent_error(const std::string& p) const {
        std::string at;
        size_t i = 1;
        while (true) {
            size_t slash = p.find('/', i);
            if (slash == std::string::npos)
                return 0; // reached the last component
            at = p.substr(0, slash);
            if (is_file(at))
                return -VFS_ENOTDIR;
            if (!is_dir(at))
                return -VFS_ENOENT;
            i = slash + 1;
        }
    }

    std::set<std::string> children(const std::string& dir) const {
        std::set<std::string> out;
        auto add = [&](const std::string& p) {
            if (p != "/" && parent(p) == dir)
                out.insert(p.substr(p.rfind('/') + 1));
        };
        for (const std::string& d : dirs) add(d);
        for (const auto& f : files) add(f.first);
        return out;
    }
};

class Fuzzer {
public:
    explicit Fuzzer(uint32_t seed) : rng_(seed) {}

    void run(int ops, int remount_every) {
        for (int i = 1; i <= ops && mismatches_ < 10; i++) {
            step(i);
            if (i % remount_every == 0) {
                if (vfs_unmount() != 0 || !mount())
                    fail(i, "remount failed");
                compare_all(i);
            }
        }
    }

    int mismatches() const { return mismatches_; }
    int ops_done() const { return done_; }

private:
    std::mt19937 rng_;
    Model m_;
    int mismatches_ = 0;
    int done_ = 0;

    uint32_t pick(uint32_t n) { return static_cast<uint32_t>(rng_() % n); }

    void fail(int op, const std::string& what) {
        if (mismatches_ < 10)
            std::printf("        op %d: %s\n", op, what.c_str());
        mismatches_++;
    }

    void expect(int op, const std::string& what, long long got, long long want) {
        if (got != want)
            fail(op, what + ": got " + std::to_string(got) + ", want " + std::to_string(want));
    }

    // Somewhere to act: usually under a directory that exists, sometimes
    // under a file or a missing directory, to exercise the errors.
    std::string random_path() {
        static const char* names[] = {"a", "b", "c", "d", "e", "f"};
        std::string base;
        uint32_t r = pick(100);
        if (r < 75 || m_.files.empty()) {
            auto it = m_.dirs.begin();
            std::advance(it, pick(static_cast<uint32_t>(m_.dirs.size())));
            base = *it;
        } else if (r < 88) {
            auto it = m_.files.begin();
            std::advance(it, pick(static_cast<uint32_t>(m_.files.size())));
            base = it->first;
        } else {
            base = "/missing";
        }
        if (std::count(base.begin(), base.end(), '/') >= 4)
            base = "/";
        return (base == "/" ? "" : base) + "/" + names[pick(6)];
    }

    std::string random_bytes(size_t n) {
        std::string s(n, '\0');
        for (char& c : s)
            c = static_cast<char>('a' + pick(26));
        return s;
    }

    void step(int op) {
        std::string p = random_path();
        int pe = m_.parent_error(p);
        done_++;

        switch (pick(9)) {
            case 0: { // mkdir
                int want = pe ? pe : m_.exists(p) ? -VFS_EEXIST : 0;
                expect(op, "mkdir " + p, vfs_mkdir(p.c_str()), want);
                if (want == 0) m_.dirs.insert(p);
                break;
            }
            case 1: case 2: { // write at an offset, creating the file if needed
                int want = pe ? pe : m_.is_dir(p) ? -VFS_EISDIR : 0;
                int fd = vfs_open(p.c_str(), VFS_O_RDWR | VFS_O_CREAT);
                expect(op, "open+creat " + p, fd < 0 ? fd : 0, want);
                if (fd < 0) break;
                std::string& content = m_.files[p];
                size_t off = pick(static_cast<uint32_t>(content.size() + 9000));
                size_t len = pick(8) == 0 ? pick(150000) : pick(5000);
                std::string data = random_bytes(len);
                vfs_lseek(fd, static_cast<off_t>(off), VFS_SEEK_SET);
                expect(op, "write " + p, vfs_write(fd, data.data(), len), static_cast<long long>(len));
                if (off + len > content.size())
                    content.resize(off + len, '\0');
                content.replace(off, len, data);
                vfs_close(fd);
                break;
            }
            case 3: { // append, or truncate and write
                int want = pe ? pe : m_.is_dir(p) ? -VFS_EISDIR : 0;
                bool trunc = pick(2) == 0;
                int flags = VFS_O_WRONLY | VFS_O_CREAT | (trunc ? VFS_O_TRUNC : VFS_O_APPEND);
                int fd = vfs_open(p.c_str(), flags);
                expect(op, std::string(trunc ? "trunc " : "append ") + p, fd < 0 ? fd : 0, want);
                if (fd < 0) break;
                std::string& content = m_.files[p];
                if (trunc) content.clear();
                std::string data = random_bytes(pick(3000));
                expect(op, "write " + p, vfs_write(fd, data.data(), data.size()),
                       static_cast<long long>(data.size()));
                content += data;
                vfs_close(fd);
                break;
            }
            case 4: { // read it all back
                std::string got;
                int want = pe ? pe : m_.is_dir(p) ? -VFS_EISDIR : !m_.is_file(p) ? -VFS_ENOENT : 0;
                int rc = read_all(p, &got);
                expect(op, "read " + p, rc, want);
                if (rc == 0 && want == 0 && got != m_.files[p])
                    fail(op, "contents of " + p + " differ");
                break;
            }
            case 5: { // unlink
                int want = pe ? pe : !m_.exists(p) ? -VFS_ENOENT : m_.is_dir(p) ? -VFS_EISDIR : 0;
                expect(op, "unlink " + p, vfs_unlink(p.c_str()), want);
                if (want == 0) m_.files.erase(p);
                break;
            }
            case 6: { // rmdir
                int want = pe ? pe : !m_.exists(p) ? -VFS_ENOENT : m_.is_file(p) ? -VFS_ENOTDIR
                         : !m_.children(p).empty() ? -VFS_ENOTEMPTY : 0;
                expect(op, "rmdir " + p, vfs_rmdir(p.c_str()), want);
                if (want == 0) m_.dirs.erase(p);
                break;
            }
            case 7: { // stat
                VfsStat st{};
                int want = pe ? pe : !m_.exists(p) ? -VFS_ENOENT : 0;
                expect(op, "stat " + p, vfs_stat(p.c_str(), &st), want);
                if (want == 0 && m_.is_file(p) && st.size != m_.files[p].size())
                    fail(op, "size of " + p + " differs");
                if (want == 0 && m_.is_dir(p) != ((st.mode & vfs::MODE_TYPE_MASK) == vfs::MODE_DIR))
                    fail(op, "type of " + p + " differs");
                break;
            }
            default: { // list a directory
                std::string dir = Model::parent(p);
                if (!m_.is_dir(dir)) break;
                if (!same_listing(dir))
                    fail(op, "listing of " + dir + " differs");
                break;
            }
        }
    }

    bool same_listing(const std::string& dir) {
        int n = vfs_readdir(dir.c_str(), nullptr, 0);
        if (n < 2)
            return false;
        std::vector<VfsDirEnt> ents(static_cast<size_t>(n));
        vfs_readdir(dir.c_str(), ents.data(), ents.size());
        std::set<std::string> got;
        for (const VfsDirEnt& e : ents) {
            std::string name = e.name;
            if (name == "." || name == "..")
                continue;
            std::string full = (dir == "/" ? "" : dir) + "/" + name;
            if ((e.type == vfs::FT_DIR) != m_.is_dir(full))
                return false;
            got.insert(name);
        }
        return got == m_.children(dir);
    }

    void compare_all(int op) {
        for (const std::string& d : m_.dirs)
            if (!same_listing(d))
                fail(op, "after remount, listing of " + d + " differs");
        for (const auto& f : m_.files) {
            std::string got;
            if (read_all(f.first, &got) != 0 || got != f.second)
                fail(op, "after remount, contents of " + f.first + " differ");
        }
    }
};

void check_model() {
    std::printf("against a reference model\n");
    vfs_unmount();
    make_image(16384); // 64 MB
    if (!mount()) { check(false, "mount"); return; }

    Fuzzer fz(20261029);
    fz.run(4000, 400);
    std::printf("        %d random operations, remounted every 400\n", fz.ops_done());
    check(fz.mismatches() == 0, "every result, listing and file matches the model");
    vfs_unmount();
    check_fsck("fsck is happy afterwards");
}

} // namespace

int main() {
    g_img = "/tmp/vfs_vfstest_" + std::to_string(getpid()) + ".img";
    g_log = g_img + ".log";

    try {
        make_image(8192); // 32 MB
        if (!mount())
            throw std::runtime_error("mount failed");
        check_big_file();
        check_flags();
        check_seeks();
        check_descriptors();
        check_not_mounted();
        check_model();
    } catch (const std::exception& e) {
        std::printf("  FAIL  unexpected exception: %s\n", e.what());
        failures++;
    }
    vfs_unmount();
    std::remove(g_img.c_str());
    std::remove(g_log.c_str());

    std::printf("\n%s\n", failures == 0 ? "stage 5: all checks passed" : "stage 5: FAILURES");
    return failures == 0 ? 0 : 1;
}
