// #22 achievement unlocks in ReXGlue's TOML format
// (runtime/runtime_achievement_store.cpp): ReXGlue's own output parses, ID 0
// is a real achievement, round trip, malformed files are refused.
#include "runtime_achievement_store.h"

#include <cstdio>
#include <string>

namespace store = darkness::achievement_store;

namespace {

bool Check(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}

}  // namespace

int main() {
    bool ok = true;
    // As ReXGlue's AchievementManager::SaveUnlockState writes it.
    const std::string rexglue =
        "# Achievement unlock state - managed by ReXGlue runtime\n\n"
        "[unlocked.7]\nfiletime = 134044092000000000\n\n"
        "[unlocked.0]\r\nfiletime = 134044091000000000\r\n\r\n";
    store::Unlocks unlocks;
    ok &= Check(store::ParseToml(rexglue, unlocks), "ReXGlue output parses");
    ok &= Check(unlocks.size() == 2 && unlocks[0] == 134044091000000000ull &&
                    unlocks[7] == 134044092000000000ull,
                "both unlocks with their times (CRLF too)");

    const std::string text = store::FormatToml(unlocks);
    store::Unlocks again;
    ok &= Check(store::ParseToml(text, again) && again == unlocks, "round trip");
    ok &= Check(text.find("[unlocked.0]\nfiletime = 134044091000000000\n") != std::string::npos,
                "ReXGlue layout");

    store::Unlocks other;
    ok &= Check(store::ParseToml("[other]\nkey = 1\n[unlocked.3]\n# no time\n", other) &&
                    other.size() == 1 && other.at(3) == 0,
                "other tables skipped; a missing time reads as 0");
    store::Unlocks bad;
    ok &= Check(!store::ParseToml("[unlocked.x]\nfiletime = 1\n", bad), "non-numeric id");
    ok &= Check(!store::ParseToml("[unlocked.1]\nfiletime = -5\n", bad), "negative time");
    ok &= Check(!store::ParseToml("[unlocked.4294967296]\n", bad), "id out of range");
    ok &= Check(!store::ParseToml("garbage\n", bad), "not TOML");
    store::Unlocks empty;
    ok &= Check(store::ParseToml("", empty) && empty.empty(), "empty file");

    if (ok) std::printf("runtime_achievement_store: all checks passed\n");
    return ok ? 0 : 1;
}
