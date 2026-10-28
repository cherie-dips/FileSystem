//
// dirtest — proves stage 4 works: directories and path lookup.
//
// Builds a small tree, checks every error a path can produce, grows a
// directory past one block, and redoes OSTEP's Figure 40.3: how many disk
// reads it takes to find /foo/bar.
//
// Run it under `make asan` as well as `make`.
//

#include "vfs/block_device.h"
#include "vfs/fsck.h"
#include "vfs/layout.h"
#include "vfs/mkfs.h"
#include "vfs/vfs.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
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

std::vector<std::string> list(const char* path) {
    std::vector<VfsDirEnt> ents(256);
    int n = vfs_readdir(path, ents.data(), ents.size());
    std::vector<std::string> names;
    for (int i = 0; i < n && i < static_cast<int>(ents.size()); i++)
        names.push_back(ents[i].name);
    return names;
}

uint16_t links(const char* path) {
    VfsStat st{};
    return vfs_stat(path, &st) == 0 ? st.link_count : 0;
}

uint32_t inode_of(const char* path) {
    VfsStat st{};
    return vfs_stat(path, &st) == 0 ? st.inode : 0;
}

bool create(const char* path) {
    int fd = vfs_open(path, VFS_O_WRONLY | VFS_O_CREAT);
    return fd >= 0 && vfs_close(fd) == 0;
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

// ------------------------------------------------------------------- tests

void check_tree() {
    std::printf("making and listing\n");
    check(vfs_mkdir("/a") == 0 && vfs_mkdir("/a/b") == 0 && vfs_mkdir("/a/b/c") == 0,
          "mkdir /a, /a/b, /a/b/c");

    std::vector<std::string> names = list("/a/b");
    check(names == std::vector<std::string>({".", "..", "c"}),
          "readdir /a/b gives \".\", \"..\", \"c\" in that order");

    VfsDirEnt one[1];
    check(vfs_readdir("/a/b", one, 1) == 3 && std::strcmp(one[0].name, ".") == 0,
          "a buffer of one still reports all three, and fills in the first");

    VfsDirEnt ents[4];
    vfs_readdir("/a/b", ents, 4);
    check(ents[0].inode == inode_of("/a/b") && ents[1].inode == inode_of("/a")
          && ents[2].inode == inode_of("/a/b/c") && ents[2].type == vfs::FT_DIR,
          "\".\" is /a/b itself, \"..\" is /a, and c is a directory");

    std::vector<std::string> root = list("/");
    check(root.size() == 3 && root[0] == "." && root[1] == ".." && root[2] == "a",
          "the root lists \".\", \"..\" and a");
}

void check_lookups() {
    std::printf("lookups\n");
    uint32_t c = inode_of("/a/b/c");
    check(c != 0, "/a/b/c resolves");
    check(inode_of("/a/./b/../b/c") == c, "/a/./b/../b/c is the same directory");
    check(inode_of("//a///b/c/") == c, "repeated and trailing slashes are ignored");
    check(inode_of("/..") == vfs::ROOT_INODE && inode_of("/a/../..") == vfs::ROOT_INODE,
          "\"..\" at the root stays at the root");
}

void check_links() {
    std::printf("link counts\n");
    check(links("/") == 3, "the root: its \".\", its \"..\", and a's \"..\"");
    check(links("/a") == 3 && links("/a/b") == 3, "/a and /a/b: 2 + one subdirectory each");
    check(links("/a/b/c") == 2, "/a/b/c: its \".\" and its entry in /a/b");

    check(create("/a/file"), "a file in /a");
    check(links("/a/file") == 1 && links("/a") == 3, "a file has one link, and adds none to its directory");

    check(vfs_rmdir("/a/b/c") == 0 && links("/a/b") == 2, "rmdir /a/b/c takes /a/b back to 2");
    check(vfs_mkdir("/a/b/c") == 0 && links("/a/b") == 3, "and mkdir puts it back to 3");
}

void check_errors() {
    std::printf("errors\n");
    check(vfs_mkdir("/x/y") == -VFS_ENOENT, "mkdir under a missing directory: VFS_ENOENT");
    check(vfs_mkdir("/a/file/g") == -VFS_ENOTDIR, "mkdir under a file: VFS_ENOTDIR");
    check(vfs_stat("/a/file/g", nullptr) == -VFS_EINVAL, "stat with nowhere to put the answer: VFS_EINVAL");
    VfsStat st;
    check(vfs_stat("/a/file/g", &st) == -VFS_ENOTDIR, "a lookup through a file: VFS_ENOTDIR");
    check(vfs_stat("/nope", &st) == -VFS_ENOENT, "stat of a missing name: VFS_ENOENT");
    check(vfs_mkdir("/a") == -VFS_EEXIST && vfs_mkdir("/a/file") == -VFS_EEXIST,
          "mkdir over a directory or a file: VFS_EEXIST");
    check(vfs_mkdir("/") == -VFS_EEXIST, "mkdir /: VFS_EEXIST");
    check(vfs_rmdir("/a") == -VFS_ENOTEMPTY, "rmdir of a directory with things in it: VFS_ENOTEMPTY");
    check(vfs_rmdir("/") == -VFS_EBUSY, "rmdir /: VFS_EBUSY");
    check(vfs_rmdir("/a/file") == -VFS_ENOTDIR, "rmdir of a file: VFS_ENOTDIR");
    check(vfs_rmdir("/nope") == -VFS_ENOENT && vfs_unlink("/nope") == -VFS_ENOENT,
          "rmdir and unlink of missing names: VFS_ENOENT");
    check(vfs_unlink("/a") == -VFS_EISDIR, "unlink of a directory: VFS_EISDIR");
    check(vfs_open("/a", VFS_O_RDONLY) == -VFS_EISDIR, "open of a directory: VFS_EISDIR");
    check(vfs_readdir("/a/file", nullptr, 0) == -VFS_ENOTDIR, "readdir of a file: VFS_ENOTDIR");
    check(vfs_mkdir("a") == -VFS_EINVAL && vfs_mkdir("") == -VFS_EINVAL,
          "a relative or empty path: VFS_EINVAL");

    std::string longest = "/" + std::string(vfs::MAX_NAME_LEN, 'n');
    std::string too_long = longest + "n";
    check(vfs_mkdir(longest.c_str()) == 0, "a 58-byte name is fine");
    check(vfs_mkdir(too_long.c_str()) == -VFS_ENAMETOOLONG, "a 59-byte one is VFS_ENAMETOOLONG");
    check(vfs_rmdir(longest.c_str()) == 0, "and the 58-byte one can be removed");

    int fd = vfs_open("/a/file", VFS_O_RDONLY);
    check(vfs_unlink("/a/file") == -VFS_EBUSY, "unlink of an open file: VFS_EBUSY");
    vfs_close(fd);
    check(vfs_unlink("/a/file") == 0, "and once it is closed, it goes");
}

void check_growth() {
    std::printf("a directory bigger than one block\n");
    check(vfs_mkdir("/big") == 0, "mkdir /big");

    // The first block has 64 slots, two of them "." and "..".
    bool made = true;
    for (int i = 0; i < 100; i++)
        made = made && create(("/big/f" + std::to_string(i)).c_str());
    check(made, "100 files created in it");

    VfsStat st{};
    vfs_stat("/big", &st);
    check(st.size == 2 * vfs::BLOCK_SIZE && st.blocks == 2, "it has grown to two blocks");
    check(vfs_readdir("/big", nullptr, 0) == 102, "and lists 102 entries");

    bool removed = true;
    for (int i = 0; i < 100; i += 2)
        removed = removed && vfs_unlink(("/big/f" + std::to_string(i)).c_str()) == 0;
    for (int i = 0; i < 20; i++)
        made = made && create(("/big/g" + std::to_string(i)).c_str());
    vfs_stat("/big", &st);
    check(removed && made && st.size == 2 * vfs::BLOCK_SIZE,
          "50 removed and 20 added: the free slots are reused, it does not grow");

    vfs_unmount();
    mount();
    std::vector<std::string> names = list("/big");
    std::set<std::string> want = {".", ".."};
    for (int i = 1; i < 100; i += 2)
        want.insert("f" + std::to_string(i));
    for (int i = 0; i < 20; i++)
        want.insert("g" + std::to_string(i));
    check(std::set<std::string>(names.begin(), names.end()) == want && names.size() == want.size(),
          "after a remount it lists exactly the 72 names it should");
}

void check_io_count() {
    std::printf("counting disk reads: OSTEP Figure 40.3\n");
    vfs_unmount();
    make_image(1024);
    mount();
    vfs_mkdir("/foo");
    create("/foo/bar");
    vfs_unmount();

    // A fresh mount has an empty cache.
    mount();
    vfs_reset_stats();
    VfsStat st{};
    int rc = vfs_stat("/foo/bar", &st);
    VfsStats cold;
    vfs_get_stats(&cold);

    vfs_reset_stats();
    vfs_stat("/foo/bar", &st);
    VfsStats warm;
    vfs_get_stats(&warm);

    std::printf("        resolving /foo/bar, cold cache: %" PRIu64 " disk reads, %" PRIu64 " cache hits\n",
                cold.disk_reads, cold.cache_hits);
    std::printf("        the book counts 5: root inode, root data, foo inode, foo data, bar inode.\n");
    std::printf("        Here inodes 1, 2 and 3 share one inode-table block, so it is read once: 3.\n");
    std::printf("        again, warm cache: %" PRIu64 " disk reads\n", warm.disk_reads);
    check(rc == 0 && cold.disk_reads == 3, "a cold lookup reads 3 blocks");
    check(warm.disk_reads == 0, "a warm one reads none");
}

} // namespace

int main() {
    g_img = "/tmp/vfs_dirtest_" + std::to_string(getpid()) + ".img";
    g_log = g_img + ".log";

    try {
        make_image(4096);
        if (!mount())
            throw std::runtime_error("mount failed");
        check_tree();
        check_lookups();
        check_links();
        check_errors();
        check_growth();
        vfs_unmount();
        std::printf("consistency\n");
        check_fsck("fsck is happy with the tree");
        mount();
        check_io_count();
    } catch (const std::exception& e) {
        std::printf("  FAIL  unexpected exception: %s\n", e.what());
        failures++;
    }
    vfs_unmount();
    std::remove(g_img.c_str());
    std::remove(g_log.c_str());

    std::printf("\n%s\n", failures == 0 ? "stage 4: all checks passed" : "stage 4: FAILURES");
    return failures == 0 ? 0 : 1;
}
