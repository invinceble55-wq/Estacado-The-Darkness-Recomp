// Cheats on the developers' own commands (runtime_cheats.h): settings and
// the impulse queue. The hooks into the title are in runtime_cheat_hooks.cpp.

#include "runtime_cheats.h"

#include "runtime_fatal.h"

#include <atomic>
#include <cstdio>

namespace {
std::atomic<bool> g_god_mode{false};
// Whether give-all was used this session.
// The impulse in flight: the gate opens for it until the server runs it.

}  // namespace

// Shared with runtime_cheat_hooks.cpp (the impulse in flight).
std::atomic<bool> g_god_toggle_sent{false};
std::atomic<bool> g_give_all_sent{false};
std::atomic<uint64_t> g_god_toggle_frame{0};
// Give-all requests not yet sent.
std::atomic<uint32_t> g_give_all_requests{0};
RuntimeCheatQueue g_queue;

void RuntimeCheatLog(const char* what, uint32_t value) {
    std::fprintf(stderr, "RUNTIME_CHEAT %s=%u frame=%llu\n", what, value,
                 static_cast<unsigned long long>(RuntimeSwapCount()));
    std::fflush(stderr);
}

void ConfigureRuntimeCheats(const RuntimeCheatSettings& settings) noexcept {
    if (g_god_mode.exchange(settings.godMode) != settings.godMode) {
        RuntimeCheatLog("god_mode_wanted", settings.godMode ? 1u : 0u);
    }
}

RuntimeCheatSettings RuntimeCheatsCurrent() noexcept {
    RuntimeCheatSettings settings;
    settings.godMode = g_god_mode.load();
    return settings;
}

void RuntimeCheatsGiveAll() noexcept {
    g_give_all_requests.fetch_add(1);
    RuntimeCheatLog("give_all_requested", 1);
}




bool RuntimeCheatsGodModeWanted() noexcept { return g_god_mode.load(); }
