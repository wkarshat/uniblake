/* uniblake CPU probe — runtime detection (R3).
 *
 * Two entry points:
 *  - ub_detect_cpu(): ISA feature flags that kernel dispatch keys on.
 *  - ub_detect_cpu_info(): richer identity (vendor, brand, arch,
 *    generation coordinates) so selection and reporting can be
 *    GENERATION-aware, not just arch-aware — the Platforms.md finding
 *    that the same code differs by core within one ISA.
 *
 * Each platform's detection is a distinct, compile-guarded path; the
 * arm64-macOS path is real and exercised on this machine, the x86 and
 * Linux paths use the standard mechanism per platform (validated on
 * real x86 at C3 / A7).
 *
 * Mechanisms: cpuid on x86 (feature leaves 1/7, brand leaves
 * 0x80000002-4, vendor leaf 0), getauxval(AT_HWCAP)/MIDR on Linux/ARM,
 * sysctlbyname on macOS. AArch64 NEON is architecturally guaranteed.
 */
#include "ub_probe.h"

#include <string.h>
#include <stdio.h>

#if defined(__APPLE__)
#  include <sys/sysctl.h>
#endif
#if defined(__linux__)
#  include <sys/auxv.h>
#  if defined(__aarch64__) || defined(__arm__)
#    include <asm/hwcap.h>
#  endif
#endif
#if defined(_MSC_VER)
#  include <intrin.h>
#endif
#if (defined(__x86_64__) || defined(__i386__)) && defined(__GNUC__)
#  include <cpuid.h>
#endif

/* ---- portable cpuid wrapper (x86 only) ---- */
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
static void ub_cpuid(unsigned leaf, unsigned subleaf, unsigned out[4]) {
#  if defined(_MSC_VER)
  int regs[4];
  __cpuidex(regs, (int)leaf, (int)subleaf);
  out[0] = regs[0]; out[1] = regs[1]; out[2] = regs[2]; out[3] = regs[3];
#  elif defined(__GNUC__)
  unsigned a = 0, b = 0, c = 0, d = 0;
  __get_cpuid_count(leaf, subleaf, &a, &b, &c, &d);
  out[0] = a; out[1] = b; out[2] = c; out[3] = d;
#  else
  out[0] = out[1] = out[2] = out[3] = 0;
#  endif
}
#endif

ub_cpu_features ub_detect_cpu(void) {
  ub_cpu_features f;
  memset(&f, 0, sizeof f);

#if defined(__aarch64__) || defined(_M_ARM64)
  f.neon = 1; /* base ISA on aarch64 */

#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
  {
    unsigned r[4];
    ub_cpuid(1, 0, r);
    f.sse41  = (r[2] & (1u << 19)) ? 1 : 0; /* ECX.SSE4.1 */
    ub_cpuid(7, 0, r);
    f.avx2    = (r[1] & (1u << 5))  ? 1 : 0; /* EBX.AVX2 */
    f.avx512f = (r[1] & (1u << 16)) ? 1 : 0; /* EBX.AVX512F */
    f.sha_ni  = (r[1] & (1u << 29)) ? 1 : 0; /* EBX.SHA */
  }
#endif

  return f;
}

const char *ub_arch_name(ub_arch_class a) {
  switch (a) {
    case UB_ARCH_X86_64:  return "x86_64";
    case UB_ARCH_AARCH64: return "aarch64";
    default:              return "unknown";
  }
}
const char *ub_vendor_name(ub_vendor v) {
  switch (v) {
    case UB_VENDOR_INTEL: return "Intel";
    case UB_VENDOR_AMD:   return "AMD";
    case UB_VENDOR_APPLE: return "Apple";
    case UB_VENDOR_ARM:   return "ARM";
    case UB_VENDOR_OTHER: return "other";
    default:              return "unknown";
  }
}

