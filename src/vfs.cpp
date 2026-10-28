// vfs.cpp — the public API: path lookup, the open file table, and every call
// as one transaction (or, for a big write, several).
//
// Lock order, always, everywhere (see the README):
//   descriptor mutex -> begin_op -> inode locks, top down the tree
//                    -> allocator -> the cache and the log (inside Journal)
// begin_op comes before any inode lock because it may wait for a commit, and
// a commit waits for every running operation to end: an operation stuck
// waiting for a lock held by a thread stuck in begin_op would never end.
//
// Two more sit outside that chain. The descriptor table's mutex is only taken
// to hand out or give back a slot, innermost, holding nothing else but the
// descriptor's own mutex. And every call passes the MountGate below, which
// never blocks a call - it only lets unmount wait for calls to finish.

#include "vfs/vfs.h"

#include "vfs/alloc.h"
#include "vfs/block_map.h"
#include "vfs/directory.h"
#include "vfs/inode_table.h"
#include "vfs/layout.h"
#include "vfs/lock_manager.h"
#include "vfs/path.h"
#include "vfs/storage.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

namespace {

using vfs::FsError;
using vfs::Inode;
using vfs::InodeLock;
using vfs::Journal;
using vfs::OpScope;

constexpr int MAX_OPEN = 1024;

// One write call becomes one transaction per this many data blocks, so no
// operation comes near Journal::MAX_OP_BLOCKS: 16 data blocks, at most 4
// indirect blocks, the inode and a few bitmap blocks.
constexpr uint64_t WRITE_CHUNK_BLOCKS = 16;

constexpr uint64_t MAX_FILE_BYTES = vfs::MAX_FILE_BLOCKS * vfs::BLOCK_SIZE;

uint64_t now() {
    return static_cast<uint64_t>(std::time(nullptr));
}

struct OpenFile {
    // One call at a time on a descriptor. Guards everything below except
    // in_use. A call on a descriptor takes only this, never the table's mutex,
    // so calls on different descriptors never meet here.
    std::mutex op;
    uint64_t offset = 0;
    uint32_t inode = 0;  // 0 unless open: set by open(), cleared by close()
    int flags = 0;
    vfs::LockManager::Entry* lock = nullptr; // a reference held while open

    // Guarded by FileSystem::table_mutex: whether the slot is taken. A slot is
    // taken before open() fills it in and given back after close() empties it.
    bool in_use = false;
};

struct FileSystem {
    vfs::Storage storage;
    const vfs::Superblock sb;
    vfs::Allocator alloc;
    vfs::LockManager locks;
    std::mutex table_mutex;
    std::array<OpenFile, MAX_OPEN> files;

    FileSystem(const char* disk, const char* wal, const vfs::StorageOptions& opts)
        : storage(disk, wal, opts),
        sb(storage.superblock()),
        alloc(storage.journal(), sb) {
    }

    Journal& j() { return storage.journal(); }
};

// Which calls are running, so that unmount can wait for them to finish.
//
// Every call goes through here, so it must not be one lock that every thread
// takes: a std::shared_mutex is exactly that, since libc++ builds it on a
// plain mutex, and it was the busiest lock in the whole file system. Instead
// each thread counts its calls in a slot of its own, on its own cache line.
// Mount and unmount, which are rare, do the expensive part: close the gate,
// then wait until every slot reads zero.
//
// Why no call slips past a closing gate: a call adds to its slot, then looks
// at closed; unmount sets closed, then looks at every slot - all sequentially
// consistent, so one of the two always sees the other. Either the call is
// counted and unmount waits for it, or it sees the gate closed and backs out.
class MountGate {
    public:
        // Counts a call in; false (and not counted) if nothing is mounted, or
        // a mount or unmount is under way.
        bool enter() {
            std::atomic<int64_t>& n = slots_[my_slot()].n;
            n.fetch_add(1);
            if (closed_.load()) {
                n.fetch_sub(1);
                return false;
            }
            return true;
        }
        void leave() { slots_[my_slot()].n.fetch_sub(1); }

        // For mount and unmount, holding change.
        void close_and_drain() {
            closed_.store(true);
            for (Slot& s : slots_) {
                while (s.n.load() != 0)
                    std::this_thread::yield();
            }
        }
        void open() { closed_.store(false); }

        std::mutex change;  // one mount or unmount at a time

