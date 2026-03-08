#include "dolos/pe_builder.h"
#include "dolos/pipe_log.h"

#include <cstring>
#include <filesystem>
#include <fstream>

namespace dolos {

namespace {

constexpr std::uint32_t AlignUp(std::uint32_t value, std::uint32_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

}  // namespace

PEBuilder::PEBuilder(std::uintptr_t image_base, std::size_t image_size)
    : image_base_(image_base), image_size_(image_size) {}

void PEBuilder::AddSection(const SectionInfo& section) {
  sections_.push_back(section);
}

std::uint32_t PEBuilder::MapProtectionToCharacteristics(DWORD protect) {
  std::uint32_t chars = 0;

  if (protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) {
    chars |= IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ;
    if (protect & (PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) {
      chars |= IMAGE_SCN_MEM_WRITE;
    }
  } else if (protect & (PAGE_READWRITE | PAGE_WRITECOPY)) {
    chars |= IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;
  } else if (protect & (PAGE_READONLY)) {
    chars |= IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;
  } else {
    chars |= IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;
  }

  return chars;
}

void PEBuilder::DeriveSectionName(SectionInfo& sec, std::size_t index) {
  std::memset(sec.name, 0, 8);

  // Name based on characteristics
  if (sec.characteristics & IMAGE_SCN_MEM_EXECUTE) {
    if (index == 0) {
      std::memcpy(sec.name, ".text", 5);
    } else {
      char buf[8];
      std::snprintf(buf, sizeof(buf), ".code%zu", index);
      std::memcpy(sec.name, buf, 7);
    }
  } else if (sec.characteristics & IMAGE_SCN_MEM_WRITE) {
    std::memcpy(sec.name, ".data", 5);
  } else {
    // Read-only data - differentiate by RVA
    if (sec.virtual_address < 0x200000) {
      std::memcpy(sec.name, ".rdata", 6);
    } else {
      std::memcpy(sec.name, ".rodata", 7);
    }
  }
}

void PEBuilder::BuildDosHeader(std::vector<std::uint8_t>& out) {
  IMAGE_DOS_HEADER dos = {};
  dos.e_magic = IMAGE_DOS_SIGNATURE;
  dos.e_cblp = 0x90;
  dos.e_cp = 0x03;
  dos.e_cparhdr = 0x04;
  dos.e_maxalloc = 0xFFFF;
  dos.e_sp = 0xB8;
  dos.e_lfarlc = 0x40;
  dos.e_lfanew = 0x80;

  const auto* ptr = reinterpret_cast<const std::uint8_t*>(&dos);
  out.insert(out.end(), ptr, ptr + sizeof(dos));

  static const std::uint8_t kDosStub[] = {0x0E, 0x1F, 0xBA, 0x0E, 0x00, 0xB4, 0x09, 0xCD, 0x21, 0xB8, 0x01, 0x4C, 0xCD,
                                          0x21, 0x54, 0x68, 0x69, 0x73, 0x20, 0x70, 0x72, 0x6F, 0x67, 0x72, 0x61, 0x6D,
                                          0x20, 0x63, 0x61, 0x6E, 0x6E, 0x6F, 0x74, 0x20, 0x62, 0x65, 0x20, 0x72, 0x75,
                                          0x6E, 0x20, 0x69, 0x6E, 0x20, 0x44, 0x4F, 0x53, 0x20, 0x6D, 0x6F, 0x64, 0x65,
                                          0x2E, 0x0D, 0x0D, 0x0A, 0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  out.insert(out.end(), kDosStub, kDosStub + sizeof(kDosStub));

  while (out.size() < 0x80) {
    out.push_back(0);
  }
}

void PEBuilder::BuildNtHeaders(std::vector<std::uint8_t>& out) {
  IMAGE_NT_HEADERS64 nt = {};

  nt.Signature = IMAGE_NT_SIGNATURE;

  nt.FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
  nt.FileHeader.NumberOfSections = static_cast<WORD>(sections_.size());
  nt.FileHeader.TimeDateStamp = 0;
  nt.FileHeader.PointerToSymbolTable = 0;
  nt.FileHeader.NumberOfSymbols = 0;
  nt.FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
  nt.FileHeader.Characteristics = IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_LARGE_ADDRESS_AWARE | IMAGE_FILE_DLL;

  nt.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
  nt.OptionalHeader.MajorLinkerVersion = 14;
  nt.OptionalHeader.MinorLinkerVersion = 0;

  std::uint32_t size_of_code = 0;
  std::uint32_t size_of_initialized_data = 0;
  std::uint32_t base_of_code = 0;

  for (const auto& sec : sections_) {
    if (sec.characteristics & IMAGE_SCN_MEM_EXECUTE) {
      size_of_code += sec.virtual_size;
      if (base_of_code == 0) {
        base_of_code = sec.virtual_address;
      }
    } else {
      size_of_initialized_data += sec.virtual_size;
    }
  }

  nt.OptionalHeader.SizeOfCode = size_of_code;
  nt.OptionalHeader.SizeOfInitializedData = size_of_initialized_data;
  nt.OptionalHeader.SizeOfUninitializedData = 0;
  nt.OptionalHeader.AddressOfEntryPoint = base_of_code;
  nt.OptionalHeader.BaseOfCode = base_of_code;

  nt.OptionalHeader.ImageBase = image_base_;
  nt.OptionalHeader.SectionAlignment = 0x1000;
  nt.OptionalHeader.FileAlignment = 0x1000;
  nt.OptionalHeader.MajorOperatingSystemVersion = 6;
  nt.OptionalHeader.MinorOperatingSystemVersion = 0;
  nt.OptionalHeader.MajorImageVersion = 0;
  nt.OptionalHeader.MinorImageVersion = 0;
  nt.OptionalHeader.MajorSubsystemVersion = 6;
  nt.OptionalHeader.MinorSubsystemVersion = 0;
  nt.OptionalHeader.Win32VersionValue = 0;
  nt.OptionalHeader.SizeOfImage = static_cast<DWORD>(image_size_);

  std::size_t headers_size = 0x80 + sizeof(IMAGE_NT_HEADERS64) + sections_.size() * sizeof(IMAGE_SECTION_HEADER);
  nt.OptionalHeader.SizeOfHeaders = AlignUp(static_cast<std::uint32_t>(headers_size), 0x1000);

  nt.OptionalHeader.CheckSum = 0;
  nt.OptionalHeader.Subsystem = IMAGE_SUBSYSTEM_WINDOWS_GUI;
  nt.OptionalHeader.DllCharacteristics = IMAGE_DLLCHARACTERISTICS_HIGH_ENTROPY_VA |
                                         IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE | IMAGE_DLLCHARACTERISTICS_NX_COMPAT;
  nt.OptionalHeader.SizeOfStackReserve = 0x100000;
  nt.OptionalHeader.SizeOfStackCommit = 0x1000;
  nt.OptionalHeader.SizeOfHeapReserve = 0x100000;
  nt.OptionalHeader.SizeOfHeapCommit = 0x1000;
  nt.OptionalHeader.LoaderFlags = 0;
  nt.OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;

  const auto* ptr = reinterpret_cast<const std::uint8_t*>(&nt);
  out.insert(out.end(), ptr, ptr + sizeof(nt));
}

void PEBuilder::BuildSectionHeaders(std::vector<std::uint8_t>& out) {
  for (const auto& sec : sections_) {
    IMAGE_SECTION_HEADER hdr = {};
    std::memcpy(hdr.Name, sec.name, 8);
    hdr.Misc.VirtualSize = sec.virtual_size;
    hdr.VirtualAddress = sec.virtual_address;
    hdr.SizeOfRawData = AlignUp(sec.virtual_size, 0x1000);
    hdr.PointerToRawData = sec.virtual_address;
    hdr.PointerToRelocations = 0;
    hdr.PointerToLinenumbers = 0;
    hdr.NumberOfRelocations = 0;
    hdr.NumberOfLinenumbers = 0;
    hdr.Characteristics = sec.characteristics;

    const auto* ptr = reinterpret_cast<const std::uint8_t*>(&hdr);
    out.insert(out.end(), ptr, ptr + sizeof(hdr));
  }
}

bool PEBuilder::Build(const std::vector<std::uint8_t>& raw_buffer, std::vector<std::uint8_t>& output) {
  if (sections_.empty()) {
    PIPE_LOG_ERROR("[PEBuilder] No sections defined");
    return false;
  }

  output.clear();

  BuildDosHeader(output);
  BuildNtHeaders(output);
  BuildSectionHeaders(output);

  std::uint32_t first_section_rva = sections_[0].virtual_address;
  while (output.size() < first_section_rva) {
    output.push_back(0);
  }

  if (first_section_rva < raw_buffer.size()) {
    output.insert(output.end(), raw_buffer.begin() + first_section_rva, raw_buffer.end());
  }

  PIPE_LOG_DEBUG("[PEBuilder] Built PE with {} sections, {} bytes total", sections_.size(), output.size());
  return true;
}

bool PEBuilder::WriteExecutable(const std::vector<std::uint8_t>& raw_buffer, const std::string& path) {
  std::vector<std::uint8_t> pe_data;
  if (!Build(raw_buffer, pe_data)) {
    return false;
  }

  std::size_t last_slash = path.find_last_of("/\\");
  if (last_slash != std::string::npos) {
    std::string dir = path.substr(0, last_slash);
    std::filesystem::create_directories(dir);
  }

  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    PIPE_LOG_ERROR("[PEBuilder] Failed to create output file: {}", path);
    return false;
  }

  file.write(reinterpret_cast<const char*>(pe_data.data()), pe_data.size());

  if (!file) {
    PIPE_LOG_ERROR("[PEBuilder] Failed to write output file");
    return false;
  }

  PIPE_LOG_DEBUG("[PEBuilder] Wrote PE dump to {}", path);
  return true;
}

bool PEBuilder::WriteMemoryDump(const std::vector<std::uint8_t>& buffer, const std::string& path) {
  if (buffer.size() < sizeof(IMAGE_DOS_HEADER)) {
    PIPE_LOG_ERROR("[PEBuilder] Buffer too small for DOS header");
    return false;
  }

  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(buffer.data());
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
    PIPE_LOG_ERROR("[PEBuilder] Invalid DOS signature");
    return false;
  }

  std::size_t nt_offset = dos->e_lfanew;
  if (nt_offset + sizeof(IMAGE_NT_HEADERS64) > buffer.size()) {
    PIPE_LOG_ERROR("[PEBuilder] NT headers beyond buffer");
    return false;
  }

  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(buffer.data() + nt_offset);
  if (nt->Signature != IMAGE_NT_SIGNATURE) {
    PIPE_LOG_ERROR("[PEBuilder] Invalid NT signature");
    return false;
  }

  WORD num_sections = nt->FileHeader.NumberOfSections;
  std::size_t section_offset = nt_offset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) +
                               nt->FileHeader.SizeOfOptionalHeader;
  if (section_offset + num_sections * sizeof(IMAGE_SECTION_HEADER) > buffer.size()) {
    PIPE_LOG_ERROR("[PEBuilder] Section headers beyond buffer");
    return false;
  }

