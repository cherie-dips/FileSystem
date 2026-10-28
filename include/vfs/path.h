#pragma once

#include <string>
#include <vector>

namespace vfs {
    // "/a//b/./c/../d" -> {"a", "b", "d"}, and "/" -> {}. ".." is resolved by
    // name - there are no links, so that is always the right answer - and
    // ".." at the root stays at the root.
    //
    // Throws FsError: VFS_EINVAL for a null, empty or relative path,
    // VFS_ENAMETOOLONG for a component longer than MAX_NAME_LEN.
    std::vector<std::string> split_path(const char* path);
}