    private:
        static constexpr size_t SLOTS = 64;
        struct alignas(64) Slot {
            std::atomic<int64_t> n{0};
        };

        static size_t my_slot() {
            static std::atomic<size_t> next{0};
            thread_local const size_t slot = next.fetch_add(1) % SLOTS;
            return slot;
        }

        Slot slots_[SLOTS];
        std::atomic<bool> closed_{true};
};

MountGate g_gate;
std::unique_ptr<FileSystem> g_fs;  // set and cleared only with the gate closed

// Holds a call in, for as long as it lives.
class CallScope {
    public:
        CallScope() : in_(g_gate.enter()) {}
        ~CallScope() {
            if (in_)
                g_gate.leave();
        }
        CallScope(const CallScope&) = delete;
        CallScope& operator=(const CallScope&) = delete;
        explicit operator bool() const { return in_; }
    private:
        const bool in_;
};

// Runs one API call against the mounted file system, and turns what it
// throws into a negative error code.
template <typename R, typename F>
R with_fs(F body) {
    CallScope call;
    if (!call)
        return -VFS_EINVAL;
    try {
        return body(*g_fs);
    } catch (const FsError& e) {
        return -e.code;
    } catch (const std::exception&) {
        return -VFS_EIO;
    }
}

// ------------------------------------------------------------ path lookup

// Walks names[0, n) down from the root, hand over hand: each directory is
// held, shared, only until the next inode's lock has been taken. So nothing
// can be removed between finding a name and locking what it names, and no
// thread ever waits for a lock above one it holds - which is what keeps
// lookups from deadlocking. The inode reached is returned locked, exclusive if
// asked.
InodeLock walk(FileSystem& fs, const std::vector<std::string>& names, size_t n, bool exclusive) {
    InodeLock cur(fs.locks, vfs::ROOT_INODE, n == 0 && exclusive);
    for (size_t i = 0; i < n; i++) {
        Inode dir = vfs::read_inode(fs.j(), fs.sb, cur.id());
        if (!vfs::is_dir(dir))
            throw FsError(VFS_ENOTDIR);
        uint32_t child = vfs::dir_lookup(fs.j(), dir, names[i]);
        if (child == 0)
            throw FsError(VFS_ENOENT);
        InodeLock next(fs.locks, child, i + 1 == n && exclusive);
        cur = std::move(next); // lets go of the directory
    }
    return cur;
}

// ------------------------------------------------------- open file table

// The table's mutex is only for taking and giving back slots. It nests inside
// a descriptor's mutex (close gives its slot back holding it), never around
// one: nothing holding the table's mutex waits for a descriptor.
int reserve_fd(FileSystem& fs) {
    std::lock_guard<std::mutex> lk(fs.table_mutex);
    for (int fd = 0; fd < MAX_OPEN; fd++) {
        OpenFile& f = fs.files[fd];
        if (!f.in_use) {
            f.in_use = true;
            return fd;
        }
    }
    throw FsError(VFS_EMFILE);
}

void release_fd(FileSystem& fs, int fd) {
    std::lock_guard<std::mutex> lk(fs.table_mutex);
    fs.files[fd].in_use = false;
}

struct Snapshot {
    uint32_t inode;
    int flags;
    vfs::LockManager::Entry* lock;
};

OpenFile& file_at(FileSystem& fs, int fd) {
    if (fd < 0 || fd >= MAX_OPEN)
        throw FsError(VFS_EBADF);
    return fs.files[fd];
}

// Call with f.op held. inode is only non-zero between open() filling the slot
// in and close() emptying it, both under f.op, so this is all it takes to
// know the descriptor is open.
Snapshot snapshot(OpenFile& f) {
    if (f.inode == 0)
        throw FsError(VFS_EBADF);
    return {f.inode, f.flags, f.lock};
}

// Called with the file's lock held - which is what keeps a concurrent unlink,
// which checks the count under the same lock taken exclusive, from missing it.
vfs::LockManager::Entry* register_open(FileSystem& fs, InodeLock& file) {
    vfs::LockManager::Entry* entry = fs.locks.acquire(file.id());
    entry->open_count++;
    return entry;
}

// -------------------------------------------------------------- creating

Inode new_inode(uint32_t id, uint16_t mode) {
    Inode ino{};
    ino.id = id;
    ino.mode = mode;
    ino.link_count = 1;
    ino.mtime = now();
    return ino;
}

// Adds a file called name to dir, which the caller holds exclusive. Writes
// both inodes. Changes nothing if it fails.
uint32_t create_file(FileSystem& fs, Inode& dir, const std::string& name) {
    Journal& j = fs.j();
    uint32_t id = fs.alloc.alloc_inode(j);
    try {
        vfs::dir_add(j, fs.alloc, dir, name, id, vfs::FT_FILE);
    } catch (...) {
        fs.alloc.free_inode(j, id);
        throw;
    }
    vfs::write_inode(j, fs.sb, new_inode(id, vfs::MODE_FILE | vfs::FILE_PERM));
    dir.mtime = now();
    vfs::write_inode(j, fs.sb, dir);
    return id;
}

// open() with O_CREAT and/or O_TRUNC: it may change the disk, so it is a
// transaction. Returns the file's inode, with a reference to its lock entry
// and its open count already taken.
uint32_t open_changing(FileSystem& fs, const std::vector<std::string>& names, int flags,
                       vfs::LockManager::Entry** entry) {
    OpScope op(fs.j());
    const bool create = (flags & VFS_O_CREAT) != 0;
    InodeLock parent = walk(fs, names, names.size() - 1, create);

    Inode dir = vfs::read_inode(fs.j(), fs.sb, parent.id());
    if (!vfs::is_dir(dir))
        throw FsError(VFS_ENOTDIR);

    const std::string& name = names.back();
    uint8_t type = 0;
    uint32_t id = vfs::dir_lookup(fs.j(), dir, name, &type);

    if (id == 0) {
        if (!create)
            throw FsError(VFS_ENOENT);
        id = create_file(fs, dir, name);
        InodeLock file(fs.locks, id, true);
        *entry = register_open(fs, file);
        return id;
    }

    if (create && (flags & VFS_O_EXCL))
        throw FsError(VFS_EEXIST);
    if (type == vfs::FT_DIR)
        throw FsError(VFS_EISDIR);

    const bool trunc = (flags & VFS_O_TRUNC) != 0;
    InodeLock file(fs.locks, id, trunc);
    if (trunc) {
        file.entry().inode_valid = false;  // rewritten here, not by vfs_write
        Inode ino = vfs::read_inode(fs.j(), fs.sb, id);
        if (ino.size != 0 || ino.block_count != 0) {
            vfs::free_all_blocks(fs.j(), fs.alloc, ino);
            ino.size = 0;
            ino.mtime = now();
            vfs::write_inode(fs.j(), fs.sb, ino);
        }
    }
    *entry = register_open(fs, file);
    return id;
}

// open() of a file that is only being read from or written to: no
// transaction, just a lookup.
uint32_t open_existing(FileSystem& fs, const std::vector<std::string>& names,
                       vfs::LockManager::Entry** entry) {
    InodeLock file = walk(fs, names, names.size(), false);
    Inode ino = vfs::read_inode(fs.j(), fs.sb, file.id());
    if (vfs::is_dir(ino))
        throw FsError(VFS_EISDIR);
    *entry = register_open(fs, file);
    return file.id();
}

// ------------------------------------------------------- reading, writing

// A file's inode, from the copy its lock entry keeps when there is one (see
// LockManager::Entry). Call holding e.lock, either way.
Inode file_inode(FileSystem& fs, vfs::LockManager::Entry& e, uint32_t id) {
    if (e.inode_valid)
        return e.inode;
    return vfs::read_inode(fs.j(), fs.sb, id);
}

size_t read_range(FileSystem& fs, const Inode& ino, uint64_t pos, uint8_t* dst, size_t n) {
    if (pos >= ino.size)
        return 0;
    uint64_t left = ino.size - pos;
    size_t total = left < n ? static_cast<size_t>(left) : n;

    vfs::Block b;
    size_t done = 0;
    while (done < total) {
        uint64_t at = pos + done;
        uint64_t index = at / vfs::BLOCK_SIZE;
        uint32_t off = static_cast<uint32_t>(at % vfs::BLOCK_SIZE);
        size_t len = std::min<size_t>(vfs::BLOCK_SIZE - off, total - done);

        uint32_t block = vfs::bmap_read(fs.j(), ino, index);
        if (block == 0) {
            std::memset(dst + done, 0, len); // a hole
        } else {
            fs.j().read(block, b);
            std::memcpy(dst + done, b.data() + off, len);
        }
        done += len;
    }
    return done;
}

} // anonymous namespace

