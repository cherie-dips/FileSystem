// storage.cpp — the storage engine: block cache, write-ahead log, recovery
// and flusher, put together behind Storage and Journal (storage.h).
//
// How a change reaches disk.img (OSTEP 42.3):
//
//   1. An operation changes blocks in the cache. Each frame joins the running
//      transaction and stays pinned in the cache (no-steal).
//   2. When no operation is running and a commit is due, the transaction's
//      blocks go to the log as BEGIN + BLOCK records, then a sync, then a
//      COMMIT record and a second sync. The first barrier is what makes the
//      COMMIT trustworthy: it cannot reach the disk before the blocks it
//      vouches for (p. 534).
//   3. The frames leave the transaction dirty, tagged with the COMMIT's lsn,
//      and may now go back to disk.img - from the flusher, from eviction or
//      at a checkpoint - never before the log is durable up to that lsn.
//   4. A checkpoint writes every dirty frame back, syncs disk.img, records the
//      last lsn in the superblock and empties the log.
//
// Operations are grouped: every operation that runs while a transaction is
// open joins it, and the commit waits until none is running, so a commit
// never holds half of an operation (group commit, as in ext3 and xv6).
// Recovery (recovery.cpp) replays committed transactions and ignores the
// rest; since nothing uncommitted ever reaches disk.img, nothing ever needs
// undoing.
//
// Lock order. A thread holding one of these may only go on to take ones
// further down:
//
//   journal gate (begin_op)
//   -> inode locks                    (the file system's)
//   -> allocator mutex                (the file system's)
//   -> cache write-back mutex -> cache shard mutex -> frame lock
//   -> journal mutex
//
// The journal gate is an atomic word, not a lock: begin_op gets through it
// with one compare-and-swap and end_op with one fetch_sub, and only an
// operation that cannot get through - or the last one out when a commit is
// wanted - takes the journal's mutex. It can still make a thread wait, which
// is why it comes before the inode locks.
//
// The journal's mutex is never held across I/O or while waiting for a cache
// lock: a commit marks itself as running, lets the mutex go, and does its I/O
// knowing that no operation can start until it has finished.

#include "vfs/storage.h"
#include "vfs/buffer_cache.h"
#include "vfs/flusher.h"
#include "vfs/recovery.h"
#include "vfs/superblock.h"
#include "vfs/wal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace vfs {

namespace {

    using Clock = std::chrono::steady_clock;

    class JournalImpl final : public Journal {
        public:
            JournalImpl(BufferCache& cache, Wal& wal, BlockDevice& dev,
                        const Superblock& sb, const StorageOptions& opts)
                : cache_(cache), wal_(wal), dev_(dev), sb_(sb),
                  enabled_(opts.journal),
                  total_blocks_(sb.total_blocks),
                  max_txn_blocks_(cache.capacity() / 2 - MAX_OP_BLOCKS),
                  checkpoint_bytes_(opts.checkpoint_bytes) {
            }

            // The fast path: one compare-and-swap on the gate, no mutex, for
            // as long as no commit is wanted and the transaction has room.
            void begin_op() override {
                if (!enabled_)
                    return;
                uint64_t g = gate_.load();
                while (!(g & CLOSED) && fits(g)) {
                    if (gate_.compare_exchange_weak(g, g + 1))
                        return;
                }
                begin_op_slow();
            }

            // The fast path: one fetch_sub. Only the last operation out of a
            // closed gate, or of a transaction past its limit, takes the
            // mutex - to commit.
            void end_op() override {
                if (!enabled_)
                    return;
                ended_.fetch_add(1);  // before the gate: see run_commit's covered
                const uint64_t g = gate_.fetch_sub(1);
                if ((g & ~CLOSED) != 1)
                    return;
                if (!(g & CLOSED) && txn_count_.load() <= max_txn_blocks_)
                    return;
                end_op_slow();
            }

            void read(uint32_t block_no, Block& dst) override {
                check_block(block_no);
                cache_.read(block_no, dst);
            }

            void write(uint32_t block_no, bool fresh,
                       const std::function<void(Block&)>& fn) override {
                check_block(block_no);

                if (!enabled_) {
                    cache_.modify(block_no, fresh, false, fn);
                    kick_if_dirty();
                    return;
                }

                // The frame joins the transaction inside modify, before fn
                // changes a byte. Recording it here afterwards is safe: no
                // commit can start while this operation is running.
                bool joined = false;
                try {
                    cache_.modify(block_no, fresh, true, fn, &joined);
                } catch (...) {
                    if (joined)
                        add_to_txn(block_no);
                    throw;
                }
                if (joined)
                    add_to_txn(block_no);
            }

