#pragma once

// storage.h — everything under the file system logic, behind one door: the
// instrumented disk, the block cache, the write-ahead log, crash recovery and
// the flusher thread. The file system above sees blocks and transactions,
// never a file descriptor.

#include <cstdint>
#include <functional>
#include <memory>

#include "vfs/block_device.h"
#include "vfs/layout.h"

namespace vfs {

    struct StorageOptions {
        uint32_t cache_frames       = 4096;
        uint32_t disk_latency_us    = 0;
        bool     journal            = true;
        bool     fsync              = true;
        bool     flusher            = true;
        uint32_t commit_interval_ms = 1000;
        uint64_t checkpoint_bytes   = 32u << 20;
    };

    struct StorageStats {
        uint64_t disk_reads = 0, disk_writes = 0;
        uint64_t cache_hits = 0, cache_misses = 0, evictions = 0;
        uint64_t log_bytes = 0, commits = 0, checkpoints = 0;
    };

    struct RecoveryInfo {
        bool     ran = false;           // the last unmount was not clean
        uint64_t txns_replayed = 0;     // committed transactions written to the image
        uint64_t blocks_replayed = 0;
        uint64_t txns_discarded = 0;    // found in the log without a COMMIT
    };

    // Every change to the disk goes through here, inside an operation.
    // Operations are grouped into transactions; a transaction reaches the log,
    // whole, before any of its blocks may reach disk.img. All methods are safe
    // to call from many threads.
    class Journal {
        public:
            // The most distinct blocks one operation may write.
            static constexpr uint32_t MAX_OP_BLOCKS = 64;

            virtual ~Journal() = default;

            // Brackets one file system operation (OpScope does both). begin_op
            // may wait: while a commit runs, or while the running transaction
            // is too full to promise MAX_OP_BLOCKS more. Lock order: call it
            // before taking any inode lock, never while holding one.
            virtual void begin_op() = 0;
            // May commit, if this was the last operation running and a commit
            // is due.
            virtual void end_op() = 0;

            // Copies the block's current contents - including changes not yet
            // committed - into dst. Inside or outside an operation.
            virtual void read(uint32_t block_no, Block& dst) = 0;

            // Changes a block in place. fn gets its bytes, locked against
            // every other reader and writer of that block. Only inside an
            // operation. fresh: the old contents do not matter, so the block
            // starts zeroed instead of being read from disk - use it for a
            // block just allocated, or one fn overwrites completely.
            virtual void write(uint32_t block_no, bool fresh,
                               const std::function<void(Block&)>& fn) = 0;

            // Makes every operation that has already ended durable. Not inside
            // an operation.
            virtual void commit() = 0;
    };

    // RAII for begin_op / end_op. An I/O error during the commit that end_op
    // may run ends the process; the log is what makes that safe.
    class OpScope {
        public:
            explicit OpScope(Journal& j) : j_(j) { j_.begin_op(); }
            ~OpScope() { j_.end_op(); }
            OpScope(const OpScope&) = delete;
            OpScope& operator=(const OpScope&) = delete;
        private:
            Journal& j_;
    };

    class Storage {
        public:
            // Opens the image and its log (created if missing). If the last
            // unmount was not clean (superblock clean_unmount == 0), replays
            // the log first. Then marks the image in use and starts the
            // flusher. Throws std::runtime_error if the image is not a file
            // system or cannot be opened.
            Storage(const char* disk_path, const char* wal_path,
                    const StorageOptions& opts = {});

            // Stops the flusher, commits, checkpoints, marks the image clean.
            // Safe to call twice.
            void close();
            ~Storage(); // close(), with errors swallowed

            Storage(const Storage&) = delete;
            Storage& operator=(const Storage&) = delete;

            const Superblock& superblock() const;
            Journal& journal();

            // The disk under the cache. Only for reading the bitmaps at mount,
            // before any operation has run.
            BlockDevice& device();

            // Commit, write every dirty block back, sync, empty the log.
            void checkpoint();

            RecoveryInfo recovery() const;
            StorageStats stats() const;
            void reset_stats();

        private:
            struct Impl;
            std::unique_ptr<Impl> impl_;
    };

} // namespace vfs
