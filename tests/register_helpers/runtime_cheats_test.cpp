// Cheats on the title's impulses (runtime/runtime_cheats.h): only idle
// impulses carry a cheat, the gate opens only at the sent cheat's site, god
// mode is toggled only when it differs and not again before it settles.
#include "runtime_cheats.h"

#include <cstdio>

namespace {
int failures = 0;
void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}
}  // namespace

int main() {
    const uint8_t idle[5] = {0x00, 0x01, 0x10, 0x00, 0x7F};
    const uint8_t buttons[5] = {0x01, 0x04, 0x10, 0x00, 0x04};
    const uint8_t movement[5] = {0x03, 0x06, 0x10, 0x00, 0x00};
    const uint8_t used[5] = {0x00, 0x01, 0x10, 0x00, 0x22};
    Check(RuntimeCheatQueue::IsIdleImpulse(idle), "an idle impulse carries a cheat");
    Check(!RuntimeCheatQueue::IsIdleImpulse(buttons) && !RuntimeCheatQueue::IsIdleImpulse(movement) &&
              !RuntimeCheatQueue::IsIdleImpulse(used) && !RuntimeCheatQueue::IsIdleImpulse(nullptr),
          "buttons, movement and real impulses are never rewritten");

    RuntimeCheatQueue queue;
    Check(queue.Empty() && queue.Take() == 0, "empty queue");
    queue.Push(kCheatImpulseGodMode);
    queue.Push(kCheatImpulseGiveAll);
    Check(queue.Take() == kCheatImpulseGodMode && queue.Take() == kCheatImpulseGiveAll &&
              queue.Take() == 0,
          "impulses go out in order, once");
    for (int i = 0; i < 20; ++i) queue.Push(kCheatImpulseGodMode);
    int taken = 0;
    while (queue.Take()) ++taken;
    Check(taken == 8, "the queue is bounded");

    Check(RuntimeCheatGateOpen(kCheatGateGodMode, true, false), "god mode site opens when sent");
    Check(!RuntimeCheatGateOpen(kCheatGateGodMode, false, true), "god mode site closed otherwise");
    Check(RuntimeCheatGateOpen(kCheatGateGiveAll, false, true), "give-all site opens when sent");
    Check(!RuntimeCheatGateOpen(0x8212748C, true, true) && !RuntimeCheatGateOpen(0x82127460, true, true),
          "noclip sites never open");
    Check(!RuntimeCheatGateOpen(0x82000000, true, true), "other callers of the stub stay at 0");

    Check(RuntimeCheatGodModeNeedsToggle(true, false, 30), "on wanted, off now: toggle");
    Check(RuntimeCheatGodModeNeedsToggle(false, true, 100), "off wanted, on now: toggle");
    Check(!RuntimeCheatGodModeNeedsToggle(true, true, 100) && !RuntimeCheatGodModeNeedsToggle(false, false, 100),
          "equal: nothing");
    Check(!RuntimeCheatGodModeNeedsToggle(true, false, 5), "not again before it settles");

    if (failures) return 1;
    std::printf("runtime_cheats: all checks passed\n");
    return 0;
}
