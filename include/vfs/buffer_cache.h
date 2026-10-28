#pragma once

// buffer_cache.h — the block cache (stage 6): hot 4 KB blocks kept in RAM,
// and the rules for when a changed one may go back to disk.img.
//
// The frames are split over 16 shards by block number, each with its own
// mutex, map and LRU list, so threads working on different blocks rarely
// meet (OSTEP 29.4). Each shard gets an equal share of the frames and evicts
// its own least recently used one. Two rules decide whether a dirty frame may
// be written back, and every write-back path checks both:
//
//   no-steal     a frame holding a change from the running transaction is
//                never written back or evicted, so nothing uncommitted can
//                reach disk.img - which is why recovery never has to undo;
//   write-ahead  a dirty frame is written back only once the log is durable
//                up to the lsn of the commit that last changed it.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

#include "vfs/block_device.h"
#include "vfs/layout.h"

namespace vfs {

    struct CacheStats {
        uint64_t hits = 0, misses = 0, evictions = 0;
    };

    class BufferCache {
        public:
            static constexpr uint32_t SHARDS     = 16;
            static constexpr uint32_t MIN_FRAMES = 256;

            // frames is raised to MIN_FRAMES if it is less. durable_lsn says
            // how far the log is safely on disk.
            BufferCache(BlockDevice& dev, uint32_t frames,
                        std::function<uint64_t()> durable_lsn);
            ~BufferCache();

            BufferCache(const BufferCache&) = delete;
            BufferCache& operator=(const BufferCache&) = delete;

            // Copies the block's current bytes into dst.
            void read(uint32_t block_no, Block& dst);

            // Runs fn on the block's bytes, locked against every other reader
            // and writer. fresh: start from zeros instead of reading the disk.
            // in_txn: the frame joins the running transaction and stays
            // pinned in it until commit_frame(); *joined is set, before fn
            // runs, to whether it was not in it already. Without in_txn the
            // frame is dirty at once and may be written back any time.
            void modify(uint32_t block_no, bool fresh, bool in_txn,
                        const std::function<void(Block&)>& fn,
                        bool* joined = nullptr);

            // For a commit: the bytes of a frame in the running transaction,
            // copied to dst (BLOCK_SIZE bytes).
            void copy_txn_frame(uint32_t block_no, uint8_t* dst);
            void copy_txn_frame(uint32_t block_no, Block& dst) { copy_txn_frame(block_no, dst.data()); }

            // The commit holding this frame is durable at lsn: the frame
            // leaves the transaction, dirty, and may now be written back.
            void commit_frame(uint32_t block_no, uint64_t lsn);

            // Writes back up to max dirty frames that are allowed to go,
            // least recently used first, neighbouring blocks in one write.
            // Returns how many it wrote. Does not sync the disk.
            size_t write_back(size_t max);
            size_t write_back_all() { return write_back(SIZE_MAX); }

            uint32_t capacity() const { return capacity_; }
            uint64_t dirty_count() const { return dirty_count_.load(); }
            bool contains(uint32_t block_no);  // is it cached? (for tests)

            CacheStats stats() const;
            void reset_stats();

        private:
            struct Frame;
            struct Shard;

            Shard& shard_for(uint32_t block_no);
            Frame* acquire(Shard& s, uint32_t block_no, bool zero_fill,
                           bool join_txn, bool* exclusive, bool* joined);
            Frame* take_frame(Shard& s);
            Frame* new_frame(Shard& s);
            void unpin(Shard& s, Frame* f);
            void finish_modify(Shard& s, Frame* f, bool in_txn);

            static void lru_unlink(Shard& s, Frame* f);
            static void lru_push_front(Shard& s, Frame* f);
            static void lru_push_back(Shard& s, Frame* f);

            BlockDevice& dev_;
            const uint32_t capacity_;
            const std::function<uint64_t()> durable_lsn_;
            std::unique_ptr<Shard[]> shards_;

            // Write-back passes run one at a time: two passes writing the
            // same frame could land in either order, and the older bytes win.
            // It also guards run_buf_, where a pass gathers neighbouring
            // blocks into a single write of up to RUN_BLOCKS.
            static constexpr uint32_t RUN_BLOCKS = 256;
            std::mutex write_back_mutex_;
            std::unique_ptr<uint8_t[]> run_buf_;

            std::atomic<uint64_t> dirty_count_{0};
            std::atomic<uint64_t> hits_{0}, misses_{0}, evictions_{0};
    };

} // namespace vfs
