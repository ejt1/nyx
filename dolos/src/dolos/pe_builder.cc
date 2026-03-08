#include "dolos/pe_builder.h"
#include "dolos/pipe_log.h"

#include <intrin.h>
#include <winternl.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

namespace dolos {

namespace {

constexpr std::uint32_t AlignUp(std::uint32_t value, std::uint32_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

constexpr std::size_t kMaxThunkInstructions = 64;

int GprIndex(ZydisRegister reg) {
  if (reg >= ZYDIS_REGISTER_RAX && reg <= ZYDIS_REGISTER_R15) return reg - ZYDIS_REGISTER_RAX;
  if (reg >= ZYDIS_REGISTER_EAX && reg <= ZYDIS_REGISTER_R15D) return reg - ZYDIS_REGISTER_EAX;
  if (reg >= ZYDIS_REGISTER_AX && reg <= ZYDIS_REGISTER_R15W) return reg - ZYDIS_REGISTER_AX;
  if (reg >= ZYDIS_REGISTER_AL && reg <= ZYDIS_REGISTER_R15B) return reg - ZYDIS_REGISTER_AL;
  return -1;
}

int GprWidth(ZydisRegister reg) {
  if (reg >= ZYDIS_REGISTER_RAX && reg <= ZYDIS_REGISTER_R15) return 64;
  if (reg >= ZYDIS_REGISTER_EAX && reg <= ZYDIS_REGISTER_R15D) return 32;
  if (reg >= ZYDIS_REGISTER_AX && reg <= ZYDIS_REGISTER_R15W) return 16;
  if (reg >= ZYDIS_REGISTER_AL && reg <= ZYDIS_REGISTER_R15B) return 8;
  return 0;
}

void WriteGpr(std::uint64_t* regs, ZydisRegister reg, std::uint64_t value) {
  int idx = GprIndex(reg);
  if (idx < 0) return;
  int width = GprWidth(reg);
  switch (width) {
    case 64:
      regs[idx] = value;
      break;
    case 32:
      // Writing to 32-bit register zero-extends to 64
      regs[idx] = value & 0xFFFFFFFF;
      break;
    case 16:
      regs[idx] = (regs[idx] & ~0xFFFFull) | (value & 0xFFFF);
      break;
    case 8:
      regs[idx] = (regs[idx] & ~0xFFull) | (value & 0xFF);
      break;
  }
}

std::uint64_t ReadGpr(const std::uint64_t* regs, ZydisRegister reg) {
  int idx = GprIndex(reg);
  if (idx < 0) return 0;
  int width = GprWidth(reg);
  switch (width) {
    case 64:
      return regs[idx];
    case 32:
      return regs[idx] & 0xFFFFFFFF;
    case 16:
      return regs[idx] & 0xFFFF;
    case 8:
      return regs[idx] & 0xFF;
  }
  return 0;
}

std::uint64_t GetImm(const ZydisDecodedOperand& op) {
  return static_cast<std::uint64_t>(op.imm.value.s);
}

bool SafeReadU64(std::uint64_t addr, std::uint64_t& out) {
  __try {
    out = *reinterpret_cast<const std::uint64_t*>(addr);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

std::size_t SafeMemcpy(void* dst, const void* src, std::size_t len) {
  __try {
    memcpy(dst, src, len);
    return len;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}

struct ResolvedImport {
  std::string dll_name;
  std::string func_name;
  WORD hint;
};

bool ResolveAddressToImport(std::uint64_t addr, ResolvedImport& out) {
  HMODULE hmod = nullptr;
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCSTR>(addr),
                          &hmod)) {
    return false;
  }

  char path[MAX_PATH];
  if (!GetModuleFileNameA(hmod, path, MAX_PATH)) return false;
  const char* slash = strrchr(path, '\\');
  out.dll_name = slash ? (slash + 1) : path;

  auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(hmod);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
  auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(reinterpret_cast<const std::uint8_t*>(hmod) + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

  auto& exp_dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
  if (exp_dir.VirtualAddress == 0 || exp_dir.Size == 0) return false;

  auto* base = reinterpret_cast<const std::uint8_t*>(hmod);
  auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + exp_dir.VirtualAddress);

  auto* funcs = reinterpret_cast<const DWORD*>(base + exports->AddressOfFunctions);
  auto* names = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
  auto* ords = reinterpret_cast<const WORD*>(base + exports->AddressOfNameOrdinals);

  std::uint64_t mod_base = reinterpret_cast<std::uint64_t>(hmod);
  std::uint64_t target_rva = addr - mod_base;

  for (DWORD i = 0; i < exports->NumberOfFunctions; ++i) {
    if (funcs[i] == static_cast<DWORD>(target_rva)) {
      for (DWORD j = 0; j < exports->NumberOfNames; ++j) {
        if (ords[j] == i) {
          out.func_name = reinterpret_cast<const char*>(base + names[j]);
          out.hint = static_cast<WORD>(i);
          return true;
        }
      }
      out.func_name = "";
      out.hint = static_cast<WORD>(i + exports->Base);
      return true;
    }
  }

  return false;
}

bool ComputeEffectiveAddr(const ZydisDecodedInstruction& insn,
                          const ZydisDecodedOperand& op,
                          const std::uint64_t* regs,
                          std::uint64_t insn_addr,
                          std::uint64_t& addr_out) {
  addr_out = 0;

  if (op.mem.segment == ZYDIS_REGISTER_GS) {
    std::uint64_t gs_val = 0;
    std::uint64_t teb = reinterpret_cast<std::uint64_t>(NtCurrentTeb());
    std::uint64_t disp = static_cast<std::uint64_t>(op.mem.disp.value);
    if (!SafeReadU64(teb + disp, addr_out)) return false;
    return true;
  }

  if (op.mem.base != ZYDIS_REGISTER_NONE) {
    if (op.mem.base == ZYDIS_REGISTER_RIP) {
      addr_out = insn_addr + insn.length;
    } else {
      addr_out = ReadGpr(regs, op.mem.base);
    }
  }
  if (op.mem.index != ZYDIS_REGISTER_NONE) {
    addr_out += ReadGpr(regs, op.mem.index) * op.mem.scale;
  }
  addr_out += static_cast<std::uint64_t>(op.mem.disp.value);
  return true;
}

bool EmulateThunk(const std::uint8_t* code,
                  std::size_t max_len,
                  std::uint64_t thunk_runtime_addr,
                  std::uint64_t& out_addr) {
  ZydisDecoder decoder;
  ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);

  std::uint64_t regs[16] = {};
  std::uint64_t stack[32] = {};
  int sp = 32;

  std::size_t offset = 0;

  for (std::size_t i = 0; i < kMaxThunkInstructions && offset < max_len; ++i) {
    ZydisDecodedInstruction insn;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];

    if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, code + offset, max_len - offset, &insn, operands))) {
      return false;
    }

