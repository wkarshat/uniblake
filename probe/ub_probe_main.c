/* Copyright (c) 2026 UniBlake Developers */
/* Print the CPU identity and ISA flags a measurement should be reported with.
 *
 * Not part of the library: the core has one scalar kernel and no dispatch, so
 * nothing in include/ or src/ consults this. It exists because a benchmark
 * figure is meaningless without the machine it came from, and because a kernel
 * selector -- if one is ever written -- has to key on core identity rather
 * than ISA flags alone. The NEON kernels in backends/ are the case in point:
 * NEON is base ISA on every aarch64 core, so "has NEON" does not predict
 * whether the NEON kernel wins.
 *
 * Run with `make probe`. Paste the output alongside any figure.
 */
#include <stdio.h>
#include "ub_probe.h"

int main(void) {
  ub_cpu_info     i = ub_detect_cpu_info();
  ub_cpu_features f = ub_detect_cpu();

  printf("brand      %s\n", i.brand[0] ? i.brand : "(unknown)");
  printf("arch       %s\n", ub_arch_name(i.arch));
  printf("vendor     %s\n", ub_vendor_name(i.vendor));

  if (i.arch == UB_ARCH_X86_64)
    printf("family     %d  model 0x%X  stepping %d\n",
           i.x86_family, i.x86_model, i.x86_stepping);

  if (i.arch == UB_ARCH_AARCH64) {
    if (i.arm_implementer || i.arm_part)
      printf("midr       implementer 0x%02X  part 0x%03X\n",
             i.arm_implementer, i.arm_part);
    else
      /* Apple does not expose MIDR to userspace; the brand string is the
       * only identity available there. */
      printf("midr       not exposed by this OS; identity is the brand string\n");
  }

  printf("features   neon=%d sse41=%d avx2=%d avx512f=%d sha_ni=%d\n",
         f.neon, f.sse41, f.avx2, f.avx512f, f.sha_ni);

  return 0;
}
