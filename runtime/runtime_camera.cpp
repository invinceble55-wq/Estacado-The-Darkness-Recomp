#include "runtime_video_mode.h"
#include "runtime_camera.h"

#include "runtime_camera_policy.h"
#include "runtime_function_trace.h"
#include "runtime_owned_source_write_watch.h"
#include "runtime_owned_source_scope.h"
#include "runtime_owned_packet_write_lease.h"
#include "runtime_owned_packet_read.h"
#include "runtime_owned_camera_mode.h"
#include "runtime_memory.h"
#include "runtime_graphics.h"
#include "runtime_motion_origins.h"
#include "runtime_motion_delta.h"
#include "runtime_temporal_camera.h"

#include "runtime_memory_access.h"
#include "ppc_recomp_shared.h"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

std::atomic<uint32_t> g_runtime_view_fov_active{};

namespace {
std::atomic<float> gameplay_fov_degrees{kOriginalGameplayFovDegrees};
std::atomic<double> view_fov_tangent_scale{1.0};
std::atomic<bool> view_fov_logged{};
// #6: per-frame FOV interpolation (RuntimeFovSmoother). Off in 0.9.1 until
// it is verified on an actual zoom (0.9.2); DARKNESS_FOV_SMOOTHING=1 or an
// fov_smoothing.txt beside TheDarkness.exe (play tests) turns it on.
std::atomic<bool> view_fov_smoothing{false};
// Play-test evidence for #6 (fov_zoom_log.txt beside TheDarkness.exe): one
// RUNTIME_FOV_ZOOM line per frame while the title's FOV or the shown FOV is
// moving (a zoom), on the console/session log; bounded per session.
std::atomic<bool> view_fov_zoom_log{false};
std::atomic<uint32_t> view_fov_zoom_log_lines{0};
constexpr uint32_t kViewFovZoomLogLines = 60000;
// Developer trace of the per-frame FOV (DARKNESS_FOV_TRACE=<file>), bounded.
std::atomic<bool> view_fov_trace_enabled{};
std::mutex view_fov_trace_mutex;
std::ofstream view_fov_trace;      // view_fov_trace_mutex
uint32_t view_fov_trace_lines = 0;  // view_fov_trace_mutex
std::atomic<bool> trace_camera_state{};
std::atomic<uint32_t> camera_trace_sequence{};
std::atomic<uint32_t> frame_viewport_trace_sequence{};
std::atomic<uint32_t> transform_input_trace_sequence{};
RuntimeCameraCaptureWindow capture_window; // camera_trace_mutex owned
RuntimeMotionSourceBudget motion_source_budget; // camera_trace_mutex owned
RuntimePacketSourceBudget packet_source_budget; // same bounded manual window
RuntimeMotionProducerBudget motion_producer_budget; // camera_trace_mutex owned
runtime_owned_camera_source::Store owned_camera_sources; // camera_trace_mutex owned
runtime_owned_camera_source::ConstantStore owned_camera_constants;
runtime_owned_camera_source::PacketConstantStore owned_camera_packets;
runtime_owned_camera_source::SelectionStats owned_source_selection_stats;
uint64_t owned_source_last_publication{}; // camera_trace_mutex owned, not a frame ID
RuntimeOwnedCameraFollowup owned_flow_followup; // All census state owns camera_trace_mutex.
struct OwnedFlowCensus {
    runtime_owned_camera_source::SelectionStats selection;
    uint64_t producer{}, producer_invalid{}, published{}, watched_items{};
    uint64_t constant_attempt{}, constant_ineligible{}, constant_stale{}, constant_mismatch{}, constants{};
    uint64_t packets{}, reads{}, read_owned{};
} owned_flow;
thread_local bool owned_constant_copy_active = false;
struct MotionBindingObservation {
    const void* context{};
    uint64_t generation{};
    uint32_t sequence{}, record{}, primary{}, matrices{};
};
thread_local MotionBindingObservation motion_binding_observation;
thread_local RuntimeTransformObservationTicket transform_observation_ticket;
std::atomic<uint32_t> main_viewport_object{};
std::mutex camera_trace_mutex;

void ReportOwnedFlow() {
    if (!owned_flow_followup.Tick()) return;
    const auto& s = owned_flow.selection;
    std::ostringstream out;
    out << "PC_OWNED_FLOW cpu_viewport_ticks=" << owned_flow_followup.Ticks()
        << " last_source=" << owned_source_last_publication
        << " producer=" << owned_flow.producer << " producer_invalid=" << owned_flow.producer_invalid
        << " published=" << owned_flow.published << " watched_items=" << owned_flow.watched_items
        << " selection_calls=" << s.calls << " candidates=" << s.candidates
        << " eligible=" << s.eligible << " selected=" << s.selected << " ambiguous=" << s.ambiguous
        << " invalid_record=" << s.invalid_record << " invalid_arena=" << s.invalid_arena
        << " invalid_backing=" << s.invalid_backing;
    constexpr const char* names[] = {"arena_header", "arena_cursor", "prefix", "head", "mode", "optional"};
    for (size_t i = 0; i < 6; ++i)
        out << " missing_" << names[i] << '=' << s.missing[i]
            << " changed_" << names[i] << '=' << s.changed[i];
    constexpr const char* reasons[] = {"untyped", "range", "overlap", "equal", "backward", "limit", "fault"};
    for (size_t i = 0; i < 7; ++i) out << " cursor_" << reasons[i] << '=' << s.cursor_reasons[i];
    const auto& retired = s.last_cursor_retirement;
    out << " cursor_unknown=" << s.cursor_reason_unknown
        << " cursor_last_ticket=" << retired.ticket.id << " cursor_last_reasons=" << retired.reasons
        << " cursor_last_previous=" << retired.previous << " cursor_last_stored=" << retired.stored
        << " cursor_last_limit=" << retired.limit << " cursor_last_address=" << retired.watched.address
        << " cursor_last_write_address=" << retired.write.address << " cursor_last_write_bytes=" << retired.write.bytes;
    out << " constant_attempt=" << owned_flow.constant_attempt
        << " constant_ineligible=" << owned_flow.constant_ineligible
        << " constant_stale=" << owned_flow.constant_stale
        << " constant_mismatch=" << owned_flow.constant_mismatch
        << " constants=" << owned_flow.constants << " packets=" << owned_flow.packets
        << " reads=" << owned_flow.reads << " read_owned=" << owned_flow.read_owned
        << " source_store=" << owned_camera_sources.Size()
        << " scope=interval_cpu_stage_census_not_rendered_frame\n";
    std::cerr << out.str(); // No payload lock or guest reads during these three writes.
    owned_flow = {};
}

float FloatFromBits(uint32_t bits) noexcept {
    float value{};
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

uint32_t FloatBits(float value) noexcept {
    uint32_t bits{};
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

void WriteMotionProducerHeader(std::ostream& out) {
    out << "MOTION_PRODUCER_CAPTURE version=1 generation=" << motion_producer_budget.generation
        << " maximum_records=" << RuntimeMotionProducerBudget::kMaximumRecords
        << " maximum_copies=" << RuntimeMotionProducerBudget::kMaximumCopies
        << " maximum_list_records=64 scope=native_builder_endpoint_not_temporal_history\n";
}

void TraceCompactMotionOutput(PPCContext& context, uint8_t* base) {
    const bool log_capture = trace_camera_state.load(std::memory_order_relaxed) &&
        motion_producer_budget.Select(capture_window, &context);
    if (!log_capture && !RuntimeOwnedCameraInputMode().Enabled()) return;
    if (owned_flow_followup.Active()) ++owned_flow.producer;
    const uint32_t sequence = motion_producer_budget.records - 1u;
    std::optional<RuntimeSourceMemoryCoordinator::Snapshot> source_snapshot;
    source_snapshot.emplace(RuntimeGuestSourceCoordinator());
    if (owned_camera_sources.Size() == runtime_owned_camera_source::Store::kCapacity)
        owned_camera_sources.RetireIf([&](const auto& value) {
            return runtime_owned_camera_source::PublicationRetired(*source_snapshot, value);
        });
    const uint32_t count = context.r5.u32, stack = context.r1.u32;
    const uint32_t job = context.r16.u32, inputs = context.r18.u32;
    const uint32_t descriptor = context.r19.u32, buffer = context.r26.u32;
    const uint32_t compact = context.r29.u32;
    uint32_t render_owner = 0, camera = 0, primary = 0, allocator = 0;
    uint32_t input12 = 0, input16 = 0, input20 = 0, vtable = 0, modes = 0;
    //82276148 preserves these nonvolatile registers through the qualified
    //8259D3B8 call. Its one allocation contains128 vector bytes,64 metadata
    //bytes and count consecutive48-byte records. No address matching heuristic.
    bool valid = RuntimeMotionSourceRange(job, 24u) &&
        RuntimeMotionSourceRange(inputs, count * 20u) &&
        RuntimeMotionSourceRange(descriptor, 24u) &&
        RuntimeMotionSourceRange(buffer, (count + 4u) * 48u, 16u) &&
        compact == buffer + 148u && context.r17.u32 == count;
    if (valid) {
        render_owner = PPC_LOAD_U32(job + 16u);
        allocator = PPC_LOAD_U32(job + 20u);
        primary = PPC_LOAD_U32(inputs + 12u);
        input12 = PPC_LOAD_U32(descriptor + 12u);
        input16 = PPC_LOAD_U32(descriptor + 16u);
        input20 = PPC_LOAD_U32(descriptor + 20u);
        vtable = PPC_LOAD_U32(compact);
        modes = PPC_LOAD_U32(compact + 28u);
        valid = allocator == context.r3.u32 && PPC_LOAD_U16(descriptor + 10u) == count &&
            vtable == 0x82067764u && modes == 0x01010404u &&
            PPC_LOAD_U32(compact + 24u) == buffer &&
            RuntimeMotionSourceRange(render_owner, 228u);
        if (valid) {
            camera = PPC_LOAD_U32(render_owner + 224u);
            valid = RuntimeMotionSourceRange(camera, 2016u, 16u);
        }
    }
    const bool composed = input20 && input16;
    struct MatrixObservation {
        const char* role;
        uint32_t source;
        bool valid{};
        uint32_t words[16]{};
    } matrices[] = {
        {"primary", primary},
        {"camera_1760", valid ? camera + 1760u : 0u},
        {"camera_1824", valid ? camera + 1824u : 0u},
        {"camera_1888", valid ? camera + 1888u : 0u},
        {"camera_1952", valid ? camera + 1952u : 0u},
        {"descriptor_12", composed ? input12 : 0u},
        {"descriptor_16", composed ? input16 : 0u},
        {"descriptor_20", composed ? input20 : 0u},
        {"native_inverse_stack", composed ? stack + 96u : 0u},
    };
    bool all_valid = valid;
    for (auto& matrix : matrices) {
        matrix.valid = valid && (!matrix.source || RuntimeMotionSourceRange(matrix.source, 64u, 16u));
        all_valid = all_valid && matrix.valid;
        if (matrix.source && matrix.valid && (log_capture || &matrix == &matrices[1]))
            for (uint32_t i = 0; i < 16u; ++i)
            matrix.words[i] = PPC_LOAD_U32(matrix.source + i * 4u);
    }
    all_valid = all_valid && primary && (!composed || input12);
    uint32_t packed[32]{};
    if (valid) for (uint32_t i = 0; i < 32u; ++i) packed[i] = PPC_LOAD_U32(buffer + i * 4u);
    struct RecordObservation {
        uint32_t address{}, source{}, flags{}, geometry{}, shader{}, primary{}, mode{}, optional{};
        bool valid{};
    } records[64]{};
    for (uint32_t i = 0; i < count; ++i) {
        auto& record = records[i];
        record.address = PPC_LOAD_U32(context.r4.u32 + i * 4u);
        record.source = valid ? inputs + i * 20u : 0u;
        record.valid = valid && record.address == buffer + 192u + i * 48u;
        if (record.valid) {
            record.flags = PPC_LOAD_U16(record.address);
            record.geometry = PPC_LOAD_U32(record.address + 4u);
            record.shader = PPC_LOAD_U32(record.address + 8u);
            record.primary = PPC_LOAD_U32(record.address + 12u);
            record.mode = PPC_LOAD_U32(record.address + 32u);
            record.optional = PPC_LOAD_U32(record.address + 44u);
            record.valid = (record.flags & 64u) && record.mode == compact && !record.optional &&
                record.geometry == PPC_LOAD_U32(record.source + 4u) &&
                record.shader == PPC_LOAD_U32(record.source + 8u) &&
                record.primary == PPC_LOAD_U32(record.source + 12u);
        }
        all_valid = all_valid && record.valid;
    }
    uint64_t owned_source_publication = 0;
    runtime_owned_camera_source::Source published_source;
    if (all_valid) {
        runtime_owned_camera_source::Publication source;
        source.job = job;
        source.render_owner = render_owner;
        source.camera = camera;
        source.buffer = buffer;
        source.compact = compact;
        source.count = count;
        if (RuntimeMotionSourceRange(allocator, 1392u)) {
            source.arena = runtime_owned_camera_source::WatchArena(*source_snapshot,
                allocator, PPC_LOAD_U32(allocator + 8u), PPC_LOAD_U32(allocator + 12u),
                PPC_LOAD_U32(allocator + 1388u), PPC_LOAD_U32(allocator + 1384u),
                buffer, 192u + count * 48u);
        }
        uint32_t physical{};
        RuntimeSourceWriteEpochs::Ticket prefix;
        if (RuntimeGraphicsGuestPhysicalRange(buffer, 192u + count * 48u, physical)) {
            const auto backing = GetGuestMemoryAccounting().IdentityForRange(
                physical, 192u + count * 48u);
            source.backing = {backing.generation, backing.physical_base, backing.bytes};
            if (backing.generation) prefix = source_snapshot->Watch({physical, 192});
        }
        for (uint32_t i = 0; i < 16u; ++i)
            source.camera_current[i] = matrices[1].words[i];
        for (uint32_t i = 0; i < 32u; ++i) source.packed[i] = packed[i];
        for (uint32_t i = 0; i < count; ++i) {
            const auto& record = records[i];
            source.items[i] = {record.address, record.primary, record.geometry, record.shader};
            source.writes[i] = runtime_owned_camera_source::WatchItem(
                *source_snapshot, prefix, record.address);
            if (owned_flow_followup.Active() &&
                runtime_owned_camera_source::ArenaUnchanged(*source_snapshot, source.arena) &&
                runtime_owned_camera_source::WritesUnchanged(*source_snapshot, source.writes[i]))
                ++owned_flow.watched_items;
        }
        owned_source_publication = owned_camera_sources.Publish(source);
        if (owned_source_publication) owned_source_last_publication = owned_source_publication;
        owned_camera_sources.Copy({owned_source_publication, 0}, published_source);
    }
    if (owned_flow_followup.Active()) {
        if (!all_valid) ++owned_flow.producer_invalid;
        if (owned_source_publication) ++owned_flow.published;
    }
    // All guest values are owned above; never hold the payload lock during I/O.
    source_snapshot.reset();
    if (!log_capture) return; // Continuous renderer input never implies continuous I/O.
    if (!all_valid) ++motion_producer_budget.invalid;
    std::error_code directory_error;
    std::filesystem::create_directories("logs", directory_error);
    std::ofstream out("logs/pc_motion_producers.log", sequence ? std::ios::app : std::ios::trunc);
    if (!sequence) WriteMotionProducerHeader(out);
    out << "MOTION_PRODUCER sequence=" << sequence << " generation=" << motion_producer_budget.generation
        << " window_copy_sequence=" << capture_window.copies - 1u
        << " main_copy_context=" << (capture_window.owner == &context ? 1 : 0)
        << " function=82276148 endpoint=8259d3b8 caller=" << std::hex << uint32_t(context.lr)
        << " stack=" << stack << " job=" << job << " inputs=" << inputs
        << " descriptor=" << descriptor << " allocator=" << allocator
        << " render_owner=" << render_owner << " camera=" << camera
        << " buffer=" << buffer << " compact=" << compact << " vtable=" << vtable
        << " modes=" << modes << " input12=" << input12 << " input16=" << input16 << " input20=" << input20
        << " list=" << context.r4.u32 << " count=" << std::dec << count
        << " valid=" << (all_valid ? 1 : 0)
        << " owned_source_publication=" << owned_source_publication
        << " backing_generation=" << published_source.backing.generation
        << " arena_batch=" << published_source.arena.batch
        << " arena_header_ticket=" << published_source.arena.header.id
        << " arena_cursor_ticket=" << published_source.arena.cursor_write.id
        << " prefix_ticket=" << published_source.writes.prefix.id
        << " head_ticket=" << published_source.writes.record_head.id
        << " mode_ticket=" << published_source.writes.record_mode.id
        << " optional_ticket=" << published_source.writes.record_optional.id
        << " scope=completed_vector_and_record_construction_before_list_publication\n";
    // Source values are snapshots at this endpoint. Their temporal meaning and
    // equality with earlier reads must be established separately; no history ID.
    for (const auto& matrix : matrices) {
        out << "MOTION_PRODUCER_MATRIX sequence=" << sequence << " role=" << matrix.role
            << " source=" << std::hex << matrix.source << " present=" << (matrix.source ? 1 : 0)
            << " valid=" << (matrix.valid ? 1 : 0) << " words=";
        if (matrix.source && matrix.valid) for (uint32_t i = 0; i < 16u; ++i)
            out << (i ? "," : "") << std::setw(8) << std::setfill('0') << matrix.words[i];
        out << std::dec << '\n';
    }
    out << "MOTION_PRODUCER_PACKED sequence=" << sequence << " source=" << std::hex << buffer
        << " valid=" << (valid ? 1 : 0) << " words=";
    if (valid) for (uint32_t i = 0; i < 32u; ++i)
        out << (i ? "," : "") << std::setw(8) << std::setfill('0') << packed[i];
    out << std::dec << '\n';
    for (uint32_t i = 0; i < count; ++i) {
        const auto& record = records[i];
        out << "MOTION_PRODUCER_RECORD sequence=" << sequence << " index=" << i
            << " address=" << std::hex << record.address << " source=" << record.source
            << " flags=" << record.flags << " geometry=" << record.geometry << " shader=" << record.shader
            << " primary=" << record.primary << " mode=" << record.mode << " optional=" << record.optional
            << " valid=" << (record.valid ? 1 : 0) << std::dec << '\n';
    }
    out.close();
    if (directory_error || out.fail()) ++motion_producer_budget.write_failures;
    RuntimeMotionOriginsObserve(context, base, 0x8259D3B8u, capture_window, sequence);
}

// Observe native inputs only. The binding label is the most recently observed
// render-state entry in this CPU context, not a persistent object or GPU-frame
// identity. Actual pool/source values must be joined offline without guessing.
void TracePacketSource(PPCContext& context, uint8_t* base) {
    if (!packet_source_budget.Select(capture_window, &context)) return;
    const uint32_t sequence = packet_source_budget.records - 1;
    const uint32_t builder = context.r3.u32;
    uint32_t begin = 0, cursor = 0, bytes = 0, physical = UINT32_MAX, flags = 0;
    bool valid = RuntimeMotionSourceRange(builder, 14908u, 4u);
    if (valid) {
        begin = PPC_LOAD_U32(builder + 14904u);
        cursor = PPC_LOAD_U32(builder + 48u);
        flags = PPC_LOAD_U8(builder + 10941u);
        valid = RuntimePacketSegmentBytes(begin, cursor, bytes) && packet_source_budget.Reserve(bytes);
    }
    std::vector<uint8_t> snapshot;
    if (valid && bytes) {
        snapshot.resize(bytes);
        valid = RuntimeGraphicsCopyPacketBytes(begin, bytes, snapshot.data(), physical);
    }
    if (!valid) ++packet_source_budget.invalid;
    std::error_code directory_error;
    std::filesystem::create_directories("logs", directory_error);
    const auto file = "logs/pc_packet_source_" + std::to_string(sequence) + ".bin";
    bool written = valid;
    uint32_t hash = 2166136261u;
    if (valid && bytes) {
        for (uint8_t byte : snapshot) { hash ^= byte; hash *= 16777619u; }
        std::ofstream payload(file, std::ios::binary | std::ios::trunc);
        payload.write(reinterpret_cast<const char*>(snapshot.data()), snapshot.size());
        payload.close();
        written = !payload.fail();
    }
    std::ofstream out("logs/pc_packet_sources.log", sequence ? std::ios::app : std::ios::trunc);
    if (!sequence) out << "PACKET_SOURCE_CAPTURE version=1 generation=" << packet_source_budget.generation
        << " max_segments=128 max_segment_bytes=262144 max_bytes=8388608 max_copies=4\n";
    out << "PACKET_SOURCE sequence=" << sequence << " copy=" << (capture_window.copies - 1)
        << " generation=" << packet_source_budget.generation << " caller=" << std::hex << context.lr
        << " builder=" << builder << " arena=" << capture_window.source_arena << " begin=" << begin
        << " cursor=" << cursor << " physical=" << physical << " flags=" << flags << " hash=" << hash
        << std::dec << " bytes=" << bytes << " valid=" << valid << " written=" << written
        << " path=" << ((valid && bytes) ? file : "none")
        << " scope=pre_publication_segment_not_camera_identity\n";
    out.close();
    if (directory_error || !written || out.fail()) ++packet_source_budget.write_failures;
}

void TraceMotionSourceBoundary(PPCContext& context) {
    if (packet_source_budget.FinishBeforeCopy(capture_window, &context)) {
        std::ofstream out("logs/pc_packet_sources.log", std::ios::app);
        out << "PACKET_SOURCE_END generation=" << packet_source_budget.generation
            << " records=" << packet_source_budget.records << " bytes=" << packet_source_budget.bytes
            << " dropped=" << packet_source_budget.dropped << " invalid=" << packet_source_budget.invalid
            << " write_failures=" << packet_source_budget.write_failures
            << " boundary=next_qualified_viewport_copy\n";
        out.close();
        if (out.fail()) ++packet_source_budget.write_failures;
        std::cerr << "PC_PACKET_SOURCE_CAPTURE generation=" << packet_source_budget.generation
            << " records=" << packet_source_budget.records << " dropped=" << packet_source_budget.dropped
            << " invalid=" << packet_source_budget.invalid << " write_failures=" << packet_source_budget.write_failures
            << " closed=1 path=logs/pc_packet_sources.log\n";
    }

    RuntimeMotionOriginsFinish(context, capture_window);
    RuntimeMotionDeltaFinish(context, capture_window);
    RuntimeTemporalCameraFinish(context, capture_window);
    if (motion_producer_budget.FinishBeforeCopy(capture_window, &context)) {
        std::ofstream out("logs/pc_motion_producers.log",
            motion_producer_budget.records ? std::ios::app : std::ios::trunc);
        if (!motion_producer_budget.records) WriteMotionProducerHeader(out);
        out << "MOTION_PRODUCER_END generation=" << motion_producer_budget.generation
            << " records=" << motion_producer_budget.records << " dropped=" << motion_producer_budget.dropped
            << " invalid=" << motion_producer_budget.invalid << " write_failures=" << motion_producer_budget.write_failures
            << " copies=" << capture_window.copies << " boundary=next_qualified_viewport_copy\n";
        out.close();
        if (out.fail()) ++motion_producer_budget.write_failures;
        std::cerr << "PC_MOTION_PRODUCER_CAPTURE generation=" << motion_producer_budget.generation
            << " records=" << motion_producer_budget.records << " dropped=" << motion_producer_budget.dropped
            << " invalid=" << motion_producer_budget.invalid << " write_failures=" << motion_producer_budget.write_failures
            << " closed=1 boundary=next_qualified_viewport_copy path=logs/pc_motion_producers.log\n";
    }
    if (motion_source_budget.FinishBeforeCopy(capture_window, &context)) {
        if (RuntimeOwnedCameraInputMode().Enabled()) owned_flow_followup.Arm();
        std::ofstream out("logs/pc_motion_matrix_sources.log", std::ios::app);
        const auto& selection = owned_source_selection_stats;
        out << "MATRIX_SOURCE_SELECTION calls=" << selection.calls
            << " invalid_record=" << selection.invalid_record
            << " candidates=" << selection.candidates << " eligible=" << selection.eligible
            << " ambiguous=" << selection.ambiguous << " selected=" << selection.selected
            << " invalid_arena=" << selection.invalid_arena
            << " invalid_backing=" << selection.invalid_backing;
        constexpr const char* names[] = {"arena_header", "arena_cursor", "prefix", "head", "mode", "optional"};
        for (size_t i = 0; i < 6; ++i)
            out << " missing_" << names[i] << '=' << selection.missing[i]
                << " changed_" << names[i] << '=' << selection.changed[i];
        out << " scope=bounded_native_selection_rejection_census\n";
        out << "MATRIX_SOURCE_END generation=" << motion_source_budget.generation
            << " owned_source_last_publication=" << owned_source_last_publication
            << " records=" << motion_source_budget.records
            << " dropped=" << motion_source_budget.dropped
            << " invalid=" << motion_source_budget.invalid
            << " write_failures=" << motion_source_budget.write_failures
            << " copies=" << capture_window.copies
            << " boundary=next_qualified_viewport_copy"
            << " scope=cpu_input_neighborhood_not_gpu_frame_or_object_history\n";
        out.close();
        if (out.fail()) ++motion_source_budget.write_failures;
        std::cerr << "PC_MATRIX_SOURCE_CAPTURE generation=" << motion_source_budget.generation
            << " records=" << motion_source_budget.records
            << " dropped=" << motion_source_budget.dropped
            << " invalid=" << motion_source_budget.invalid
            << " write_failures=" << motion_source_budget.write_failures
            << " closed=1 boundary=next_qualified_viewport_copy path=logs/pc_motion_matrix_sources.log\n";
    }
    motion_binding_observation = {};
}

void TraceMotionSourceInputs(PPCContext& context, uint8_t* base, uint32_t address) {
    const bool binding = address == 0x82299E30u;
    if (!binding && (address != 0x8224A2E8u ||
        !RuntimeIsMotionConstantCopy(uint32_t(context.lr), context.r1.u32,
            context.r3.u32, context.r5.u32, context.r6.u32, context.r7.u32))) return;
    // Preserve all existing startup and partial-frame capture gates. Do not
    // inspect guest pointers before the selected manual CPU neighborhood.
    if (!capture_window.manual || !capture_window.generation ||
        capture_window.owner != &context || motion_source_budget.closed) return;
    if (!binding && !PPC_LOAD_U16(context.r3.u32 + 20u)) return;
    if (!motion_source_budget.Select(capture_window, &context)) return;
    const uint32_t sequence = motion_source_budget.records - 1u;
    constexpr uint32_t gpu_state = 0x82A69B00u;
    const uint32_t pool = PPC_LOAD_U32(gpu_state + 8224u);
    const uint32_t pool_index = PPC_LOAD_U32(gpu_state + 8232u);
    const uint32_t pool_matrix = RuntimeObservedTransformAddress(pool, pool_index);
    uint32_t object = 0, primary = 0, matrix_array = context.r6.u32;
    bool valid = true;
    if (binding) {
        object = context.r3.u32;
        valid = RuntimeMotionSourceRange(object, 48u);
        if (valid) {
            // Raw82299E4C/E50 forwards these exact fields to82763D50.
            primary = PPC_LOAD_U32(object + 12u);
            matrix_array = PPC_LOAD_U32(object + 44u);
        } else {
            matrix_array = 0;
        }
        motion_binding_observation = {&context, motion_source_budget.generation,
            sequence, object, primary, matrix_array};
    }
    const bool have_binding = motion_binding_observation.context == &context &&
        motion_binding_observation.generation == motion_source_budget.generation;
    uint32_t matrix_addresses[8]{}, matrix_words[8][16]{};
    bool matrix_valid[8]{};
    valid = valid && (!matrix_array || RuntimeMotionSourceRange(matrix_array, 32u));
    for (uint32_t slot = 0; slot < 8; ++slot) {
        if (valid && matrix_array) matrix_addresses[slot] = PPC_LOAD_U32(matrix_array + slot * 4u);
        const uint32_t source = matrix_addresses[slot];
        matrix_valid[slot] = valid && (!source || RuntimeMotionSourceRange(source, 64u, 16u));
        if (source && matrix_valid[slot]) {
            for (uint32_t word = 0; word < 16; ++word)
                matrix_words[slot][word] = PPC_LOAD_U32(source + word * 4u);
        }
    }
    bool all_valid = valid;
    for (bool slot_valid : matrix_valid) all_valid = all_valid && slot_valid;
    uint32_t vector_source = 0, vector_mask = 0, vector_count = 0, vector_words[32]{};
    uint32_t vector_default_zero = 0, vector_default_one = 0;
    bool vector_pair = false, vector_valid = true;
    if (!binding && all_valid) {
        vector_pair = RuntimeIsMotionVectorPair(PPC_LOAD_U8(context.r7.u32 + 48u),
            PPC_LOAD_U8(context.r7.u32 + 49u), matrix_addresses[0], matrix_addresses[1],
            PPC_LOAD_U16(context.r3.u32 + 20u));
        if (vector_pair) {
            // Raw8224A3AC selects this stream;8224A51C selects low-to-high
            // nibble bits and advances only for present source vectors.
            vector_source = PPC_LOAD_U32(context.r7.u32 + 76u);
            vector_mask = PPC_LOAD_U32(context.r7.u32 + 44u);
            // Exact scalar constants loaded at8224A3B4/B8, preserved as bits.
            vector_default_zero = PPC_LOAD_U32(0x8209DCBCu);
            vector_default_one = PPC_LOAD_U32(0x8205C06Cu);
            vector_count = vector_source ? RuntimeMotionVectorPairCount(vector_mask) : 0u;
            vector_valid = !vector_count || RuntimeMotionSourceRange(vector_source, vector_count * 16u, 16u);
            if (vector_valid) for (uint32_t word = 0; word < vector_count * 4u; ++word)
                vector_words[word] = PPC_LOAD_U32(vector_source + word * 4u);
        }
    }
    all_valid = all_valid && vector_valid;
    if (!all_valid) ++motion_source_budget.invalid;
    std::error_code directory_error;
    std::filesystem::create_directories("logs", directory_error);
    std::ofstream out("logs/pc_motion_matrix_sources.log",
        sequence ? std::ios::app : std::ios::trunc);
    if (!sequence) {
        out << "MATRIX_SOURCE_CAPTURE version=3 generation=" << motion_source_budget.generation
            << " maximum_records=" << RuntimeMotionSourceBudget::kMaximumRecords
            << " maximum_copies=" << RuntimeMotionSourceBudget::kMaximumCopies
            << " scope=cpu_input_neighborhood_not_gpu_frame_or_object_history\n";
    }
    out << "MATRIX_SOURCE_INPUT sequence=" << sequence
        << " generation=" << motion_source_budget.generation
        << " copy_sequence=" << capture_window.copies - 1u
        << " stage=" << (binding ? "render_binding_entry" : "constant_copy_entry")
        << " function=" << std::hex << address << " caller=" << uint32_t(context.lr)
        << " copy_allocator=" << capture_window.source_arena
        << " stack=" << context.r1.u32 << " record=" << object
        << " primary=" << primary << " matrix_array=" << matrix_array
        << " pool=" << pool << " pool_index=" << pool_index << " pool_matrix=" << pool_matrix
        << " builder=" << PPC_LOAD_U32(gpu_state + 15748u)
        << " descriptor=" << (binding ? 0u : context.r3.u32)
        << " declared_vectors=" << std::dec << (binding ? 0u : uint32_t(PPC_LOAD_U16(context.r3.u32 + 20u)))
        << " last_binding_sequence=" << (have_binding ? motion_binding_observation.sequence : UINT32_MAX)
        << " last_binding_record=" << std::hex << (have_binding ? motion_binding_observation.record : 0u)
        << " last_binding_matrix_array=" << (have_binding ? motion_binding_observation.matrices : 0u)
        << std::dec << " submission_source_publication="
        << runtime_owned_camera_source::SubmissionScope::Current(&context).publication
        << " submission_source_item="
        << runtime_owned_camera_source::SubmissionScope::Current(&context).item
        << " valid=" << (all_valid ? 1 : 0) << " modes=" << std::hex;
    if (!binding) {
        // Raw82248C80 selects optional pool matrices using these same eight
        // mode bytes;8224A2E8 also consumes them for additional vector packing.
        for (uint32_t slot = 0; slot < 8; ++slot)
            out << (slot ? "," : "") << std::setw(2) << std::setfill('0')
                << uint32_t(PPC_LOAD_U8(context.r7.u32 + 48u + slot));
    } else {
        out << "absent";
    }
    out << " scope=observed_native_inputs_not_completed_copy_or_history\n";
    for (uint32_t slot = 0; slot < 8; ++slot) {
        out << "MATRIX_SOURCE_SLOT sequence=" << std::dec << sequence << " slot=" << slot
            << " source=" << std::hex << matrix_addresses[slot]
            << " present=" << (matrix_addresses[slot] ? 1 : 0)
            << " valid=" << (matrix_valid[slot] ? 1 : 0) << " words=";
        if (matrix_addresses[slot] && matrix_valid[slot]) {
            for (uint32_t word = 0; word < 16; ++word)
                out << (word ? "," : "") << std::setw(8) << std::setfill('0') << matrix_words[slot][word];
        }
        out << '\n';
    }
    out << "MATRIX_SOURCE_VECTOR_STREAM sequence=" << std::dec << sequence
        << " eligible=" << (vector_pair ? 1 : 0) << " source=" << std::hex << vector_source
        << " mask=" << vector_mask << " default_zero=" << vector_default_zero
        << " default_one=" << vector_default_one << " vectors=" << std::dec << vector_count
        << " valid=" << (vector_valid ? 1 : 0) << " words=" << std::hex;
    if (vector_valid) for (uint32_t word = 0; word < vector_count * 4u; ++word)
        out << (word ? "," : "") << std::setw(8) << std::setfill('0') << vector_words[word];
    out << '\n';
    out.close();
    if (directory_error || out.fail()) ++motion_source_budget.write_failures;
}

bool IsViewportSetter(uint32_t address) noexcept {
    return address == 0x82389CA0u || address == 0x82389CB8u ||
        address == 0x82389CC8u || address == 0x82389CE8u ||
        address == 0x82389CF8u;
}

void TraceCameraSetter(PPCContext& context, uint8_t* base, uint32_t address,
                       uint32_t objectVtable) {
    if (!trace_camera_state.load(std::memory_order_relaxed) ||
        !IsViewportSetter(address)) {
        return;
    }
    const uint32_t sequence =
        camera_trace_sequence.fetch_add(1, std::memory_order_relaxed);
    if (sequence >= 128u) return;
    const uint32_t object = context.r3.u32;
    const uint32_t mainObject = main_viewport_object.load(std::memory_order_acquire);
    std::lock_guard lock(camera_trace_mutex);
    std::filesystem::create_directories("logs");
    std::ofstream out("logs/pc_camera_state.log",
                      sequence ? std::ios::app : std::ios::trunc);
    out << "CAMERA_SETTER sequence=" << sequence
        << " function=0x" << std::hex << address
        << " caller=0x" << context.lr
        << " object=0x" << object
        << " vtable=0x" << objectVtable
        << " main=" << (object && object == mainObject ? 1 : 0)
        << std::dec << " value=" << static_cast<float>(context.f1.f64);
    if (object && object <= 0xFFFFFE40u) {
        out << " target_fov="
            << FloatFromBits(PPC_LOAD_U32(object + 276u))
            << " current_fov="
            << FloatFromBits(PPC_LOAD_U32(object + 436u))
            << " aspect_scale="
            << FloatFromBits(PPC_LOAD_U32(object + 288u));
    }
    out << '\n';
}
}

bool RuntimeCopyOwnedCameraSource(runtime_owned_camera_source::Token token,
                                 runtime_owned_camera_source::Source& output) {
    std::lock_guard lock(camera_trace_mutex);
    return owned_camera_sources.Copy(token, output);
}

bool RuntimeSelectOwnedCameraSource(uint32_t record,
                                   runtime_owned_camera_source::Source& output) {
    if (!RuntimeGuestSourceCoordinator().TrackingEnabled()) return false;
    const bool continuous = RuntimeOwnedCameraInputMode().Enabled();
    if (!continuous && !trace_camera_state.load(std::memory_order_relaxed)) return false;
    std::lock_guard lock(camera_trace_mutex);
    const bool diagnostic = capture_window.manual && capture_window.generation &&
        capture_window.copies && !motion_source_budget.closed;
    if (!continuous && !diagnostic) return false;
    RuntimeSourceMemoryCoordinator::Snapshot snapshot(RuntimeGuestSourceCoordinator());
    return runtime_owned_camera_source::SelectWatchedSource(owned_camera_sources,
        snapshot, record, [](const runtime_owned_camera_source::BackingAllocation& backing,
                             uint32_t physical, uint32_t bytes) {
            return GetGuestMemoryAccounting().IdentityMatches(
                {backing.generation, backing.physical_base, backing.bytes}, physical, bytes);
        }, output, owned_flow_followup.Active() ? &owned_flow.selection :
                   diagnostic ? &owned_source_selection_stats : nullptr);
}

bool RuntimeCopyActiveOwnedCameraSource(const PPCContext& context,
                                       runtime_owned_camera_source::Source& output) {
    return RuntimeCopyUnchangedOwnedCameraSource(
        runtime_owned_camera_source::SubmissionScope::Current(&context), output);
}

bool RuntimeCopyUnchangedOwnedCameraSource(runtime_owned_camera_source::Token token,
                                          runtime_owned_camera_source::Source& output) {
    std::lock_guard lock(camera_trace_mutex);
    RuntimeSourceMemoryCoordinator::Snapshot snapshot(RuntimeGuestSourceCoordinator());
    runtime_owned_camera_source::Source candidate;
    if (!owned_camera_sources.Copy(token, candidate) ||
        !runtime_owned_camera_source::ArenaUnchanged(snapshot, candidate.arena) ||
        !runtime_owned_camera_source::WritesUnchanged(snapshot, candidate.writes)) return false;
    uint32_t physical{};
    if (!RuntimeGraphicsGuestPhysicalRange(candidate.native_item.record, 48, physical) ||
        !GetGuestMemoryAccounting().IdentityMatches(
            {candidate.backing.generation, candidate.backing.physical_base, candidate.backing.bytes},
            physical, 48)) return false;
    output = candidate;
    return true;
}

void RuntimeRunOwnedCameraConstantCopy(PPCContext& context, uint8_t* base,
                                      void (*native_copy)(PPCContext&, uint8_t*)) {
    using namespace runtime_owned_camera_source;
    const auto token = SubmissionScope::Current(&context);
    if (!token.publication || owned_constant_copy_active ||
        !RuntimeIsMotionConstantCopy(uint32_t(context.lr), context.r1.u32,
            context.r3.u32, context.r5.u32, context.r6.u32, context.r7.u32)) {
        native_copy(context, base); return;
    }
    std::unique_lock lock(camera_trace_mutex);
    if (owned_flow_followup.Active()) ++owned_flow.constant_attempt;
    std::optional<RuntimeSourceMemoryCoordinator::Snapshot> snapshot;
    snapshot.emplace(RuntimeGuestSourceCoordinator());
    Source source;
    const auto source_valid = [&] {
        uint32_t physical{};
        return owned_camera_sources.Copy(token, source) &&
            ArenaUnchanged(*snapshot, source.arena) && WritesUnchanged(*snapshot, source.writes) &&
            RuntimeCanonicalGuestPhysicalRange(source.native_item.record, 48, physical) &&
            GetGuestMemoryAccounting().IdentityMatches({source.backing.generation,
                source.backing.physical_base, source.backing.bytes}, physical, 48);
    };
    uint32_t destination = 0, physical = 0;
    bool eligible = source_valid();
    if (eligible) {
        const auto modes = context.r7.u32;
        const auto builder = PPC_LOAD_U32(0x82A69B00u + 15748u);
        eligible = RuntimeMotionSourceRange(builder, 2240u, 16u) &&
            PPC_LOAD_U16(context.r3.u32 + 20u) == 8 &&
            RuntimeIsMotionVectorPair(PPC_LOAD_U8(modes + 48), PPC_LOAD_U8(modes + 49),
                PPC_LOAD_U32(context.r6.u32), PPC_LOAD_U32(context.r6.u32 + 4), 8) &&
            PPC_LOAD_U32(modes + 76) == source.buffer &&
            (PPC_LOAD_U32(modes + 44) & 255u) == 255u;
        for (uint32_t i = 2; eligible && i < 8; ++i)
            eligible = PPC_LOAD_U8(modes + 48 + i) == 4;
        if (eligible) {
            destination = builder + 2112;
            eligible = RuntimeCanonicalGuestPhysicalRange(destination, 128, physical);
        }
    }
    if (!eligible) {
        if (owned_flow_followup.Active()) ++owned_flow.constant_ineligible;
        snapshot.reset(); lock.unlock(); native_copy(context, base); return;
    }
    // This qualified shape only performs local arithmetic and native memcpy.
    // Serialize its reads/writes against other memory writers; no GPU wait or
    // command publication occurs here. Maintain camera-before-payload lock order.
    TraceMotionSourceInputs(context, base, 0x8224A2E8u);
    struct Active {
        Active() { owned_constant_copy_active = true; }
        ~Active() { owned_constant_copy_active = false; }
    } active;
    native_copy(context, base);
    if (context.r3.u32 != 8 || !source_valid()) {
        if (owned_flow_followup.Active()) ++owned_flow.constant_stale;
        return;
    }
    for (uint32_t i = 0; i < 32; ++i)
        if (PPC_LOAD_U32(destination + i * 4) != source.packed[i]) {
            if (owned_flow_followup.Active()) ++owned_flow.constant_mismatch;
            return;
        }
    if (owned_camera_constants.Size() == runtime_owned_camera_source::ConstantStore::kCapacity)
        owned_camera_constants.Retire(*snapshot);
    const auto publication = owned_camera_constants.Publish(source, physical,
        snapshot->Watch({physical, 128}));
    if (owned_flow_followup.Active() && publication) ++owned_flow.constants;
    snapshot.reset(); // Owned values only below; release payload lock before I/O.
    if (publication && capture_window.manual && capture_window.generation &&
        capture_window.copies && !motion_source_budget.closed) {
        std::ofstream out("logs/pc_motion_matrix_sources.log", std::ios::app);
        out << "MATRIX_SOURCE_OWNED_CONSTANT publication=" << publication
            << " source_publication=" << source.token.publication
            << " source_item=" << source.token.item << " physical=" << std::hex << physical
            << std::dec << " bytes=128 register_base=12 vectors=8"
            << " scope=completed_native_constants_not_gpu_submission\n";
        out.close();
        if (out.fail()) ++motion_source_budget.write_failures;
    }
}

bool RuntimeCopyOwnedCameraConstants(uint32_t address, uint32_t bytes,
                                    runtime_owned_camera_source::ConstantPublication& output) {
    uint32_t physical{};
    if (!RuntimeCanonicalGuestPhysicalRange(address, bytes, physical)) return false;
    std::lock_guard lock(camera_trace_mutex);
    RuntimeSourceMemoryCoordinator::Snapshot snapshot(RuntimeGuestSourceCoordinator());
    return owned_camera_constants.Copy(snapshot, physical, bytes, output);
}

bool RuntimeCopyOwnedCameraPacket(void* physical_base, uint32_t physical, uint32_t header,
    uint32_t count, uint32_t* payload, rex::graphics::pc_owned_camera_packet::Source* source) noexcept {
    if (!physical_base || !RuntimeGuestSourceCoordinator().TrackingEnabled()) return false;
    std::lock_guard lock(camera_trace_mutex);
    RuntimeSourceMemoryCoordinator::Snapshot snapshot(RuntimeGuestSourceCoordinator());
    const auto* bytes = static_cast<const uint8_t*>(physical_base);
    const bool copied = RuntimeReadOwnedCameraPacket(owned_camera_packets, snapshot, physical, header,
        count, payload, source, [&](uint32_t offset) {
            uint32_t word;
            std::memcpy(&word, bytes + offset, sizeof(word));
            return _byteswap_ulong(word);
        });
    if (owned_flow_followup.Active()) {
        ++owned_flow.reads;
        if (copied) ++owned_flow.read_owned;
    }
    return copied;
}

namespace {
struct OwnedPacketWrite {
    struct Event {
        uint64_t id, constant;
        runtime_owned_camera_source::Token source;
        uint32_t physical, packet_words, first, words;
    };
    PPCContext* context;
    uint8_t* base;
    uint32_t builder, cursor{}, limit{};
    bool cancelled{};
    runtime_owned_camera_source::PacketWriteLease lease;
    std::vector<Event> events;
    OwnedPacketWrite(PPCContext& ctx, uint8_t* memory, uint32_t scratch)
        : context(&ctx), base(memory), builder(ctx.r3.u32),
          lease(camera_trace_mutex, RuntimeGuestSourceCoordinator(),
                owned_camera_constants, owned_camera_packets, scratch) {}
    bool Resume() {
        if (cancelled || !lease.Resume()) return false;
        if (!RuntimeOwnedCameraInputMode().Enabled() &&
            (!capture_window.manual || !capture_window.generation ||
             !capture_window.copies || capture_window.copies > RuntimeMotionSourceBudget::kMaximumCopies)) {
            Cancel(); return false;
        }
        cursor = PPC_LOAD_U32(builder + 48);
        limit = PPC_LOAD_U32(builder + 52);
        uint32_t physical{};
        if ((cursor & 3) || limit < cursor || limit - cursor > 262144 ||
            !RuntimeCanonicalGuestPhysicalRange(cursor, limit - cursor + 4, physical)) {
            Cancel(); return false;
        }
        return true;
    }
    void Cancel() { cancelled = true; lease.Release(); }
    void Seal() {
        if (!lease.Active()) return;
        const uint32_t end = PPC_LOAD_U32(builder + 48);
        if (end == cursor) return; // A full buffer can spill before any copy.
        uint32_t physical{};
        if ((end & 3) || end < cursor || end > limit ||
            !RuntimeCanonicalGuestPhysicalRange(cursor + 4, end - cursor, physical)) {
            Cancel(); return;
        }
        std::vector<uint32_t> words((end - cursor) / 4);
        for (uint32_t i = 0; i < words.size(); ++i)
            words[i] = PPC_LOAD_U32(cursor + 4 + i * 4);
        if (!lease.Seal(physical, words.data(), static_cast<uint32_t>(words.size()),
                [&](uint64_t id, uint64_t constant, runtime_owned_camera_source::Token source,
                    uint32_t address, uint32_t packet_words, uint32_t first, uint32_t count) {
                    if (owned_flow_followup.Active()) ++owned_flow.packets;
                    if (capture_window.manual && capture_window.generation && capture_window.copies &&
                        capture_window.copies <= RuntimeMotionSourceBudget::kMaximumCopies && !motion_source_budget.closed)
                        events.push_back({id, constant, source, address, packet_words, first, count});
                })) Cancel();
    }
    void Log() {
        if (events.empty()) return;
        // The lease is already released: neither payload nor camera locks are
        // retained during I/O or native submission. Serialize only the log write.
        std::lock_guard lock(camera_trace_mutex);
        std::ofstream out("logs/pc_motion_matrix_sources.log", std::ios::app);
        for (const auto& e : events)
            out << "MATRIX_SOURCE_OWNED_PACKET publication=" << e.id
                << " constant_publication=" << e.constant
                << " source_publication=" << e.source.publication
                << " source_item=" << e.source.item << " physical=" << std::hex << e.physical
                << " first_register=" << e.first << std::dec
                << " packet_words=" << e.packet_words << " source_words=" << e.words
                << " scope=completed_native_packet_not_gpu_execution\n";
        out.close();
        if (out.fail()) ++motion_source_budget.write_failures;
    }
};
thread_local OwnedPacketWrite* active_packet_write{};
struct PacketWriteScope {
    OwnedPacketWrite* previous = active_packet_write;
    explicit PacketWriteScope(OwnedPacketWrite* current) { active_packet_write = current; }
    ~PacketWriteScope() { active_packet_write = previous; }
};
} // namespace

void RuntimeRunOwnedCameraPacketWrite(PPCContext& context, uint8_t* base,
                                     void (*native_write)(PPCContext&, uint8_t*)) {
    // Unexpected nesting abandons the outer fragment and hides its provenance.
    // This also prevents a nested observer from deadlocking on the camera lock.
    if (active_packet_write) {
        active_packet_write->Cancel();
        PacketWriteScope unknown(nullptr);
        native_write(context, base); return;
    }
    const uint32_t builder = context.r3.u32;
    uint32_t scratch{};
    if (!context.r4.u64 || context.r5.u32 != 0x4000 ||
        !RuntimeMotionSourceRange(builder, 6016, 16) ||
        context.r6.u32 != builder + 1920 ||
        !RuntimeCanonicalGuestPhysicalRange(builder + 2112, 128, scratch)) {
        native_write(context, base); return;
    }
    OwnedPacketWrite transaction(context, base, scratch);
    if (!transaction.Resume()) { native_write(context, base); return; }
    PacketWriteScope active(&transaction);
    native_write(context, base);
    transaction.Seal();
    transaction.lease.Release();
    transaction.Log();
}

void RuntimeRunOwnedCameraPacketFlush(PPCContext& context, uint8_t* base,
                                     void (*native_flush)(PPCContext&, uint8_t*)) {
    auto* transaction = active_packet_write;
    if (!transaction) { native_flush(context, base); return; }
    const bool expected = transaction->context == &context && transaction->base == base &&
        transaction->builder == context.r3.u32 && uint32_t(context.lr) == 0x82870B54;
    if (expected) transaction->Seal();
    else transaction->Cancel();
    transaction->lease.Release();
    {
        // Native submission may wait for GPU writes that need the payload lock.
        // Calls made during submission have no relationship to the paused writer.
        PacketWriteScope submitting(nullptr);
        native_flush(context, base);
    }
    if (expected) transaction->Resume();
}

void RuntimeScaleClientViewFov(PPCContext& context, uint8_t* base) noexcept {
    // V391: the scaled FOV reaches the client's copy and, through the view
    // builder's write-back, the persistent view the renderer copies; a view
    // still holding the last scaled value is scaled from its title value
    // (runtime_camera_policy.h). The entry macro still calls in for the
    // write-back (0x8249A1EC, runtime_function_trace.h): a no-op since V391.
    thread_local RuntimeViewFovScaling last;
    // One smoother per client view (the copy's destination, client + 2112).
    struct FovSmoothing {
        uint32_t client_view = 0;
        RuntimeFovSmoother smoother;
    };
    thread_local FovSmoothing smoothing[4];
    const uint32_t caller = uint32_t(context.lr);
    const uint32_t view = context.r4.u32;
    if (!view || view > UINT32_MAX - kClientViewBytes) return;
    // VR investigation (DARKNESS_VR_VIEW_DUMP=1): the 416-byte view as floats,
    // every 30th write-back (the built view), 200 lines. Off in every player launch.
    static const bool view_dump = std::getenv("DARKNESS_VR_VIEW_DUMP") != nullptr;
    if (view_dump && caller == kClientViewWriteBackReturn) {
        static std::atomic<uint32_t> calls{0};
        const uint32_t call = calls.fetch_add(1, std::memory_order_relaxed);
        if (call % 30 == 0 && call / 30 < 200) {
            std::string line = "RUNTIME_VIEW_DUMP call=" + std::to_string(call) + " client=" +
                               std::to_string(context.r3.u32) + " v=";
            char number[32];
            for (uint32_t offset = 0; offset < kClientViewBytes; offset += 4) {
                std::snprintf(number, sizeof(number), "%.5g,",
                              FloatFromBits(PPC_LOAD_U32(view + offset)));
                line += number;
            }
            std::fprintf(stderr, "%s\n", line.c_str());
        }
    }
    if (!RuntimeIsClientViewCopy(caller, context.r5.u32)) return;
    const uint32_t title_bits =
        RuntimeViewFovTitleBits(last, PPC_LOAD_U32(view + kClientViewFovOffset));
    const float fov = FloatFromBits(title_bits);
    float shown = fov;
    const double now = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (view_fov_smoothing.load(std::memory_order_relaxed)) {
        const uint32_t client_view = context.r3.u32;
        FovSmoothing* slot = nullptr;
        for (FovSmoothing& candidate : smoothing) {
            if (candidate.client_view == client_view) slot = &candidate;
        }
        for (FovSmoothing& candidate : smoothing) {
            if (!slot && !candidate.client_view) slot = &candidate;
        }
        if (!slot) {
            slot = &smoothing[0];
            *slot = {};
        }
        slot->client_view = client_view;
        shown = slot->smoother.Next(fov, now);
    }
    const float scaled =
        RuntimeScaledViewFov(shown, view_fov_tangent_scale.load(std::memory_order_relaxed));
    last = {title_bits, FloatBits(scaled)};
    PPC_STORE_U32(view + kClientViewFovOffset, last.scaled_bits);
    if (view_fov_zoom_log.load(std::memory_order_relaxed)) {
        thread_local float previous_title = 0.0f;
        thread_local bool moving = false;
        const bool now_moving = fov != previous_title || std::fabs(shown - fov) > 1e-4f;
        if ((now_moving || moving) &&
            view_fov_zoom_log_lines.fetch_add(1, std::memory_order_relaxed) <
                kViewFovZoomLogLines) {
            std::fprintf(stderr, "RUNTIME_FOV_ZOOM t=%.6f view=%08X title=%.4f shown=%.4f written=%.4f\n",
                         now, context.r3.u32, fov, shown, scaled);
        }
        previous_title = fov;
        moving = now_moving;
    }
    if (view_fov_trace_enabled.load(std::memory_order_relaxed)) {
        std::lock_guard lock(view_fov_trace_mutex);
        if (++view_fov_trace_lines >= 3000000) view_fov_trace_enabled = false;
        view_fov_trace << std::fixed << std::setprecision(6) << now << ',' << std::hex
                       << context.r3.u32 << std::dec << ',' << fov << ',' << shown << ','
                       << scaled << '\n';
        if (!(view_fov_trace_lines % 256)) view_fov_trace.flush();
    }
    if (scaled == fov) return;
    if (!view_fov_logged.exchange(true, std::memory_order_relaxed)) {
        std::cout << "PC_GAMEPLAY_FOV_APPLIED view=0x" << std::hex << view << std::dec
                  << " title_fov=" << fov << " scaled_fov=" << scaled << " setting_16x9="
                  << gameplay_fov_degrees.load(std::memory_order_relaxed)
                  << " source=client_view_copy" << std::endl;
    }
}

void ConfigureRuntimeCamera(float configured_fov_degrees,
                            bool enable_camera_trace, bool manual_capture) {
    if (!std::isfinite(configured_fov_degrees) ||
        configured_fov_degrees < 60.0f || configured_fov_degrees > 120.0f) {
        configured_fov_degrees = kOriginalGameplayFovDegrees;
    }
    gameplay_fov_degrees.store(configured_fov_degrees,
                               std::memory_order_release);
    double tangent_scale = RuntimeGameplayFovTangentScale(configured_fov_degrees);
    // V407 widescreen on a taller screen (16:10): the title keeps the vertical
    // field of view, so the gameplay view would narrow; keep its 16:9
    // horizontal field of view instead (more view at the top and bottom).
    {
        const double guest_aspect = double(darkness::guest_video_mode::DisplayWidth()) /
                                    double(darkness::guest_video_mode::DisplayHeight());
        const double taller = (16.0 / 9.0) / guest_aspect;
        if (taller > 1.001) tangent_scale *= taller;
    }
    view_fov_tangent_scale.store(tangent_scale, std::memory_order_relaxed);
    view_fov_logged.store(false, std::memory_order_relaxed);
    const auto marker = [](const wchar_t* name) {
        wchar_t executable[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
        if (!length || length >= MAX_PATH) return false;
        std::error_code error;
        return std::filesystem::exists(
            std::filesystem::path(executable, executable + length).parent_path() / name, error);
    };
    const char* smoothing_env = std::getenv("DARKNESS_FOV_SMOOTHING");
    const bool smoothing =
        (smoothing_env && smoothing_env[0] == '1') || marker(L"fov_smoothing.txt");
    view_fov_smoothing.store(smoothing, std::memory_order_relaxed);
    view_fov_zoom_log.store(marker(L"fov_zoom_log.txt"), std::memory_order_relaxed);
    std::fprintf(stderr, "RUNTIME_FOV_SMOOTHING enabled=%d zoom_log=%d\n", smoothing ? 1 : 0,
                 view_fov_zoom_log.load(std::memory_order_relaxed) ? 1 : 0);
    if (const char* trace = std::getenv("DARKNESS_FOV_TRACE"); trace && *trace) {
        std::lock_guard lock(view_fov_trace_mutex);
        view_fov_trace.open(std::filesystem::u8path(trace), std::ios::trunc);
        view_fov_trace << "seconds,client_view,title_fov,shown_fov,written_fov\n";
        view_fov_trace_lines = 0;
        view_fov_trace_enabled = view_fov_trace.is_open();
    }
    g_runtime_view_fov_active.store(tangent_scale != 1.0 || smoothing ? 1u : 0u,
                                    std::memory_order_release);
    trace_camera_state.store(enable_camera_trace, std::memory_order_release);
    RuntimeSetEntrySlowBit(kRuntimeEntrySlowCameraTrace, enable_camera_trace);
    camera_trace_sequence.store(0, std::memory_order_relaxed);
    frame_viewport_trace_sequence.store(0, std::memory_order_relaxed);
    transform_input_trace_sequence.store(0, std::memory_order_relaxed);
    {
        std::lock_guard lock(camera_trace_mutex);
        capture_window = {};
        capture_window.manual = manual_capture;
        motion_source_budget = {};
        packet_source_budget = {};
        motion_producer_budget = {};
        owned_camera_sources.Clear();
        owned_camera_constants.Clear();
        owned_camera_packets.Clear();
        owned_source_selection_stats = {};
        owned_source_last_publication = 0;
        owned_flow_followup = {};
        owned_flow = {};
        RuntimeMotionOriginsReset();
        RuntimeMotionDeltaReset();
        RuntimeTemporalCameraReset();
        motion_binding_observation = {};
    }
    main_viewport_object.store(0, std::memory_order_release);
    std::cout << "PC_CAMERA_CONFIG gameplay_fov=" << configured_fov_degrees
              << " original=" << kOriginalGameplayFovDegrees
              << " tangent_scale=" << tangent_scale
              << " trace=" << (enable_camera_trace ? 1 : 0)
              << " capture=" << (manual_capture ? "first_manual_request" : "startup")
              << '\n';
}

static __declspec(noinline) void RuntimeCameraFunctionEnterSelected(
        PPCContext& context, uint8_t* base, uint32_t address) {
    // The owned constant transaction has already run this observer and holds
    // camera/payload locks through the bounded math+memcpy implementation.
    if (address == 0x8224A2E8u && owned_constant_copy_active) return;
    if (address == 0x8285D208u && trace_camera_state.load(std::memory_order_relaxed)) {
        std::lock_guard lock(camera_trace_mutex);
        TracePacketSource(context, base);
    }

    if ((address == 0x8249ADF0u || address == 0x82114838u || address == 0x823EBAB8u ||
         address == 0x829B917Cu || address == 0x825EB6B8u || address == 0x8249AD40u || address == 0x82498F68u) &&
        trace_camera_state.load(std::memory_order_relaxed) &&
        RuntimeTemporalCameraFunction(address, uint32_t(context.lr)) != RuntimeTemporalCameraSite::None) {
        std::lock_guard lock(camera_trace_mutex);
        RuntimeTemporalCameraObserve(context, base, address, capture_window);
    }
    if ((address == 0x82114838u || address == 0x82498B08u || address == 0x82498F68u || address == 0x825EB6B8u) &&
        trace_camera_state.load(std::memory_order_relaxed) &&
        RuntimeMotionDeltaFunction(address, uint32_t(context.lr)) != RuntimeMotionDeltaSite::None) {
        std::lock_guard lock(camera_trace_mutex);
        RuntimeMotionDeltaObserve(context, base, address, capture_window);
    }
    if ((address == 0x821D3700u || (address >= 0x825C4C50u && address <= 0x825E54B0u)) &&
        trace_camera_state.load(std::memory_order_relaxed)) {
        const auto origin_site = RuntimeMotionOriginFunction(address, uint32_t(context.lr));
        if (origin_site != RuntimeMotionOriginSite::None) {
            std::lock_guard lock(camera_trace_mutex);
            RuntimeMotionOriginsObserve(context, base, address, capture_window);
        }
    }
    if (address == 0x8259D3B8u &&
        (trace_camera_state.load(std::memory_order_relaxed) || RuntimeOwnedCameraInputMode().Enabled()) &&
        RuntimeIsCompactMotionPublication(address, uint32_t(context.lr), context.r1.u32, context.r4.u32, context.r5.u32)) {
        std::lock_guard lock(camera_trace_mutex);
        TraceCompactMotionOutput(context, base);
    }
    if ((address == 0x82299E30u || address == 0x8224A2E8u) &&
        trace_camera_state.load(std::memory_order_relaxed)) {
        std::lock_guard lock(camera_trace_mutex);
        TraceMotionSourceInputs(context, base, address);
    }
    if ((address == 0x82762790u || address == 0x820E2E50u ||
         address == 0x82249580u || address == 0x82248A78u) &&
        trace_camera_state.load(std::memory_order_relaxed)) {
        // Opt-in only. Serialize the bounded observation state and output; do
        // not read GPU-thread frame state or modify any guest camera data.
        std::lock_guard lock(camera_trace_mutex);
        if (address == 0x82249580u)
            transform_observation_ticket.Publication(&context, uint32_t(context.lr));
        else if (address != 0x82248A78u) transform_observation_ticket.Clear();
        if (address == 0x820E2E50u) {
            capture_window.FrameEnter();
            motion_binding_observation = {};
        }
        uint32_t correlated_copy_sequence{};
        const bool correlated = address == 0x82248A78u &&
            transform_observation_ticket.Consume(&context, correlated_copy_sequence);
        if (address == 0x82248A78u && capture_window.SelectBuilder(&context)) {
            const uint32_t sequence = transform_input_trace_sequence.fetch_add(
                1, std::memory_order_relaxed);
            constexpr uint32_t gpu_state = 0x82A69B00u;
            const uint32_t pool = PPC_LOAD_U32(gpu_state + 8224u);
            const uint32_t index = PPC_LOAD_U32(gpu_state + 8232u);
            const uint32_t source = RuntimeObservedTransformAddress(pool, index);
            const uint32_t builder = PPC_LOAD_U32(gpu_state + 15748u);
            // These are imminent guest reads, not a synthesized camera. VMX
            // addresses are aligned; reject wrapping/misaligned observation.
            uint32_t matrix[32]{};
            if (source) {
                for (uint32_t i = 0; i < 16u; ++i) {
                    matrix[i] = PPC_LOAD_U32(source + i * 4u);
                    matrix[16u + i] = PPC_LOAD_U32(gpu_state + 17088u + i * 4u);
                }
            }
            std::filesystem::create_directories("logs");
            std::ofstream out("logs/pc_transform_inputs.log",
                              sequence ? std::ios::app : std::ios::trunc);
            out << "TRANSFORM_INPUT sequence=" << sequence
                << " generation=" << capture_window.generation
                << " copy_sequence=" << (correlated ? correlated_copy_sequence : UINT32_MAX)
                << " copy_associated=" << (correlated ? 1 : 0)
                << " window_copy_sequence=" << capture_window.copies - 1u
                << " function=0x" << std::hex << address
                << " caller=0x" << context.lr << " pool=0x" << pool
                << " index=0x" << index << " source=0x" << source
                << " builder=0x" << builder
                << " valid=" << (source ? 1 : 0)
                << " scope=builder_inputs_not_camera_history words=";
            if (source) for (uint32_t i = 0; i < 32u; ++i) {
                if (i) out << ',';
                out << std::setw(8) << std::setfill('0') << matrix[i];
            }
            out << '\n';
        }
        const bool viewport_copy = address == 0x82762790u &&
            RuntimeIsFrameViewportCopy(address, uint32_t(context.lr),
                                       context.r1.u32, context.r4.u32);
        // Census cadence must use actual copies, independently of the closed
        // diagnostic budget. Loop entry820E2E50 need not repeat during gameplay.
        if (viewport_copy) ReportOwnedFlow();
        const bool qualified_copy = viewport_copy && capture_window.copies < 16u;
        //820E2E50 owns a render loop and need not re-enter on each frame.
        // Close before the next actual qualified copy, while the preceding
        // neighborhoods and their owner are still the selected window.
        if (qualified_copy) TraceMotionSourceBoundary(context);
        if (qualified_copy &&
            capture_window.SelectCopy(capture_window.manual
                ? RuntimeGraphicsCameraCaptureGeneration() : 0u, &context, context.r31.u32)) {
            const uint32_t sequence = frame_viewport_trace_sequence.fetch_add(
                1, std::memory_order_relaxed);
            transform_observation_ticket.Arm(&context, sequence);
            // Observe the already-copied consumer-owned record. Never mutate
            // guest matrices or confuse the nested viewport stack with history.
            uint32_t words[104]{};
            for (uint32_t i = 0; i < 104; ++i)
                words[i] = PPC_LOAD_U32(context.r4.u32 + i * 4u);
            std::filesystem::create_directories("logs");
            std::ofstream out("logs/pc_frame_viewport.log",
                              sequence ? std::ios::app : std::ios::trunc);
            out << "FRAME_VIEWPORT_COPY sequence=" << sequence
                << " generation=" << capture_window.generation
                << " function=0x" << std::hex << address
                << " caller=0x" << context.lr << " source=0x" << context.r4.u32
                << " owner_r31=0x" << context.r31.u32
                << " render_context=0x" << context.r3.u32
                << " scope=consumer_copy_not_temporal_history words=";
            for (uint32_t i = 0; i < 104; ++i) {
                if (i) out << ',';
                out << std::setw(8) << std::setfill('0') << words[i];
            }
            out << '\n';
        }
    }
    if (address < 0x82389CA0u || address > 0x82389CF8u) return;
    const uint32_t object = context.r3.u32;
    const uint32_t objectVtable =
        object && object <= 0xFFFFFFFCu ? PPC_LOAD_U32(object) : 0u;
    // The main viewport is only observed: its FOV never reaches the render
    // view (the client overwrites it every frame; see RuntimeScaleClientViewFov).
    if (address == kViewportSetCurrentFovFunction &&
        context.lr == kMainViewportInitialFovCaller &&
        objectVtable == kMainViewportVtable) {
        main_viewport_object.store(object, std::memory_order_release);
    }
    TraceCameraSetter(context, base, address, objectVtable);
}

void RuntimeCameraFunctionEnter(PPCContext& context, uint8_t* base, uint32_t address) {
    // Called at every generated function entry. Keep the large diagnostic
    // body's stack frame/probe off the ordinary path while retaining every
    // FOV setter, all enabled observations, and continuous compact publication.
    if ((address >= 0x82389CA0u && address <= 0x82389CF8u) ||
        trace_camera_state.load(std::memory_order_relaxed) ||
        (address == 0x8259D3B8u && RuntimeOwnedCameraInputMode().Enabled())) {
        RuntimeCameraFunctionEnterSelected(context, base, address);
    }
}
