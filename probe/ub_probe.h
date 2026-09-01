/* uniblake CPU probe interface.
 *
 * Two layers:
 *  - ub_cpu_features: the boolean ISA flags the KERNEL SELECTION needs
 *    (does this CPU have NEON / SSE4.1 / AVX2 …). This is what dispatch
 *    keys on.
 *  - ub_cpu_info: richer identity — vendor, brand string, arch class,
 *    and generation coordinates (x86 family/model/stepping; ARM
 *    implementer/part or the OS-reported brand). This is what lets
 *    selection and reporting be GENERATION-aware, not just
 *    architecture-aware — the Platforms.md lesson that the same code
 *    can be faster or slower on two cores of the same ISA (Cortex-A53
 *    vs A57; M-series vs A9). Selection can consult it; ub_test/kbench
 *    print it so any measurement is tied to an exact machine (§4a).
 */
#ifndef UB_PROBE_H
#define UB_PROBE_H

typedef struct {
  /* --- ISA feature flags (what dispatch keys on) --- */
  int neon;    /* AArch64 ASIMD (base ISA on aarch64) */
  int sse41;   /* x86 SSE4.1 */
  int avx2;    /* x86 AVX2 */
  int avx512f; /* x86 AVX-512 Foundation (opportunistic; see Platforms.md) */
  int sha_ni;  /* x86 SHA extensions (not BLAKE2; recorded for completeness) */
} ub_cpu_features;

typedef enum {
  UB_ARCH_UNKNOWN = 0,
  UB_ARCH_X86_64,
  UB_ARCH_AARCH64
} ub_arch_class;

typedef enum {
  UB_VENDOR_UNKNOWN = 0,
  UB_VENDOR_INTEL,
  UB_VENDOR_AMD,
  UB_VENDOR_APPLE,
  UB_VENDOR_ARM,      /* generic ARM Ltd. core (Cortex, Neoverse) */
  UB_VENDOR_OTHER
} ub_vendor;

typedef struct {
  ub_arch_class arch;
  ub_vendor     vendor;
  char          brand[64];  /* human brand string, e.g. "Apple M4 Pro" */

  /* x86 generation coordinates (0 on non-x86). family/model together
   * identify the microarchitecture (e.g. 6/0x8C = Tiger Lake). */
  int x86_family;
  int x86_model;
  int x86_stepping;

  /* ARM generation coordinates (0 on non-ARM). implementer is the
   * CPUID implementer byte (0x41 = ARM Ltd., 0x61 = Apple); part is the
   * primary-part-number field. On macOS these may be 0 (Apple does not
   * expose MIDR to userspace) — the brand string carries the identity
   * there instead. */
  int arm_implementer;
  int arm_part;
} ub_cpu_info;

/* Feature flags for dispatch. */
ub_cpu_features ub_detect_cpu(void);

/* Full identity for generation-aware selection + reporting. */
ub_cpu_info ub_detect_cpu_info(void);

/* Convenience: short human strings for logging. */
const char *ub_arch_name(ub_arch_class a);
const char *ub_vendor_name(ub_vendor v);

#endif /* UB_PROBE_H */
