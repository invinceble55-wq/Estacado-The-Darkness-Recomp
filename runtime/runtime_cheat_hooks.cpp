// Cheats (runtime_cheats.h): the hooks into the title (game executable
// only; the unit tests build the state without them).

#include "runtime_cheats.h"

#include "runtime_fatal.h"
#include "runtime_memory_access.h"
#include "ppc_recomp_shared.h"

#include <atomic>
#include <cstdio>

extern "C" PPC_FUNC(__imp__sub_825B75B8);
extern "C" PPC_FUNC(__imp__sub_82126828);

extern std::atomic<bool> g_god_toggle_sent;
extern std::atomic<bool> g_give_all_sent;
extern std::atomic<uint64_t> g_god_toggle_frame;
extern RuntimeCheatQueue g_queue;
extern std::atomic<uint32_t> g_give_all_requests;
void RuntimeCheatLog(const char* what, uint32_t value);
bool RuntimeCheatsGodModeWanted() noexcept;

void RuntimeCheatsRewriteCommand(uint8_t* base, uint32_t command) noexcept {
    if (!command || uint64_t(command) + 5u > 0x100000000ull) return;
    uint8_t bytes[5];
    for (uint32_t i = 0; i < 5; ++i) bytes[i] = PPC_LOAD_U8(command + i);
    if (!RuntimeCheatQueue::IsIdleImpulse(bytes)) return;
    if (g_give_all_requests.load() && !g_give_all_sent.load()) {
        g_give_all_requests.fetch_sub(1);
        g_give_all_sent.store(true);
        g_queue.Push(kCheatImpulseGiveAll);
    }
    const uint8_t impulse = g_queue.Take();
    if (!impulse) return;
    PPC_STORE_U8(command + 4, impulse);
    RuntimeCheatLog("impulse_sent", impulse);
}


// The shared "return 0" stub: the character's cheats-allowed check among
// others. Yes only at the call site of a cheat the runtime sent.
PPC_FUNC(sub_825B75B8) {
    const uint32_t site = uint32_t(ctx.lr);
    if (RuntimeCheatGateOpen(site, g_god_toggle_sent.load(), g_give_all_sent.load())) {
        if (site == kCheatGateGodMode) g_god_toggle_sent.store(false);
        if (site == kCheatGateGiveAll) g_give_all_sent.store(false);
        ctx.r3.u64 = 1;
        RuntimeCheatLog("gate_open", site);
        return;
    }
    __imp__sub_825B75B8(ctx, base);
}

// The server's client-command parser (r3 = the player character): keeps god
// mode equal to the setting before each command is handled.
namespace {
// The character the god mode toggle landed on: only its bit is followed (a
// scene with a second command-parsing object, such as the Chapter 1 car,
// would otherwise flip the bit back and forth). Forgotten when it has not
// parsed a command for a while (level load).
std::atomic<uint32_t> g_god_character{0};
std::atomic<uint64_t> g_god_character_seen{0};
}  // namespace

PPC_FUNC(sub_82126828) {
    const uint32_t character = ctx.r3.u32;
    const uint64_t now = RuntimeSwapCount();
    if (const uint32_t tracked = g_god_character.load()) {
        if (tracked == character) {
            g_god_character_seen.store(now);
        } else if (now - g_god_character_seen.load() > 120) {
            g_god_character.store(0);
            RuntimeCheatLog("god_mode_character_forgotten", 1);
        }
    }
    const uint32_t tracked = g_god_character.load();
    if (character && (!tracked || tracked == character) &&
        uint64_t(character) + kCheatCharacterState + 4u <= 0x100000000ull) {
        const uint32_t state = PPC_LOAD_U32(character + kCheatCharacterState);
        if (state && uint64_t(state) + kCheatStateFlags + 4u <= 0x100000000ull) {
            const bool current = (PPC_LOAD_U32(state + kCheatStateFlags) & kCheatGodModeBit) != 0;
            const uint64_t frame = RuntimeSwapCount();
            const uint64_t since = frame - g_god_toggle_frame.load();
            // A toggle that never reached the server (menu, level load) is
            // given up after a while and sent again.
            if (g_god_toggle_sent.load() && since > 240 && g_queue.Empty()) {
                g_god_toggle_sent.store(false);
                RuntimeCheatLog("god_mode_toggle_lost", 1);
            }
            if (!g_god_toggle_sent.load() &&
                RuntimeCheatGodModeNeedsToggle(RuntimeCheatsGodModeWanted(), current,
                                               since > 0xFFFFFFFFull ? 0xFFFFFFFFu : uint32_t(since))) {
                g_god_toggle_frame.store(frame);
                g_god_toggle_sent.store(true);
                g_queue.Push(kCheatImpulseGodMode);
                RuntimeCheatLog("god_mode_toggle_queued", current ? 0u : 1u);
            }
        }
    }
    const bool sent_before = g_god_toggle_sent.load();
    uint32_t flags_before = 0;
    uint32_t state_before = 0;
    if (sent_before && character) {
        state_before = PPC_LOAD_U32(character + kCheatCharacterState);
        if (state_before) flags_before = PPC_LOAD_U32(state_before + kCheatStateFlags);
    }
    __imp__sub_82126828(ctx, base);
    // The call that ran our toggle: what it did to the flag word.
    if (sent_before && !g_god_toggle_sent.load() && character) {
        g_god_character.store(character);
        g_god_character_seen.store(now);
        const uint32_t state = PPC_LOAD_U32(character + kCheatCharacterState);
        const uint32_t flags = state ? PPC_LOAD_U32(state + kCheatStateFlags) : 0;
        std::fprintf(stderr, "RUNTIME_CHEAT god_toggle_result char=0x%08X state=0x%08X/0x%08X flags=0x%08X->0x%08X\n",
                     character, state_before, state, flags_before, flags);
    }
}
