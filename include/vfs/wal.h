#pragma once

// wal.h — the write-ahead log (stage 7): a file of checksummed records that
// describes every change before disk.img sees it.
//
// A transaction is a BEGIN record, one BLOCK record per block it changed -
// carrying that block's full new contents - and a COMMIT record. Logging whole
// blocks wastes space, but replaying one is the same however many times it
// happens, which is what makes recovery simple (OSTEP 42.3).

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <type_traits>
#include <vector>

#include "vfs/layout.h"

namespace vfs {

    constexpr uint32_t WAL_MAGIC = 0x524C4157;  // reads "WALR" in a hex dump

    // WalRecordHeader::type
    constexpr uint16_t WAL_BEGIN  = 1;
    constexpr uint16_t WAL_BLOCK  = 2;
    constexpr uint16_t WAL_COMMIT = 3;

    struct WalRecordHeader {
        uint32_t magic;        // WAL_MAGIC: where a record starts
        uint16_t type;
        uint16_t reserved;     // 0
        uint32_t block_no;     // WAL_BLOCK: the block it is about; else 0
        uint32_t payload_len;  // BLOCK_SIZE for WAL_BLOCK, else 0
        uint64_t lsn;          // +1 per record, never reused
        uint64_t txn_id;       // the lsn of the transaction's BEGIN record
        uint32_t checksum;     // crc32 of this header (checksum = 0), then the payload
        uint32_t pad;          // 0
    };

    static_assert(sizeof(WalRecordHeader) == 40, "WalRecordHeader changed size");
    static_assert(std::has_unique_object_representations_v<WalRecordHeader>,
                  "WalRecordHeader must not contain padding");

    struct WalRecord {
        WalRecordHeader header;
        Block payload;         // WAL_BLOCK only
    };

    class Wal {
        public:
            // Opens the log, creating an empty one if there is none.
            // sync_enabled = false makes sync() skip the fsync.
            Wal(const char* path, bool sync_enabled);
            ~Wal();

            Wal(const Wal&) = delete;
            Wal& operator=(const Wal&) = delete;

            // Every intact record from the start of the log, in order. Stops
            // at the first one that is cut short, has a bad magic, checksum,
            // type or payload length, names a block outside [1, total_blocks)
            // (total_blocks = 0 skips that check), or whose lsn is not one
            // more than the record before it. Whatever follows a crash is
            // dropped there.
            std::vector<WalRecord> scan(uint32_t total_blocks) const;

            // Appends one record, in a single write, and returns its lsn.
            // payload is required for WAL_BLOCK and forbidden otherwise. Not
            // durable until sync().
            uint64_t append(uint16_t type, uint64_t txn_id, uint32_t block_no,
                            const Block* payload);

            // As append(), but the record waits in a buffer and goes to the
            // file with the ones after it, in writes of up to BATCH_BYTES - a
            // commit's thousands of BLOCK records become a handful of writes.
            // The bytes in the file are exactly the ones append() would have
            // written. flush() writes out what is waiting; append() and sync()
            // flush first, so records always reach the file in lsn order.
            static constexpr size_t BATCH_BYTES = 1u << 20;
            uint64_t stage(uint16_t type, uint64_t txn_id, uint32_t block_no,
                           const Block* payload);
            // A BLOCK record whose payload fill writes straight into the
            // batch - for a commit, copying a cache frame - so a logged block
            // is copied once, not twice.
            uint64_t stage_block(uint64_t txn_id, uint32_t block_no,
                                 const std::function<void(uint8_t*)>& fill);
            void flush();

            // Forces everything appended so far to disk.
            void sync();

            // Empties the log. The next record gets lsn next_lsn.
            void reset(uint64_t next_lsn);

            uint64_t next_lsn() const { return next_lsn_; }
            // Highest lsn known to be on disk. Safe from any thread.
            uint64_t durable_lsn() const { return durable_lsn_.load(); }
            uint64_t size_bytes() const { return end_; }

            uint64_t bytes_appended() const { return bytes_appended_.load(); }
            void reset_stats() { bytes_appended_.store(0); }

        private:
            // Lays a record out at the end of the batch - fill writes the
            // payload, if it has one - and returns its lsn.
            uint64_t encode(uint16_t type, uint64_t txn_id, uint32_t block_no,
                            bool has_payload, const std::function<void(uint8_t*)>& fill);
            uint64_t stage_record(uint16_t type, uint64_t txn_id, uint32_t block_no,
                                  bool has_payload, const std::function<void(uint8_t*)>& fill);
            void write_out(const uint8_t* bytes, size_t len);

            int fd_ = -1;
            const bool sync_enabled_;
            uint64_t end_ = 0;       // where the next write goes
            uint64_t next_lsn_ = 1;
            std::atomic<uint64_t> durable_lsn_{0};
            std::atomic<uint64_t> bytes_appended_{0};
            // Records waiting to be written, back to back: batch_len_ bytes
            // of room for BATCH_BYTES plus one more record.
            std::unique_ptr<uint8_t[]> batch_;
            size_t batch_len_ = 0;
    };

} // namespace vfs
