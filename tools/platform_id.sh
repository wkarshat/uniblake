#!/bin/sh
# Derive a stable platform id: <os>-<arch>-<model>-<cores>[-vm][-<uuid8>]
#
# Generated rather than typed, because a hand-written id drifts: the same
# machine gets two spellings and its rows stop grouping. Derived from the
# hardware, it is the same string every run.
#
#   tools/platform_id.sh          -> mac-arm64-m4pro-14c
#   tools/platform_id.sh --long   -> ...-4b4ceac8   (disambiguates identical hosts)
set -u
long=0; [ "${1:-}" = "--long" ] && long=1

slug() { printf '%s' "$1" | tr 'A-Z' 'a-z' | sed 's/[^a-z0-9]//g'; }

case "$(uname -s)" in
  Darwin)
    os=mac
    model=$(sysctl -n machdep.cpu.brand_string 2>/dev/null | sed 's/^Apple //')
    cores=$(sysctl -n hw.ncpu 2>/dev/null)
    uuid=$(ioreg -rd1 -c IOPlatformExpertDevice 2>/dev/null |
           awk -F'"' '/IOPlatformUUID/{print tolower(substr($4,1,8))}')
    vm=""
    ;;
  Linux)
    os=linux
    model=$(awk -F: '/model name/{print $2; exit}' /proc/cpuinfo 2>/dev/null |
            sed 's/^ *//; s/(R)//g; s/(TM)//g; s/ CPU.*//; s/ @.*//')
    [ -z "$model" ] && model=$(awk -F: '/^Model/{print $2; exit}' /proc/cpuinfo 2>/dev/null | sed 's/^ *//')
    cores=$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN)
    uuid=$(cat /etc/machine-id 2>/dev/null | cut -c1-8)
    # A VM's numbers carry hypervisor overhead and contention; say so in the id
    # rather than discovering it when two hosts disagree.
    vm=""
    if grep -qa hypervisor /proc/cpuinfo 2>/dev/null ||
       [ -n "$(systemd-detect-virt --quiet 2>/dev/null && systemd-detect-virt)" ]; then
      vm="-vm"
    fi
    ;;
  *) os=$(slug "$(uname -s)"); model=$(uname -m); cores=""; uuid=""; vm="" ;;
esac

arch=$(uname -m)
case "$arch" in x86_64|amd64) arch=x86 ;; aarch64|arm64) arch=arm64 ;; esac

id="$os-$arch-$(slug "$model")-${cores}c$vm"
[ "$long" = 1 ] && [ -n "$uuid" ] && id="$id-$uuid"
printf '%s\n' "$id"