// ===================================================================== API

int vfs_mount(const char* disk_path, const char* wal_path, const VfsOptions* opts) {
    std::lock_guard<std::mutex> lk(g_gate.change);
    if (g_fs)
        return -VFS_EBUSY;
    if (disk_path == nullptr || wal_path == nullptr)
        return -VFS_EINVAL;
    if (::access(disk_path, F_OK) != 0)
        return -VFS_ENOENT;

    VfsOptions o = opts ? *opts : VfsOptions{};
    if (o.cache_frames < 256)
        return -VFS_EINVAL;

    vfs::StorageOptions so;
    so.cache_frames       = o.cache_frames;
    so.disk_latency_us    = o.disk_latency_us;
    so.journal            = o.journal;
    so.fsync              = o.fsync;
    so.flusher            = o.flusher;
    so.commit_interval_ms = o.commit_interval_ms;
    so.checkpoint_bytes   = o.checkpoint_bytes;

    // The gate is closed whenever nothing is mounted, so no call can see
    // g_fs change under it.
    try {
        g_fs = std::make_unique<FileSystem>(disk_path, wal_path, so);
    } catch (const FsError& e) {
        return -e.code;
    } catch (const std::exception&) {
        return -VFS_EINVAL;
    }
    g_gate.open();
    return 0;
}

