#include "dolos/offset_cache.h"
#include "dolos/pipe_log.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

#include <windows.h>

namespace dolos {

namespace {

// TODO: move elsewhere, this can be used by others
constexpr std::uint32_t kFnvPrime32 = 0x01000193;
constexpr std::uint32_t kFnvOffset32 = 0x811c9dc5;

std::uint32_t Fnv1aHash(const void* data, std::size_t size) {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  std::uint32_t hash = kFnvOffset32;
  for (std::size_t i = 0; i < size; ++i) {
    hash ^= bytes[i];
    hash *= kFnvPrime32;
  }
  return hash;
}

std::uint32_t Fnv1aHash(std::string_view str) {
  return Fnv1aHash(str.data(), str.size());
}

}  // namespace

OffsetCacheManager::OffsetCacheManager() {
  char exe_path[MAX_PATH];
  GetModuleFileNameA(nullptr, exe_path, MAX_PATH);

  std::filesystem::path exe_dir = std::filesystem::path(exe_path).parent_path();
  cache_dir_ = (exe_dir / "cache" / "offsets").string();
}

OffsetCacheManager::OffsetCacheManager(const std::string& cache_dir)
    : cache_dir_(cache_dir) {}

std::uint64_t OffsetCacheManager::ComputeExecutableHash() {
  char path[MAX_PATH];
  GetModuleFileNameA(nullptr, path, MAX_PATH);

  HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    PIPE_LOG_ERROR("[OffsetCache] Failed to open executable for hashing");
    return 0;
  }

  LARGE_INTEGER file_size;
  if (!GetFileSizeEx(file, &file_size)) {
    CloseHandle(file);
    return 0;
  }

  IMAGE_DOS_HEADER dos_header;
  DWORD bytes_read;
  if (!ReadFile(file, &dos_header, sizeof(dos_header), &bytes_read, nullptr) || bytes_read != sizeof(dos_header)) {
    CloseHandle(file);
    return 0;
  }

  SetFilePointer(file, dos_header.e_lfanew, nullptr, FILE_BEGIN);

  IMAGE_NT_HEADERS64 nt_headers;
  if (!ReadFile(file, &nt_headers, sizeof(nt_headers), &bytes_read, nullptr) || bytes_read != sizeof(nt_headers)) {
    CloseHandle(file);
    return 0;
  }

  CloseHandle(file);

  std::uint64_t hash = static_cast<std::uint64_t>(file_size.QuadPart);
  hash ^= static_cast<std::uint64_t>(nt_headers.FileHeader.TimeDateStamp) << 32;
  hash ^= static_cast<std::uint64_t>(nt_headers.OptionalHeader.CheckSum) << 16;

  PIPE_LOG_DEBUG("[OffsetCache] Computed executable hash: {:016X}", hash);
  return hash;
}

std::uint32_t OffsetCacheManager::ComputeSignatureHash(const std::vector<SignatureDef>& signatures) {
  // hash all pattern strings together
  // if any pattern changes, the hash changes and cache is invalidated
  std::uint32_t hash = kFnvOffset32;

  for (const auto& sig : signatures) {
    for (char c : std::string_view(sig.name)) {
      hash ^= static_cast<std::uint8_t>(c);
      hash *= kFnvPrime32;
    }

    hash ^= 0xFF;
    hash *= kFnvPrime32;

    for (char c : sig.pattern) {
      hash ^= static_cast<std::uint8_t>(c);
      hash *= kFnvPrime32;
    }

    hash ^= 0xFE;
    hash *= kFnvPrime32;
  }

  PIPE_LOG_DEBUG("[OffsetCache] Computed signature hash: {:08X}", hash);
  return hash;
}

std::string OffsetCacheManager::GetCachePath(std::uint64_t exe_hash) {
  std::ostringstream ss;
  ss << cache_dir_ << "/" << std::hex << std::setfill('0') << std::setw(16) << exe_hash << ".bin";
  return ss.str();
}

std::string OffsetCacheManager::GetCachePath(const std::string& filename) {
  return cache_dir_ + "/" + filename;
}

bool OffsetCacheManager::EnsureCacheDirectory() {
  try {
    std::filesystem::create_directories(cache_dir_);
    return true;
  } catch (const std::exception& e) {
    PIPE_LOG_ERROR("[OffsetCache] Failed to create cache directory: {}", e.what());
    return false;
  }
}

