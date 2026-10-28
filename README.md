# ConcurrentFS

A file system written from scratch in C++17 that lives inside one ordinary file. It has its
own inodes, free-space bitmaps, directories, block cache, write-ahead log and crash
recovery. Many threads can use it at once, and it survives being killed in the middle of a
write.

Nothing wraps the host file system. The only calls it makes to the OS are `pread`, `pwrite`
and `fsync`, on two files: the disk image and its log.

Built stage by stage from *Operating Systems: Three Easy Pieces* (OSTEP).

---

## Build and test

Needs a C++17 compiler and `make`; on macOS, `xcode-select --install`. Nothing else.

```
make                  # build                         -> build/release/
make test             # run all 11 test programs
make MODE=asan test   # the same under AddressSanitizer + UBSan
make MODE=tsan test   # the same under ThreadSanitizer
make bench            # benchmarks
```

## Use it

```
$ vfs_shell mkfs disk.img 256M
$ vfs_shell mkdir disk.img -p /notes/2026
$ vfs_shell write disk.img /notes/2026/todo.txt finish the file system
$ vfs_shell ls disk.img /notes/2026
todo.txt
$ vfs_shell cat disk.img /notes/2026/todo.txt
finish the file system
$ vfs_shell fsck disk.img
disk.img: clean (files 1, directories 3, inodes in use 4, blocks in use 136)
```

Commands: `mkfs dump mkdir ls cat write rm sync fsck` — run `vfs_shell` for their arguments.
The log of `disk.img` is `disk.img.log`.

From C++, include `vfs/vfs.h`. Every call returns `>= 0`, or `-VFS_Exxx` (`errors.h`), and
is safe from any number of threads:

```cpp
vfs_mount("disk.img", "disk.img.log");
int fd = vfs_open("/hello.txt", VFS_O_CREAT | VFS_O_WRONLY);
vfs_write(fd, "hi\n", 3);
vfs_fsync(fd);              // durable from here on
vfs_close(fd);
vfs_unmount();
```

---

## The pipeline

```
   your program  /  vfs_shell
         │  vfs_open  vfs_read  vfs_write  vfs_mkdir  vfs_unlink  vfs_fsync ...
         ▼
 ┌─ API ──────────── open file table, per-inode reader/writer locks     vfs.cpp, lock_manager.cpp
 ├─ File system ──── path → directory → inode → block map → allocator   path, directory, inode_table,
 │                                                                       block_map, alloc
 ├─ Journal ──────── groups operations into transactions                storage.cpp
 ├─ Block cache ──── 4 KB frames, 16 shards, LRU, dirty bits, no-steal  buffer_cache.cpp
 ├─ Write-ahead log  disk.img.log: BEGIN, full block images, COMMIT     wal.cpp, crc32.cpp
 └─ Disk ─────────── disk.img, read and written 4 KB at a time          block_device.cpp

   + a flusher thread: commits every second and writes dirty blocks back   flusher.cpp
```

Each layer talks only to the one below it, and exactly one file (`block_device.cpp`)
touches `disk.img`.

### A write, start to finish

1. `vfs_write` cuts the data into chunks of up to 16 blocks. Each chunk is one operation.
2. It locks the file exclusively, maps each file offset to a disk block, and allocates
   blocks (and indirect blocks) from the bitmap where there are none yet.
3. Every block it changes is changed in the cache, joins the running transaction, and is
   pinned there. It cannot reach `disk.img` yet.
4. **Commit** — on `vfs_fsync`, every second, or when the transaction is full: full copies
   of all its blocks go to the log, then `fsync`, then a COMMIT record, then `fsync` again.
   Operations running at the same time share one commit.
5. **Write-back** — the flusher, cache eviction or a checkpoint copies blocks to
   `disk.img`, but never a block whose COMMIT is not yet safely in the log.
6. **Checkpoint** — when the log passes 32 MB, on `vfs_sync` and on unmount: write
   everything back, sync, record it in the superblock, empty the log.

