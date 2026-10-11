#pragma once

// #22: the achievement unlocks in ReXGlue's format
// (<saves>/content/achievements/<TITLEID>.toml, AchievementManager):
//   [unlocked.<decimal id>]
//   filetime = <Windows FILETIME>
// The Darkness's catalog starts at ID 0, so 0 is a real achievement here.

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace darkness::achievement_store {

using Unlocks = std::map<uint32_t, uint64_t>;  // id -> unlock FILETIME

// Reads the [unlocked.N] tables (comments, blank lines and other keys are
// skipped). False on a malformed line; unlocks holds what was read before.
bool ParseToml(std::string_view text, Unlocks& unlocks);

std::string FormatToml(const Unlocks& unlocks);

}  // namespace darkness::achievement_store