int vfs_unmount() {
    std::lock_guard<std::mutex> lk(g_gate.change);
    if (!g_fs)
        return -VFS_EINVAL;

    g_gate.close_and_drain();
    std::unique_ptr<FileSystem> fs = std::move(g_fs);
    // Nothing else can be running: the gate is closed and every call that got
    // in before it closed has left.
    for (OpenFile& f : fs->files) {
        if (f.inode != 0) {
            f.lock->open_count--;
            fs->locks.release(f.inode);
        }
        f.inode = 0;
        f.lock = nullptr;
        f.in_use = false;
    }

    try {
        fs->storage.close();
    } catch (const std::exception&) {
        return -VFS_EIO;
    }
    return 0;
}

int vfs_open(const char* path, int flags) {
    return with_fs<int>([&](FileSystem& fs) -> int {
        int access = flags & VFS_O_ACCMODE;
        if (access == VFS_O_ACCMODE)
            throw FsError(VFS_EINVAL);
        if ((flags & VFS_O_TRUNC) && access == VFS_O_RDONLY)
            throw FsError(VFS_EINVAL);

        std::vector<std::string> names = vfs::split_path(path);
        if (names.empty())
            throw FsError(VFS_EISDIR);

        // Taken first, so a full table fails before anything is created.
        int fd = reserve_fd(fs);
        try {
            vfs::LockManager::Entry* entry = nullptr;
            uint32_t id = (flags & (VFS_O_CREAT | VFS_O_TRUNC))
                        ? open_changing(fs, names, flags, &entry)
                        : open_existing(fs, names, &entry);

            OpenFile& f = fs.files[fd];
            std::lock_guard<std::mutex> lk(f.op);
            f.inode = id;
            f.flags = flags;
            f.lock = entry;
            f.offset = 0;
        } catch (...) {
            release_fd(fs, fd);
            throw;
        }
        return fd;
    });
}

int vfs_close(int fd) {
    return with_fs<int>([&](FileSystem& fs) -> int {
        OpenFile& f = file_at(fs, fd);
        std::lock_guard<std::mutex> op(f.op);
        Snapshot s = snapshot(f);

        s.lock->open_count--;
        fs.locks.release(s.inode);
        f.inode = 0;
        f.lock = nullptr;
        release_fd(fs, fd);
        return 0;
    });
}

ssize_t vfs_read(int fd, void* buf, size_t n) {
    return with_fs<ssize_t>([&](FileSystem& fs) -> ssize_t {
        OpenFile& f = file_at(fs, fd);
        std::lock_guard<std::mutex> op(f.op);
        Snapshot s = snapshot(f);
        if ((s.flags & VFS_O_ACCMODE) == VFS_O_WRONLY)
            throw FsError(VFS_EBADF);
        if (n == 0)
            return 0;
        if (buf == nullptr)
            throw FsError(VFS_EINVAL);
        n = std::min<size_t>(n, SSIZE_MAX);

        size_t got;
        {
            std::shared_lock<std::shared_mutex> lk(s.lock->lock);
            Inode ino = file_inode(fs, *s.lock, s.inode);
            got = read_range(fs, ino, f.offset, static_cast<uint8_t*>(buf), n);
        }
        f.offset += got;
        return static_cast<ssize_t>(got);
    });
}

