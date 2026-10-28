#include "vfs/path.h"
#include "vfs/errors.h"
#include "vfs/layout.h"

namespace vfs {

    std::vector<std::string> split_path(const char* path) {
        if (path == nullptr || path[0] != '/')
            throw FsError(VFS_EINVAL);

        std::vector<std::string> names;
        const char* p = path;

        while (*p != '\0') {
            while (*p == '/')
                p++;
            const char* start = p;
            while (*p != '\0' && *p != '/')
                p++;

            size_t len = static_cast<size_t>(p - start);
            if (len == 0)
                continue;  // a trailing slash
            if (len > MAX_NAME_LEN)
                throw FsError(VFS_ENAMETOOLONG);
            if (len == 1 && start[0] == '.')
                continue;
            if (len == 2 && start[0] == '.' && start[1] == '.') {
                if (!names.empty())
                    names.pop_back();
                continue;
            }
            names.emplace_back(start, len);
        }
        return names;
    }

} // namespace vfs