std::optional<OffsetCache> OffsetCacheManager::LoadCache(std::uint64_t expected_exe_hash,
                                                         std::uint32_t expected_sig_hash) {
  std::string cache_path = GetCachePath(expected_exe_hash);

  std::ifstream file(cache_path, std::ios::binary);
  if (!file) {
    PIPE_LOG_DEBUG("[OffsetCache] No cache file found at {}", cache_path);
    return std::nullopt;
  }

  std::uint32_t magic, version;
  std::uint64_t exe_hash;
  std::uint32_t entry_count, sig_hash;

  file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
  file.read(reinterpret_cast<char*>(&version), sizeof(version));
  file.read(reinterpret_cast<char*>(&exe_hash), sizeof(exe_hash));
  file.read(reinterpret_cast<char*>(&entry_count), sizeof(entry_count));
  file.read(reinterpret_cast<char*>(&sig_hash), sizeof(sig_hash));

  if (!file) {
    PIPE_LOG_WARN("[OffsetCache] Failed to read cache header");
    return std::nullopt;
  }

  if (magic != kCacheMagic) {
    PIPE_LOG_WARN("[OffsetCache] Invalid cache magic: {:08X}", magic);
    return std::nullopt;
  }

  if (version != kCacheVersion) {
    PIPE_LOG_WARN("[OffsetCache] Cache version mismatch: {} (expected {})", version, kCacheVersion);
    return std::nullopt;
  }

  if (exe_hash != expected_exe_hash) {
    PIPE_LOG_WARN("[OffsetCache] Executable hash mismatch: {:016X} (expected {:016X})", exe_hash, expected_exe_hash);
    return std::nullopt;
  }

  if (sig_hash != expected_sig_hash) {
    PIPE_LOG_WARN("[OffsetCache] Signature hash mismatch: {:08X} (expected {:08X}) - patterns changed",
                  sig_hash,
                  expected_sig_hash);
    return std::nullopt;
  }

  if (entry_count > 1000) {
    PIPE_LOG_WARN("[OffsetCache] Suspicious entry count: {}", entry_count);
    return std::nullopt;
  }

  OffsetCache cache;
  cache.exe_hash = exe_hash;
  cache.signature_hash = sig_hash;
  cache.entries.reserve(entry_count);

  for (std::uint32_t i = 0; i < entry_count; ++i) {
    std::uint32_t name_length;
    file.read(reinterpret_cast<char*>(&name_length), sizeof(name_length));

    if (!file || name_length > 256) {
      PIPE_LOG_WARN("[OffsetCache] Invalid entry name length: {}", name_length);
      return std::nullopt;
    }

    CacheEntry entry;
    entry.name.resize(name_length);
    file.read(entry.name.data(), name_length);

    file.read(reinterpret_cast<char*>(&entry.offset), sizeof(entry.offset));

    if (!file) {
      PIPE_LOG_WARN("[OffsetCache] Failed to read cache entry {}", i);
      return std::nullopt;
    }

    cache.entries.push_back(std::move(entry));
  }

  PIPE_LOG_DEBUG("[OffsetCache] Loaded {} entries from cache", cache.entries.size());
  return cache;
}

bool OffsetCacheManager::SaveCache(const OffsetCache& cache) {
  return SaveCacheTo(cache, GetCachePath(cache.exe_hash));
}

bool OffsetCacheManager::SaveCacheTo(const OffsetCache& cache, const std::string& cache_path) {
  std::size_t last_slash = cache_path.find_last_of("/\\");
  if (last_slash != std::string::npos) {
    std::filesystem::create_directories(cache_path.substr(0, last_slash));
  }

  std::ofstream file(cache_path, std::ios::binary | std::ios::trunc);
  if (!file) {
    PIPE_LOG_ERROR("[OffsetCache] Failed to create cache file: {}", cache_path);
    return false;
  }

  std::uint32_t magic = kCacheMagic;
  std::uint32_t version = kCacheVersion;
  std::uint32_t entry_count = static_cast<std::uint32_t>(cache.entries.size());

  file.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
  file.write(reinterpret_cast<const char*>(&version), sizeof(version));
  file.write(reinterpret_cast<const char*>(&cache.exe_hash), sizeof(cache.exe_hash));
  file.write(reinterpret_cast<const char*>(&entry_count), sizeof(entry_count));
  file.write(reinterpret_cast<const char*>(&cache.signature_hash), sizeof(cache.signature_hash));

  for (const auto& entry : cache.entries) {
    std::uint32_t name_length = static_cast<std::uint32_t>(entry.name.size());
    file.write(reinterpret_cast<const char*>(&name_length), sizeof(name_length));
    file.write(entry.name.data(), name_length);
    file.write(reinterpret_cast<const char*>(&entry.offset), sizeof(entry.offset));
  }

  if (!file) {
    PIPE_LOG_ERROR("[OffsetCache] Failed to write cache file");
    return false;
  }

  PIPE_LOG_DEBUG("[OffsetCache] Saved {} entries to cache", cache.entries.size());
  return true;
}

}  // namespace dolos