    std::uint64_t insn_addr = thunk_runtime_addr + offset;
    offset += insn.length;

    auto ReadSrc = [&](const ZydisDecodedOperand& op, std::uint64_t& val) -> bool {
      if (op.type == ZYDIS_OPERAND_TYPE_REGISTER) {
        val = ReadGpr(regs, op.reg.value);
        return true;
      }
      if (op.type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
        val = GetImm(op);
        return true;
      }
      if (op.type == ZYDIS_OPERAND_TYPE_MEMORY) {
        if (op.mem.segment == ZYDIS_REGISTER_GS) {
          return ComputeEffectiveAddr(insn, op, regs, insn_addr, val);
        }
        if (op.mem.base == ZYDIS_REGISTER_RSP && op.mem.index == ZYDIS_REGISTER_NONE && op.mem.disp.value == 0) {
          if (sp >= 0 && sp < 32) {
            val = stack[sp];
            return true;
          }
          return false;
        }
        std::uint64_t addr = 0;
        if (!ComputeEffectiveAddr(insn, op, regs, insn_addr, addr)) return false;
        return SafeReadU64(addr, val);
      }
      return false;
    };

    auto WriteDst = [&](const ZydisDecodedOperand& op, std::uint64_t val) -> bool {
      if (op.type == ZYDIS_OPERAND_TYPE_REGISTER) {
        WriteGpr(regs, op.reg.value, val);
        return true;
      }
      if (op.type == ZYDIS_OPERAND_TYPE_MEMORY) {
        if (op.mem.base == ZYDIS_REGISTER_RSP && op.mem.index == ZYDIS_REGISTER_NONE && op.mem.disp.value == 0) {
          if (sp >= 0 && sp < 32) {
            stack[sp] = val;
            return true;
          }
          return false;
        }
        return true;
      }
      return false;
    };

    auto ReadDst = [&](const ZydisDecodedOperand& op, std::uint64_t& val) -> bool {
      if (op.type == ZYDIS_OPERAND_TYPE_REGISTER) {
        val = ReadGpr(regs, op.reg.value);
        return true;
      }
      if (op.type == ZYDIS_OPERAND_TYPE_MEMORY) {
        if (op.mem.base == ZYDIS_REGISTER_RSP && op.mem.index == ZYDIS_REGISTER_NONE && op.mem.disp.value == 0) {
          if (sp >= 0 && sp < 32) {
            val = stack[sp];
            return true;
          }
          return false;
        }
        std::uint64_t addr = 0;
        if (!ComputeEffectiveAddr(insn, op, regs, insn_addr, addr)) return false;
        return SafeReadU64(addr, val);
      }
      return false;
    };

