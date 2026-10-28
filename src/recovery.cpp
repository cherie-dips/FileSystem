#include "vfs/recovery.h"
#include "vfs/superblock.h"

#include <algorithm>
#include <unordered_set>
#include <vector>

namespace vfs {

    RecoveryInfo recover(BlockDevice& dev, Wal& wal, Superblock& sb) {
        RecoveryInfo info;
        info.ran = true;

        // The scan stops at the first torn or corrupt record, so everything
        // it returns was written whole - and a COMMIT among them means every
        // record of that transaction before it was too.
        const std::vector<WalRecord> records = wal.scan(sb.total_blocks);

        std::unordered_set<uint64_t> committed;
        for (const WalRecord& r : records) {
            if (r.header.type == WAL_COMMIT)
                committed.insert(r.header.txn_id);
        }

        uint64_t last_lsn = sb.last_checkpoint_lsn;
        for (const WalRecord& r : records) {
            const WalRecordHeader& h = r.header;
            last_lsn = std::max(last_lsn, h.lsn);

            // At or before the checkpoint: already in disk.img. Such records
            // are only here if the crash came between the checkpoint's
            // superblock write and its emptying of the log.
            if (h.lsn <= sb.last_checkpoint_lsn)
                continue;

            const bool done = committed.count(h.txn_id) != 0;
            if (h.type == WAL_BEGIN)
                (done ? info.txns_replayed : info.txns_discarded)++;
            if (h.type == WAL_BLOCK && done) {
                dev.write_block(h.block_no, r.payload);
                info.blocks_replayed++;
            }
        }
        dev.sync();

        // Say everything up to last_lsn is in disk.img before emptying the
        // log, so a crash in between costs only a replay of records already
        // applied.
        sb.last_checkpoint_lsn = last_lsn;
        write_superblock(dev, sb);
        dev.sync();
        wal.reset(last_lsn + 1);

        return info;
    }

} // namespace vfs
