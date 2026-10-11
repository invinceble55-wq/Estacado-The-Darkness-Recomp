#pragma once

#include <cstdint>
#include <array>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Minimal state for the reached File System Cache (FSC) configuration API.
// It intentionally does not claim to implement an Xbox filesystem cache.
class GuestFileCacheConfiguration {
public:
    void SetElementCount(uint32_t cache, uint32_t count);
    uint32_t ElementCount(uint32_t cache) const;
    void ResetForTests();

private:
    uint32_t cacheZeroElementCount_{};
};

GuestFileCacheConfiguration& GetGuestFileCacheConfiguration();

struct GuestFileAttributes {
    uint64_t creationTime{};
    uint64_t lastAccessTime{};
    uint64_t lastWriteTime{};
    uint64_t changeTime{};
    uint64_t allocationSize{};
    uint64_t endOfFile{};
    uint32_t attributes{};
};

struct GuestVolumeSizeInformation {
    uint64_t totalAllocationUnits{};
    uint64_t availableAllocationUnits{};
    uint32_t sectorsPerAllocationUnit{};
    uint32_t bytesPerSector{};
};

struct GuestDirectoryEntry {
    std::string name;
    GuestFileAttributes attributes;
    uint32_t index{};
};

enum class GuestDirectoryQueryResult {
    Success,
    NoMoreFiles,
    NoSuchFile,
    InvalidHandle,
};

enum class GuestFileOpenStatus {
    Success,
    InvalidParameter,
    NoSuchFile,
    NameCollision,
    AccessDenied,
};

struct GuestFileOpenResult {
    GuestFileOpenStatus status{GuestFileOpenStatus::InvalidParameter};
    uint32_t handle{};
    uint32_t information{};
};

enum class GuestFileWriteResult {
    Success,
    InvalidHandle,
    AccessDenied,
};

// Ordered highest-precedence first. Roots are canonical, read-only host
// directories under the runtime's dedicated mods directory.
struct GuestContentOverlay {
    std::string id;
    std::filesystem::path root;
    int32_t priority{};
};

// Minimal virtual filesystem state. The supplied extracted game directory is
// read-only title content. Explicit XAM portable-content mounts may additionally
// expose paths rooted under runtime_data/content; arbitrary console devices are
// never redirected to host files.
class GuestFileSystem {
public:
    static constexpr uint32_t kInvalidHandle = 0;

