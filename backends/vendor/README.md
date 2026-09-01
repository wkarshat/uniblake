# Third-party kernel sources

Empty. Kernel sources copied from another project belong here, unmodified,
each with a note giving its upstream link and the commit it was taken from.

Held open because a SIMD or batch kernel adopted later is likely to start from
a donor rather than from scratch. The BLAKE2 reference package's NEON macros
were vendored here while an unrolled kernel was being evaluated; that kernel
measured slower than the one in `backends/` and was removed. Recover both from
the `neon-both-kernels` tag.

Files here keep their upstream copyright and license. Files elsewhere in this
repository are MIT, Copyright (c) 2026 UniBlake Developers.
