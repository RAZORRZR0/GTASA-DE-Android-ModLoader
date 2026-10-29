// Host test: clang++ -std=c++17 -O2 -Wall -Wextra test_host.cpp -o test_host
// Usage: ./test_host <libUE4.so> <nm_mangled.txt>
#include "gtade.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string>
#include <vector>

static const char* kFunc = "_ZN7CObject11SetIsStaticEb";

static uint64_t Le(const uint8_t* p, int n) {
  uint64_t v = 0;
  memcpy(&v, p, n);
  return v;
}

struct Seg { uint32_t type, flags; uint64_t off, va, filesz, memsz; };

int main(int argc, char** argv) {
  if (argc < 3) { fprintf(stderr, "usage: %s libUE4.so nm_mangled.txt\n", argv[0]); return 2; }

  // 1. Parse ELF64 program headers and build a flat image indexed by vaddr.
  int fd = open(argv[1], O_RDONLY);
  if (fd < 0) { perror("open"); return 1; }
  uint8_t eh[64];
  if (pread(fd, eh, 64, 0) != 64 || memcmp(eh, "\x7f" "ELF", 4) != 0 || eh[4] != 2) { fprintf(stderr, "not ELF64\n"); return 1; }
  uint64_t phoff = Le(eh + 0x20, 8);
  int phnum = (int)Le(eh + 0x38, 2), phent = (int)Le(eh + 0x36, 2);
  std::vector<Seg> loads;
  uint64_t image_size = 0;
  for (int i = 0; i < phnum; ++i) {
    uint8_t ph[56];
    if (pread(fd, ph, 56, phoff + (off_t)i * phent) != 56) return 1;
    Seg s{(uint32_t)Le(ph, 4), (uint32_t)Le(ph + 4, 4), Le(ph + 8, 8), Le(ph + 16, 8), Le(ph + 32, 8), Le(ph + 40, 8)};
    if (s.type == 1) {  // PT_LOAD
      loads.push_back(s);
      if (s.va + s.memsz > image_size) image_size = s.va + s.memsz;
    }
  }
  image_size = (image_size + 0xFFF) & ~uint64_t{0xFFF};
  uint8_t* image = static_cast<uint8_t*>(mmap(nullptr, image_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (image == MAP_FAILED) { perror("mmap"); return 1; }
  for (const Seg& s : loads)
    if (s.filesz && pread(fd, image + s.va, s.filesz, s.off) != (ssize_t)s.filesz) { perror("pread"); return 1; }
  close(fd);
  // Image base is page-aligned, so ADRP page math in pointer space equals vaddr page math.
  const uintptr_t base = reinterpret_cast<uintptr_t>(image);
  auto ToVa = [&](uintptr_t p) { return (uint64_t)(p - base); };

  Seg text{};

  // 2. Find the symbol in nm_mangled.txt. Keep every T/t entry for BL target lookup.
  FILE* nm = fopen(argv[2], "r");
  if (!nm) { perror("fopen nm"); return 1; }
  std::vector<std::pair<uint64_t, std::string>> syms;
  char line[4096];
  uint64_t func_va = 0;
  while (fgets(line, sizeof line, nm)) {
    char type = 0; char name[3072];
    unsigned long long a;
    if (sscanf(line, "%llx %c %3071s", &a, &type, name) != 3 || (type != 'T' && type != 't')) continue;
    syms.emplace_back(a, name);
    if (!strcmp(name, kFunc)) func_va = a;
  }
  fclose(nm);
  if (!func_va) { fprintf(stderr, "%s not in nm\n", kFunc); return 1; }

  // vaddr -> file offset via the PT_LOAD that contains it.
  uint64_t func_off = 0;
  for (const Seg& s : loads)
    if (func_va >= s.va && func_va < s.va + s.filesz) func_off = func_va - s.va + s.off;
  printf("%s\n  vaddr=0x%" PRIx64 " file_off=0x%" PRIx64 "\n", kFunc, func_va, func_off);
  for (const Seg& s : loads)
    if ((s.flags & 1) && func_va >= s.va && func_va < s.va + s.memsz) text = s;  // exec PT_LOAD holding the function
  printf("image: %" PRIu64 " bytes, text PT_LOAD va=0x%" PRIx64 " off=0x%" PRIx64 " memsz=0x%" PRIx64 "\n",
         image_size, text.va, text.off, text.memsz);

  // 3. Build pattern from the first 32 bytes (16 is not unique: 695 hits); BL/ADRP words get immediate wildcards.
  uintptr_t fa = base + func_va;
  std::string pat;
  int masked = 0;
  for (int w = 0; w < 8; ++w) {
    uint32_t ins;
    memcpy(&ins, reinterpret_cast<void*>(fa + 4 * w), 4);
    const uint8_t* b = reinterpret_cast<const uint8_t*>(fa + 4 * w);
    if ((ins & 0xFC000000u) == 0x94000000u) {         // BL imm26: low 3 bytes + high nibble of byte3 fixed
      pat += "?? ?? ?? 9? "; ++masked;
    } else if ((ins & 0x9F000000u) == 0x90000000u) {  // ADRP immlo/immhi/Rd
      pat += "?? ?? ?? ?0 "; ++masked;
    } else {
      char tmp[16];
      snprintf(tmp, sizeof tmp, "%02X %02X %02X %02X ", b[0], b[1], b[2], b[3]);
      pat += tmp;
    }
  }
  pat.pop_back();
  printf("pattern (%d BL/ADRP words masked): %s\n", masked, pat.c_str());

  // 4. Unique scan across the executable PT_LOAD.
  uintptr_t tb = base + text.va, te = tb + text.memsz;
  uintptr_t hit = gtade::ScanUnique(tb, te, pat.c_str());
  uint64_t hit_va = hit ? ToVa(hit) : 0;
  printf("ScanUnique -> va=0x%" PRIx64 " (expected 0x%" PRIx64 ") %s\n", hit_va, func_va,
         hit_va == func_va ? "OK" : "MISMATCH");
  if (hit_va != func_va) return 1;

  // 5. Decode the first BL in the function body (stop at RET).
  uintptr_t bl = 0;
  for (uintptr_t a = fa; a < fa + 0x400 && !bl; a += 4) {
    uint32_t ins;
    memcpy(&ins, reinterpret_cast<void*>(a), 4);
    if (ins == 0xD65F03C0u) break;  // RET
    if ((ins & 0xFC000000u) == 0x94000000u) bl = a;
  }
  if (!bl) { printf("no BL in function\n"); return 1; }
  uintptr_t t = gtade::BranchTarget(bl);
  printf("first BL at va=0x%" PRIx64 " -> target va=0x%" PRIx64 "\n", ToVa(bl), ToVa(t));
  for (auto& s : syms)
    if (s.first == ToVa(t)) { printf("  nm symbol: %s\nPASS\n", s.second.c_str()); return 0; }

  // Target has no symbol: it is a PLT stub (adrp x16; ldr x17,[x16,#off]; add x16; br x17).
  // ResolveAdrpLdr gives the GOT slot; the dynamic relocation for that slot names the import.
  uintptr_t slot = gtade::ResolveAdrpLdr(t);
  if (!slot) { printf("  target is neither a symbol nor a PLT stub\n"); return 1; }
  uint64_t slot_va = ToVa(slot);
  uint64_t dyn_va = 0;
  for (int i = 0; i < phnum; ++i) {
    const uint8_t* ph = image + phoff + (uint64_t)i * phent;  // phdrs live in the first PT_LOAD
    if (Le(ph, 4) == 2) dyn_va = Le(ph + 16, 8);             // PT_DYNAMIC
  }
  auto DynVal = [&](uint64_t tag) -> uint64_t {
    for (const uint8_t* d = image + dyn_va; Le(d, 8); d += 16)
      if (Le(d, 8) == tag) return Le(d + 8, 8);
    return 0;
  };
  const uint8_t* dynsym = image + DynVal(6);  // DT_SYMTAB
  const char* dynstr = reinterpret_cast<const char*>(image + DynVal(5));  // DT_STRTAB
  std::string imp;
  auto FindRela = [&](uint64_t tab, uint64_t sz) {
    for (uint64_t o = 0; o + 24 <= sz && imp.empty(); o += 24) {
      const uint8_t* r = image + tab + o;
      uint64_t si = Le(r + 8, 8) >> 32;
      if (Le(r, 8) == slot_va && si) imp = dynstr + Le(dynsym + si * 24, 4);
    }
  };
  FindRela(DynVal(23), DynVal(2));  // DT_JMPREL / DT_PLTRELSZ
  FindRela(DynVal(7), DynVal(8));   // DT_RELA / DT_RELASZ
  printf("  PLT stub: GOT slot va=0x%" PRIx64 " -> import \"%s\"\n", slot_va, imp.c_str());
  if (imp.empty()) return 1;

  // The import must appear as an undefined (U) entry in nm.
  std::string want = " U " + imp + "\n";
  FILE* nm2 = fopen(argv[2], "r");
  bool in_nm = false;
  while (nm2 && !in_nm && fgets(line, sizeof line, nm2)) {
    size_t len = strlen(line);
    in_nm = len >= want.size() && strcmp(line + len - want.size(), want.c_str()) == 0;
  }
  if (nm2) fclose(nm2);
  printf("  nm has U entry: %s\n", in_nm ? "yes" : "no");
  printf(in_nm ? "PASS\n" : "FAIL\n");
  return in_nm ? 0 : 1;
}
