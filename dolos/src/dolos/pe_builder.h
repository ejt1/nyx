#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <windows.h>

namespace dolos {

struct SectionInfo {
  char name[8];
  std::uint32_t virtual_address;  // RVA from module base
  std::uint32_t virtual_size;
  std::uint32_t characteristics;  // Section characteristics (IMAGE_SCN_*)
};

class PEBuilder {
 public:
  PEBuilder(std::uintptr_t image_base, std::size_t image_size);

  void AddSection(const SectionInfo& section);

  bool Build(const std::vector<std::uint8_t>& raw_buffer, std::vector<std::uint8_t>& output);

  bool WriteExecutable(const std::vector<std::uint8_t>& raw_buffer, const std::string& path);

  static bool WriteMemoryDump(const std::vector<std::uint8_t>& buffer, const std::string& path);

  static std::uint32_t MapProtectionToCharacteristics(DWORD protect);

  static void DeriveSectionName(SectionInfo& sec, std::size_t index);

 private:
  void BuildDosHeader(std::vector<std::uint8_t>& out);
  void BuildNtHeaders(std::vector<std::uint8_t>& out);
  void BuildSectionHeaders(std::vector<std::uint8_t>& out);

  std::uintptr_t image_base_;
  std::size_t image_size_;
  std::vector<SectionInfo> sections_;
};

}  // namespace dolos