            void commit() override {
                if (!enabled_) {
                    // No log: durable means written back.
                    cache_.write_back_all();
                    dev_.sync();
                    return;
                }

                std::unique_lock<std::mutex> lk(m_);
                const uint64_t target = ended_.load();
                while (committed_through_ < target) {
                    rethrow_if_failed();
                    // run_commit commits now, or - if operations are running -
                    // closes the gate and leaves it to the last of them.
                    if (committing_ || !run_commit(lk))
                        cv_.wait(lk);
                }
            }

            // Commit, then checkpoint. Not inside an operation.
            void checkpoint() {
                if (!enabled_) {
                    commit();
                    return;
                }

                std::unique_lock<std::mutex> lk(m_);
                // A checkpoint already running when this is called counts:
                // nothing can run alongside one, so it covers every operation
                // that ended before now.
                const uint64_t target = checkpoints_done_ + 1;
                while (checkpoints_done_ < target) {
                    rethrow_if_failed();
                    if (committing_) {
                        cv_.wait(lk);
                        continue;
                    }
                    checkpoint_requested_ = true;
                    if (!run_commit(lk))
                        cv_.wait(lk);
                }
            }

            // For the flusher: commit the running transaction if it has been
            // open longer than age.
            void commit_if_older(Clock::duration age) {
                if (!enabled_)
                    return;
                std::unique_lock<std::mutex> lk(m_);
                if (failed_ || committing_ || txn_set_.empty() ||
                    Clock::now() - txn_started_ < age)
                    return;
                run_commit(lk);  // or, if operations are running, the last to end will
            }

            // At unmount, with nothing else running: checkpoint, then mark
            // the image clean.
            void close() {
                checkpoint();
                // Nothing else runs now, and checkpoint() returned through m_,
                // so every earlier change to sb_ is visible here.
                sb_.clean_unmount = 1;
                write_superblock(dev_, sb_);
                dev_.sync();
            }

            void set_kick(std::function<void()> kick) { kick_ = std::move(kick); }

            uint64_t commits() const { return commits_.load(); }
            uint64_t checkpoints() const { return checkpoints_.load(); }
            void reset_stats() {
                commits_.store(0);
                checkpoints_.store(0);
            }

        private:
            void check_block(uint32_t block_no) const {
                // Block 0 is written straight to the device by the journal
                // itself; a copy in the cache could only ever be stale.
                if (block_no == 0 || block_no >= total_blocks_)
                    throw std::out_of_range("journal: block " + std::to_string(block_no)
                                            + " is not a block it may touch");
            }

            void add_to_txn(uint32_t block_no) {
                std::lock_guard<std::mutex> lk(m_);
                if (txn_set_.insert(block_no).second) {
                    if (txn_blocks_.empty())
                        txn_started_ = Clock::now();
                    txn_blocks_.push_back(block_no);
                    txn_count_.store(txn_set_.size());
                }
            }

            // Whether one more operation may join the transaction, with
            // `running` already in it. Every running operation may still
            // write MAX_OP_BLOCKS, so each is counted at that.
            bool fits(uint64_t running) const {
                const uint64_t blocks = txn_count_.load();
                if (running == 0 && blocks == 0)
                    return true;
                return blocks + (running + 1) * MAX_OP_BLOCKS <= max_txn_blocks_;
            }

            void begin_op_slow() {
                std::unique_lock<std::mutex> lk(m_);
                while (true) {
                    rethrow_if_failed();
                    if (committing_) {
                        cv_.wait(lk);
                        continue;
                    }

                    // Under m_ nothing can close or open the gate but this
                    // thread, so what it reads here stays true.
                    uint64_t g = gate_.load();
                    if (!(g & CLOSED) && !fits(g & ~CLOSED)) {
                        // Full: the transaction closes, and no operation
                        // starts until it has committed.
                        commit_requested_ = true;
                        g = gate_.fetch_or(CLOSED) | CLOSED;
                    }
                    if (!(g & CLOSED)) {
                        gate_.fetch_add(1);
                        return;
                    }
                    if (!run_commit(lk))
                        cv_.wait(lk);
                }
            }

            void end_op_slow() {
                std::unique_lock<std::mutex> lk(m_);
                if (failed_ || committing_)
                    return;
                // Through an open gate another operation may have got in; it
                // will find the same thing when it ends.
                if ((gate_.load() & ~CLOSED) != 0)
                    return;
                if (commit_requested_ || checkpoint_requested_ ||
                    txn_set_.size() > max_txn_blocks_)
                    run_commit(lk);
            }