  std::vector<std::uint8_t> output(buffer);

  auto* out_nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(output.data() + nt_offset);

  {
    std::uint64_t aslr_base = out_nt->OptionalHeader.ImageBase;
    std::uint64_t orig_base = aslr_base;

    char exe_path[MAX_PATH];
    GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
    HANDLE hFile = CreateFileA(exe_path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
      IMAGE_DOS_HEADER disk_dos{};
      DWORD bytes_read = 0;
      if (ReadFile(hFile, &disk_dos, sizeof(disk_dos), &bytes_read, nullptr) &&
          bytes_read == sizeof(disk_dos) && disk_dos.e_magic == IMAGE_DOS_SIGNATURE) {
        SetFilePointer(hFile, disk_dos.e_lfanew, nullptr, FILE_BEGIN);
        IMAGE_NT_HEADERS64 disk_nt{};
        if (ReadFile(hFile, &disk_nt, sizeof(disk_nt), &bytes_read, nullptr) &&
            bytes_read == sizeof(disk_nt) && disk_nt.Signature == IMAGE_NT_SIGNATURE) {
          orig_base = disk_nt.OptionalHeader.ImageBase;
        }
      }
      CloseHandle(hFile);
    }

    if (aslr_base != orig_base) {
      std::int64_t delta = static_cast<std::int64_t>(aslr_base) - static_cast<std::int64_t>(orig_base);

      PIPE_LOG_DEBUG("[PEBuilder] Rebasing ImageBase from 0x{:X} to 0x{:X} (delta=0x{:X})",
                     aslr_base, orig_base, delta);
      out_nt->OptionalHeader.ImageBase = orig_base;

      // Rebase read-only sections (.rdata) using the reloc table
      auto* sec_hdrs = reinterpret_cast<const IMAGE_SECTION_HEADER*>(output.data() + section_offset);
      auto is_readonly_rva = [&](std::size_t rva) -> bool {
        for (WORD s = 0; s < num_sections; ++s) {
          if (rva >= sec_hdrs[s].VirtualAddress &&
              rva < sec_hdrs[s].VirtualAddress + sec_hdrs[s].Misc.VirtualSize) {
            return !(sec_hdrs[s].Characteristics & IMAGE_SCN_MEM_WRITE);
          }
        }
        return false;
      };

      auto& reloc_dir = out_nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
      std::size_t reloc_fixups = 0;
      if (reloc_dir.VirtualAddress != 0 && reloc_dir.Size != 0) {
        std::size_t reloc_rva = reloc_dir.VirtualAddress;
        std::size_t reloc_end = reloc_rva + reloc_dir.Size;

        while (reloc_rva + sizeof(IMAGE_BASE_RELOCATION) <= reloc_end && reloc_rva < output.size()) {
          auto* block = reinterpret_cast<IMAGE_BASE_RELOCATION*>(output.data() + reloc_rva);
          if (block->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION) || block->SizeOfBlock > reloc_end - reloc_rva) {
            break;
          }

          DWORD entry_count = (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
          auto* entries = reinterpret_cast<WORD*>(
              output.data() + reloc_rva + sizeof(IMAGE_BASE_RELOCATION));

          for (DWORD j = 0; j < entry_count; ++j) {
            WORD type = entries[j] >> 12;
            WORD offset = entries[j] & 0xFFF;
            std::size_t target_rva = block->VirtualAddress + offset;

            if (type == IMAGE_REL_BASED_DIR64 && target_rva + 8 <= output.size() &&
                is_readonly_rva(target_rva)) {
              auto* ptr = reinterpret_cast<std::uint64_t*>(output.data() + target_rva);
              *ptr -= delta;
              ++reloc_fixups;
            } else if (type == IMAGE_REL_BASED_HIGHLOW && target_rva + 4 <= output.size() &&
                       is_readonly_rva(target_rva)) {
              auto* ptr = reinterpret_cast<std::uint32_t*>(output.data() + target_rva);
              *ptr -= static_cast<std::uint32_t>(delta);
              ++reloc_fixups;
            }
          }

          reloc_rva += block->SizeOfBlock;
        }
      }

      // Heuristic rebase of writable sections (.data)
      std::size_t heuristic_fixups = 0;
      std::uint64_t aslr_lo = aslr_base;
      std::uint64_t aslr_hi = aslr_base + out_nt->OptionalHeader.SizeOfImage;

      for (WORD s = 0; s < num_sections; ++s) {
        if (!(sec_hdrs[s].Characteristics & IMAGE_SCN_MEM_WRITE)) continue;

        std::size_t sec_start = sec_hdrs[s].VirtualAddress;
        std::size_t sec_end = sec_start + sec_hdrs[s].Misc.VirtualSize;
        if (sec_end > output.size()) sec_end = output.size();

        for (std::size_t off = sec_start; off + 8 <= sec_end; off += 8) {
          auto* ptr = reinterpret_cast<std::uint64_t*>(output.data() + off);
          if (*ptr >= aslr_lo && *ptr < aslr_hi) {
            *ptr -= delta;
            ++heuristic_fixups;
          }
        }
      }

      PIPE_LOG_DEBUG("[PEBuilder] Rebase: {} reloc fixups, {} heuristic .data fixups",
                     reloc_fixups, heuristic_fixups);
    }
  }

