#include "runtime_update_check.h"

#include <windows.h>
#include <winhttp.h>

#include <cctype>
#include <iterator>
#include <vector>

namespace darkness::update_check {
namespace {

// The string value of "key": "..." starting at from (GitHub's JSON escapes
// nothing in tags or booleans that matters here).
std::optional<std::string> StringField(std::string_view json, std::string_view key, size_t from,
                                       size_t until) {
    const std::string pattern = "\"" + std::string(key) + "\"";
    size_t at = json.find(pattern, from);
    if (at == std::string_view::npos || at >= until) return std::nullopt;
    at = json.find(':', at + pattern.size());
    if (at == std::string_view::npos) return std::nullopt;
    at = json.find_first_not_of(" \t\r\n", at + 1);
    if (at == std::string_view::npos || json[at] != '"') return std::nullopt;
    const size_t end = json.find('"', at + 1);
    if (end == std::string_view::npos) return std::nullopt;
    return std::string(json.substr(at + 1, end - at - 1));
}

bool BoolFieldTrue(std::string_view json, std::string_view key, size_t from, size_t until) {
    const std::string pattern = "\"" + std::string(key) + "\"";
    size_t at = json.find(pattern, from);
    if (at == std::string_view::npos || at >= until) return false;
    at = json.find(':', at + pattern.size());
    if (at == std::string_view::npos) return false;
    at = json.find_first_not_of(" \t\r\n", at + 1);
    return at != std::string_view::npos && json.substr(at, 4) == "true";
}

}  // namespace

std::optional<Version> ParseVersion(std::string_view text) {
    if (!text.empty() && (text.front() == 'v' || text.front() == 'V')) text.remove_prefix(1);
    Version version{};
    size_t part = 0;
    size_t i = 0;
    while (true) {
        if (part >= version.size() || i >= text.size() || !std::isdigit(uint8_t(text[i])))
            return std::nullopt;
        int value = 0;
        while (i < text.size() && std::isdigit(uint8_t(text[i]))) {
            value = value * 10 + (text[i] - '0');
            if (value > 99999) return std::nullopt;
            ++i;
        }
        version[part++] = value;
        if (i == text.size()) break;
        if (text[i] != '.') return std::nullopt;
        ++i;
    }
    if (part < 2) return std::nullopt;
    return version;
}

std::optional<Release> NewestNewerRelease(std::string_view json, std::string_view current) {
    const std::optional<Version> mine = ParseVersion(current);
    if (!mine) return std::nullopt;
    std::optional<Release> best;
    Version bestVersion = *mine;
    // Each release object carries one "tag_name"; its "draft" flag follows
    // within the same object (before the next release's tag).
    size_t at = 0;
    while ((at = json.find("\"tag_name\"", at)) != std::string_view::npos) {
        const size_t next = json.find("\"tag_name\"", at + 10);
        const size_t until = next == std::string_view::npos ? json.size() : next;
        const std::optional<std::string> tag = StringField(json, "tag_name", at, until);
        const bool draft = BoolFieldTrue(json, "draft", at, until);
        at += 10;
        if (!tag || draft) continue;
        const std::optional<Version> version = ParseVersion(*tag);
        if (!version || *version <= bestVersion) continue;
        bestVersion = *version;
        Release release;
        release.tag = *tag;
        release.version = (tag->front() == 'v' || tag->front() == 'V') ? tag->substr(1) : *tag;
        release.url = std::string(kReleasePageUrl) + *tag;
        best = std::move(release);
    }
    return best;
}

bool FetchText(const std::wstring& url, std::string& body, size_t maxBytes, std::string& error) {
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256]{}, path[2048]{};
    parts.lpszHostName = host;
    parts.dwHostNameLength = DWORD(std::size(host));
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = DWORD(std::size(path));
    wchar_t extra[1024]{};
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = DWORD(std::size(extra));
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) {
        error = "not an https URL";
        return false;
    }
    const std::wstring object = std::wstring(path) + extra;
    struct Handle {
        HINTERNET value = nullptr;
        ~Handle() { if (value) WinHttpCloseHandle(value); }
    } session, connection, request;
    session.value = WinHttpOpen(L"EstacadoLauncher/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.value) {
        session.value = WinHttpOpen(L"EstacadoLauncher/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (session.value) {
        WinHttpSetTimeouts(session.value, 5000, 5000, 5000, 5000);
        connection.value = WinHttpConnect(session.value, host, parts.nPort, 0);
    }
    if (connection.value) {
        request.value = WinHttpOpenRequest(connection.value, L"GET", object.c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           WINHTTP_FLAG_SECURE);
    }
    static constexpr wchar_t kHeaders[] = L"Accept: application/vnd.github+json\r\n";
    if (!request.value ||
        !WinHttpSendRequest(request.value, kHeaders, DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.value, nullptr)) {
        error = "connection failed (" + std::to_string(GetLastError()) + ")";
        return false;
    }
    DWORD status = 0, statusSize = sizeof(status);
    WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        error = "server answered " + std::to_string(status);
        return false;
    }
    body.clear();
    std::vector<char> buffer(1u << 15);
    for (;;) {
        DWORD got = 0;
        if (!WinHttpReadData(request.value, buffer.data(), DWORD(buffer.size()), &got)) {
            error = "read failed (" + std::to_string(GetLastError()) + ")";
            return false;
        }
        if (!got) break;
        if (body.size() + got > maxBytes) {
            error = "answer too large";
            return false;
        }
        body.append(buffer.data(), got);
    }
    return true;
}

}  // namespace darkness::update_check
