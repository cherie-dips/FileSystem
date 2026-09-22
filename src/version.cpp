#include "vfs/version.h"

#include <string>

namespace vfs {

std::string version_string() {
    return std::string(kName) + " "
         + std::to_string(kVersionMajor) + "."
         + std::to_string(kVersionMinor)
         + " (" + kStage + ")";
}

} // namespace vfs
