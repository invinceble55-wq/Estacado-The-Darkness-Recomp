#pragma once

// Update check for the launcher (#24): the project's public GitHub releases,
// read once in the background when the launcher starts (players can turn it
// off in About). A newer regular release shows an "Update available" button
// that opens its release page; nothing is downloaded or installed.

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace darkness::update_check {

inline constexpr wchar_t kReleasesApiUrl[] =
    L"https://api.github.com/repos/invinceble55-wq/Estacado-The-Darkness-Recomp/releases?per_page=20";
inline constexpr char kReleasePageUrl[] =
    "https://github.com/invinceble55-wq/Estacado-The-Darkness-Recomp/releases/tag/";

using Version = std::array<int, 4>;

// "0.9.6", "v0.9.3.1" -> numbers; anything else (test builds such as
// "issue16-test2", "0.9.7-rc1") -> nothing.
std::optional<Version> ParseVersion(std::string_view text);

struct Release {
    std::string tag;      // "v0.9.7"
    std::string version;  // "0.9.7"
    std::string url;      // its release page
};

// The newest regular release in a GitHub releases list (JSON) that is newer
// than current. Drafts and tags that are not plain versions are skipped.
std::optional<Release> NewestNewerRelease(std::string_view releasesJson, std::string_view current);

// HTTPS GET of a small text (WinHTTP, short timeouts, at most maxBytes).
bool FetchText(const std::wstring& url, std::string& body, size_t maxBytes, std::string& error);

}  // namespace darkness::update_check