ssize_t vfs_write(int fd, const void* buf, size_t n) {
    return with_fs<ssize_t>([&](FileSystem& fs) -> ssize_t {
        OpenFile& f = file_at(fs, fd);
        std::lock_guard<std::mutex> op_guard(f.op);
        Snapshot s = snapshot(f);
        if ((s.flags & VFS_O_ACCMODE) == VFS_O_RDONLY)
            throw FsError(VFS_EBADF);
        if (n == 0)
            return 0;
        if (buf == nullptr)
            throw FsError(VFS_EINVAL);
        n = std::min<size_t>(n, SSIZE_MAX);

        const uint8_t* src = static_cast<const uint8_t*>(buf);
        const bool append = (s.flags & VFS_O_APPEND) != 0;
        uint64_t pos = f.offset;
        size_t done = 0;
        bool disk_full = false;

        while (done < n && !disk_full) {
            // One transaction per chunk. The file's lock is let go between
            // chunks, because the next begin_op may wait for a commit - and a
            // thread must never wait in begin_op holding an inode lock.
            OpScope op(fs.j());
            std::unique_lock<std::shared_mutex> lk(s.lock->lock);

            Inode ino = file_inode(fs, *s.lock, s.inode);
            // Not to be trusted until the inode is whole again at the end of
            // the chunk: if anything below throws, the next call reads the
            // inode afresh.
            s.lock->inode_valid = false;
            if (append)
                pos = ino.size;
            if (pos >= MAX_FILE_BYTES) {
                if (done == 0)
                    throw FsError(VFS_EFBIG);
                break;
            }

            uint64_t chunk_end = std::min<uint64_t>({
                pos + (n - done),
                (pos / vfs::BLOCK_SIZE + WRITE_CHUNK_BLOCKS) * vfs::BLOCK_SIZE,
                MAX_FILE_BYTES});

            uint64_t at = pos;
            bool changed = false;
            while (at < chunk_end) {
                uint64_t index = at / vfs::BLOCK_SIZE;
                uint32_t off = static_cast<uint32_t>(at % vfs::BLOCK_SIZE);
                size_t len = static_cast<size_t>(
                    std::min<uint64_t>(vfs::BLOCK_SIZE - off, chunk_end - at));

                bool fresh = false;
                uint32_t block;
                try {
                    block = vfs::bmap_alloc(fs.j(), fs.alloc, ino, index, &fresh);
                } catch (const FsError& e) {
                    if (e.code != VFS_ENOSPC)
                        throw;
                    disk_full = true; // keep what is written so far
                    break;
                }
                changed = changed || fresh;

                // A block written whole does not need its old contents read.
                bool whole = off == 0 && len == vfs::BLOCK_SIZE;
                const uint8_t* from = src + done + (at - pos);
                fs.j().write(block, fresh || whole, [&](vfs::Block& b) {
                    std::memcpy(b.data() + off, from, len);
                });
                at += len;
            }

            // The inode is written back only if something in it changed: a
            // new block, a new size, or a new second. An overwrite inside the
            // same second leaves it alone - which matters, because 32 inodes
            // share an inode table block, and writing it locks it against
            // every other file in it.
            if (at > pos) {
                if (at > ino.size) {
                    ino.size = at;
                    changed = true;
                }
                const uint64_t t = now();
                if (ino.mtime != t) {
                    ino.mtime = t;
                    changed = true;
                }
            }
            if (changed)
                vfs::write_inode(fs.j(), fs.sb, ino);
            s.lock->inode = ino;
            s.lock->inode_valid = true;

            done += static_cast<size_t>(at - pos);
            pos = at;
        }

        f.offset = pos;
        if (done == 0 && disk_full)
            throw FsError(VFS_ENOSPC);
        return static_cast<ssize_t>(done);
    });
}

