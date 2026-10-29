// gtade.h - header-only helpers for AML mods on Android GTA SA DE (arm64 libUE4.so).
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#if defined(__linux__) || defined(__ANDROID__)
#include <link.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

namespace gtade {

struct Range { uintptr_t begin, end; };

#if defined(__linux__) || defined(__ANDROID__)
namespace detail {
inline bool NameMatches(const char* name, const char* soname) {
  size_t n = strlen(name), m = strlen(soname);
  return n >= m && strcmp(name + n - m, soname) == 0 && (n == m || name[n - m - 1] == '/');
}
struct PhdrQuery { const char* soname; uintptr_t base; Range text; bool found; };
inline int PhdrCb(dl_phdr_info* info, size_t, void* data) {
  auto* q = static_cast<PhdrQuery*>(data);
  if (!info->dlpi_name || !NameMatches(info->dlpi_name, q->soname)) return 0;
  q->found = true;
  q->base = info->dlpi_addr;
  for (int i = 0; i < info->dlpi_phnum; ++i) {
    const ElfW(Phdr)& p = info->dlpi_phdr[i];
    if (p.p_type == PT_LOAD && (p.p_flags & PF_X)) {
      q->text = { info->dlpi_addr + p.p_vaddr, info->dlpi_addr + p.p_vaddr + p.p_memsz };
      break;
    }
  }
  return 1;
}
inline PhdrQuery Query(const char* soname) {
  PhdrQuery q{soname, 0, {0, 0}, false};
  dl_iterate_phdr(PhdrCb, &q);
  return q;
}
}  // namespace detail

// Load base (dlpi_addr) of a loaded module matched by file name suffix; 0 if absent.
inline uintptr_t ModuleBase(const char* soname) { return detail::Query(soname).base; }
// Executable PT_LOAD range of a loaded module; {0,0} if absent.
inline Range TextRange(const char* soname) { return detail::Query(soname).text; }

// Reads n bytes from our own address space; false if any byte is unmapped.
inline bool SafeRead(const void* src, void* dst, size_t n) {
  iovec local{dst, n}, remote{const_cast<void*>(src), n};
  return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(n);
}
#endif

// Thin dlsym wrapper; T must be a pointer type (e.g. void (*)(int)).
template <class T>
T Sym(void* handle, const char* mangled) {
  return reinterpret_cast<T>(dlsym(handle, mangled));
}

// ---- Pattern scan ----------------------------------------------------------
// Pattern: space-separated bytes. "??"/"?" = full wildcard; "9?" / "?F" = nibble wildcard.
namespace detail {
constexpr size_t kMaxPat = 256;
struct Pat { uint8_t v[kMaxPat], m[kMaxPat]; size_t n; };

inline int HexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
inline bool Compile(const char* p, Pat& out) {
  out.n = 0;
  for (;;) {
    while (*p == ' ') ++p;
    if (!*p) return out.n > 0;
    size_t len = 0;
    while (p[len] && p[len] != ' ') ++len;
    if (len == 1 && p[0] == '?') { len = 2; }  // "?" == "??"
    if (len != 2 || out.n >= kMaxPat) return false;
    uint8_t v = 0, m = 0;
    for (int i = 0; i < 2; ++i) {
      char c = p[i];
      int shift = i == 0 ? 4 : 0;
      if (c == '?') continue;
      int h = HexVal(c);
      if (h < 0) return false;
      v |= static_cast<uint8_t>(h << shift);
      m |= static_cast<uint8_t>(0xF << shift);
    }
    out.v[out.n] = v;
    out.m[out.n] = m;
    ++out.n;
    p += len;
  }
}
// True if pattern matches at address a (caller guarantees a + n <= end).
inline bool At(uintptr_t a, const Pat& pt) {
  const uint8_t* s = reinterpret_cast<const uint8_t*>(a);
  for (size_t k = 0; k < pt.n; ++k)
    if ((s[k] & pt.m[k]) != pt.v[k]) return false;
  return true;
}
}  // namespace detail

// First match address in [begin, end), or 0.
inline uintptr_t Scan(uintptr_t begin, uintptr_t end, const char* pattern) {
  detail::Pat pt;
  if (!detail::Compile(pattern, pt) || end < begin || end - begin < pt.n) return 0;
  for (uintptr_t a = begin; a + pt.n <= end; ++a)
    if (detail::At(a, pt)) return a;
  return 0;
}

// Address of the only match in [begin, end); 0 if there are zero or 2+ matches.
inline uintptr_t ScanUnique(uintptr_t begin, uintptr_t end, const char* pattern) {
  uintptr_t first = Scan(begin, end, pattern);
  if (!first) return 0;
  return Scan(first + 1, end, pattern) ? 0 : first;
}

// ---- arm64 decoders (take the instruction address) -------------------------
// B / BL: returns target address, or 0 if the instruction is neither.
inline uintptr_t BranchTarget(uintptr_t at) {
  uint32_t ins;
  memcpy(&ins, reinterpret_cast<const void*>(at), 4);
  if ((ins & 0x7C000000u) != 0x14000000u) return 0;  // B = 000101, BL = 100101
  int32_t imm26 = static_cast<int32_t>(ins << 6) >> 6;
  return at + static_cast<intptr_t>(imm26) * 4;
}

// ADRP: returns the 4 KiB page address it computes, or 0 if not ADRP.
inline uintptr_t AdrpTarget(uintptr_t adrp_addr) {
  uint32_t ins;
  memcpy(&ins, reinterpret_cast<const void*>(adrp_addr), 4);
  if ((ins & 0x9F000000u) != 0x90000000u) return 0;
  uint32_t imm21 = (((ins >> 5) & 0x7FFFFu) << 2) | ((ins >> 29) & 3u);
  intptr_t imm = static_cast<int32_t>(imm21 << 11) >> 11;
  return (adrp_addr & ~uintptr_t{0xFFF}) + static_cast<uintptr_t>(imm << 12);
}

namespace detail {
inline uint32_t Ins(uintptr_t a) {
  uint32_t ins;
  memcpy(&ins, reinterpret_cast<const void*>(a), 4);
  return ins;
}
}  // namespace detail

// ADRP Xd + ADD Xd, Xd, #imm12 (next instruction). Returns the final address or 0.
inline uintptr_t ResolveAdrpAdd(uintptr_t adrp_addr) {
  uintptr_t page = AdrpTarget(adrp_addr);
  if (!page) return 0;
  uint32_t rd = detail::Ins(adrp_addr) & 31;
  uint32_t add = detail::Ins(adrp_addr + 4);
  if ((add & 0x7F800000u) != 0x11000000u || ((add >> 5) & 31) != rd) return 0;  // ADD imm, Rn==Rd
  uintptr_t imm = (add >> 10) & 0xFFF;
  if ((add >> 22) & 1) imm <<= 12;
  return page + imm;
}

// ADRP Xd + LDR Wt/Xt, [Xd, #uimm12] (next instruction). Returns the ADDRESS of the
// memory slot to load from (caller dereferences), or 0.
inline uintptr_t ResolveAdrpLdr(uintptr_t adrp_addr) {
  uintptr_t page = AdrpTarget(adrp_addr);
  if (!page) return 0;
  uint32_t rd = detail::Ins(adrp_addr) & 31;
  uint32_t ldr = detail::Ins(adrp_addr + 4);
  uintptr_t scale;
  if ((ldr & 0xFFC00000u) == 0xF9400000u) scale = 8;       // LDR Xt, [Xn, #imm]
  else if ((ldr & 0xFFC00000u) == 0xB9400000u) scale = 4; // LDR Wt, [Xn, #imm]
  else return 0;
  if (((ldr >> 5) & 31) != rd) return 0;
  return page + (((ldr >> 10) & 0xFFF) * scale);
}

}  // namespace gtade
