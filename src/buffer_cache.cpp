#include "vfs/buffer_cache.h"

#include <algorithm>
#include <cstring>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

// Locking. A shard's mutex guards its map, its LRU list and every frame's
// bookkeeping (pins, dirty, in_txn, lsn, version). A frame's own lock guards
// only its bytes. The two nest one way only: shard, then frame - and only for
// a frame with no pins, whose lock nobody can be holding, so that never
// waits. Everything else drops the shard before touching the bytes, and drops
// the bytes before taking the shard back.
//
// A pin means "a thread is using this frame": it cannot be evicted or reused
// until the pin is dropped. Everyone pins before taking a frame's lock.

namespace vfs {

    struct BufferCache::Frame {
        // Guarded by the shard's mutex.
        uint32_t block_no = 0;
        bool     mapped   = false;    // in the shard's map, holding block_no
        bool     dirty    = false;
        bool     in_txn   = false;
        uint64_t lsn      = 0;        // the commit that last changed it
        uint64_t version  = 0;        // +1 on every change
        int      pins     = 0;
        Frame*   prev     = nullptr;  // LRU list: most recently used at the head
        Frame*   next     = nullptr;

        // Guarded by lock.
        std::shared_mutex lock;
        bool  load_failed = false;
        Block data{};
    };

    struct BufferCache::Shard {
        std::mutex m;
        std::unordered_map<uint32_t, Frame*> map;
        Frame* head = nullptr;
        Frame* tail = nullptr;
        std::vector<std::unique_ptr<Frame>> frames;
        size_t share = 0;             // frames this shard may have
    };

    BufferCache::BufferCache(
        BlockDevice& dev,
        uint32_t frames,
        std::function<uint64_t()> durable_lsn
    )
        : dev_(dev),
        capacity_(frames < MIN_FRAMES ? MIN_FRAMES : frames),
        durable_lsn_(std::move(durable_lsn)),
        shards_(new Shard[SHARDS]),
        run_buf_(std::make_unique<uint8_t[]>(size_t{RUN_BLOCKS} * BLOCK_SIZE)) {
        for (uint32_t i = 0; i < SHARDS; i++)
            shards_[i].share = capacity_ / SHARDS + (i < capacity_ % SHARDS ? 1 : 0);
    }

    BufferCache::~BufferCache() = default;

    BufferCache::Shard& BufferCache::shard_for(uint32_t block_no) {
        return shards_[block_no % SHARDS];
    }

    void BufferCache::lru_unlink(Shard& s, Frame* f) {
        if (f->prev) f->prev->next = f->next; else s.head = f->next;
        if (f->next) f->next->prev = f->prev; else s.tail = f->prev;
        f->prev = f->next = nullptr;
    }

    void BufferCache::lru_push_front(Shard& s, Frame* f) {
        f->prev = nullptr;
        f->next = s.head;
        if (s.head) s.head->prev = f; else s.tail = f;
        s.head = f;
    }

    void BufferCache::lru_push_back(Shard& s, Frame* f) {
        f->next = nullptr;
        f->prev = s.tail;
        if (s.tail) s.tail->next = f; else s.head = f;
        s.tail = f;
    }

    BufferCache::Frame* BufferCache::new_frame(Shard& s) {
        s.frames.push_back(std::make_unique<Frame>());
        Frame* f = s.frames.back().get();
        lru_push_front(s, f);
        return f;
    }

    // A frame for a block not in the cache, taken with the shard locked. It
    // comes back unmapped and unpinned.
    BufferCache::Frame* BufferCache::take_frame(Shard& s) {
        if (s.frames.size() < s.share)
            return new_frame(s);

        // The least recently used frame that nobody is using and no
        // transaction holds. Unmapped frames sit at the tail, so they go
        // first; then the oldest clean one; a dirty one only when there is no
        // clean one, since it has to be written first.
        const uint64_t durable = durable_lsn_();
        Frame* victim = nullptr;
        Frame* dirty_victim = nullptr;
        for (Frame* f = s.tail; f; f = f->prev) {
            if (f->pins > 0 || f->in_txn)
                continue;
            if (!f->mapped || !f->dirty) {
                victim = f;
                break;
            }
            if (!dirty_victim && f->lsn <= durable)
                dirty_victim = f;
        }

        if (!victim && dirty_victim) {
            // Written back with the shard held: simple, and rare, because
            // the flusher keeps clean frames around for exactly this.
            {
                std::shared_lock<std::shared_mutex> bytes(dirty_victim->lock);
                dev_.write_block(dirty_victim->block_no, dirty_victim->data);
            }
            dirty_victim->dirty = false;
            dirty_count_--;
            victim = dirty_victim;
        }

        if (!victim) {
            // Every frame here is pinned or in the running transaction.
            // Stretching past the share beats failing the operation; the
            // transaction size limit bounds how far this can go.
            return new_frame(s);
        }

        if (victim->mapped) {
            s.map.erase(victim->block_no);
            victim->mapped = false;
            evictions_++;
        }
        return victim;
    }

