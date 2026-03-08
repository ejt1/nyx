#pragma once

#include "dolos/pattern_scanner.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dolos {

// Binary format for storing resolved offsets on disk.
//
// Header (24 bytes):
//   uint32_t magic;           // "D2RO" (0x4F523244)
//   uint32_t version;         // Cache format version
//   uint64_t exe_hash;        // Hash of executable for version detection
//   uint32_t entry_count;     // Number of cached offsets
//   uint32_t signature_hash;  // Hash of all patterns (dirty detection)
//
// Entries (variable size, repeated entry_count times):
//   uint32_t name_length;     // Length of name string
//   char name[name_length];   // Offset name (no null terminator)
//   uint64_t offset_value;    // Resolved offset value

constexpr std::uint32_t kCacheMagic = 0x4F523244;  // "D2RO"
constexpr std::uint32_t kCacheVersion = 1;

struct CacheEntry {
  std::string name;
  std::uint64_t offset;
};

struct OffsetCache {
  std::uint64_t exe_hash;
  std::uint32_t signature_hash;
  std::vector<CacheEntry> entries;
};

class OffsetCacheManager {
 public:
  OffsetCacheManager();
  explicit OffsetCacheManager(const std::string& cache_dir);

  std::uint64_t ComputeExecutableHash();
  std::uint32_t ComputeSignatureHash(const std::vector<SignatureDef>& signatures);

  // Try to load cache from disk
  // Returns nullopt if:
  //   - Cache file doesn't exist
  //   - Cache is invalid (wrong magic/version)
  //   - exe_hash doesn't match
  //   - signature_hash doesn't match
  std::optional<OffsetCache> LoadCache(std::uint64_t expected_exe_hash, std::uint32_t expected_sig_hash);

  bool SaveCache(const OffsetCache& cache);
  bool SaveCacheTo(const OffsetCache& cache, const std::string& path);

  std::string GetCachePath(std::uint64_t exe_hash);
  std::string GetCachePath(const std::string& filename);
  bool EnsureCacheDirectory();

 private:
  std::string cache_dir_;
};

}  // namespace dolos
