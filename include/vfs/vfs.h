#pragma once

// vfs.h — the public API. The only header a program using the file system
// needs. POSIX-shaped: every call returns >= 0 on success and -VFS_Exxx on
// failure. One file system is mounted per process at a time. Every call is
// safe to make from many threads at once.
//
// Paths are absolute ("/a/b"). "." and ".." are resolved by name, and there
// are no links, so "/a/../b" is "/b".

#include <cstddef>
#include <cstdint>
#include <sys/types.h>

#include "vfs/errors.h"

struct VfsOptions {
    uint32_t cache_frames       = 4096;      // block cache size: 4096 x 4 KB = 16 MB. At least 256.
    uint32_t disk_latency_us    = 0;         // sleep before every disk.img block read or write (benchmarks)
    bool     journal            = true;      // false: no write-ahead log. Faster, not crash-safe
    bool     fsync              = true;      // false: never fsync. Only a power cut can then lose data
    bool     flusher            = true;      // background thread: writes dirty blocks back, commits periodically
    uint32_t commit_interval_ms = 1000;      // the flusher commits the running transaction this often
    uint64_t checkpoint_bytes   = 32u << 20; // checkpoint once the log grows past this
};

// open() flags. Exactly one of the first three, plus any of the rest.
constexpr int VFS_O_RDONLY  = 0;
constexpr int VFS_O_WRONLY  = 1;
constexpr int VFS_O_RDWR    = 2;
constexpr int VFS_O_ACCMODE = 3;
constexpr int VFS_O_CREAT   = 0x100;  // create the file if it does not exist (its directory must)
constexpr int VFS_O_EXCL    = 0x200;  // with O_CREAT: fail with VFS_EEXIST if it exists
constexpr int VFS_O_TRUNC   = 0x400;  // cut to zero length; needs write access
constexpr int VFS_O_APPEND  = 0x800;  // every write goes to the end

constexpr int VFS_SEEK_SET = 0;
constexpr int VFS_SEEK_CUR = 1;
constexpr int VFS_SEEK_END = 2;

struct VfsStat {
    uint32_t inode;
    uint16_t mode;        // MODE_FILE or MODE_DIR, plus permission bits
    uint16_t link_count;
    uint64_t size;
    uint32_t blocks;      // blocks owned, indirect blocks included
    uint64_t mtime;       // seconds since the epoch
};

struct VfsDirEnt {
    uint32_t inode;
    uint8_t  type;        // FT_FILE or FT_DIR
    char     name[59];    // NUL-terminated
};

struct VfsStats {
    uint64_t disk_reads, disk_writes;       // blocks read from / written to disk.img
    uint64_t cache_hits, cache_misses, evictions;
    uint64_t log_bytes;                     // bytes appended to the log
    uint64_t commits, checkpoints;
    uint64_t recovered_txns, recovered_blocks;  // replayed by the last mount
};

// Mounts the image at disk_path, with its write-ahead log at wal_path (created
// if missing). If the image was not unmounted cleanly, the log is replayed
// first. opts may be null for the defaults. VFS_EBUSY if something is
// already mounted; VFS_EINVAL if the image is not a file system.
int     vfs_mount(const char* disk_path, const char* wal_path, const VfsOptions* opts = nullptr);

// Commits, writes everything back and marks the image clean. Open file
// descriptors are closed.
int     vfs_unmount();

// Directories cannot be opened (VFS_EISDIR); use vfs_readdir. A file that is
// open cannot be unlinked (VFS_EBUSY). At most 1024 files open at once.
int     vfs_open(const char* path, int flags);
ssize_t vfs_read(int fd, void* buf, size_t n);
ssize_t vfs_write(int fd, const void* buf, size_t n);  // may write less than n only when the disk fills
off_t   vfs_lseek(int fd, off_t off, int whence);      // past the end is allowed: the gap reads as zeros
int     vfs_close(int fd);
int     vfs_fsync(int fd);  // makes every operation that has returned durable, not just this file's
int     vfs_sync();         // vfs_fsync, then writes everything back to the image and empties the log

int     vfs_mkdir(const char* path);  // the parent must exist
int     vfs_unlink(const char* path); // files only
int     vfs_rmdir(const char* path);  // empty directories only; never "/"
int     vfs_stat(const char* path, VfsStat* out);

// Fills out[0 .. max) with the entries of a directory, "." and ".." first,
// and returns how many it has in total - which may be more than max.
int     vfs_readdir(const char* path, VfsDirEnt* out, size_t max);

// Counters since the mount, or since the last reset. Zeros when not mounted.
void    vfs_get_stats(VfsStats* out);
void    vfs_reset_stats();