    // Finds or loads block_no and pins it. If this call loaded it, the frame
    // comes back with its lock held exclusively (*exclusive = true), so the
    // caller sees the bytes before anyone else can. join_txn marks the frame
    // as in the running transaction - before anyone can change its bytes -
    // and *joined says whether it was not already.
    BufferCache::Frame* BufferCache::acquire(
        Shard& s,
        uint32_t block_no,
        bool zero_fill,
        bool join_txn,
        bool* exclusive,
        bool* joined
    ) {
        std::unique_lock<std::mutex> lk(s.m);

        auto it = s.map.find(block_no);
        if (it != s.map.end()) {
            Frame* f = it->second;
            f->pins++;
            lru_unlink(s, f);
            lru_push_front(s, f);
            if (join_txn) {
                if (joined) *joined = !f->in_txn;
                f->in_txn = true;
            }
            hits_++;
            *exclusive = false;
            return f;
        }

        misses_++;
        Frame* f = take_frame(s);
        f->block_no = block_no;
        f->mapped   = true;
        f->dirty    = false;
        f->in_txn   = join_txn;
        f->lsn      = 0;
        f->version  = 0;
        f->pins     = 1;
        if (joined) *joined = join_txn;
        s.map.emplace(block_no, f);
        lru_unlink(s, f);
        lru_push_front(s, f);

        // Unpinned until a moment ago, so nobody holds this lock and it never
        // waits. Taking it before letting go of the shard means anyone who
        // finds the frame from now on waits for its bytes to arrive.
        f->lock.lock();
        lk.unlock();

        f->load_failed = false;
        if (zero_fill) {
            f->data.fill(0);
        } else {
            try {
                dev_.read_block(block_no, f->data);
            } catch (...) {
                f->load_failed = true;
                f->lock.unlock();
                lk.lock();
                s.map.erase(block_no);
                f->mapped = false;
                f->in_txn = false;
                f->pins--;
                lru_unlink(s, f);
                lru_push_back(s, f);
                throw;
            }
        }

        *exclusive = true;
        return f;
    }

    void BufferCache::unpin(Shard& s, Frame* f) {
        std::lock_guard<std::mutex> lk(s.m);
        f->pins--;
    }

    void BufferCache::read(uint32_t block_no, Block& dst) {
        Shard& s = shard_for(block_no);
        bool exclusive = false;
        Frame* f = acquire(s, block_no, false, false, &exclusive, nullptr);

        bool failed = false;
        if (exclusive) {
            dst = f->data;
            f->lock.unlock();
        } else {
            std::shared_lock<std::shared_mutex> bytes(f->lock);
            failed = f->load_failed;
            if (!failed)
                dst = f->data;
        }
        unpin(s, f);

        if (failed)
            throw std::runtime_error("cache: block " + std::to_string(block_no)
                                     + " could not be read");
    }

    void BufferCache::modify(
        uint32_t block_no,
        bool fresh,
        bool in_txn,
        const std::function<void(Block&)>& fn,
        bool* joined
    ) {
        Shard& s = shard_for(block_no);
        bool exclusive = false;
        bool joined_now = false;
        Frame* f = acquire(s, block_no, fresh, in_txn, &exclusive, &joined_now);
        if (!exclusive)
            f->lock.lock();

        if (f->load_failed) {
            f->lock.unlock();
            unpin(s, f);
            throw std::runtime_error("cache: block " + std::to_string(block_no)
                                     + " could not be read");
        }
        if (joined)
            *joined = joined_now;

        if (fresh && !exclusive)
            f->data.fill(0);

        try {
            fn(f->data);
        } catch (...) {
            f->lock.unlock();
            finish_modify(s, f, in_txn);
            throw;
        }
        f->lock.unlock();
        finish_modify(s, f, in_txn);
    }

    void BufferCache::finish_modify(Shard& s, Frame* f, bool in_txn) {
        std::lock_guard<std::mutex> lk(s.m);
        f->version++;
        // A change in a transaction becomes dirty when it commits; one
        // outside a transaction is dirty now.
        if (!in_txn && !f->dirty) {
            f->dirty = true;
            dirty_count_++;
        }
        f->pins--;
    }

    void BufferCache::copy_txn_frame(uint32_t block_no, uint8_t* dst) {
        Shard& s = shard_for(block_no);
        Frame* f;
        {
            std::lock_guard<std::mutex> lk(s.m);
            auto it = s.map.find(block_no);
            if (it == s.map.end() || !it->second->in_txn)
                throw std::logic_error("cache: block " + std::to_string(block_no)
                                       + " is not in the running transaction");
            f = it->second;
            f->pins++;
        }
        {
            std::shared_lock<std::shared_mutex> bytes(f->lock);
            std::memcpy(dst, f->data.data(), BLOCK_SIZE);
        }
        unpin(s, f);
    }

