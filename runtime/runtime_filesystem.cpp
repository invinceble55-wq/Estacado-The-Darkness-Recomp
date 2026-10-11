#include "runtime_filesystem.h"
#include "runtime_xbox_path_policy.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>

#include <Windows.h>

struct GuestFileSystem::HostFile {
    void* handle = INVALID_HANDLE_VALUE;
    uint64_t size{};
    HostFile() = default;
    HostFile(const HostFile&) = delete;
    HostFile& operator=(const HostFile&) = delete;
    ~HostFile() {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(static_cast<HANDLE>(handle));
    }
    // Positional I/O (OVERLAPPED offset on a synchronous handle): safe from
    // several threads at once, no shared file pointer.
    bool ReadAt(uint64_t offset, uint8_t* destination, uint32_t count) const {
        while (count) {
            OVERLAPPED at{};
            at.Offset = DWORD(offset);
            at.OffsetHigh = DWORD(offset >> 32);
            DWORD done{};
            if (!ReadFile(static_cast<HANDLE>(handle), destination, count, &done, &at) || !done)
                return false;
            offset += done;
            destination += done;
            count -= done;
        }
        return true;
    }
    bool WriteAt(uint64_t offset, const uint8_t* source, uint32_t count) const {
        while (count) {
            OVERLAPPED at{};
            at.Offset = DWORD(offset);
            at.OffsetHigh = DWORD(offset >> 32);
            DWORD done{};
            if (!WriteFile(static_cast<HANDLE>(handle), source, count, &done, &at) || !done)
                return false;
            offset += done;
            source += done;
            count -= done;
        }
        return true;
    }
};

