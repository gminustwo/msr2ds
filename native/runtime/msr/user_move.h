// Moving the user folder between two places (user_move.cpp): msr-native's Android app between the phone's storage and
// the SD card (#39), each its folder for the app (Android/data/<app>/files) holding user/ and the shader caches beside it.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace MSR::UserMove {

struct Size {
    std::uint64_t bytes = 0;
    std::uint64_t files = 0;
};

// What a move from `root` takes: user/ and the shader caches beside it (pwr_*.bin)
Size Measure(const std::filesystem::path& root);

// user/ and the caches copied from `from` to `to` (each a folder holding user/), checked, and put in place there: a
// user/ already at `to` is set aside first (`aside`: where it went; empty when there was none), and game-path.txt is
// pointed at the game file's new place when it was in the user folder. `from` is left as it was, for the caller to
// remove once it has made `to` the place (Remove). `progress(done, total)`, in bytes, from the copying thread. "" when
// done, else what failed: then `to` holds what it held before.
std::string Move(const std::filesystem::path& from, const std::filesystem::path& to,
                 const std::function<void(std::uint64_t, std::uint64_t)>& progress, std::filesystem::path* aside);

// A move taken back (the place could not be recorded): user/ removed from `to`, and the one set aside put back
void Undo(const std::filesystem::path& to, const std::filesystem::path& aside);

// The old place emptied after a move: its user/ and caches. "" when done, else what was left.
std::string Remove(const std::filesystem::path& from);

} // namespace MSR::UserMove