ub_cpu_info ub_detect_cpu_info(void) {
  ub_cpu_info info;
  memset(&info, 0, sizeof info);
  strcpy(info.brand, "(unknown)");

#if defined(__aarch64__) || defined(_M_ARM64)
  info.arch = UB_ARCH_AARCH64;
#  if defined(__APPLE__)
  info.vendor = UB_VENDOR_APPLE;
  {
    size_t len = sizeof info.brand;
    /* "Apple M4 Pro" etc. Apple does not expose MIDR to userspace, so
     * the brand string is the identity handle on this platform. */
    if (sysctlbyname("machdep.cpu.brand_string", info.brand, &len, NULL, 0) != 0)
      strcpy(info.brand, "Apple silicon");
  }
#  elif defined(__linux__)
  {
    /* Linux AArch64: read MIDR via getauxval-adjacent means is not
     * portable; the reliable userspace source is /proc/cpuinfo's
     * "CPU implementer"/"CPU part". Parse it lightly. */
    FILE *fp = fopen("/proc/cpuinfo", "r");
    if (fp) {
      char line[256];
      while (fgets(line, sizeof line, fp)) {
        unsigned v;
        if (sscanf(line, "CPU implementer : 0x%x", &v) == 1) info.arm_implementer = (int)v;
        else if (sscanf(line, "CPU part : 0x%x", &v) == 1)   info.arm_part = (int)v;
      }
      fclose(fp);
    }
    info.vendor = (info.arm_implementer == 0x41) ? UB_VENDOR_ARM
                : (info.arm_implementer == 0x61) ? UB_VENDOR_APPLE
                : UB_VENDOR_OTHER;
    snprintf(info.brand, sizeof info.brand, "aarch64 impl=0x%02x part=0x%03x",
             info.arm_implementer, info.arm_part);
  }
#  endif

#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
  info.arch = UB_ARCH_X86_64;
  {
    unsigned r[4];

    /* Vendor string: leaf 0 → EBX,EDX,ECX. */
    ub_cpuid(0, 0, r);
    char vendor[13];
    memcpy(vendor + 0, &r[1], 4);
    memcpy(vendor + 4, &r[3], 4);
    memcpy(vendor + 8, &r[2], 4);
    vendor[12] = 0;
    if (strcmp(vendor, "GenuineIntel") == 0)      info.vendor = UB_VENDOR_INTEL;
    else if (strcmp(vendor, "AuthenticAMD") == 0) info.vendor = UB_VENDOR_AMD;
    else                                          info.vendor = UB_VENDOR_OTHER;

    /* Family/model/stepping: leaf 1 → EAX, with the extended-family and
     * extended-model fixups the SDM specifies. */
    ub_cpuid(1, 0, r);
    unsigned eax = r[0];
    unsigned base_family = (eax >> 8)  & 0xF;
    unsigned base_model  = (eax >> 4)  & 0xF;
    unsigned ext_family  = (eax >> 20) & 0xFF;
    unsigned ext_model   = (eax >> 16) & 0xF;
    info.x86_stepping = (int)(eax & 0xF);
    info.x86_family = (int)(base_family == 0xF ? base_family + ext_family
                                               : base_family);
    info.x86_model  = (int)((base_family == 0x6 || base_family == 0xF)
                              ? (ext_model << 4) + base_model
                              : base_model);

    /* Brand string: leaves 0x80000002..0x80000004, 48 bytes. */
    unsigned max_ext;
    ub_cpuid(0x80000000u, 0, r);
    max_ext = r[0];
    if (max_ext >= 0x80000004u) {
      char brand[49];
      unsigned *bp = (unsigned *)brand;
      for (unsigned leaf = 0x80000002u; leaf <= 0x80000004u; ++leaf) {
        ub_cpuid(leaf, 0, r);
        *bp++ = r[0]; *bp++ = r[1]; *bp++ = r[2]; *bp++ = r[3];
      }
      brand[48] = 0;
      /* trim leading spaces Intel pads with */
      const char *s = brand;
      while (*s == ' ') ++s;
      snprintf(info.brand, sizeof info.brand, "%s", s);
    }
  }
#endif

  return info;
}