### Mounting after a crash

The superblock says the last unmount was not clean, so before anything else:

1. read the log from the start, stopping at the first torn or corrupt record (CRC32);
2. write every block of every *committed* transaction to `disk.img`, in order;
3. sync, and empty the log.

Transactions without a COMMIT are dropped. Their blocks never reached `disk.img` (step 3
above), so there is nothing to undo: recovery is redo-only.

---

## On disk

`disk.img`, at 256 MB:

```
 block   0           1           2-3          4-131            132-65535
      ┌───────────┬───────────┬────────────┬───────────────┬─────────────────────┐
      │ super-    │ inode     │ data       │ inode table   │ data blocks         │
      │ block     │ bitmap    │ bitmap     │ 4096 inodes   │ (root dir in 132)   │
      └───────────┴───────────┴────────────┴───────────────┴─────────────────────┘
```

- **Superblock** — magic `CFS1`, where each region starts, a clean-unmount flag, and the
  log sequence number of the last checkpoint. Region sizes are worked out from the disk
  size.
- **Inode**, 128 bytes — type, link count, size, 12 direct block pointers, one
  single-indirect and one double-indirect: files up to about 4 GB.
- **Directory entry**, 64 bytes — inode number, type, a name of up to 58 bytes. The root is
  inode 1; an entry holding inode 0 is an empty slot.

`disk.img.log` — a sequence of records, each a 40-byte header (magic `WALR`, type BEGIN /
BLOCK / COMMIT, block number, sequence number, transaction id, CRC32), followed, for a
BLOCK record, by the block's full new 4 KB.

The exact rules a consistent image follows are written at the bottom of
`include/vfs/layout.h`; `fsck` checks every one.

---

## Concurrency

- **One reader/writer lock per inode**, made when needed and dropped when unused. Reads of
  a file share it, writes take it alone; two different files never wait for each other.
- **Path lookup is hand-over-hand**, top-down: lock a directory, find the name, lock the
  child, let go of the directory.
- **One fixed lock order**, which rules out deadlock:
  `descriptor → begin_op → inode locks (parent before child) → allocator → cache → journal`.
  No inode lock is ever held across a commit or a disk write.
- **Group commit**: every operation running when a commit starts goes into it, so sixteen
  threads calling `fsync` share one pair of log syncs.
- **The flusher** is OSTEP's producer/consumer: writers make dirty blocks, and it waits on a
  condition variable with a timeout and writes them back.

---

## Tests

| program | proves |
|---|---|
| `disktest` | blocks reach the file and survive the process exiting |
| `mkfstest` | bitmaps, the layout, and that a fresh image holds exactly what it should |
| `inodetest` | block mapping at every boundary (blocks 11/12, 1035/1036); one byte at offset 5,000,000 |
| `dirtest` | directories and their errors; a cold `/foo/bar` lookup costs 3 disk reads, a warm one 0 (OSTEP Fig. 40.3) |
| `vfstest` | a 10 MB file read back byte for byte; 4,000 random operations checked against an in-memory model |
| `cachetest` | LRU, write-back, no-steal, the write-ahead rule |
| `waltest` | every log record reads back in order; torn and corrupt records are cut off |
| `journaltest` | killed at every write of a workload: each transaction is all or nothing |
| `fscktest` | `fsck` names each of 24 kinds of damage |
| `crashtest` | **killed at 500 points (100 of them during recovery too): `fsck` clean every time, and the files are exactly what some prefix of the operations left** |
| `concurrenttest` | **16 threads, 5 s (`--seconds 60` for the long run): no lost or mixed-up data, zero ThreadSanitizer reports** |

---

## Benchmarks

`make bench`, on an Apple Silicon MacBook, one run; expect ±20% between runs. `fsync` is
off unless a row says otherwise. "Disk latency" is a sleep added to every disk write call,
to imitate a slow disk.

