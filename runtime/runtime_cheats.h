#pragma once

// Cheats built on the developers' own commands (docs/CHEATS_PLAN.md). The
// title registers client impulses (sub_823FF1D0): noclip 32, godmode 34,
// giveall 35, noclip2 36, givedarknesslevel1-5 55-59, givedemonarmlevel1-2
// 60-61. A client sends an impulse as a type-0 command [00, 01, time, 00, id]
// (every gameplay frame carries an idle one with id 0x7F); the server's
// command parser (sub_82126828) runs the cheat only when the character's
// "cheats allowed" check (vtable slot 81, the shared return-0 stub
// sub_825B75B8 in retail) says yes.
//
// The runtime sends an impulse by rewriting the payload of one of the game's
// own idle impulses (no extra command in the ring) and opens the check only
// at the call site of the cheat it sent, only while that cheat is wanted.
// God mode is kept equal to the setting: the server's toggle flips bit 0x200
// of the character state word (+0x18C -> +0x2800); after a checkpoint load
// or a toggle that did not land, the impulse is sent again.

#include <cstdint>
#include <deque>
#include <mutex>

inline constexpr uint8_t kCheatImpulseGodMode = 34;
inline constexpr uint8_t kCheatImpulseGiveAll = 35;
inline constexpr uint8_t kCheatIdleImpulse = 0x7F;
// Return addresses of the server's "cheats allowed" calls.
inline constexpr uint32_t kCheatGateGodMode = 0x821274C4;
inline constexpr uint32_t kCheatGateGiveAll = 0x82127868;
// Character state (+0x18C) flag word (+0x2800) and its god mode bit.
inline constexpr uint32_t kCheatCharacterState = 0x18C;
inline constexpr uint32_t kCheatStateFlags = 0x2800;
inline constexpr uint32_t kCheatGodModeBit = 0x200;

struct RuntimeCheatSettings {
    bool godMode = false;
};

// Pure decisions (unit-tested in runtime_cheats_test).
class RuntimeCheatQueue {
public:
    // An idle impulse command ([00, 01, time, 00, 7F]) that may carry ours.
    static bool IsIdleImpulse(const uint8_t* command) noexcept {
        return command && command[0] == 0 && command[1] == 1 && command[4] == kCheatIdleImpulse;
    }
    void Push(uint8_t impulse) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_.size() < 8) pending_.push_back(impulse);
    }
    // The impulse to put into an idle command now (0 = none).
    uint8_t Take() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_.empty()) return 0;
        const uint8_t impulse = pending_.front();
        pending_.pop_front();
        return impulse;
    }
    bool Empty() {
        std::lock_guard<std::mutex> lock(mutex_);
        return pending_.empty();
    }

private:
    std::mutex mutex_;
    std::deque<uint8_t> pending_;
};

// God mode sync: given the wanted state, the character's bit and the frames
// since the last toggle was sent, whether to send the toggle now. A toggle
// needs a few frames to reach the server, so it is not repeated sooner.
inline bool RuntimeCheatGodModeNeedsToggle(bool wanted, bool current, uint32_t framesSinceSent,
                                           uint32_t settleFrames = 30) noexcept {
    return wanted != current && framesSinceSent >= settleFrames;
}

// Whether the server's cheats-allowed check at return address `site` may
// say yes: only at the site of a cheat the runtime itself sent and wants.
inline bool RuntimeCheatGateOpen(uint32_t site, bool godModeToggleSent,
                                 bool giveAllSent) noexcept {
    return (site == kCheatGateGodMode && godModeToggleSent) ||
           (site == kCheatGateGiveAll && giveAllSent);
}

void ConfigureRuntimeCheats(const RuntimeCheatSettings& settings) noexcept;
RuntimeCheatSettings RuntimeCheatsCurrent() noexcept;
// One-shot: every weapon and the highest Darkness and Demon Arm levels
// (saved with the game; cannot be taken back).
void RuntimeCheatsGiveAll() noexcept;
// Achievements unlock normally while cheats are on (owner, 2026-10-12).
// Called by the client AddCommand hook with the command's guest bytes:
// rewrites an idle impulse with a pending cheat impulse.
void RuntimeCheatsRewriteCommand(uint8_t* base, uint32_t command) noexcept;
