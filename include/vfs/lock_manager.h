#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

#include "vfs/layout.h"

namespace vfs {

    // One reader-writer lock per inode being used, created on first use and
    // dropped when the last holder lets go - so memory follows the number of
    // inodes in use right now, not the number on the disk.
    class LockManager {
        public:
            struct Entry {
                std::shared_mutex lock;
                std::atomic<uint32_t> open_count{0}; // open file descriptors on this inode
                uint32_t refs = 0;                   // guarded by map_mutex_

                // A file's inode, kept by vfs_write so that rewriting a file
                // does not read the inode table block it shares with 31 other
                // inodes - a block every writer would otherwise queue for.
                // Guarded by lock: set only with it held exclusive, read with
                // it held either way. Whenever inode_valid is true, inode is
                // exactly what the cache holds; anything that rewrites or
                // frees the inode some other way clears inode_valid first.
                Inode inode{};
                bool inode_valid = false;
            };

            // Takes a reference to inode id's entry, creating it if needed.
            // Does not lock it. Every acquire needs one release.
            Entry* acquire(uint32_t id);
            void release(uint32_t id);

            size_t size() const;

        private:
            mutable std::mutex map_mutex_;
            // Node-based, so an Entry never moves while someone points at it.
            std::unordered_map<uint32_t, Entry> entries_;
    };

    // Holds one inode's lock, shared or exclusive, for as long as it lives.
    // Moving it hands the lock over without releasing it, which is what
    // hand-over-hand path lookup needs.
    class InodeLock {
        public:
            InodeLock(LockManager& lm, uint32_t id, bool exclusive);
            ~InodeLock();

            InodeLock(InodeLock&& other) noexcept;
            InodeLock& operator=(InodeLock&& other) noexcept;
            InodeLock(const InodeLock&) = delete;
            InodeLock& operator=(const InodeLock&) = delete;

            uint32_t id() const { return id_; }
            LockManager::Entry& entry() { return *entry_; }

        private:
            void drop() noexcept;

            LockManager* lm_;
            uint32_t id_;
            LockManager::Entry* entry_;
            bool exclusive_;
    };

} // namespace vfs