    switch (insn.mnemonic) {
      case ZYDIS_MNEMONIC_MOV: {
        std::uint64_t val = 0;
        if (!ReadSrc(operands[1], val)) return false;
        if (!WriteDst(operands[0], val)) return false;
        break;
      }

      case ZYDIS_MNEMONIC_LEA: {
        if (operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER && operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY) {
          std::uint64_t addr = 0;
          if (operands[1].mem.base != ZYDIS_REGISTER_NONE) {
            if (operands[1].mem.base == ZYDIS_REGISTER_RIP)
              addr = insn_addr + insn.length;
            else
              addr = ReadGpr(regs, operands[1].mem.base);
          }
          if (operands[1].mem.index != ZYDIS_REGISTER_NONE)
            addr += ReadGpr(regs, operands[1].mem.index) * operands[1].mem.scale;
          addr += static_cast<std::uint64_t>(operands[1].mem.disp.value);
          WriteGpr(regs, operands[0].reg.value, addr);
        }
        break;
      }

      case ZYDIS_MNEMONIC_PUSH: {
        if (sp <= 0) return false;
        std::uint64_t val = 0;
        if (!ReadSrc(operands[0], val)) return false;
        --sp;
        stack[sp] = val;
        break;
      }

      case ZYDIS_MNEMONIC_POP: {
        if (sp >= 32) return false;
        std::uint64_t val = stack[sp];
        ++sp;
        if (!WriteDst(operands[0], val)) return false;
        break;
      }

      case ZYDIS_MNEMONIC_XOR: {
        std::uint64_t dst = 0, src = 0;
        if (!ReadDst(operands[0], dst)) return false;
        if (!ReadSrc(operands[1], src)) return false;
        if (!WriteDst(operands[0], dst ^ src)) return false;
        break;
      }

      case ZYDIS_MNEMONIC_ADD: {
        std::uint64_t dst = 0, src = 0;
        if (!ReadDst(operands[0], dst)) return false;
        if (!ReadSrc(operands[1], src)) return false;
        if (!WriteDst(operands[0], dst + src)) return false;
        break;
      }

      case ZYDIS_MNEMONIC_SUB: {
        std::uint64_t dst = 0, src = 0;
        if (!ReadDst(operands[0], dst)) return false;
        if (!ReadSrc(operands[1], src)) return false;
        if (!WriteDst(operands[0], dst - src)) return false;
        break;
      }

      case ZYDIS_MNEMONIC_INC: {
        std::uint64_t val = 0;
        if (!ReadDst(operands[0], val)) return false;
        if (!WriteDst(operands[0], val + 1)) return false;
        break;
      }

      case ZYDIS_MNEMONIC_DEC: {
        std::uint64_t val = 0;
        if (!ReadDst(operands[0], val)) return false;
        if (!WriteDst(operands[0], val - 1)) return false;
        break;
      }

      case ZYDIS_MNEMONIC_ROL: {
        std::uint64_t val = 0;
        if (!ReadDst(operands[0], val)) return false;
        int shift = static_cast<int>(operands[1].imm.value.u) & 63;
        if (!WriteDst(operands[0], _rotl64(val, shift))) return false;
        break;
      }

      case ZYDIS_MNEMONIC_ROR: {
        std::uint64_t val = 0;
        if (!ReadDst(operands[0], val)) return false;
        int shift = static_cast<int>(operands[1].imm.value.u) & 63;
        if (!WriteDst(operands[0], _rotr64(val, shift))) return false;
        break;
      }

      case ZYDIS_MNEMONIC_NOT: {
        std::uint64_t val = 0;
        if (!ReadDst(operands[0], val)) return false;
        if (!WriteDst(operands[0], ~val)) return false;
        break;
      }

      case ZYDIS_MNEMONIC_NEG: {
        std::uint64_t val = 0;
        if (!ReadDst(operands[0], val)) return false;
        if (!WriteDst(operands[0], static_cast<std::uint64_t>(-static_cast<std::int64_t>(val)))) return false;
        break;
      }

      case ZYDIS_MNEMONIC_IMUL: {
        if (operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER) {
          std::uint64_t val = ReadGpr(regs, operands[0].reg.value);
          if (insn.operand_count_visible >= 3 && operands[2].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
            std::uint64_t src = 0;
            if (!ReadSrc(operands[1], src)) return false;
            val = src * GetImm(operands[2]);
          } else if (operands[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
            val *= GetImm(operands[1]);
          } else if (operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER) {
            val *= ReadGpr(regs, operands[1].reg.value);
          }
          WriteGpr(regs, operands[0].reg.value, val);
        }
        break;
      }

      case ZYDIS_MNEMONIC_AND: {
        std::uint64_t dst = 0, src = 0;
        if (!ReadDst(operands[0], dst)) return false;
        if (!ReadSrc(operands[1], src)) return false;
        if (!WriteDst(operands[0], dst & src)) return false;
        break;
      }

      case ZYDIS_MNEMONIC_OR: {
        std::uint64_t dst = 0, src = 0;
        if (!ReadDst(operands[0], dst)) return false;
        if (!ReadSrc(operands[1], src)) return false;
        if (!WriteDst(operands[0], dst | src)) return false;
        break;
      }

      case ZYDIS_MNEMONIC_XCHG: {
        std::uint64_t a = 0, b = 0;
        if (!ReadDst(operands[0], a)) return false;
        if (!ReadDst(operands[1], b)) return false;
        if (!WriteDst(operands[0], b)) return false;
        if (!WriteDst(operands[1], a)) return false;
        break;
      }

      case ZYDIS_MNEMONIC_CALL: {
        std::uint64_t target = 0;
        if (operands[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
          target = insn_addr + insn.length + static_cast<std::int64_t>(operands[0].imm.value.s);
        } else if (operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER) {
          target = ReadGpr(regs, operands[0].reg.value);
        } else if (operands[0].type == ZYDIS_OPERAND_TYPE_MEMORY) {
          std::uint64_t addr = 0;
          if (!ComputeEffectiveAddr(insn, operands[0], regs, insn_addr, addr)) return false;
          if (!SafeReadU64(addr, target)) return false;
        }

        std::uint64_t gadget_bytes = 0;
        if (SafeReadU64(target, gadget_bytes)) {
          if ((gadget_bytes & 0xFFFFFFFF) == 0xC3008B48) {
            std::uint64_t deref = 0;
            if (!SafeReadU64(regs[0], deref)) return false;
            regs[0] = deref;
            break;
          }
        }
        return false;
      }

      case ZYDIS_MNEMONIC_JMP: {
        if (operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER) {
          out_addr = ReadGpr(regs, operands[0].reg.value);
          return true;
        }
        if (operands[0].type == ZYDIS_OPERAND_TYPE_MEMORY) {
          std::uint64_t addr = 0;
          if (!ComputeEffectiveAddr(insn, operands[0], regs, insn_addr, addr)) return false;
          if (!SafeReadU64(addr, out_addr)) return false;
          return true;
        }
        if (operands[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
          out_addr = insn_addr + insn.length + static_cast<std::int64_t>(operands[0].imm.value.s);
          return true;
        }
        return false;
      }

      case ZYDIS_MNEMONIC_RET: {
        if (sp < 32) {
          out_addr = stack[sp];
          ++sp;
        } else {
          out_addr = regs[0];
        }
        return true;
      }

      case ZYDIS_MNEMONIC_PUSHF:
      case ZYDIS_MNEMONIC_PUSHFQ:
      case ZYDIS_MNEMONIC_POPF:
      case ZYDIS_MNEMONIC_POPFQ:
      case ZYDIS_MNEMONIC_NOP:
      case ZYDIS_MNEMONIC_INT3:
        break;

      default:
        return false;
    }
  }

  return false;
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

std::size_t PEBuilder::DeobfuscateIAT(std::vector<std::uint8_t>& buffer,
                                      std::uint64_t image_base,
                                      std::uint64_t aslr_base,
                                      std::uint64_t image_size) {
  if (buffer.size() < sizeof(IMAGE_DOS_HEADER)) return 0;

  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(buffer.data());
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;

  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(buffer.data() + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

  auto& import_dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (import_dir.VirtualAddress == 0 || import_dir.Size == 0) {
    PIPE_LOG_DEBUG("[PEBuilder] No import directory found");
    return 0;
  }

  if (import_dir.VirtualAddress + import_dir.Size > buffer.size()) {
    PIPE_LOG_WARN("[PEBuilder] Import directory beyond buffer bounds");
    return 0;
  }

  PIPE_LOG_DEBUG("[PEBuilder] IAT scan: image_base=0x{:X}, aslr_base=0x{:X}, image_size=0x{:X}",
                 image_base,
                 aslr_base,
                 image_size);
  PIPE_LOG_DEBUG("[PEBuilder] Import directory: RVA=0x{:X}, Size=0x{:X}", import_dir.VirtualAddress, import_dir.Size);

  std::size_t resolved = 0;
  std::size_t failed = 0;
  std::size_t total = 0;

  auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(buffer.data() + import_dir.VirtualAddress);

  for (; desc->FirstThunk != 0; ++desc) {
    if (desc->FirstThunk >= buffer.size()) break;

    // Get DLL name for logging
    const char* dll_name = "<unknown>";
    if (desc->Name != 0 && desc->Name < buffer.size()) {
      dll_name = reinterpret_cast<const char*>(buffer.data() + desc->Name);
    }

    auto* iat = reinterpret_cast<std::uint64_t*>(buffer.data() + desc->FirstThunk);
    std::size_t iat_max_entries = (buffer.size() - desc->FirstThunk) / sizeof(std::uint64_t);

    std::size_t dll_resolved = 0;
    std::size_t dll_failed = 0;

    for (std::size_t j = 0; j < iat_max_entries; ++j) {
      if (iat[j] == 0) break;
      ++total;

      std::uint64_t thunk_addr = iat[j];

      if (thunk_addr >= image_base && thunk_addr < image_base + image_size) {
        thunk_addr = aslr_base + (thunk_addr - image_base);
      }

      std::uint8_t thunk_code[256];
      std::size_t bytes_copied = SafeMemcpy(thunk_code, reinterpret_cast<const void*>(thunk_addr), sizeof(thunk_code));
      if (bytes_copied == 0) {
        ++dll_failed;
        ++failed;
        continue;
      }

      std::uint64_t real_addr = 0;
      if (EmulateThunk(thunk_code, bytes_copied, thunk_addr, real_addr)) {
        iat[j] = real_addr;
        ++dll_resolved;
        ++resolved;
      } else {
        if (dll_failed == 0) {
          std::string hex;
          for (std::size_t b = 0; b < 32; ++b) {
            char buf[4];
            snprintf(buf, sizeof(buf), "%02X ", thunk_code[b]);
            hex += buf;
          }
          PIPE_LOG_DEBUG("[PEBuilder] IAT {} thunk at 0x{:X} failed to emulate. Bytes: {}", dll_name, thunk_addr, hex);
        }
        ++dll_failed;
        ++failed;
      }
    }

    PIPE_LOG_DEBUG("[PEBuilder] IAT {}: resolved={}, failed={}", dll_name, dll_resolved, dll_failed);
  }

  PIPE_LOG_INFO("[PEBuilder] IAT deobfuscation: {}/{} resolved, {} failed", resolved, total, failed);

  WORD num_secs_scan = nt->FileHeader.NumberOfSections;
  std::size_t sec_hdr_off = reinterpret_cast<const std::uint8_t*>(nt) - buffer.data() + sizeof(DWORD) +
                            sizeof(IMAGE_FILE_HEADER) + nt->FileHeader.SizeOfOptionalHeader;
  auto* sec_hdrs_scan = reinterpret_cast<const IMAGE_SECTION_HEADER*>(buffer.data() + sec_hdr_off);

  std::size_t thunks_emulated = 0;
  std::size_t direct_resolved = 0;

  std::set<std::uint32_t> formal_rvas;
  {
    auto* d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(buffer.data() + import_dir.VirtualAddress);
    for (; d->FirstThunk != 0; ++d) {
      if (d->FirstThunk >= buffer.size()) break;
      auto* iat2 = reinterpret_cast<std::uint64_t*>(buffer.data() + d->FirstThunk);
      std::size_t iat2_max = (buffer.size() - d->FirstThunk) / sizeof(std::uint64_t);
      for (std::size_t j = 0; j < iat2_max && iat2[j] != 0; ++j) {
        formal_rvas.insert(static_cast<std::uint32_t>(d->FirstThunk + j * sizeof(std::uint64_t)));
      }
    }
  }

  struct DataSectionEntry {
    std::uint32_t rva;
    ResolvedImport import;
  };
  std::vector<DataSectionEntry> data_section_entries;

  for (WORD s = 0; s < num_secs_scan; ++s) {
    DWORD ch = sec_hdrs_scan[s].Characteristics;
    bool is_data = (ch & IMAGE_SCN_MEM_READ) && !(ch & IMAGE_SCN_MEM_EXECUTE);
    if (!is_data) continue;

    std::uint32_t sec_start = sec_hdrs_scan[s].VirtualAddress;
    std::uint32_t sec_end = sec_start + sec_hdrs_scan[s].Misc.VirtualSize;
    if (sec_end > buffer.size()) sec_end = static_cast<std::uint32_t>(buffer.size());

    for (std::uint32_t off = sec_start; off + 8 <= sec_end; off += 8) {
      if (formal_rvas.count(off)) continue;

      auto* slot = reinterpret_cast<std::uint64_t*>(buffer.data() + off);
      std::uint64_t val = *slot;

      if (val == 0 || val < 0x10000) continue;
      if (val >= image_base && val < image_base + image_size) continue;
      if (val >= aslr_base && val < aslr_base + image_size) continue;

      ResolvedImport ri;
      if (ResolveAddressToImport(val, ri)) {
        data_section_entries.push_back({off, ri});
        ++direct_resolved;
        continue;
      }

      std::uint8_t thunk_code[256];
      std::size_t bytes = SafeMemcpy(thunk_code, reinterpret_cast<const void*>(val), sizeof(thunk_code));
      if (bytes == 0) continue;

      std::uint64_t real_addr = 0;
      if (EmulateThunk(thunk_code, bytes, val, real_addr)) {
        *slot = real_addr;
        ++thunks_emulated;
        ResolvedImport ri2;
        if (ResolveAddressToImport(real_addr, ri2)) {
          data_section_entries.push_back({off, ri2});
        }
      }
    }
  }

  PIPE_LOG_INFO("[PEBuilder] Data section scan: {} thunks emulated, {} direct API pointers found, "
                "{} total entries for import rebuild",
                thunks_emulated,
                direct_resolved,
                data_section_entries.size());

  struct ImportEntry {
    std::string func_name;
    WORD hint;
    std::uint32_t iat_rva;
  };

  struct ImportRun {
    std::string dll_name;
    std::vector<ImportEntry> entries;
    std::uint32_t first_thunk_rva;
  };
  std::vector<ImportRun> import_runs;

  desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(buffer.data() + import_dir.VirtualAddress);
  for (; desc->FirstThunk != 0; ++desc) {
    if (desc->FirstThunk >= buffer.size()) break;
    auto* iat2 = reinterpret_cast<std::uint64_t*>(buffer.data() + desc->FirstThunk);
    std::size_t iat2_max = (buffer.size() - desc->FirstThunk) / sizeof(std::uint64_t);

    std::string current_dll;
    ImportRun current_run;

    for (std::size_t j = 0; j < iat2_max; ++j) {
      if (iat2[j] == 0) break;
      std::uint32_t rva = static_cast<std::uint32_t>(desc->FirstThunk + j * sizeof(std::uint64_t));
      ResolvedImport ri;
      if (ResolveAddressToImport(iat2[j], ri)) {
        ImportEntry entry;
        entry.func_name = ri.func_name;
        entry.hint = ri.hint;
        entry.iat_rva = rva;

        if (ri.dll_name != current_dll && !current_run.entries.empty()) {
          import_runs.push_back(std::move(current_run));
          current_run = {};
        }
        if (current_run.entries.empty()) {
          current_run.dll_name = ri.dll_name;
          current_run.first_thunk_rva = rva;
        }
        current_dll = ri.dll_name;
        current_run.entries.push_back(entry);
      }
    }
    if (!current_run.entries.empty()) {
      import_runs.push_back(std::move(current_run));
    }
  }

  {
    ImportRun current_run;
    for (std::size_t i = 0; i < data_section_entries.size(); ++i) {
      auto& dse = data_section_entries[i];
      bool start_new_run = false;

      if (current_run.entries.empty()) {
        start_new_run = true;
      } else {
        std::uint32_t expected_rva = current_run.entries.back().iat_rva + 8;
        if (dse.rva != expected_rva || dse.import.dll_name != current_run.dll_name) {
          import_runs.push_back(std::move(current_run));
          current_run = {};
          start_new_run = true;
        }
      }

      if (start_new_run) {
        current_run.dll_name = dse.import.dll_name;
        current_run.first_thunk_rva = dse.rva;
      }

      ImportEntry entry;
      entry.func_name = dse.import.func_name;
      entry.hint = dse.import.hint;
      entry.iat_rva = dse.rva;
      current_run.entries.push_back(entry);
    }
    if (!current_run.entries.empty()) {
      import_runs.push_back(std::move(current_run));
    }
  }

  if (import_runs.empty()) return resolved;

  std::size_t total_import_entries = 0;
  for (auto& run : import_runs) total_import_entries += run.entries.size();

  PIPE_LOG_DEBUG(
      "[PEBuilder] Rebuilding import directory: {} runs, {} total entries", import_runs.size(), total_import_entries);

  std::vector<std::uint8_t> blob;
  std::size_t num_runs = import_runs.size();

  std::size_t desc_table_size = (num_runs + 1) * sizeof(IMAGE_IMPORT_DESCRIPTOR);
  blob.resize(desc_table_size, 0);

  std::size_t blob_rva = (buffer.size() + 15) & ~15ull;

  struct RunFixup {
    std::size_t desc_offset;     
    std::size_t ilt_blob_offset; 
    std::size_t name_blob_offset;
  };
  std::vector<RunFixup> fixups;

  for (std::size_t r = 0; r < num_runs; ++r) {
    auto& run = import_runs[r];
    RunFixup fix;
    fix.desc_offset = r * sizeof(IMAGE_IMPORT_DESCRIPTOR);

    fix.ilt_blob_offset = blob.size();
    std::size_t ilt_size = (run.entries.size() + 1) * sizeof(std::uint64_t);
    blob.resize(blob.size() + ilt_size, 0);

    fix.name_blob_offset = blob.size();
    blob.insert(blob.end(), run.dll_name.begin(), run.dll_name.end());
    blob.push_back(0);
    if (blob.size() & 1) blob.push_back(0);

    fixups.push_back(fix);
  }

  for (std::size_t r = 0; r < num_runs; ++r) {
    auto& run = import_runs[r];
    auto& fix = fixups[r];

    for (std::size_t j = 0; j < run.entries.size(); ++j) {
      auto& entry = run.entries[j];
      std::uint64_t thunk_data;

      if (entry.func_name.empty()) {
        thunk_data = IMAGE_ORDINAL_FLAG64 | entry.hint;
      } else {
        // IMAGE_IMPORT_BY_NAME: { WORD Hint; char Name[]; }
        std::size_t ibn_offset = blob.size();
        blob.push_back(static_cast<std::uint8_t>(entry.hint & 0xFF));
        blob.push_back(static_cast<std::uint8_t>((entry.hint >> 8) & 0xFF));
        blob.insert(blob.end(), entry.func_name.begin(), entry.func_name.end());
        blob.push_back(0);
        if (blob.size() & 1) blob.push_back(0);

        thunk_data = blob_rva + ibn_offset;
      }

      auto* ilt = reinterpret_cast<std::uint64_t*>(blob.data() + fix.ilt_blob_offset);
      ilt[j] = thunk_data;
    }
  }

  for (std::size_t r = 0; r < num_runs; ++r) {
    auto& run = import_runs[r];
    auto& fix = fixups[r];
    auto* new_desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(blob.data() + fix.desc_offset);

    new_desc->OriginalFirstThunk = static_cast<DWORD>(blob_rva + fix.ilt_blob_offset);
    new_desc->TimeDateStamp = 0;
    new_desc->ForwarderChain = 0;
    new_desc->Name = static_cast<DWORD>(blob_rva + fix.name_blob_offset);
    new_desc->FirstThunk = run.first_thunk_rva;

    PIPE_LOG_DEBUG("[PEBuilder] Import descriptor: {} ({} functions, IAT RVA=0x{:X})",
                   run.dll_name,
                   run.entries.size(),
                   run.first_thunk_rva);
  }

  buffer.resize(blob_rva, 0);  // padding
  buffer.insert(buffer.end(), blob.begin(), blob.end());

  auto* out_nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(
      buffer.data() + reinterpret_cast<const IMAGE_DOS_HEADER*>(buffer.data())->e_lfanew);
  out_nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress = static_cast<DWORD>(blob_rva);
  out_nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size = static_cast<DWORD>(blob.size());

  std::uint32_t new_image_size =
      static_cast<std::uint32_t>((buffer.size() + out_nt->OptionalHeader.SectionAlignment - 1) &
                                 ~static_cast<std::size_t>(out_nt->OptionalHeader.SectionAlignment - 1));
  if (new_image_size > out_nt->OptionalHeader.SizeOfImage) {
    out_nt->OptionalHeader.SizeOfImage = new_image_size;
  }

  WORD num_secs = out_nt->FileHeader.NumberOfSections;
  std::size_t sec_off = reinterpret_cast<const IMAGE_DOS_HEADER*>(buffer.data())->e_lfanew + sizeof(DWORD) +
                        sizeof(IMAGE_FILE_HEADER) + out_nt->FileHeader.SizeOfOptionalHeader;
  auto* secs = reinterpret_cast<IMAGE_SECTION_HEADER*>(buffer.data() + sec_off);
  if (num_secs > 0) {
    auto& last = secs[num_secs - 1];
    std::uint32_t last_end = last.VirtualAddress + last.Misc.VirtualSize;
    if (buffer.size() > last_end) {
      last.Misc.VirtualSize = static_cast<DWORD>(buffer.size() - last.VirtualAddress);
    }
  }

  PIPE_LOG_INFO("[PEBuilder] Import directory rebuilt: {} descriptors ({} entries), {} bytes appended at RVA 0x{:X}",
                num_runs,
                total_import_entries,
                blob.size(),
                blob_rva);

  return resolved;
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
  std::size_t section_offset =
      nt_offset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) + nt->FileHeader.SizeOfOptionalHeader;
  if (section_offset + num_sections * sizeof(IMAGE_SECTION_HEADER) > buffer.size()) {
    PIPE_LOG_ERROR("[PEBuilder] Section headers beyond buffer");
    return false;
  }

  std::vector<std::uint8_t> output(buffer);

  auto* out_nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(output.data() + nt_offset);

  std::uint64_t aslr_base = out_nt->OptionalHeader.ImageBase;
  {
    std::uint64_t orig_base = aslr_base;

    char exe_path[MAX_PATH];
    GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
    HANDLE hFile = CreateFileA(exe_path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
      IMAGE_DOS_HEADER disk_dos{};
      DWORD bytes_read = 0;
      if (ReadFile(hFile, &disk_dos, sizeof(disk_dos), &bytes_read, nullptr) && bytes_read == sizeof(disk_dos) &&
          disk_dos.e_magic == IMAGE_DOS_SIGNATURE) {
        SetFilePointer(hFile, disk_dos.e_lfanew, nullptr, FILE_BEGIN);
        IMAGE_NT_HEADERS64 disk_nt{};
        if (ReadFile(hFile, &disk_nt, sizeof(disk_nt), &bytes_read, nullptr) && bytes_read == sizeof(disk_nt) &&
            disk_nt.Signature == IMAGE_NT_SIGNATURE) {
          orig_base = disk_nt.OptionalHeader.ImageBase;
        }
      }
      CloseHandle(hFile);
    }

    if (aslr_base != orig_base) {
      std::int64_t delta = static_cast<std::int64_t>(aslr_base) - static_cast<std::int64_t>(orig_base);

      PIPE_LOG_DEBUG(
          "[PEBuilder] Rebasing ImageBase from 0x{:X} to 0x{:X} (delta=0x{:X})", aslr_base, orig_base, delta);
      out_nt->OptionalHeader.ImageBase = orig_base;

      // Rebase read-only sections (.rdata) using the reloc table
      auto* sec_hdrs = reinterpret_cast<const IMAGE_SECTION_HEADER*>(output.data() + section_offset);
      auto is_readonly_rva = [&](std::size_t rva) -> bool {
        for (WORD s = 0; s < num_sections; ++s) {
          if (rva >= sec_hdrs[s].VirtualAddress && rva < sec_hdrs[s].VirtualAddress + sec_hdrs[s].Misc.VirtualSize) {
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
          auto* entries = reinterpret_cast<WORD*>(output.data() + reloc_rva + sizeof(IMAGE_BASE_RELOCATION));

          for (DWORD j = 0; j < entry_count; ++j) {
            WORD type = entries[j] >> 12;
            WORD offset = entries[j] & 0xFFF;
            std::size_t target_rva = block->VirtualAddress + offset;

            if (type == IMAGE_REL_BASED_DIR64 && target_rva + 8 <= output.size() && is_readonly_rva(target_rva)) {
              auto* ptr = reinterpret_cast<std::uint64_t*>(output.data() + target_rva);
              if (*ptr >= aslr_base && *ptr < aslr_base + out_nt->OptionalHeader.SizeOfImage) {
                *ptr -= delta;
                ++reloc_fixups;
              }
            } else if (type == IMAGE_REL_BASED_HIGHLOW && target_rva + 4 <= output.size() &&
                       is_readonly_rva(target_rva)) {
              auto* ptr = reinterpret_cast<std::uint32_t*>(output.data() + target_rva);
              auto aslr32 = static_cast<std::uint32_t>(aslr_base);
              auto size32 = static_cast<std::uint32_t>(out_nt->OptionalHeader.SizeOfImage);
              if (*ptr >= aslr32 && *ptr < aslr32 + size32) {
                *ptr -= static_cast<std::uint32_t>(delta);
                ++reloc_fixups;
              }
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

      PIPE_LOG_DEBUG("[PEBuilder] Rebase: {} reloc fixups, {} heuristic .data fixups", reloc_fixups, heuristic_fixups);
    }
  }

  DeobfuscateIAT(output, out_nt->OptionalHeader.ImageBase, aslr_base, out_nt->OptionalHeader.SizeOfImage);

  out_nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(output.data() + nt_offset);
  out_nt->OptionalHeader.DllCharacteristics &= ~IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE;

  auto* sections = reinterpret_cast<IMAGE_SECTION_HEADER*>(output.data() + section_offset);
  for (WORD i = 0; i < num_sections; ++i) {
    sections[i].PointerToRawData = sections[i].VirtualAddress;
    sections[i].SizeOfRawData = AlignUp(sections[i].Misc.VirtualSize, out_nt->OptionalHeader.SectionAlignment);
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

  PIPE_LOG_DEBUG("[PEBuilder] Wrote memory dump PE ({} sections, {} bytes) to {}", num_sections, output.size(), path);
  return true;
}

}  // namespace dolos
