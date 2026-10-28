#pragma once

// recovery.h — crash recovery (stage 8), run at mount when the last unmount
// was not clean.

#include "vfs/block_device.h"
#include "vfs/layout.h"
#include "vfs/storage.h"
#include "vfs/wal.h"

namespace vfs {

    // Redo-only. Writes to dev, in log order, the blocks of every transaction
    // that has a COMMIT record and came after the last checkpoint; a
    // transaction without one is ignored, which is all an unfinished one
    // needs, because no-steal kept its changes out of disk.img. Then syncs
    // dev, records the new checkpoint lsn in sb and in block 0, and empties
    // the log. Safe to run again if it is interrupted: putting back a whole
    // block gives the same result every time.
    RecoveryInfo recover(BlockDevice& dev, Wal& wal, Superblock& sb);

} // namespace vfs