    void BufferCache::commit_frame(uint32_t block_no, uint64_t lsn) {
        Shard& s = shard_for(block_no);
        std::lock_guard<std::mutex> lk(s.m);
        auto it = s.map.find(block_no);
        if (it == s.map.end() || !it->second->in_txn)
            throw std::logic_error("cache: block " + std::to_string(block_no)
                                   + " is not in the running transaction");
        Frame* f = it->second;
        f->in_txn = false;
        f->lsn = lsn;
        f->version++;
        if (!f->dirty) {
            f->dirty = true;
            dirty_count_++;
        }
    }

    size_t BufferCache::write_back(size_t max) {
        std::lock_guard<std::mutex> one_pass(write_back_mutex_);

        struct Pick {
            Frame*   f;
            Shard*   s;
            uint32_t block_no;  // fixed while pinned: a pinned frame is never reused
            uint64_t version;
        };

        // Up to max frames that may go, least recently used first in each
        // shard, pinned so they stay put while they are written.
        std::vector<Pick> picks;
        for (uint32_t i = 0; i < SHARDS && picks.size() < max; i++) {
            Shard& s = shards_[i];
            std::lock_guard<std::mutex> lk(s.m);
            const uint64_t durable = durable_lsn_();
            for (Frame* f = s.tail; f && picks.size() < max; f = f->prev) {
                if (f->mapped && f->dirty && !f->in_txn && f->lsn <= durable) {
                    f->pins++;
                    picks.push_back({f, &s, f->block_no, f->version});
                }
            }
        }

        // In block order, so neighbours on disk - which consecutive blocks of
        // a file usually are - go out in one write instead of one each.
        std::sort(picks.begin(), picks.end(),
                  [](const Pick& a, const Pick& b) { return a.block_no < b.block_no; });

        size_t written = 0;
        size_t next = 0;        // the first pick not yet dealt with
        size_t run_start = 0;   // picks [run_start, next) are copied into run_buf_
        uint32_t run_len = 0;

        auto unpin_picks = [&](size_t from, size_t to) {
            for (size_t k = from; k < to; k++) {
                std::lock_guard<std::mutex> lk(picks[k].s->m);
                picks[k].f->pins--;
            }
        };

        // Writes the run, then marks clean every frame in it that has not
        // changed since it was picked.
        auto flush_run = [&] {
            if (run_len == 0)
                return;
            dev_.write_run(picks[run_start].block_no, run_buf_.get(), run_len);
            written += run_len;
            for (size_t k = run_start; k < run_start + run_len; k++) {
                Pick& p = picks[k];
                std::lock_guard<std::mutex> lk(p.s->m);
                if (p.f->version == p.version && p.f->dirty) {
                    p.f->dirty = false;
                    dirty_count_--;
                }
                p.f->pins--;
            }
            run_len = 0;
        };

        try {
            while (next < picks.size()) {
                Pick& p = picks[next];
                const bool joins_run = run_len > 0 && run_len < RUN_BLOCKS &&
                    p.block_no == picks[run_start].block_no + run_len;
                if (!joins_run) {
                    flush_run();
                    run_start = next;
                }

                {
                    std::shared_lock<std::shared_mutex> bytes(p.f->lock);
                    std::memcpy(run_buf_.get() + size_t{run_len} * BLOCK_SIZE,
                                p.f->data.data(), BLOCK_SIZE);
                }

                // Checked again now the bytes are copied. If a transaction
                // took the frame in the meantime, the copy may hold its
                // uncommitted change and must not reach the disk. A change
                // that committed, or was made outside a transaction, is fine
                // to write; it just means the frame stays dirty afterwards.
                bool may_write;
                {
                    std::lock_guard<std::mutex> lk(p.s->m);
                    may_write = !p.f->in_txn && p.f->lsn <= durable_lsn_();
                }
                next++;

                if (may_write) {
                    run_len++;
                } else {
                    // Left out, so the run cannot carry on past it. Its pin
                    // goes only after the run is written: if that throws, the
                    // catch below still owns it.
                    flush_run();
                    {
                        std::lock_guard<std::mutex> lk(p.s->m);
                        p.f->pins--;
                    }
                    run_start = next;
                }
            }
            flush_run();
        } catch (...) {
            unpin_picks(run_start, picks.size());
            throw;
        }
        return written;
    }

    bool BufferCache::contains(uint32_t block_no) {
        Shard& s = shard_for(block_no);
        std::lock_guard<std::mutex> lk(s.m);
        return s.map.count(block_no) != 0;
    }

    CacheStats BufferCache::stats() const {
        CacheStats c;
        c.hits      = hits_.load();
        c.misses    = misses_.load();
        c.evictions = evictions_.load();
        return c;
    }

    void BufferCache::reset_stats() {
        hits_.store(0);
        misses_.store(0);
        evictions_.store(0);
    }

} // namespace vfs
