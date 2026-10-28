#include "vfs/lock_manager.h"

namespace vfs {

    LockManager::Entry* LockManager::acquire(uint32_t id) {
        std::lock_guard<std::mutex> lk(map_mutex_);
        Entry& e = entries_[id];
        e.refs++;
        return &e;
    }

    void LockManager::release(uint32_t id) {
        std::lock_guard<std::mutex> lk(map_mutex_);
        auto it = entries_.find(id);
        if (it != entries_.end() && --it->second.refs == 0)
            entries_.erase(it);
    }

    size_t LockManager::size() const {
        std::lock_guard<std::mutex> lk(map_mutex_);
        return entries_.size();
    }

    // The reference is taken first and the lock second, never under the map
    // mutex: blocking on an inode lock while holding the map would stall every
    // other thread's lookups.
    InodeLock::InodeLock(LockManager& lm, uint32_t id, bool exclusive)
        : lm_(&lm), id_(id), entry_(lm.acquire(id)), exclusive_(exclusive) {
        if (exclusive_)
            entry_->lock.lock();
        else
            entry_->lock.lock_shared();
    }

    InodeLock::~InodeLock() {
        drop();
    }

    InodeLock::InodeLock(InodeLock&& other) noexcept
        : lm_(other.lm_), id_(other.id_), entry_(other.entry_),
        exclusive_(other.exclusive_) {
        other.entry_ = nullptr;
    }

    InodeLock& InodeLock::operator=(InodeLock&& other) noexcept {
        if (this != &other) {
            drop();
            lm_ = other.lm_;
            id_ = other.id_;
            entry_ = other.entry_;
            exclusive_ = other.exclusive_;
            other.entry_ = nullptr;
        }
        return *this;
    }

    void InodeLock::drop() noexcept {
        if (entry_ == nullptr)
            return;
        if (exclusive_)
            entry_->lock.unlock();
        else
            entry_->lock.unlock_shared();
        lm_->release(id_);
        entry_ = nullptr;
    }

} // namespace vfs