            void kick_if_dirty() {
                if (kick_ && cache_.dirty_count() > cache_.capacity() / 2)
                    kick_();
            }

            void rethrow_if_failed() {
                if (failed_)
                    std::rethrow_exception(failed_);
            }

            // Called holding m_, with no commit running. Closes the gate. If
            // no operation is running, commits (and checkpoints, if asked)
            // and returns true; otherwise marks a commit wanted and returns
            // false - the last operation to end runs it. Returns holding m_.
            bool run_commit(std::unique_lock<std::mutex>& lk) {
                const uint64_t g = gate_.fetch_or(CLOSED);
                if ((g & ~CLOSED) != 0) {
                    commit_requested_ = true;
                    return false;
                }

                committing_ = true;
                const bool checkpoint_wanted = checkpoint_requested_;
                commit_requested_ = false;
                checkpoint_requested_ = false;
                std::vector<uint32_t> blocks;
                blocks.swap(txn_blocks_);
                txn_set_.clear();
                txn_count_.store(0);
                // Every operation counts itself in ended_ before it leaves the
                // gate, and none can enter a closed one: with none running,
                // this is exactly the operations this commit holds.
                const uint64_t covered = ended_.load();
                lk.unlock();

                bool checkpointed = false;
                std::exception_ptr error;
                try {
                    if (!blocks.empty())
                        write_txn(blocks);
                    if (checkpoint_wanted || wal_.size_bytes() > checkpoint_bytes_) {
                        do_checkpoint();
                        checkpointed = true;
                    }
                } catch (...) {
                    error = std::current_exception();
                }

                if (!error)
                    kick_if_dirty();

                lk.lock();
                committing_ = false;
                if (error) {
                    // The transaction's frames are stuck in it and the log
                    // may end in a torn record. Nothing more can be made
                    // durable; recovery at the next mount sorts it out. The
                    // gate stays closed, so begin_op finds out.
                    failed_ = error;
                } else {
                    committed_through_ = covered;
                    if (checkpointed)
                        checkpoints_done_++;
                    // Nobody asks for a commit while one runs - they all wait
                    // for committing_ - so there is nothing left to wait for.
                    gate_.fetch_and(~CLOSED);
                }
                cv_.notify_all();
                if (error)
                    std::rethrow_exception(error);
                return true;
            }

            void write_txn(const std::vector<uint32_t>& blocks) {
                // The BEGIN record's lsn doubles as the transaction id: unique
                // for good, since lsns are never reused.
                // BEGIN and the BLOCK records go out in big batched writes;
                // the sync after them is the first barrier, so the COMMIT
                // still cannot reach the disk before them.
                const uint64_t txn = wal_.next_lsn();
                wal_.stage(WAL_BEGIN, txn, 0, nullptr);

                for (uint32_t b : blocks) {
                    wal_.stage_block(txn, b, [&](uint8_t* payload) {
                        cache_.copy_txn_frame(b, payload);
                    });
                }
                wal_.sync();

                const uint64_t commit_lsn = wal_.append(WAL_COMMIT, txn, 0, nullptr);
                wal_.sync();

                for (uint32_t b : blocks)
                    cache_.commit_frame(b, commit_lsn);
                commits_++;
            }

            void do_checkpoint() {
                cache_.write_back_all();
                dev_.sync();

                const uint64_t last = wal_.next_lsn() - 1;
                sb_.last_checkpoint_lsn = last;
                write_superblock(dev_, sb_);
                dev_.sync();

                wal_.reset(last + 1);
                checkpoints_++;
            }

            BufferCache& cache_;
            Wal& wal_;
            BlockDevice& dev_;
            Superblock sb_;  // the journal's own copy: the one written to block 0
            const bool enabled_;
            const uint32_t total_blocks_;
            const uint64_t max_txn_blocks_;
            const uint64_t checkpoint_bytes_;
            std::function<void()> kick_;

            // The gate operations pass through: CLOSED, plus how many are
            // running. Closed while a commit is wanted or running, or after
            // one failed - always changed under m_ - so that operations only
            // need m_ when they cannot get through.
            static constexpr uint64_t CLOSED = uint64_t{1} << 63;
            std::atomic<uint64_t> gate_{0};
            std::atomic<uint64_t> txn_count_{0};  // txn_set_.size(), for the fast path
            std::atomic<uint64_t> ended_{0};      // operations ended, ever

