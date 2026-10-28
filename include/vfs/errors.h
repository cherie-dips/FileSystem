#pragma once

// errors.h — the error codes the public API returns, negated (-VFS_ENOENT).

#include <stdexcept>

enum : int {
    VFS_ENOENT       = 2,   // no such file or directory
    VFS_EIO          = 5,   // the disk image or the log failed
    VFS_EBADF        = 9,   // not an open file descriptor, or wrong access mode
    VFS_EBUSY        = 16,  // in use: an open file, the root, an existing mount
    VFS_EEXIST       = 17,
    VFS_ENOTDIR      = 20,
    VFS_EISDIR       = 21,
    VFS_EINVAL       = 22,  // bad argument, relative path, or not mounted
    VFS_EMFILE       = 24,  // the open file table is full
    VFS_EFBIG        = 27,  // past the largest file the block pointers reach
    VFS_ENOSPC       = 28,  // no free blocks or inodes
    VFS_ENAMETOOLONG = 36,  // a path component longer than MAX_NAME_LEN
    VFS_ENOTEMPTY    = 39,
};

// Takes a code or a negated code.
inline const char* vfs_strerror(int code) {
    switch (code < 0 ? -code : code) {
        case 0:                return "success";
        case VFS_ENOENT:       return "no such file or directory";
        case VFS_EIO:          return "input/output error";
        case VFS_EBADF:        return "bad file descriptor";
        case VFS_EBUSY:        return "resource busy";
        case VFS_EEXIST:       return "file exists";
        case VFS_ENOTDIR:      return "not a directory";
        case VFS_EISDIR:       return "is a directory";
        case VFS_EINVAL:       return "invalid argument";
        case VFS_EMFILE:       return "too many open files";
        case VFS_EFBIG:        return "file too large";
        case VFS_ENOSPC:       return "no space left on device";
        case VFS_ENAMETOOLONG: return "file name too long";
        case VFS_ENOTEMPTY:    return "directory not empty";
        default:               return "unknown error";
    }
}

namespace vfs {
    // Thrown inside the file system. The functions in vfs.h catch it and
    // return -code.
    struct FsError : std::runtime_error {
        int code;
        explicit FsError(int c) : std::runtime_error(vfs_strerror(c)), code(c) {}
    };
}