**The cache absorbs writes.** 16 MB written sequentially, disk latency 200 µs:

| cache | write | write + sync |
|---|---|---|
| 1 MB | 68 MB/s | 66 MB/s |
| 64 MB | 3,256 MB/s | 917 MB/s |

With a big cache, writes return at memory speed and the cost moves to `vfs_sync`, where
neighbouring blocks go out together in one write.

**The hit rate is what theory predicts.** Random 4 KB reads over a 32 MB file:

| cache | disk reads per read | predicted (1 − cache / file) | reads/s |
|---|---|---|---|
| 1 MB | 0.969 | 0.969 | 14,877 |
| 4 MB | 0.877 | 0.875 | 16,050 |
| 16 MB | 0.500 | 0.500 | 28,160 |
| 64 MB | 0.000 | 0.000 | 1,812,340 |

**Threads.** 4 KB writes, 64 MB cache, operations per second:

| threads | different files | same file | different files, `fsync` after each (fsync on) |
|---|---|---|---|
| 1 | 3.85 M | 3.75 M | 131 |
| 4 | 5.05 M | 0.95 M | 378 |
| 16 | 2.94 M | 0.16 M | 661 |

Different files share no lock, so throughput holds as threads are added (this machine has 4
fast cores). One file takes one writer at a time, so it does not scale, by design. Durable
writes scale 5×, because one commit serves many threads' `fsync`s.

**The cost of the log.** Everything is synced into `disk.img` before the clock stops:

| workload | log off | log on |
|---|---|---|
| 16 MB sequential write | 1,958 MB/s | 1,450 MB/s |
| 1,000 file creates | 206,840 /s | 156,835 /s |
| 100 creates, `fsync` after each (fsync on) | 261 /s | 130 /s |

Every block is written twice, once to the log and once to its home, and each `fsync`
waits for two log syncs instead of one.

---

## Design decisions

- **`pread`/`pwrite`, not `mmap`.** The log has to decide when a block may reach the disk.
  With `mmap` the OS writes a changed page back whenever it likes.
- **Redo-only log, no-steal cache.** Uncommitted changes never reach `disk.img`, so recovery
  never has to undo anything. This is ext3's choice, and a few hundred lines instead of ARIES.
- **Whole blocks in the log.** Wasteful, but replaying a record twice is harmless, and
  recovery cannot always know whether it already did.
- **File data is logged too**, not only metadata, so after a crash a file never contains
  blocks of garbage.
- **Lowest free block first.** Simple and clearly correct; OSTEP 41.4 has the faster policy.
- **`F_FULLFSYNC` on macOS**, where plain `fsync` does not force the drive's own cache.

## Limits

- One mounted file system per process. No hard links, symlinks or rename; permissions are
  fixed at 0644 for files and 0755 for directories.
- A file that is open cannot be unlinked (`VFS_EBUSY`).
- A single `vfs_write` of more than 16 blocks is several transactions, so a crash can keep
  the first part of it (POSIX allows this).
- Directories never shrink.
- It is not mounted into the OS. FUSE is the next step, and only `vfs.cpp` would need a
  bridge.

---

## Stages and the book

| stage | built | OSTEP |
|---|---|---|
| 1 | the disk: one file, 4 KB blocks | ch. 36, 37, 39.7 |
| 2 | bitmaps, `mkfs`, `dump` | 40.2, 40.5, ch. 17 |
| 3 | inodes, direct / indirect / double-indirect blocks | 40.3 |
| 4 | directories, path lookup | 40.4, 40.6 |
| 5 | the public API, the open file table | ch. 39 |
| 6 | the block cache: LRU, dirty bits, 16 shards | 40.7, ch. 22, 29.4 |
| 7 | the write-ahead log | 42.1, 42.3 |
| 8 | recovery, `fsck`, the crash test | 42.2, ch. 45 |
| 9 | per-inode locks, lock order, the flusher thread | ch. 28–30, 32 |
| 10 | benchmarks, this README | — |