            std::mutex m_;
            std::condition_variable cv_;
            bool committing_ = false;
            bool commit_requested_ = false;
            bool checkpoint_requested_ = false;
            std::exception_ptr failed_;
            uint64_t committed_through_ = 0;    // the first this many are durable
            uint64_t checkpoints_done_ = 0;
            std::vector<uint32_t> txn_blocks_;  // the running transaction, in order
            std::unordered_set<uint32_t> txn_set_;
            Clock::time_point txn_started_{};

            std::atomic<uint64_t> commits_{0};
            std::atomic<uint64_t> checkpoints_{0};
    };

} // anonymous namespace

    struct Storage::Impl {
        PreadDevice raw;
        InstrumentedDevice dev;
        Superblock sb;
        Wal wal;
        RecoveryInfo recovery;
        std::unique_ptr<BufferCache> cache;
        std::unique_ptr<JournalImpl> journal;
        std::unique_ptr<Flusher> flusher;  // last: stopped before the rest goes
        bool closed = false;

        Impl(const char* disk_path, const char* wal_path, const StorageOptions& opts)
            : raw(PreadDevice::open(disk_path)),
              dev(raw, opts.disk_latency_us, opts.fsync),
              sb(read_superblock(dev)),
              wal(wal_path, opts.fsync) {
            if (sb.clean_unmount == 0) {
                recovery = recover(dev, wal, sb);
            } else {
                // A clean image owes the log nothing. Whatever is in it is
                // stale - perhaps from another image that used this path - and
                // must never be replayed.
                wal.reset(sb.last_checkpoint_lsn + 1);
            }

            // From now until close(), the image is in use: a crash leaves
            // clean_unmount == 0, and the next mount replays the log.
            sb.clean_unmount = 0;
            write_superblock(dev, sb);
            dev.sync();

            cache = std::make_unique<BufferCache>(
                dev, opts.cache_frames, [this] { return wal.durable_lsn(); });
            journal = std::make_unique<JournalImpl>(*cache, wal, dev, sb, opts);

            dev.reset_counts();
            wal.reset_stats();

            if (opts.flusher) {
                const auto interval = std::chrono::milliseconds(
                    std::max<uint32_t>(opts.commit_interval_ms, 1));
                const auto period = std::min(interval, std::chrono::milliseconds(200));
                flusher = std::make_unique<Flusher>(period, [this, interval] {
                    flush_tick(interval);
                });
                journal->set_kick([this] { flusher->kick(); });
                flusher->start();
            }
        }

        void flush_tick(std::chrono::milliseconds interval) {
            journal->commit_if_older(interval);

            // Keep three quarters of the cache clean, so eviction rarely has
            // to stop and write.
            const uint64_t dirty = cache->dirty_count();
            const uint64_t allowed = cache->capacity() / 4;
            if (dirty > allowed)
                cache->write_back(static_cast<size_t>(dirty - allowed));
        }
    };

    Storage::Storage(const char* disk_path, const char* wal_path, const StorageOptions& opts)
        : impl_(std::make_unique<Impl>(disk_path, wal_path, opts)) {
    }

    void Storage::close() {
        if (!impl_ || impl_->closed)
            return;
        impl_->closed = true;
        if (impl_->flusher)
            impl_->flusher->stop();
        impl_->journal->close();
    }

    Storage::~Storage() {
        try {
            close();
        } catch (...) {
            // The image stays marked unclean, so the next mount recovers it.
        }
    }

    const Superblock& Storage::superblock() const {
        return impl_->sb;
    }

    Journal& Storage::journal() {
        return *impl_->journal;
    }

    BlockDevice& Storage::device() {
        return impl_->dev;
    }

    void Storage::checkpoint() {
        impl_->journal->checkpoint();
    }

    RecoveryInfo Storage::recovery() const {
        return impl_->recovery;
    }

    StorageStats Storage::stats() const {
        StorageStats s;
        s.disk_reads  = impl_->dev.reads();
        s.disk_writes = impl_->dev.writes();
        const CacheStats c = impl_->cache->stats();
        s.cache_hits   = c.hits;
        s.cache_misses = c.misses;
        s.evictions    = c.evictions;
        s.log_bytes    = impl_->wal.bytes_appended();
        s.commits      = impl_->journal->commits();
        s.checkpoints  = impl_->journal->checkpoints();
        return s;
    }

    void Storage::reset_stats() {
        impl_->dev.reset_counts();
        impl_->cache->reset_stats();
        impl_->wal.reset_stats();
        impl_->journal->reset_stats();
    }

} // namespace vfs