namespace {
constexpr char kRawCachePartition[] = "\\Device\\Harddisk0\\partition0";
std::mutex portableContentRootMutex;
std::filesystem::path portableContentRootOverride;
std::mutex portableContentOperationMutex;

bool EqualsIgnoreCase(const char* path, uint32_t length, const char* expected) {
    const size_t expectedLength = std::strlen(expected);
    if (length != expectedLength) return false;
    for (size_t index = 0; index < expectedLength; ++index) {
        if (std::tolower(static_cast<unsigned char>(path[index])) !=
            std::tolower(static_cast<unsigned char>(expected[index]))) return false;
    }
    return true;
}

uint64_t ToFileTime(const FILETIME& time) {
    return (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

bool ReadHostAttributes(const std::filesystem::path& hostPath, GuestFileAttributes* attributes) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(hostPath.c_str(), GetFileExInfoStandard, &data)) return false;
    attributes->creationTime = ToFileTime(data.ftCreationTime);
    attributes->lastAccessTime = ToFileTime(data.ftLastAccessTime);
    attributes->lastWriteTime = ToFileTime(data.ftLastWriteTime);
    attributes->changeTime = attributes->lastWriteTime;
    attributes->endOfFile = (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    // Xenia's virtual-file contract supplies its allocation size here. The
    // extracted host directory exposes no Xbox allocation-unit metadata, so
    // retain the conservative, observable file length instead of inventing it.
    attributes->allocationSize = attributes->endOfFile;
    attributes->attributes = data.dwFileAttributes;
    return true;
}

bool IsGuestGamePath(const std::string& path, std::filesystem::path* relative) {
    if (path.empty()) return false;
    std::string normalized = path;
    for (char& character : normalized) {
        if (character == '\\') character = '/';
    }
    if (normalized.size() >= 3 &&
        (normalized[0] == 'D' || normalized[0] == 'd') &&
        normalized[1] == ':' && normalized[2] == '/') {
        normalized.erase(0, 3);
    } else if (normalized[0] == '/' || normalized.find(':') != std::string::npos) {
        return false;
    }

    const std::filesystem::path candidate(normalized);
    if (candidate.empty() || candidate.is_absolute()) return false;
    for (const auto& component : candidate) {
        if (component == "..") return false;
    }
    *relative = candidate;
    return true;
}

bool EqualPathComponent(const std::filesystem::path& left,
                        const std::filesystem::path& right) {
    // Case-insensitive like NTFS names, and wide: the saves live under the
    // user's profile, whose name may be beyond the system code page.
    const std::wstring& leftText = left.native();
    const std::wstring& rightText = right.native();
    return CompareStringOrdinal(leftText.c_str(), int(leftText.size()), rightText.c_str(),
                                int(rightText.size()), TRUE) == CSTR_EQUAL;
}

bool IsPathWithin(const std::filesystem::path& root,
                  const std::filesystem::path& candidate) {
    auto rootPart = root.begin();
    auto candidatePart = candidate.begin();
    for (; rootPart != root.end(); ++rootPart, ++candidatePart) {
        if (candidatePart == candidate.end() ||
            !EqualPathComponent(*rootPart, *candidatePart)) {
            return false;
        }
    }
    return true;
}

bool ResolveCaseInsensitiveContainedPath(
    const std::filesystem::path& root,
    const std::filesystem::path& relative, bool allowMissingFinal,
    std::filesystem::path* candidate) {
    if (!candidate || root.empty() || relative.is_absolute()) return false;

    std::error_code error;
    const std::filesystem::path canonicalRoot =
        std::filesystem::canonical(root, error);
    if (error || !std::filesystem::is_directory(canonicalRoot, error) || error)
        return false;

    std::vector<std::filesystem::path> components;
    for (const auto& component : relative) {
        if (component.empty() || component == ".") continue;
        if (component == "..") return false;
        components.push_back(component);
    }

    std::filesystem::path current = canonicalRoot;
    for (size_t componentIndex = 0; componentIndex < components.size();
         ++componentIndex) {
        const std::filesystem::path direct = current / components[componentIndex];
        error.clear();
        const bool directExists = std::filesystem::exists(direct, error);
        if (error) return false;
        if (directExists) {
            current = direct;
            continue;
        }

        std::vector<std::filesystem::path> entries;
        std::vector<std::wstring> names;
        for (std::filesystem::directory_iterator it(current, error), end;
             !error && it != end; it.increment(error)) {
            entries.push_back(it->path());
            names.push_back(it->path().filename().wstring());
        }
        if (error) return false;
        const auto match = darkness::xbox_path::SelectUniqueComponent(
            names, components[componentIndex].wstring());
        if (match.ambiguous) return false;
        if (match.found) {
            current = entries[match.index];
            continue;
        }
        if (allowMissingFinal && componentIndex + 1 == components.size()) {
            const std::filesystem::path canonicalParent =
                std::filesystem::canonical(current, error);
            if (error || !IsPathWithin(canonicalRoot, canonicalParent)) return false;
            *candidate = canonicalParent / components[componentIndex];
            return true;
        }
        return false;
    }

    const std::filesystem::path canonicalCandidate =
        std::filesystem::canonical(current, error);
    if (error || !IsPathWithin(canonicalRoot, canonicalCandidate)) return false;
    *candidate = canonicalCandidate;
    return true;
}

bool ResolveOverlayCandidate(const GuestContentOverlay& overlay,
                             const std::filesystem::path& relative,
                             std::filesystem::path* candidate) {
    return ResolveCaseInsensitiveContainedPath(overlay.root, relative, false,
                                               candidate);
}

bool ResolveGamePath(const std::filesystem::path& root, const std::string& guestPath,
                     const std::unordered_map<std::string, std::string>& symbolicLinks,
                     const std::unordered_map<std::string, std::filesystem::path>& hostMounts,
                     const std::vector<GuestContentOverlay>& overlays,
                     std::filesystem::path* hostPath, bool* hostMounted = nullptr,
                     std::filesystem::path* gameRelative = nullptr) {
    std::string resolvedPath = guestPath;
    // Symbolic links are case-insensitive Xbox object-manager names. Resolve
    // nested links with a strict bound so malformed guest state cannot loop.
    for (size_t depth = 0; depth <= symbolicLinks.size(); ++depth) {
        std::string lower = resolvedPath;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
        const auto link = std::find_if(symbolicLinks.begin(), symbolicLinks.end(),
            [&](const auto& entry) { return lower.rfind(entry.first, 0) == 0; });
        if (link == symbolicLinks.end()) break;
        resolvedPath = link->second + resolvedPath.substr(link->first.size());
        if (depth == symbolicLinks.size()) return false;
    }

    std::string lower = resolvedPath;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    for (const auto& mount : hostMounts) {
        if (lower.rfind(mount.first, 0) != 0) continue;
        std::string suffix = resolvedPath.substr(mount.first.size());
        while (!suffix.empty() && (suffix.front() == '\\' || suffix.front() == '/'))
            suffix.erase(suffix.begin());
        for (char& character : suffix) {
            if (character == '\\') character = '/';
        }
        const std::filesystem::path relative(suffix);
        if (relative.is_absolute()) return false;
        for (const auto& component : relative) {
            if (component == "..") return false;
        }
        if (!ResolveCaseInsensitiveContainedPath(mount.second, relative, true,
                                                 hostPath)) {
            return false;
        }
        if (hostMounted) *hostMounted = true;
        return true;
    }
    std::filesystem::path relative;
    if (root.empty() || !IsGuestGamePath(resolvedPath, &relative)) return false;
    if (gameRelative) *gameRelative = relative;
    for (const auto& overlay : overlays) {
        if (ResolveOverlayCandidate(overlay, relative, hostPath)) {
            if (hostMounted) *hostMounted = false;
            return true;
        }
    }
    // Preserve a contained missing final leaf so the existing immutable-title
    // authorization layer can distinguish a denied create/write from an
    // ordinary missing read. Intermediate parents must still exist.
    if (!ResolveCaseInsensitiveContainedPath(root, relative, true, hostPath))
        return false;
    if (hostMounted) *hostMounted = false;
    return true;
}

uint32_t ReadBigEndian32(const uint8_t* bytes) {
    return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) |
           (uint32_t(bytes[2]) << 8) | uint32_t(bytes[3]);
}

std::string HexComponent(uint32_t value) {
    std::ostringstream text;
    text << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << value;
    return text.str();
}

bool IsSafeContentComponent(const std::string& value) {
    if (value.empty() || value.size() > 42 || value == "." || value == "..") return false;
    constexpr char kInvalid[] = "<>:\"/\\|?*";
    return std::none_of(value.begin(), value.end(), [&](unsigned char character) {
        return character < 0x20 || std::strchr(kInvalid, character) != nullptr;
    });
}

std::filesystem::path PortablePackageRoot(uint32_t titleId, uint32_t contentType) {
    return GuestPortableContentDeviceRoot() / HexComponent(titleId) / HexComponent(contentType);
}

std::filesystem::path PortableHeaderRoot(uint32_t titleId, uint32_t contentType) {
    return GuestPortableContentDeviceRoot() / HexComponent(titleId) / L"Headers" /
           HexComponent(contentType);
}

std::string CanonicalSymbolicLinkName(std::string path) {
    for (char& character : path) {
        if (character == '/') character = '\\';
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    if (path.rfind("\\??\\", 0) == 0) path.erase(0, 4);
    return path;
}

bool MatchesPattern(const std::string& text, const std::string& pattern, size_t textOffset = 0,
                    size_t patternOffset = 0) {
    while (patternOffset < pattern.size()) {
        const char token = pattern[patternOffset];
        if (token == '*') {
            ++patternOffset;
            if (patternOffset == pattern.size()) return true;
            for (size_t index = textOffset; index <= text.size(); ++index) {
                if (MatchesPattern(text, pattern, index, patternOffset)) return true;
            }
            return false;
        }
        if (textOffset == text.size()) return false;
        if (token != '?' && std::tolower(static_cast<unsigned char>(token)) !=
            std::tolower(static_cast<unsigned char>(text[textOffset]))) return false;
        ++textOffset;
        ++patternOffset;
    }
    return textOffset == text.size();
}

}

std::filesystem::path GuestPortableContentDeviceRoot() {
    {
        std::lock_guard<std::mutex> lock(portableContentRootMutex);
        if (!portableContentRootOverride.empty()) return portableContentRootOverride;
    }
    std::vector<wchar_t> modulePath(32768);
    const DWORD length = GetModuleFileNameW(nullptr, modulePath.data(),
                                            static_cast<DWORD>(modulePath.size()));
    if (!length || length >= modulePath.size()) return {};
    return std::filesystem::path(std::wstring(modulePath.data(), length)).parent_path() /
           L"runtime_data" / L"content";
}

void ConfigureGuestPortableContentDeviceRoot(const std::filesystem::path& root) {
    std::lock_guard<std::mutex> operationLock(portableContentOperationMutex);
    std::lock_guard<std::mutex> lock(portableContentRootMutex);
    portableContentRootOverride = root;
}

bool EnsureGuestPortableContentDevice(uint64_t requestedBytes) {
    const std::filesystem::path root = GuestPortableContentDeviceRoot();
    if (root.empty()) return false;
    std::error_code error;
    std::filesystem::create_directories(root, error);
    if (error || !std::filesystem::is_directory(root, error) || error) return false;
    const std::filesystem::space_info space = std::filesystem::space(root, error);
    return !error && space.available >= requestedBytes;
}

GuestPortableContentOperation OpenGuestPortableContent(
    const std::string& rootName, uint32_t titleId, uint32_t contentType,
    const std::string& fileName,
    const std::array<uint8_t, kGuestXContentDataBytes>& rawContentData,
    uint32_t createDisposition, uint64_t requestedBytes) {
    std::lock_guard<std::mutex> operationLock(portableContentOperationMutex);
    GuestPortableContentOperation operation{};
    if (rootName.empty() || rootName.size() > 64 || rootName.find(':') != std::string::npos ||
        !titleId || contentType != ReadBigEndian32(rawContentData.data() + 4) ||
        ReadBigEndian32(rawContentData.data()) != kGuestPortableContentDeviceId ||
        !IsSafeContentComponent(fileName) || createDisposition < 1 || createDisposition > 5) {
        return operation;
    }
    if (!EnsureGuestPortableContentDevice(requestedBytes)) {
        operation.result = GuestPortableContentResult::AccessDenied;
        return operation;
    }

    const auto packagePath = PortablePackageRoot(titleId, contentType) / fileName;
    const auto headerPath = PortableHeaderRoot(titleId, contentType) / (fileName + ".header");
    operation.packagePath = packagePath;
    std::error_code error;
    const auto packageStatus = std::filesystem::status(packagePath, error);
    if (error == std::errc::no_such_file_or_directory) error.clear();
    if (error) {
        operation.result = GuestPortableContentResult::AccessDenied;
        return operation;
    }
    const bool exists = std::filesystem::exists(packageStatus);
    if (exists && !std::filesystem::is_directory(packageStatus)) {
        operation.result = GuestPortableContentResult::AccessDenied;
        return operation;
    }

    bool create = false;
    switch (createDisposition) {
    case 1: // CREATE_NEW
        if (exists) {
            operation.result = GuestPortableContentResult::AlreadyExists;
            return operation;
        }
        create = true;
        break;
    case 2: // CREATE_ALWAYS
        GetGuestFileSystem().UnregisterHostMount(rootName);
        if (exists) {
            std::filesystem::remove_all(packagePath, error);
            if (error) {
                operation.result = GuestPortableContentResult::AccessDenied;
                return operation;
            }
        }
        create = true;
        break;
    case 3: // OPEN_EXISTING
        if (!exists) {
            operation.result = GuestPortableContentResult::PathNotFound;
            return operation;
        }
        break;
    case 4: // OPEN_ALWAYS
        create = !exists;
        break;
    case 5: // TRUNCATE_EXISTING
        if (!exists) {
            operation.result = GuestPortableContentResult::PathNotFound;
            return operation;
        }
        GetGuestFileSystem().UnregisterHostMount(rootName);
        std::filesystem::remove_all(packagePath, error);
        if (error) {
            operation.result = GuestPortableContentResult::AccessDenied;
            return operation;
        }
        create = true;
        break;
    }

    if (create) {
        std::filesystem::create_directories(packagePath, error);
        if (error || !std::filesystem::is_directory(packagePath, error) || error) {
            operation.result = GuestPortableContentResult::AccessDenied;
            return operation;
        }
        std::filesystem::create_directories(headerPath.parent_path(), error);
        if (error) {
            operation.result = GuestPortableContentResult::AccessDenied;
            return operation;
        }
        std::ofstream header(headerPath, std::ios::binary | std::ios::trunc);
        header.write(reinterpret_cast<const char*>(rawContentData.data()),
                     rawContentData.size());
        if (!header) {
            operation.result = GuestPortableContentResult::AccessDenied;
            return operation;
        }
        operation.disposition = 1;
    } else {
        operation.disposition = 2;
    }

    if (!GetGuestFileSystem().RegisterHostMount(rootName, packagePath)) {
        operation.result = GuestPortableContentResult::AlreadyExists;
        operation.disposition = 0;
        return operation;
    }
    operation.result = GuestPortableContentResult::Success;
    return operation;
}

GuestPortableContentOperation DeleteGuestPortableContent(
    uint32_t titleId, uint32_t contentType, const std::string& fileName,
    const std::array<uint8_t, kGuestXContentDataBytes>& rawContentData) {
    std::lock_guard<std::mutex> operationLock(portableContentOperationMutex);
    GuestPortableContentOperation operation{};
    if (!titleId || contentType != ReadBigEndian32(rawContentData.data() + 4) ||
        ReadBigEndian32(rawContentData.data()) != kGuestPortableContentDeviceId ||
        !IsSafeContentComponent(fileName)) {
        return operation;
    }

    const std::filesystem::path root = GuestPortableContentDeviceRoot();
    if (root.empty()) {
        operation.result = GuestPortableContentResult::AccessDenied;
        return operation;
    }
    const std::filesystem::path packagePath =
        PortablePackageRoot(titleId, contentType) / fileName;
    const std::filesystem::path headerPath =
        PortableHeaderRoot(titleId, contentType) / (fileName + ".header");
    operation.packagePath = packagePath;

    // Match the pinned content-manager contract: a mounted package is open and
    // cannot be deleted. This check also prevents removing a directory while
    // the guest still has an authoritative root mapped to it.
    if (GetGuestFileSystem().IsHostPathMounted(packagePath)) {
        operation.result = GuestPortableContentResult::AccessDenied;
        return operation;
    }

    std::error_code error;
    const std::filesystem::file_status packageStatus =
        std::filesystem::symlink_status(packagePath, error);
    if (error == std::errc::no_such_file_or_directory) error.clear();
    if (error) {
        operation.result = GuestPortableContentResult::AccessDenied;
        return operation;
    }
    const bool packageExists = std::filesystem::exists(packageStatus);
    if (packageExists && (!std::filesystem::is_directory(packageStatus) ||
                          std::filesystem::is_symlink(packageStatus))) {
        operation.result = GuestPortableContentResult::AccessDenied;
        return operation;
    }

    const std::filesystem::file_status headerStatus =
        std::filesystem::symlink_status(headerPath, error);
    if (error == std::errc::no_such_file_or_directory) error.clear();
    if (error) {
        operation.result = GuestPortableContentResult::AccessDenied;
        return operation;
    }
    const bool headerExists = std::filesystem::exists(headerStatus);
    if (headerExists && (!std::filesystem::is_regular_file(headerStatus) ||
                         std::filesystem::is_symlink(headerStatus))) {
        operation.result = GuestPortableContentResult::AccessDenied;
        return operation;
    }
    if (!packageExists && !headerExists) {
        operation.result = GuestPortableContentResult::FileNotFound;
        return operation;
    }

    const uintmax_t removedEntries = packageExists
        ? std::filesystem::remove_all(packagePath, error)
        : 0;
    if (error) {
        operation.result = GuestPortableContentResult::AccessDenied;
        return operation;
    }
    const bool removedHeader = headerExists
        ? std::filesystem::remove(headerPath, error)
        : false;
    if (error) {
        operation.result = GuestPortableContentResult::AccessDenied;
        return operation;
    }
    operation.result = (removedEntries || removedHeader)
        ? GuestPortableContentResult::Success
        : GuestPortableContentResult::FileNotFound;
    return operation;
}

bool CloseGuestPortableContent(const std::string& rootName) {
    // Create/open, close, enumerate, and delete share one content-manager
    // operation boundary. In particular, deletion must not observe a package
    // between removing its mount and completing the close operation.
    std::lock_guard<std::mutex> operationLock(portableContentOperationMutex);
    return GetGuestFileSystem().UnregisterHostMount(rootName);
}

std::vector<uint8_t> EnumerateGuestPortableContent(uint32_t titleId,
                                                   uint32_t contentType) {
    std::lock_guard<std::mutex> operationLock(portableContentOperationMutex);
    std::vector<uint8_t> result;
    const auto headerRoot = PortableHeaderRoot(titleId, contentType);
    std::error_code error;
    if (!std::filesystem::is_directory(headerRoot, error) || error) return result;
    std::vector<std::filesystem::path> headers;
    for (std::filesystem::directory_iterator it(headerRoot, error), end;
         !error && it != end; it.increment(error)) {
        if (it->is_regular_file(error) && !error && it->path().extension() == L".header")
            headers.push_back(it->path());
    }
    if (error) return {};
    std::sort(headers.begin(), headers.end());
    for (const auto& headerPath : headers) {
        std::ifstream header(headerPath, std::ios::binary);
        std::array<uint8_t, kGuestXContentDataBytes> bytes{};
        header.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
        if (header.gcount() != static_cast<std::streamsize>(bytes.size()) ||
            ReadBigEndian32(bytes.data()) != kGuestPortableContentDeviceId ||
            ReadBigEndian32(bytes.data() + 4) != contentType) continue;
        result.insert(result.end(), bytes.begin(), bytes.end());
    }
    return result;
}

void GuestFileCacheConfiguration::SetElementCount(uint32_t cache, uint32_t count) {
    // Cache selector zero is the only selector dynamically reached. Keep it
    // as explicit state so a later FscGetCacheElementCount path has a coherent
    // source of truth instead of treating this configuration call as a no-op.
    if (cache == 0) cacheZeroElementCount_ = count;
}

uint32_t GuestFileCacheConfiguration::ElementCount(uint32_t cache) const {
    return cache == 0 ? cacheZeroElementCount_ : 0;
}

void GuestFileCacheConfiguration::ResetForTests() {
    cacheZeroElementCount_ = 0;
}

GuestFileCacheConfiguration& GetGuestFileCacheConfiguration() {
    static GuestFileCacheConfiguration configuration;
    return configuration;
}

void GuestFileSystem::SetGameRoot(const std::filesystem::path& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    gameRoot_ = std::filesystem::absolute(path).lexically_normal();
}

void GuestFileSystem::SetContentOverlays(
    std::vector<GuestContentOverlay> overlays) {
    std::lock_guard<std::mutex> lock(mutex_);
    contentOverlays_ = std::move(overlays);
}

bool GuestFileSystem::RegisterSymbolicLink(const std::string& path,
                                           const std::string& target) {
    const std::string name = CanonicalSymbolicLinkName(path);
    if (name.empty() || target.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    // Match the Xbox VFS contract used by ReXGlue: registering an existing
    // name does not replace its target, but the operation still succeeds.
    symbolicLinks_.emplace(name, target);
    return true;
}

bool GuestFileSystem::UnregisterSymbolicLink(const std::string& path) {
    const std::string name = CanonicalSymbolicLinkName(path);
    std::lock_guard<std::mutex> lock(mutex_);
    return symbolicLinks_.erase(name) != 0;
}

bool GuestFileSystem::RegisterHostMount(const std::string& rootName,
                                        const std::filesystem::path& hostRoot) {
    std::string name = CanonicalSymbolicLinkName(rootName);
    if (name.empty() || hostRoot.empty()) return false;
    if (name.back() != ':') name.push_back(':');
    std::error_code error;
    const auto normalized = std::filesystem::absolute(hostRoot, error).lexically_normal();
    if (error || !std::filesystem::is_directory(normalized, error) || error) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    return hostMounts_.emplace(std::move(name), normalized).second;
}

bool GuestFileSystem::UnregisterHostMount(const std::string& rootName) {
    std::string name = CanonicalSymbolicLinkName(rootName);
    if (!name.empty() && name.back() != ':') name.push_back(':');
    std::lock_guard<std::mutex> lock(mutex_);
    return hostMounts_.erase(name) != 0;
}

bool GuestFileSystem::IsHostPathMounted(const std::filesystem::path& hostRoot) const {
    std::error_code error;
    const std::filesystem::path normalized =
        std::filesystem::absolute(hostRoot, error).lexically_normal();
    if (error || normalized.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    return std::any_of(hostMounts_.begin(), hostMounts_.end(),
                       [&](const auto& mount) { return mount.second == normalized; });
}

uint32_t GuestFileSystem::OpenRawCachePartition(const char* path, uint32_t length) {
    if (!path || !EqualsIgnoreCase(path, length, kRawCachePartition)) return kInvalidHandle;
    // Verified portable policy: there is no raw Xbox cache medium or
    // console-private signing capability. Expose the ordinary missing-device
    // result so The Darkness takes its own cache-unavailable path instead of
    // creating an unsigned console-bound record.
    return kInvalidHandle;
}

uint32_t GuestFileSystem::OpenGameFile(const std::string& guestPath, bool synchronous,
                                       bool deleteRequested) {
    const GuestFileOpenResult result =
        CreateOrOpenGameFile(guestPath, synchronous, 1, false, deleteRequested);
    return result.status == GuestFileOpenStatus::Success ? result.handle : kInvalidHandle;
}

GuestFileOpenResult GuestFileSystem::CreateOrOpenGameFile(
    const std::string& guestPath, bool synchronous, uint32_t createDisposition,
    bool writeRequested, bool deleteRequested) {
    GuestFileOpenResult result{};
    if (createDisposition > 5) return result;

    std::filesystem::path root;
    std::unordered_map<std::string, std::string> symbolicLinks;
    std::unordered_map<std::string, std::filesystem::path> hostMounts;
    std::vector<GuestContentOverlay> overlays;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        root = gameRoot_;
        symbolicLinks = symbolicLinks_;
        hostMounts = hostMounts_;
        overlays = contentOverlays_;
    }
    std::filesystem::path hostPath;
    bool hostMounted{};
    if (!ResolveGamePath(root, guestPath, symbolicLinks, hostMounts, overlays, &hostPath,
                         &hostMounted)) {
        result.status = GuestFileOpenStatus::NoSuchFile;
        return result;
    }

    std::error_code error;
    const bool exists = std::filesystem::exists(hostPath, error);
    if (error) {
        result.status = GuestFileOpenStatus::AccessDenied;
        return result;
    }
    if (exists && !std::filesystem::is_regular_file(hostPath, error)) {
        result.status = GuestFileOpenStatus::AccessDenied;
        return result;
    }
    if (error) {
        result.status = GuestFileOpenStatus::AccessDenied;
        return result;
    }

    // The extracted title tree is immutable. Only XAM-created host mounts are
    // eligible for create/replace behavior, so save persistence cannot write
    // into or fabricate original game content.
    if (!hostMounted && (createDisposition != 1 || writeRequested)) {
        result.status = GuestFileOpenStatus::AccessDenied;
        return result;
    }

    switch (createDisposition) {
    case 1: // FILE_OPEN
    case 4: // FILE_OVERWRITE
        if (!exists) {
            result.status = GuestFileOpenStatus::NoSuchFile;
            return result;
        }
        break;
    case 2: // FILE_CREATE
        if (exists) {
            result.status = GuestFileOpenStatus::NameCollision;
            return result;
        }
        break;
    default:
        break;
    }

    const bool create = !exists;
    const bool truncate = exists &&
        (createDisposition == 0 || createDisposition == 4 || createDisposition == 5);
    if ((create || truncate) && (!hostMounted || !writeRequested)) {
        result.status = GuestFileOpenStatus::AccessDenied;
        return result;
    }
    if (create) {
        if (!std::filesystem::is_directory(hostPath.parent_path(), error) || error) {
            result.status = GuestFileOpenStatus::NoSuchFile;
            return result;
        }
    }
    if (create || truncate) {
        std::ofstream output(hostPath, std::ios::binary | std::ios::trunc);
        if (!output) {
            result.status = GuestFileOpenStatus::AccessDenied;
            return result;
        }
        output.flush();
        if (!output) {
            result.status = GuestFileOpenStatus::AccessDenied;
            return result;
        }
    }

    // Keep the host file open and read it on demand: copying a whole file
    // into memory at open made opening the 424 MB voice archive take seconds
    // and several open archives exhaust memory (#25).
    const bool hostWritable = hostMounted && writeRequested;
    auto file = std::make_shared<HostFile>();
    file->handle = CreateFileW(hostPath.c_str(),
        GENERIC_READ | (hostWritable ? GENERIC_WRITE : 0),
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    LARGE_INTEGER hostSize{};
    if (file->handle == INVALID_HANDLE_VALUE ||
        !GetFileSizeEx(static_cast<HANDLE>(file->handle), &hostSize) ||
        hostSize.QuadPart < 0 || uint64_t(hostSize.QuadPart) > UINT32_MAX) {
        result.status = GuestFileOpenStatus::AccessDenied;
        return result;
    }
    file->size = uint64_t(hostSize.QuadPart);
    if (file->size >= (16u << 20) && std::getenv("DARKNESS_LARGE_FILE_TRACE")) {
        std::fprintf(stderr, "REX_LARGE_FILE_OPEN path=%s size=%llu\n", guestPath.c_str(),
                     static_cast<unsigned long long>(file->size));
    }
    GuestFileAttributes attributes{};
    if (!ReadHostAttributes(hostPath, &attributes)) {
        result.status = GuestFileOpenStatus::AccessDenied;
        return result;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const uint32_t handle = nextHandle_++;
    gameFileHandles_.emplace(handle, OpenFileData{std::move(file), attributes,
        guestPath, hostPath, synchronous, hostMounted && writeRequested,
        hostMounted && deleteRequested});
    result.status = GuestFileOpenStatus::Success;
    result.handle = handle;
    result.information = create ? 2u : createDisposition == 0 ? 0u :
        truncate ? 3u : 1u;
    return result;
}

uint32_t GuestFileSystem::OpenGameDirectory(const std::string& guestPath) {
    std::filesystem::path root;
    std::unordered_map<std::string, std::string> symbolicLinks;
    std::unordered_map<std::string, std::filesystem::path> hostMounts;
    std::vector<GuestContentOverlay> overlays;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        root = gameRoot_;
        symbolicLinks = symbolicLinks_;
        hostMounts = hostMounts_;
        overlays = contentOverlays_;
    }
    std::filesystem::path hostPath;
    bool hostMounted{};
    std::filesystem::path relative;
    if (!ResolveGamePath(root, guestPath, symbolicLinks, hostMounts, overlays,
                         &hostPath, &hostMounted, &relative) ||
        !std::filesystem::is_directory(hostPath)) {
        return kInvalidHandle;
    }
    GuestFileAttributes attributes{};
    if (!ReadHostAttributes(hostPath, &attributes)) return kInvalidHandle;

    OpenDirectoryData directory{};
    directory.attributes = attributes;
    std::vector<std::filesystem::path> directorySources;
    if (hostMounted) {
        directorySources.push_back(hostPath);
    } else {
        for (const auto& overlay : overlays) {
            std::filesystem::path candidate;
            if (ResolveOverlayCandidate(overlay, relative, &candidate) &&
                std::filesystem::is_directory(candidate)) {
                directorySources.push_back(std::move(candidate));
            }
        }
        std::filesystem::path baseDirectory;
        if (ResolveCaseInsensitiveContainedPath(root, relative, false,
                                                &baseDirectory) &&
            std::filesystem::is_directory(baseDirectory))
            directorySources.push_back(baseDirectory);
    }
    std::unordered_map<std::string, size_t> entryByName;
    for (const auto& source : directorySources) {
        for (const auto& child : std::filesystem::directory_iterator(source)) {
            std::string key = child.path().filename().string();
            std::transform(key.begin(), key.end(), key.begin(),
                           [](unsigned char value) {
                               return static_cast<char>(std::tolower(value));
                           });
            if (entryByName.find(key) != entryByName.end()) continue;
            GuestFileAttributes childAttributes{};
            if (!ReadHostAttributes(child.path(), &childAttributes)) continue;
            entryByName.emplace(key, directory.entries.size());
            directory.entries.push_back(
                {child.path().filename().string(), childAttributes, 0});
        }
    }
    std::sort(directory.entries.begin(), directory.entries.end(), [](const auto& left, const auto& right) {
        std::string leftName = left.name;
        std::string rightName = right.name;
        std::transform(leftName.begin(), leftName.end(), leftName.begin(), [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
        std::transform(rightName.begin(), rightName.end(), rightName.begin(), [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
        return leftName < rightName;
    });
    for (size_t index = 0; index < directory.entries.size(); ++index)
        directory.entries[index].index = static_cast<uint32_t>(index + 1);

    std::lock_guard<std::mutex> lock(mutex_);
    const uint32_t handle = nextHandle_++;
    gameDirectoryHandles_.emplace(handle, std::move(directory));
    return handle;
}

bool GuestFileSystem::QueryGamePath(const std::string& guestPath, GuestFileAttributes* attributes) const {
    if (!attributes) return false;

    std::filesystem::path root;
    std::unordered_map<std::string, std::string> symbolicLinks;
    std::unordered_map<std::string, std::filesystem::path> hostMounts;
    std::vector<GuestContentOverlay> overlays;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        root = gameRoot_;
        symbolicLinks = symbolicLinks_;
        hostMounts = hostMounts_;
        overlays = contentOverlays_;
    }
    if (root.empty()) return false;
    std::filesystem::path hostPath;
    if (!ResolveGamePath(root, guestPath, symbolicLinks, hostMounts, overlays,
                         &hostPath)) return false;
    return ReadHostAttributes(hostPath, attributes);
}

bool GuestFileSystem::QueryOpenFile(uint32_t handle, GuestFileAttributes* attributes) const {
    if (!attributes) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto file = gameFileHandles_.find(handle);
    if (file != gameFileHandles_.end()) {
        *attributes = file->second.attributes;
        return true;
    }
    const auto directory = gameDirectoryHandles_.find(handle);
    if (directory == gameDirectoryHandles_.end()) return false;
    *attributes = directory->second.attributes;
    return true;
}

bool GuestFileSystem::QueryOpenFileSynchronous(uint32_t handle, bool* synchronous) const {
    if (!synchronous) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto file = gameFileHandles_.find(handle);
    if (file == gameFileHandles_.end()) return false;
    *synchronous = file->second.synchronous;
    return true;
}

bool GuestFileSystem::QueryOpenFilePath(uint32_t handle, std::string* guestPath) const {
    if (!guestPath) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto file = gameFileHandles_.find(handle);
    if (file == gameFileHandles_.end()) return false;
    *guestPath = file->second.guestPath;
    return true;
}

bool GuestFileSystem::SetOpenFilePosition(uint32_t handle, uint64_t position) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto file = gameFileHandles_.find(handle);
    if (file == gameFileHandles_.end()) return false;
    file->second.currentPosition = position;
    return true;
}

bool GuestFileSystem::QueryOpenFilePosition(uint32_t handle, uint64_t* position) const {
    if (!position) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto file = gameFileHandles_.find(handle);
    if (file == gameFileHandles_.end()) return false;
    *position = file->second.currentPosition;
    return true;
}

GuestFileWriteResult GuestFileSystem::SetOpenFileDisposition(uint32_t handle,
                                                              bool deleteOnClose) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto file = gameFileHandles_.find(handle);
    if (file == gameFileHandles_.end()) return GuestFileWriteResult::InvalidHandle;
    // FileDispositionInformation requires DELETE access, independently of
    // data-write access. Extracted title data remains immutable because the
    // capability is granted only to handles under an explicit portable-content
    // mount that requested DELETE when opened.
    if (deleteOnClose && !file->second.deleteAllowed)
        return GuestFileWriteResult::AccessDenied;
    file->second.deleteOnClose = deleteOnClose;
    return GuestFileWriteResult::Success;
}

GuestFileWriteResult GuestFileSystem::SetOpenFileLength(uint32_t handle, uint64_t length) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto file = gameFileHandles_.find(handle);
    if (file == gameFileHandles_.end()) return GuestFileWriteResult::InvalidHandle;
    if (!file->second.writable || length > UINT32_MAX)
        return GuestFileWriteResult::AccessDenied;

    HostFile& host = *file->second.file;
    LARGE_INTEGER end{};
    end.QuadPart = LONGLONG(length);
    if (!SetFilePointerEx(static_cast<HANDLE>(host.handle), end, nullptr, FILE_BEGIN) ||
        !SetEndOfFile(static_cast<HANDLE>(host.handle)))
        return GuestFileWriteResult::AccessDenied;
    host.size = length;
    ReadHostAttributes(file->second.hostPath, &file->second.attributes);
    return GuestFileWriteResult::Success;
}

GuestDirectoryQueryResult GuestFileSystem::QueryDirectory(uint32_t handle, const std::string& pattern,
    bool restartScan, GuestDirectoryEntry* entry) {
    if (!entry) return GuestDirectoryQueryResult::InvalidHandle;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto directory = gameDirectoryHandles_.find(handle);
    if (directory == gameDirectoryHandles_.end()) return GuestDirectoryQueryResult::InvalidHandle;

    auto& state = directory->second;
    // NtQueryDirectoryFile establishes the wildcard on the first query and
    // retains it in the open file object for null-name continuations. This is
    // also the behavior of the pinned Xenia XFile::QueryDirectory model. The
    // title relies on it while enumerating Registry\MultiplayerMaps*: dropping
    // the rule after the first result incorrectly exposes every later .xcr
    // file as part of that query.
    if (!pattern.empty()) {
        state.searchPattern = pattern;
        state.nextEntry = 0;
    } else if (restartScan) {
        state.nextEntry = 0;
    }
    for (size_t index = state.nextEntry; index < state.entries.size(); ++index) {
        if (!state.searchPattern.empty() &&
            !MatchesPattern(state.entries[index].name, state.searchPattern)) continue;
        *entry = state.entries[index];
        state.nextEntry = index + 1;
        return GuestDirectoryQueryResult::Success;
    }
    return pattern.empty() ? GuestDirectoryQueryResult::NoMoreFiles
                           : GuestDirectoryQueryResult::NoSuchFile;
}

bool GuestFileSystem::QueryVolumeSize(uint32_t handle, GuestVolumeSizeInformation* information) const {
    if (!information) return false;
    std::filesystem::path root;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (gameFileHandles_.find(handle) == gameFileHandles_.end() &&
            gameDirectoryHandles_.find(handle) == gameDirectoryHandles_.end()) return false;
        root = gameRoot_;
    }
    DWORD sectorsPerCluster{};
    DWORD bytesPerSector{};
    DWORD freeClusters{};
    DWORD totalClusters{};
    const std::filesystem::path volumeRoot = root.root_path();
    if (volumeRoot.empty() ||
        !GetDiskFreeSpaceW(volumeRoot.c_str(), &sectorsPerCluster, &bytesPerSector, &freeClusters, &totalClusters) ||
        !sectorsPerCluster || !bytesPerSector) return false;
    information->totalAllocationUnits = totalClusters;
    information->availableAllocationUnits = freeClusters;
    information->sectorsPerAllocationUnit = sectorsPerCluster;
    information->bytesPerSector = bytesPerSector;
    return true;
}

bool GuestFileSystem::Read(uint32_t handle, uint64_t offset, uint8_t* destination,
                           uint32_t byteCount, uint32_t* bytesRead,
                           bool useCurrentPosition) {
    std::shared_ptr<HostFile> host;
    uint32_t count{};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto file = gameFileHandles_.find(handle);
        if (file == gameFileHandles_.end() || !destination) return false;
        if (useCurrentPosition) offset = file->second.currentPosition;
        host = file->second.file;
        const uint64_t size = host->size;
        if (offset > size) return false;
        count = static_cast<uint32_t>(std::min<uint64_t>(byteCount, size - offset));
        if (useCurrentPosition) file->second.currentPosition += count;
    }
    // The disk read runs outside the filesystem lock, so streams on other
    // threads (music, voice, textures) are never queued behind it.
    if (count && !host->ReadAt(offset, destination, count)) return false;
    if (bytesRead) *bytesRead = count;
    return true;
}

GuestFileWriteResult GuestFileSystem::Write(uint32_t handle, uint64_t offset,
                                              const uint8_t* source, uint32_t byteCount,
                                              uint32_t* bytesWritten,
                                              bool useCurrentPosition) {
    if (bytesWritten) *bytesWritten = 0;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto file = gameFileHandles_.find(handle);
    if (file == gameFileHandles_.end() || (!source && byteCount))
        return GuestFileWriteResult::InvalidHandle;
    if (!file->second.writable) return GuestFileWriteResult::AccessDenied;
    if (useCurrentPosition) offset = file->second.currentPosition;
    const uint64_t end = offset + uint64_t(byteCount);
    if (end < offset || end > UINT32_MAX) return GuestFileWriteResult::AccessDenied;

    HostFile& host = *file->second.file;
    if (byteCount && !host.WriteAt(offset, source, byteCount))
        return GuestFileWriteResult::AccessDenied;
    if (end > host.size) host.size = end;
    ReadHostAttributes(file->second.hostPath, &file->second.attributes);
    if (useCurrentPosition) file->second.currentPosition += byteCount;
    if (bytesWritten) *bytesWritten = byteCount;
    return GuestFileWriteResult::Success;
}

GuestFileWriteResult GuestFileSystem::Flush(uint32_t handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto file = gameFileHandles_.find(handle);
    if (file == gameFileHandles_.end()) return GuestFileWriteResult::InvalidHandle;

    // Read-only title handles cannot contain writes made through this runtime,
    // so there is no dirty host state to commit. Writable XAM content is the
    // dynamically relevant path: reopen the same authoritative host file with
    // write access and issue the synchronous stable-storage flush required by
    // NtFlushBuffersFile.
    if (!file->second.writable) return GuestFileWriteResult::Success;
    const HANDLE hostFile = CreateFileW(
        file->second.hostPath.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hostFile == INVALID_HANDLE_VALUE) return GuestFileWriteResult::AccessDenied;
    const bool flushed = FlushFileBuffers(hostFile) != FALSE;
    CloseHandle(hostFile);
    return flushed ? GuestFileWriteResult::Success : GuestFileWriteResult::AccessDenied;
}

bool GuestFileSystem::Close(uint32_t handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto file = gameFileHandles_.find(handle);
    if (file != gameFileHandles_.end()) {
        const bool deleteOnClose = file->second.deleteOnClose;
        const std::filesystem::path hostPath = file->second.hostPath;
        gameFileHandles_.erase(file);
        if (deleteOnClose) {
            bool stillOpen{};
            for (auto& [otherHandle, otherFile] : gameFileHandles_) {
                if (otherFile.hostPath == hostPath) {
                    // FileDispositionInformation belongs to the file object.
                    // Retain the pending state until the final open handle is
                    // closed instead of deleting storage still in active use.
                    otherFile.deleteOnClose = true;
                    stillOpen = true;
                }
            }
            if (!stillOpen) {
                std::error_code error;
                std::filesystem::remove(hostPath, error);
            }
        }
        return true;
    }
    return gameDirectoryHandles_.erase(handle) != 0;
}

void GuestFileSystem::ResetForTests() {
    std::lock_guard<std::mutex> lock(mutex_);
    nextHandle_ = 0x40000000;
    gameFileHandles_.clear();
    gameDirectoryHandles_.clear();
    symbolicLinks_.clear();
    hostMounts_.clear();
    contentOverlays_.clear();
}

GuestFileSystem& GetGuestFileSystem() {
    static GuestFileSystem filesystem;
    return filesystem;
}
