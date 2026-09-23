# ConcurrentFS — a file system built from scratch, in user space

A file system written in C++ that stores everything inside one ordinary file on your
laptop. It will have its own inodes, its own free-space bitmaps, its own directories, its
own block cache, and its own write-ahead log so it survives being killed in the middle of
a write. Many threads will be able to use it at once.

Nothing here wraps the file system your OS already has. Every piece is built by hand.

The book behind this project is *Operating Systems: Three Easy Pieces*. Each stage below
says which chapter and page it comes from.


---

## Build and run

Needs a C++17 compiler and `make`. Both come with the Xcode command line tools on macOS
(`xcode-select --install`). No other dependencies.

```
make            # build, optimised                -> build/release/
make debug      # no optimisation, debug symbols  -> build/debug/
make asan       # + AddressSanitizer and UBSan    -> build/asan/
make tsan       # + ThreadSanitizer               -> build/tsan/
make run        # build and run the shell
make clean      # delete every build directory
```

Two binaries come out of it: `vfs_shell`, which you drive the file system with, and
`disktest`, which checks that the layer built in stage 1 actually works.

Each mode builds into its own directory, so switching between them never leaves stale
object files behind.

```
$ make
$ ./build/release/vfs_shell --version
ConcurrentFS 0.1 (stage 1 - the fake disk)
```

Running it with no arguments lists every command the file system will eventually have,
next to the stage that makes each one work:

```
$ ./build/release/vfs_shell
ConcurrentFS 0.1 (stage 1 - the fake disk)

usage: vfs_shell [--version] [--help] <command> [args]

commands:
  mkfs     format a new disk image              stage 2
  dump     print a superblock, bitmap or inode  stage 2
  mkdir    create a directory                   stage 4
  ls       list a directory                     stage 4
  cat      print a file                         stage 5
  write    write text into a file               stage 5
  rm       remove a file                        stage 5
  sync     flush everything to the disk image   stage 6
  fsck     check the disk image for damage      stage 8
```

Asking for one of those today tells you where the build is rather than failing quietly:

```
$ ./build/release/vfs_shell mkfs
vfs_shell: 'mkfs' is not built yet - it arrives at stage 2
```

And the stage 1 checks:

```
$ ./build/release/disktest
round trip within one process
  ok    block_count reports what we asked for
  ok    what goes in comes out
bounds
  ok    reading one past the last block is rejected
  ...
persistence across two processes
  ok    block 500 survived the process exiting

stage 1: all checks passed
```

---

## What is being built

Six layers. Each one only talks to the layer below it. Layers 0 and 5 exist today; for the
rest, the file names below are where each layer will go, not files that are there now.

```
+-------------------------------------------------------------------+
|  Layer 5  Test programs, a small shell, benchmarks                |
|           tools/vfs_shell.cpp                          <- stage 0 |
+-------------------------------------------------------------------+
|  Layer 4  The public API                                          |
|           vfs_open, vfs_read, vfs_write, vfs_mkdir ...            |
|           include/vfs/vfs.h, src/vfs.cpp               <- stage 5 |
+-------------------------------------------------------------------+
|  Layer 3  File system logic                                       |
|           inodes, directories, path lookup, bitmaps               |
|           + per-inode locks so threads stay out of each           |
|             other's way                          <- stages 2-4, 9 |
+-------------------------------------------------------------------+
|  Layer 2  Write-ahead log                                         |
|           every change is written to a log first, then applied    |
|           src/wal.cpp, src/recovery.cpp             <- stages 7-8 |
+-------------------------------------------------------------------+
|  Layer 1  Block cache                                             |
|           keeps hot 4 KB blocks in RAM, writes dirty ones back    |
|           src/buffer_cache.cpp, src/flusher.cpp        <- stage 6 |
+-------------------------------------------------------------------+
|  Layer 0  The fake disk                                           |
|           one big file, read and written 4 KB at a time           |
|           src/block_device.cpp                         <- stage 1 |
+-------------------------------------------------------------------+
```

The "disk" will be a file called `disk.img`. A 256 MB one is a 256 MB disk with 65,536
blocks of 4 KB each. Deleting the file wipes the file system; copying it clones it.

---

## Project structure

Everything that exists today. Each stage adds its own files; nothing is here in advance.

```
FileSystem/
├── Makefile                  # four build modes, one directory each
├── README.md
├── .gitignore
│
├── include/vfs/              # one header per module — what each module does
│   ├── version.h             # who we are, how far the build got
│   ├── layout.h              # BLOCK_SIZE, and the Block type
│   └── block_device.h        # the disk: read_block / write_block / sync
│
├── src/                      # library code, linked into every tool
│   ├── version.cpp
│   └── block_device.cpp      # PreadDevice — the disk, over a real file
│
└── tools/                    # programs you run from the terminal
    ├── vfs_shell.cpp         # drives the file system
    └── disktest.cpp          # proves stage 1 works
```

**Why `src/` and `tools/` are separate.** A file in `src/` is library code: compiled once
and linked into every tool. A file in `tools/` has its own `main()` and becomes its own
binary. The Makefile already treats them differently, so stage 2 can drop in `tools/mkfs.cpp`
and get a second binary with no build change.

**The rule this project follows:** a header in `include/vfs/` says *what* a module does;
the matching `.cpp` in `src/` is the only file that knows *how*. If a future
`directory.cpp` ever calls `pread` directly, something has gone wrong — it should be asking
the buffer cache.

---

## Stage 0 — the skeleton
A build system, the directory layout, and a program that runs.

There is no operating systems content in this stage and no chapter of the book behind it.
That is the point: get the boring parts settled while they are cheap, so that from stage 1
onward every change is about the file system and never about the build.

What it sets up, and why each piece matters later:

