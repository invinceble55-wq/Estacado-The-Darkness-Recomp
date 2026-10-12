#include "runtime_movement_packet.h"
#include "runtime_cheats.h"
#include "runtime_fatal.h"
#include "runtime_memory_access.h"
#include "ppc_recomp_shared.h"

#include <cstdio>
#include <cstdlib>

#include <array>
#include <atomic>

extern "C" PPC_FUNC(__imp__sub_82433810);
extern "C" PPC_FUNC(__imp__sub_824A1720);

namespace {
std::atomic<bool> movement_packet_compaction{};
}

void ConfigureRuntimeMovementPacketCompaction(bool enabled) noexcept {
    movement_packet_compaction.store(enabled, std::memory_order_relaxed);
}

PPC_FUNC(sub_82433810) {
    if (movement_packet_compaction.load(std::memory_order_relaxed) &&
        RuntimeMovementPacketCaller(static_cast<uint32_t>(ctx.lr))) {
        const uint32_t destination = ctx.r3.u32;
        const uint32_t source = ctx.r4.u32;
        // The builder owns the destination packet and a stack copy of its
        // current command. Never edit the input ring, cursors or time here;
        // the original builder retains its normal consumption/retirement.
        if (destination && source && uint64_t(destination)+256 <= 0x100000000ull &&
            uint64_t(source)+10 <= 0x100000000ull &&
            PPC_LOAD_U8(source) == 3 && PPC_LOAD_U8(source+1) == 6) {
            std::array<uint8_t, 10> incoming{};
            for (size_t i = 0; i < incoming.size(); ++i)
                incoming[i] = PPC_LOAD_U8(source+static_cast<uint32_t>(i));
            const size_t bytes = size_t(PPC_LOAD_U8(destination+1))+2;
            if (bytes <= 256) {
                std::array<uint8_t, 256> packet{};
                for (size_t i = 0; i < bytes; ++i)
                    packet[i] = PPC_LOAD_U8(destination+static_cast<uint32_t>(i));
                const size_t replace = RuntimeMovementPacketReplacement(
                    packet.data(), bytes, incoming.data(), incoming.size());
                if (replace) {
                    for (size_t i = 0; i < incoming.size(); ++i)
                        PPC_STORE_U8(destination+static_cast<uint32_t>(replace+i), incoming[i]);
                    ctx.r3.u64 = 1; // Same successful append contract.
                    return;
                }
            }
        }
    }
    __imp__sub_82433810(ctx, base);
}

namespace {
std::atomic<uint32_t> traced_stick_commands{};
}  // namespace

// AddCommand(client, command): merge a same-time movement vector into the
// newest queued one (runtime_movement_packet.h, V379).
PPC_FUNC(sub_824A1720) {
    // Developer trace of every queued client command (cheats work: the
    // packet format of controls). DARKNESS_TRACE_COMMANDS=1, first 4000.
    static const bool trace_all = std::getenv("DARKNESS_TRACE_COMMANDS") != nullptr;
    static std::atomic<uint32_t> traced_all{};
    if (trace_all && traced_all.fetch_add(1, std::memory_order_relaxed) < 4000) {
        const uint32_t command = ctx.r4.u32;
        char bytes[24 * 2 + 1] = {};
        static constexpr char kHex[] = "0123456789abcdef";
        if (command && uint64_t(command) + 24u <= 0x100000000ull) {
            for (uint32_t i = 0; i < 24; ++i) {
                const uint8_t value = PPC_LOAD_U8(command + i);
                bytes[i * 2] = kHex[value >> 4];
                bytes[i * 2 + 1] = kHex[value & 15];
            }
        }
        std::printf("RUNTIME_CMD frame=%llu lr=0x%08X client=0x%08X cmd=%s\n",
                    static_cast<unsigned long long>(RuntimeSwapCount()), uint32_t(ctx.lr),
                    ctx.r3.u32, bytes);
    }
    // Cheats (runtime_cheats.h): a pending impulse rides in an idle one.
    RuntimeCheatsRewriteCommand(base, ctx.r4.u32);
    if (RuntimeStickCommandTraceActive() &&
        traced_stick_commands.fetch_add(1, std::memory_order_relaxed) < 3000) {
        const uint32_t command = ctx.r4.u32;
        char bytes[16 * 2 + 1] = {};
        static constexpr char kHex[] = "0123456789abcdef";
        if (command && uint64_t(command) + 16u <= 0x100000000ull) {
            for (uint32_t i = 0; i < 16; ++i) {
                const uint8_t value = PPC_LOAD_U8(command + i);
                bytes[i * 2] = kHex[value >> 4];
                bytes[i * 2 + 1] = kHex[value & 15];
            }
        }
        std::printf("RUNTIME_STICK_CMD frame=%llu lr=0x%08X cmd=%s\n",
                    static_cast<unsigned long long>(RuntimeSwapCount()), uint32_t(ctx.lr), bytes);
    }
    if (movement_packet_compaction.load(std::memory_order_relaxed)) {
        const uint32_t client = ctx.r3.u32;
        const uint32_t command = ctx.r4.u32;
        if (client && command && uint64_t(client) + 2828u <= 0x100000000ull &&
            uint64_t(command) + 10u <= 0x100000000ull && PPC_LOAD_U8(command) == 3 &&
            PPC_LOAD_U8(command + 1) == 6) {
            std::array<uint8_t, 10> incoming{};
            for (size_t i = 0; i < incoming.size(); ++i)
                incoming[i] = PPC_LOAD_U8(command + static_cast<uint32_t>(i));
            const uint32_t ring = PPC_LOAD_U32(client + 2824u);
            const uint32_t descriptor =
                ring && uint64_t(ring) + 24u <= 0x100000000ull ? PPC_LOAD_U32(ring + 12u) : 0u;
            if (descriptor && uint64_t(descriptor) + 28u <= 0x100000000ull) {
                const uint32_t capacity = PPC_LOAD_U32(descriptor + 4u);
                const uint32_t entries = PPC_LOAD_U32(descriptor + 24u);
                const int64_t newest = RuntimeMovementRingNewest(
                    PPC_LOAD_U32(ring + 16u), PPC_LOAD_U32(ring + 20u), capacity);
                if (entries && newest >= 0 &&
                    uint64_t(entries) + uint64_t(capacity) * 36u <= 0x100000000ull) {
                    const uint32_t entry = entries + static_cast<uint32_t>(newest) * 36u;
                    const std::array<uint8_t, 3> queued{PPC_LOAD_U8(entry), PPC_LOAD_U8(entry + 1),
                                                        PPC_LOAD_U8(entry + 2)};
                    if (RuntimeMovementRingMerges(queued.data(), incoming.data())) {
                        for (uint32_t i = 4; i < 10; ++i) PPC_STORE_U8(entry + i, incoming[i]);
                        ctx.r3.u64 = 1;  // Queued, as AddCommand reports.
                        return;
                    }
                }
            }
        }
    }
    __imp__sub_824A1720(ctx, base);
}
