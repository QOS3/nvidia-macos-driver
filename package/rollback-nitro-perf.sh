#!/bin/bash
# rollback-nitro-perf.sh: restores the stock NullMoth 3.0 binaries, kexts, and AuxiliaryKernelExtensions.kc
# from /Library/NullMoth/backup-before-nitro-perf.
set -euo pipefail
PATH=/usr/bin:/bin:/usr/sbin:/sbin; export PATH

[ "$(id -u)" = 0 ] || { echo "STOP: run with sudo" >&2; exit 2; }
EXT=/Library/Extensions
GB=/Library/GPUBundles
KC=/Library/KernelCollections/AuxiliaryKernelExtensions.kc
KB=/System/Library/KernelCollections/BootKernelExtensions.kc
KS=/System/Library/KernelCollections/SystemKernelExtensions.kc
K=/System/Library/Kernels/kernel
KARG=(--allow-missing-kdk); [ -f "$K" ] && KARG+=(--kernel "$K")
BK=/Library/NullMoth/backup-before-nitro-perf
KEXTS="NVRM NVAccel NVRMFB NVRMAGDC"

[ -d "$BK" ] || { echo "STOP: backup folder $BK not found" >&2; exit 1; }

for k in $KEXTS; do
  if [ -d "$BK/$k.kext" ]; then
    rm -rf "$EXT/$k.kext"
    ditto "$BK/$k.kext" "$EXT/$k.kext"
    chown -R root:wheel "$EXT/$k.kext"
    chmod -R go-w,a+rX "$EXT/$k.kext"
  fi
done

if [ -d "$BK/NVMTLDriver.bundle" ]; then
  rm -rf "$GB/NVMTLDriver.bundle"
  ditto "$BK/NVMTLDriver.bundle" "$GB/NVMTLDriver.bundle"
fi
[ -f "$BK/nvmtl/libvulkan_nouveau.dylib" ] && cp -p "$BK/nvmtl/libvulkan_nouveau.dylib" "$GB/nvmtl/libvulkan_nouveau.dylib"
[ -f "$BK/nvmtl/nvrm610.conf" ] && cp -p "$BK/nvmtl/nvrm610.conf" "$GB/nvmtl/nvrm610.conf"

chown -R root:wheel "$GB/NVMTLDriver.bundle" "$GB/nvmtl" "$GB/nvmtl-allow.txt"
chmod -R go-w,a+rX "$GB/NVMTLDriver.bundle" "$GB/nvmtl"
chmod 644 "$GB/nvmtl-allow.txt"
chown -R _windowserver:wheel "$GB/nvmtl/aircache" "$GB/nvmtl/spvcache"
chmod 0755 "$GB/nvmtl/aircache" "$GB/nvmtl/spvcache"

if [ "${1:-}" != "--no-kc" ]; then
  if [ -f "$BK/AuxiliaryKernelExtensions.kc" ]; then
    cp -p "$BK/AuxiliaryKernelExtensions.kc" "$KC"
  else
    NEWKC="$KC.nitro-rollback-new"
    kmutil create -n aux --volume-root / "${KARG[@]}" -B "$KB" -S "$KS" --repository "$EXT" -A "$NEWKC" -z
    mv -f "$NEWKC" "$KC"
  fi
fi

echo "OK: restored stock NullMoth 3.0 driver from $BK. Reboot to apply."