  // Remove ASLR flag
  out_nt->OptionalHeader.DllCharacteristics &= ~IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE;

  auto* sections = reinterpret_cast<IMAGE_SECTION_HEADER*>(output.data() + section_offset);
  for (WORD i = 0; i < num_sections; ++i) {
    sections[i].PointerToRawData = sections[i].VirtualAddress;
    sections[i].SizeOfRawData = AlignUp(sections[i].Misc.VirtualSize,
                                        out_nt->OptionalHeader.SectionAlignment);
  }

  auto& debug_dir = out_nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
  if (debug_dir.VirtualAddress != 0 && debug_dir.Size >= sizeof(IMAGE_DEBUG_DIRECTORY) &&
      debug_dir.VirtualAddress + debug_dir.Size <= output.size()) {
    std::size_t count = debug_dir.Size / sizeof(IMAGE_DEBUG_DIRECTORY);
    auto* entries = reinterpret_cast<IMAGE_DEBUG_DIRECTORY*>(output.data() + debug_dir.VirtualAddress);
    for (std::size_t i = 0; i < count; ++i) {
      entries[i].PointerToRawData = entries[i].AddressOfRawData;
    }
    PIPE_LOG_DEBUG("[PEBuilder] Patched {} debug directory entries", count);
  }

  std::size_t last_slash = path.find_last_of("/\\");
  if (last_slash != std::string::npos) {
    std::filesystem::create_directories(path.substr(0, last_slash));
  }

  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    PIPE_LOG_ERROR("[PEBuilder] Failed to create output file: {}", path);
    return false;
  }

  file.write(reinterpret_cast<const char*>(output.data()), output.size());
  if (!file) {
    PIPE_LOG_ERROR("[PEBuilder] Failed to write output file");
    return false;
  }

  PIPE_LOG_DEBUG("[PEBuilder] Wrote memory dump PE ({} sections, {} bytes) to {}",
                 num_sections, output.size(), path);
  return true;
}

}  // namespace dolos
