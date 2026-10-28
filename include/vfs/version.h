#pragma once

// version.h — who we are and how far along the build we got.


#include <string>

namespace vfs {
    inline constexpr const char* kName  = "ConcurrentFS";
    inline constexpr int kVersionMajor  = 1;
    inline constexpr int kVersionMinor  = 0;

    // Bumped as each stage of the build plan landed.
    inline constexpr const char* kStage = "all ten stages";

    // e.g. "ConcurrentFS 0.1 (stage 0 - skeleton)"
    // Returns by value: no shared buffer, so it is safe to call from any thread.
    std::string version_string();
} // namespace vfs
