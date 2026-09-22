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

Each mode builds into its own directory, so switching between them never leaves stale
object files behind.

```
$ make
$ ./build/release/vfs_shell --version
ConcurrentFS 0.1 (stage 0 - skeleton)
```

Running it with no arguments lists every command the file system will eventually have,
next to the stage that makes each one work:

```
$ ./build/release/vfs_shell
ConcurrentFS 0.1 (stage 0 - skeleton)

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

---

## What is being built

Six layers. Each one only talks to the layer below it. Only the outermost shell exists
today — the file names below are where each layer will go, not files that are there now.

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
|             other's way                              <- stages 2-4, 9 |
+-------------------------------------------------------------------+
|  Layer 2  Write-ahead log                                         |
|           every change is written to a log first, then applied    |
|           src/wal.cpp, src/recovery.cpp              <- stages 7-8 |
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
│   └── version.h             # who we are, how far the build got
│
├── src/                      # library code, linked into every tool
│   └── version.cpp           # how version.h does it
│
└── tools/                    # programs you run from the terminal
    └── vfs_shell.cpp         # drives the file system
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

## What comes next

| Stage | What gets built | From the book |
|---|---|---|
| **1** | The fake disk — one file, read and written 4 KB at a time | Ch 36 (p. 419), Ch 37 (p. 433), 39.7 (p. 477) |
| 2 | Free-space bitmaps, and `mkfs` to format a disk | 40.5 (p. 501), Ch 17 (p. 167) |
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

Stage 1 is next, and this README grows to cover it when it lands.
