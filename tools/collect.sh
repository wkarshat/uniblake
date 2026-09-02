#!/bin/sh
# Run every benchmark and analysis target, capture output with the conditions
# needed to interpret it later.
#
#   tools/collect.sh [SODIUM_PREFIX] > run.txt
#
# Exists because these were being run by hand, one at a time, with the machine
# and oracle details left in a chat window instead of beside the numbers.
set -u
SOD="${1:-${SODIUM:-/usr}}"

hr() { echo; echo "=== $* ==="; }

hr environment
date -u +'utc: %Y-%m-%dT%H:%M:%SZ'
echo "host: $(uname -srm)"
[ -r /proc/cpuinfo ] && grep -m1 'model name' /proc/cpuinfo
command -v lscpu >/dev/null && lscpu | grep -E '^(Model name|Socket|Core|Thread|CPU\(s\)|Hypervisor|Flags)' | cut -c1-160
command -v sysctl >/dev/null && sysctl -n machdep.cpu.brand_string 2>/dev/null
printf 'cc: '; ${CC:-cc} --version 2>/dev/null | head -1
echo "oracle prefix: $SOD"
sed -n 's/.*SODIUM_VERSION_STRING "\(.*\)".*/oracle version: \1/p' "$SOD/include/sodium/version.h" 2>/dev/null
git rev-parse --short HEAD 2>/dev/null | sed 's/^/tree: /'
git status --porcelain 2>/dev/null | head -5 | sed 's/^/dirty: /'

for t in kernel-stats bench-isa bench-phases; do
  hr "$t"; make -s "$t" 2>&1
done

for t in bench bench-compare; do
  hr "$t"; make -s "$t" SODIUM="$SOD" 2>&1
done

case "$(uname -m)" in
  x86_64|amd64)
    for t in check-avx2 bench-avx2 bench-compare-avx2; do
      hr "$t"; make -s "$t" SODIUM="$SOD" 2>&1
    done ;;
  arm64|aarch64)
    for t in bench-neon check-avx2-rosetta; do
      hr "$t"; make -s "$t" SODIUM="$SOD" 2>&1
    done ;;
esac

for n in 2 4 8; do
  hr "bench-threads THREADS=$n"; make -s bench-threads THREADS="$n" SODIUM="$SOD" 2>&1
done

hr done