    void SetGameRoot(const std::filesystem::path& path);
    void SetContentOverlays(std::vector<GuestContentOverlay> overlays);
    bool RegisterSymbolicLink(const std::string& path, const std::string& target);
    bool UnregisterSymbolicLink(const std::string& path);
    bool RegisterHostMount(const std::string& rootName, const std::filesystem::path& hostRoot);
    bool UnregisterHostMount(const std::string& rootName);
    bool IsHostPathMounted(const std::filesystem::path& hostRoot) const;
    uint32_t OpenRawCachePartition(const char* path, uint32_t length);
    uint32_t OpenGameFile(const std::string& guestPath, bool synchronous,
        bool deleteRequested = false);
    GuestFileOpenResult CreateOrOpenGameFile(const std::string& guestPath,
        bool synchronous, uint32_t createDisposition, bool writeRequested,
        bool deleteRequested = false);
    uint32_t OpenGameDirectory(const std::string& guestPath);
    bool QueryGamePath(const std::string& guestPath, GuestFileAttributes* attributes) const;
    bool QueryOpenFile(uint32_t handle, GuestFileAttributes* attributes) const;
    bool QueryOpenFilePath(uint32_t handle, std::string* guestPath) const;
    bool QueryOpenFileSynchronous(uint32_t handle, bool* synchronous) const;
    bool QueryOpenFilePosition(uint32_t handle, uint64_t* position) const;
    bool SetOpenFilePosition(uint32_t handle, uint64_t position);
    GuestFileWriteResult SetOpenFileDisposition(uint32_t handle, bool deleteOnClose);
    GuestFileWriteResult SetOpenFileLength(uint32_t handle, uint64_t length);
    bool QueryVolumeSize(uint32_t handle, GuestVolumeSizeInformation* information) const;
    GuestDirectoryQueryResult QueryDirectory(uint32_t handle, const std::string& pattern,
        bool restartScan, GuestDirectoryEntry* entry);
    bool Read(uint32_t handle, uint64_t offset, uint8_t* destination, uint32_t byteCount,
        uint32_t* bytesRead, bool useCurrentPosition = false);
    GuestFileWriteResult Write(uint32_t handle, uint64_t offset, const uint8_t* source,
        uint32_t byteCount, uint32_t* bytesWritten, bool useCurrentPosition = false);
    GuestFileWriteResult Flush(uint32_t handle);
    bool Close(uint32_t handle);
    void ResetForTests();

private:
    uint32_t nextHandle_ = 0x40000000;
    std::filesystem::path gameRoot_;
    std::vector<GuestContentOverlay> contentOverlays_;
    std::unordered_map<std::string, std::string> symbolicLinks_;
    std::unordered_map<std::string, std::filesystem::path> hostMounts_;
    // An open host file read and written at explicit offsets (#25: files are
    // no longer copied into memory whole at open; the game's streamed audio
    // and texture archives are hundreds of megabytes each).
    struct HostFile;
    struct OpenFileData {
        std::shared_ptr<HostFile> file;
        GuestFileAttributes attributes;
        std::string guestPath;
        std::filesystem::path hostPath;
        bool synchronous{};
        bool writable{};
        bool deleteAllowed{};
        uint64_t currentPosition{};
        bool deleteOnClose{};
    };
    std::unordered_map<uint32_t, OpenFileData> gameFileHandles_;
    struct OpenDirectoryData {
        GuestFileAttributes attributes;
        std::vector<GuestDirectoryEntry> entries;
        size_t nextEntry{};
        std::string searchPattern;
    };
    std::unordered_map<uint32_t, OpenDirectoryData> gameDirectoryHandles_;
    mutable std::mutex mutex_;
};

GuestFileSystem& GetGuestFileSystem();

// The native port exposes one real, portable writable content device beside
// the runtime executable. It is intentionally distinct from the extracted,
// read-only game tree and from unavailable console-private cache storage.
inline constexpr uint32_t kGuestPortableContentDeviceId = 1;
inline constexpr size_t kGuestXContentDataBytes = 0x134;

enum class GuestPortableContentResult {
    Success,
    InvalidParameter,
    AlreadyExists,
    FileNotFound,
    PathNotFound,
    AccessDenied,
};

struct GuestPortableContentOperation {
    GuestPortableContentResult result{GuestPortableContentResult::InvalidParameter};
    uint32_t disposition{}; // 1 = created, 2 = opened
    std::filesystem::path packagePath;
};

std::filesystem::path GuestPortableContentDeviceRoot();
void ConfigureGuestPortableContentDeviceRoot(const std::filesystem::path& root);
bool EnsureGuestPortableContentDevice(uint64_t requestedBytes);
GuestPortableContentOperation OpenGuestPortableContent(
    const std::string& rootName, uint32_t titleId, uint32_t contentType,
    const std::string& fileName,
    const std::array<uint8_t, kGuestXContentDataBytes>& rawContentData,
    uint32_t createDisposition, uint64_t requestedBytes);
GuestPortableContentOperation DeleteGuestPortableContent(
    uint32_t titleId, uint32_t contentType, const std::string& fileName,
    const std::array<uint8_t, kGuestXContentDataBytes>& rawContentData);
bool CloseGuestPortableContent(const std::string& rootName);
std::vector<uint8_t> EnumerateGuestPortableContent(uint32_t titleId,
                                                   uint32_t contentType);
