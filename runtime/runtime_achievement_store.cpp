#include "runtime_achievement_store.h"

#include <cctype>
#include <optional>

namespace darkness::achievement_store {
namespace {

std::string_view Trim(std::string_view text) {
    while (!text.empty() && std::isspace(uint8_t(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(uint8_t(text.back()))) text.remove_suffix(1);
    return text;
}

std::optional<uint64_t> Decimal(std::string_view text, uint64_t maximum) {
    if (text.empty() || text.size() > 20) return std::nullopt;
    uint64_t value = 0;
    for (const char c : text) {
        if (!std::isdigit(uint8_t(c))) return std::nullopt;
        const uint64_t digit = uint64_t(c - '0');
        if (value > (maximum - digit) / 10) return std::nullopt;
        value = value * 10 + digit;
    }
    return value;
}

}  // namespace

bool ParseToml(std::string_view text, Unlocks& unlocks) {
    std::optional<uint32_t> current;  // inside [unlocked.N]
    while (!text.empty()) {
        const size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        if (const size_t hash = line.find('#'); hash != std::string_view::npos) {
            line = line.substr(0, hash);
        }
        line = Trim(line);
        if (line.empty()) continue;
        if (line.front() == '[') {
            current.reset();
            if (line.back() != ']') return false;
            const std::string_view name = Trim(line.substr(1, line.size() - 2));
            constexpr std::string_view kPrefix = "unlocked.";
            if (name.substr(0, kPrefix.size()) != kPrefix) continue;  // another table
            const auto id = Decimal(name.substr(kPrefix.size()), UINT32_MAX);
            if (!id) return false;
            current = uint32_t(*id);
            unlocks.emplace(*current, 0);
            continue;
        }
        const size_t equals = line.find('=');
        if (equals == std::string_view::npos) return false;
        if (!current || Trim(line.substr(0, equals)) != "filetime") continue;
        const auto filetime = Decimal(Trim(line.substr(equals + 1)), INT64_MAX);
        if (!filetime) return false;
        unlocks[*current] = *filetime;
    }
    return true;
}

std::string FormatToml(const Unlocks& unlocks) {
    std::string out = "# Achievement unlock state - managed by ReXGlue runtime\n\n";
    for (const auto& [id, filetime] : unlocks) {
        out += "[unlocked." + std::to_string(id) + "]\nfiletime = " + std::to_string(filetime) +
               "\n\n";
    }
    return out;
}

}  // namespace darkness::achievement_store
