#pragma once

// version.h — who we are and how far along the build we got.


#include <string>

namespace vfs {
    inline constexpr const char* kName  = "ConcurrentFS";
    inline constexpr int kVersionMajor  = 0;
    inline constexpr int kVersionMinor  = 1;

    // Bumped as each stage of the build plan lands.
    inline constexpr const char* kStage = "stage 1 - the fake disk";

    // e.g. "ConcurrentFS 0.1 (stage 0 - skeleton)"
    // Returns by value: no shared buffer, so it is safe to call from any thread.
    std::string version_string();
} // namespace vfs