off_t vfs_lseek(int fd, off_t off, int whence) {
    return with_fs<off_t>([&](FileSystem& fs) -> off_t {
        OpenFile& f = file_at(fs, fd);
        std::lock_guard<std::mutex> op(f.op);
        Snapshot s = snapshot(f);

        off_t base;
        switch (whence) {
            case VFS_SEEK_SET:
                base = 0;
                break;
            case VFS_SEEK_CUR:
                base = static_cast<off_t>(f.offset);
                break;
            case VFS_SEEK_END: {
                std::shared_lock<std::shared_mutex> lk(s.lock->lock);
                base = static_cast<off_t>(file_inode(fs, *s.lock, s.inode).size);
                break;
            }
            default:
                throw FsError(VFS_EINVAL);
        }

        off_t result;
        if (__builtin_add_overflow(base, off, &result) || result < 0)
            throw FsError(VFS_EINVAL);
        f.offset = static_cast<uint64_t>(result);
        return result;
    });
}

int vfs_fsync(int fd) {
    return with_fs<int>([&](FileSystem& fs) -> int {
        OpenFile& f = file_at(fs, fd);
        {
            // Checked, then let go: a commit can take a while, and other
            // calls on this descriptor need not wait for it.
            std::lock_guard<std::mutex> lk(f.op);
            snapshot(f);
        }
        fs.j().commit();
        return 0;
    });
}

int vfs_sync() {
    return with_fs<int>([&](FileSystem& fs) -> int {
        fs.storage.checkpoint();
        return 0;
    });
}

int vfs_mkdir(const char* path) {
    return with_fs<int>([&](FileSystem& fs) -> int {
        std::vector<std::string> names = vfs::split_path(path);
        if (names.empty())
            throw FsError(VFS_EEXIST); // "/"

        Journal& j = fs.j();
        OpScope op(j);
        InodeLock parent = walk(fs, names, names.size() - 1, true);
        Inode dir = vfs::read_inode(j, fs.sb, parent.id());
        if (!vfs::is_dir(dir))
            throw FsError(VFS_ENOTDIR);

        const std::string& name = names.back();
        if (vfs::dir_lookup(j, dir, name) != 0)
            throw FsError(VFS_EEXIST);
        if (dir.link_count == UINT16_MAX)
            throw FsError(VFS_ENOSPC); // no room to count another ".."

        // Everything that can run out is taken first, and handed back if a
        // later step fails, so a failed mkdir leaves nothing behind.
        uint32_t id = fs.alloc.alloc_inode(j);
        uint32_t block;
        try {
            block = fs.alloc.alloc_block(j);
        } catch (...) {
            fs.alloc.free_inode(j, id);
            throw;
        }
        try {
            vfs::dir_add(j, fs.alloc, dir, name, id, vfs::FT_DIR);
        } catch (...) {
            fs.alloc.free_blocks(j, {block});
            fs.alloc.free_inode(j, id);
            throw;
        }

        vfs::dir_init(j, block, id, parent.id());
        Inode ino = new_inode(id, vfs::MODE_DIR | vfs::DIR_PERM);
        ino.link_count = 2;  // its "." and its entry in the parent
        ino.size = vfs::BLOCK_SIZE;
        ino.block_count = 1;
        ino.direct[0] = block;
        vfs::write_inode(j, fs.sb, ino);

        dir.link_count++;    // the new directory's ".."
        dir.mtime = now();
        vfs::write_inode(j, fs.sb, dir);
        return 0;
    });
}

int vfs_unlink(const char* path) {
    return with_fs<int>([&](FileSystem& fs) -> int {
        std::vector<std::string> names = vfs::split_path(path);
        if (names.empty())
            throw FsError(VFS_EISDIR);

        Journal& j = fs.j();
        OpScope op(j);
        InodeLock parent = walk(fs, names, names.size() - 1, true);
        Inode dir = vfs::read_inode(j, fs.sb, parent.id());
        if (!vfs::is_dir(dir))
            throw FsError(VFS_ENOTDIR);

        const std::string& name = names.back();
        uint8_t type = 0;
        uint32_t id = vfs::dir_lookup(j, dir, name, &type);
        if (id == 0)
            throw FsError(VFS_ENOENT);
        if (type == vfs::FT_DIR)
            throw FsError(VFS_EISDIR);

        InodeLock file(fs.locks, id, true);
        if (file.entry().open_count.load() > 0)
            throw FsError(VFS_EBUSY);

        // The inode is about to be freed, and its number may be handed to a
        // new file while this entry is still alive.
        file.entry().inode_valid = false;
        Inode ino = vfs::read_inode(j, fs.sb, id);
        vfs::dir_remove(j, dir, name);
        vfs::free_all_blocks(j, fs.alloc, ino);
        vfs::clear_inode(j, fs.sb, id);
        fs.alloc.free_inode(j, id);

        dir.mtime = now();
        vfs::write_inode(j, fs.sb, dir);
        return 0;
    });
}