- **Four build modes in separate directories.** ThreadSanitizer becomes non-negotiable at
  stage 9, and AddressSanitizer is useful from stage 2. Sanitizer builds need different
  compiler flags, and mixing objects built with and without those flags produces confusing
  failures. Separate directories per mode make that impossible.
- **`-Wall -Wextra -Wpedantic`, on from the first line of code.** Cheaper than debugging.
- **Automatic header dependency tracking** (`-MMD -MP`), so editing a header rebuilds
  everything that included it. Easy to add now, annoying to add once there are thirty files.
- **`vfs_shell` lists every future command with its stage.** It doubles as a progress
  tracker: the plan is visible from the terminal, not only from this file. It is also the
  only place stage 0 mentions later stages — no empty files or directories are created in
  advance, so the tree always shows exactly what has been built.

**Deliverable, met:** `make && ./build/release/vfs_shell --version` prints a version string,
and all four modes compile clean with no warnings.

---

## Stage 1 — the fake disk

**The disk is a file.** `disk.img` is an ordinary file cut into 4096-byte blocks: block 37
is the bytes at offset 37 × 4096. That is the whole idea. Everything built above this layer
thinks in block numbers and never touches a file descriptor, which means there is exactly
one place in the codebase where bytes reach the disk — and that will matter enormously at
stage 7, when the write-ahead log needs to control the order writes happen in.

Four operations, and nothing else:

```cpp
class BlockDevice {
public:
    virtual void read_block (uint32_t block_no, Block& dst)       = 0;
    virtual void write_block(uint32_t block_no, const Block& src) = 0;
    virtual void sync()                                           = 0;
    virtual uint32_t block_count() const                          = 0;
};
```

`PreadDevice` implements them over a real file with `pread`, `pwrite` and `fsync`. It is a
dumb pass-through: every call goes straight to the file. Caching comes at stage 6 and sits
*above* this layer, not inside it.

### Decisions worth knowing about

**`Block`, not `void*`.** `Block` is `std::array<uint8_t, 4096>`. The obvious signature is
`read_block(uint32_t, void*)`, but that carries an unwritten promise that the caller
allocated at least 4096 bytes. Break the promise and you get a silent 4 KB stack smash.
With the size in the type, the compiler catches it:

```
error: non-const lvalue reference to type 'Block' cannot bind to a value of
       unrelated type 'char[64]'
```

**`pread`/`pwrite`, not `mmap`.** Mapping the file into memory would make a read a plain
`memcpy` and skip a system call. The problem is that a write to mapped memory only marks a
page dirty; the OS copies it to the disk later, on its own schedule, and will not tell you
when. The journal at stage 7 depends on holding a change back until its log record is safely
down, and `mmap` gives no way to hold anything back. The system call it would have saved is
one the stage 6 cache removes anyway.

### The details that actually bite

- **Offset overflow.** `block_no * BLOCK_SIZE` on a `uint32_t` wraps at block 1,048,576 —
  a 4 GB disk — and silently corrupts block 0. The cast has to come first:
  `static_cast<off_t>(block_no) * BLOCK_SIZE`.
- **Short reads and writes.** `pread` is not obliged to move all 4096 bytes in one call, so
  both helpers loop until the block is complete.
- **`EINTR`.** A signal makes the call return `-1` without having failed. Retry, don't throw.
  `read`, `write` and `sync` all handle it.
- **`create()` refuses to overwrite.** `O_EXCL`, not `O_TRUNC` — creating a disk on top of a
  file that already holds a file system should never happen by accident.
- **`fsync` is not enough on macOS.** It pushes data to the drive but does not ask the drive
  to persist its own cache. `sync()` tries `fcntl(fd, F_FULLFSYNC, 0)` first and falls back
  to `fsync` where that is unsupported.
- **`errno` is saved before `close()`** in the error paths, because `close` can overwrite it
  and turn a real error message into a misleading one.

Bad input is rejected rather than half-handled: a block number past the end, a file that
is not a whole number of blocks, an empty file, a disk of zero blocks.

### From the book

- **Ch 36, "I/O Devices", p. 419** — why the unit of transfer is a block at all. 36.5 on
  DMA (p. 424) is why it is a big one rather than a byte.
- **Ch 37, "Hard Disk Drives", p. 433** — 37.4, "I/O Time: Doing The Math" (p. 438), is why
  a real disk makes you care how many blocks you touch and where they are.
- **39.7, "Writing Immediately With fsync()", p. 477** — two pages, and exactly why `sync()`
  has to be a separate call from `write_block`.

---

## What comes next

| Stage | What gets built | From the book |
|---|---|---|
| **2** | Free-space bitmaps, and `mkfs` to format a disk | 40.5 (p. 501), Ch 17 (p. 167) |
| 3 | Inodes, and direct / indirect / double-indirect block mapping | 40.3 (p. 496) |
| 4 | Directories and path lookup | 40.4 (p. 501), 40.6 (p. 502) |
| 5 | The public API, and the open file table | Ch 39 (p. 467), 39.6 (p. 475) |
| 6 | The block cache — LRU, dirty bits, pinning | 40.7 (p. 506), Ch 22 (p. 243) |
| 7 | The write-ahead log | 42.1 (p. 526), 42.3 (p. 531) |
| 8 | Crash recovery, `fsck`, and killing the process 500 times | 42.2 (p. 529), Ch 45 (p. 587) |
| 9 | Many threads: per-inode locks, lock ordering, a flusher thread | Ch 28–30, Ch 32 (p. 385) |
| 10 | Benchmarks, design write-up, polish | — |

After stage 10, one extension: mounting the whole thing with FUSE so `ls`, `cat` and `vim`
talk to it like any other folder (OSTEP 39.17, p. 488).

Stage 2 is next, and this README grows to cover it when it lands.