int vfs_rmdir(const char* path) {
    return with_fs<int>([&](FileSystem& fs) -> int {
        std::vector<std::string> names = vfs::split_path(path);
        if (names.empty())
            throw FsError(VFS_EBUSY); // never the root

        Journal& j = fs.j();
        OpScope op(j);
        InodeLock parent = walk(fs, names, names.size() - 1, true);
        Inode dir = vfs::read_inode(j, fs.sb, parent.id());
        if (!vfs::is_dir(dir))
            throw FsError(VFS_ENOTDIR);

        const std::string& name = names.back();
        uint8_t type = 0;
        uint32_t id = vfs::dir_lookup(j, dir, name, &type);
        if (id == 0)
            throw FsError(VFS_ENOENT);
        if (type != vfs::FT_DIR)
            throw FsError(VFS_ENOTDIR);

        InodeLock child(fs.locks, id, true);
        child.entry().inode_valid = false;  // as in unlink
        Inode ino = vfs::read_inode(j, fs.sb, id);
        if (!vfs::dir_is_empty(j, ino))
            throw FsError(VFS_ENOTEMPTY);

        vfs::dir_remove(j, dir, name);
        vfs::free_all_blocks(j, fs.alloc, ino);
        vfs::clear_inode(j, fs.sb, id);
        fs.alloc.free_inode(j, id);

        dir.link_count--;    // its ".." is gone
        dir.mtime = now();
        vfs::write_inode(j, fs.sb, dir);
        return 0;
    });
}

int vfs_stat(const char* path, VfsStat* out) {
    return with_fs<int>([&](FileSystem& fs) -> int {
        if (out == nullptr)
            throw FsError(VFS_EINVAL);
        std::vector<std::string> names = vfs::split_path(path);
        InodeLock node = walk(fs, names, names.size(), false);
        Inode ino = vfs::read_inode(fs.j(), fs.sb, node.id());

        out->inode = ino.id;
        out->mode = ino.mode;
        out->link_count = ino.link_count;
        out->size = ino.size;
        out->blocks = ino.block_count;
        out->mtime = ino.mtime;
        return 0;
    });
}

int vfs_readdir(const char* path, VfsDirEnt* out, size_t max) {
    return with_fs<int>([&](FileSystem& fs) -> int {
        if (out == nullptr && max > 0)
            throw FsError(VFS_EINVAL);
        std::vector<std::string> names = vfs::split_path(path);
        InodeLock node = walk(fs, names, names.size(), false);
        Inode dir = vfs::read_inode(fs.j(), fs.sb, node.id());
        if (!vfs::is_dir(dir))
            throw FsError(VFS_ENOTDIR);

        std::vector<vfs::DirItem> items = vfs::dir_list(fs.j(), dir);
        for (size_t i = 0; i < items.size() && i < max; i++) {
            out[i] = VfsDirEnt{};
            out[i].inode = items[i].inode;
            out[i].type = items[i].type;
            std::memcpy(out[i].name, items[i].name.data(), items[i].name.size());
        }
        return static_cast<int>(std::min<size_t>(items.size(), INT_MAX));
    });
}

void vfs_get_stats(VfsStats* out) {
    if (out == nullptr)
        return;
    *out = VfsStats{};
    CallScope call;
    if (!call)
        return;

    vfs::StorageStats s = g_fs->storage.stats();
    vfs::RecoveryInfo r = g_fs->storage.recovery();
    out->disk_reads = s.disk_reads;
    out->disk_writes = s.disk_writes;
    out->cache_hits = s.cache_hits;
    out->cache_misses = s.cache_misses;
    out->evictions = s.evictions;
    out->log_bytes = s.log_bytes;
    out->commits = s.commits;
    out->checkpoints = s.checkpoints;
    out->recovered_txns = r.txns_replayed;
    out->recovered_blocks = r.blocks_replayed;
}

void vfs_reset_stats() {
    CallScope call;
    if (call)
        g_fs->storage.reset_stats();
}
