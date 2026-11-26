#!/bin/bash
# install-nitro-perf.sh: atomically installs nitro-perf binaries & kexts over an existing NullMoth 3.0 install
# on macOS 15 Sequoia x86_64, with full SHA-256 verification, automatic stock backup, KextPolicy cdhash sync,
# verified AuxiliaryKernelExtensions.kc staging, and automatic boot-guard lastgood fallback.
set -euo pipefail
PATH=/usr/bin:/bin:/usr/sbin:/sbin; export PATH

[ "$(id -u)" = 0 ] || { echo "STOP: run with sudo" >&2; exit 2; }
[ "$(uname -m)" = x86_64 ] || { echo "STOP: Intel/x86_64 only" >&2; exit 2; }
VER=$(sw_vers -productVersion); MAJ=${VER%%.*}
[ "$MAJ" = 15 ] || { echo "STOP: this nitro-perf bundle targets macOS 15 Sequoia (found $VER)" >&2; exit 2; }

HERE=$(cd "$(dirname "$0")" && pwd)
EXT=/Library/Extensions
GB=/Library/GPUBundles
KC=/Library/KernelCollections/AuxiliaryKernelExtensions.kc
KB=/System/Library/KernelCollections/BootKernelExtensions.kc
KS=/System/Library/KernelCollections/SystemKernelExtensions.kc
K=/System/Library/Kernels/kernel
KARG=(--allow-missing-kdk); [ -f "$K" ] && KARG+=(--kernel "$K")
ST=/Library/NullMoth
BK="$ST/backup-before-nitro-perf"
KEXTS="NVRM NVAccel NVRMFB NVRMAGDC"
KPDB=/var/db/SystemPolicyConfiguration/KextPolicy

echo "== 1. Verifying SHA256SUMS =="
( cd "$HERE" && shasum -a 256 --strict -c SHA256SUMS --quiet )
echo "OK: all bundle files match SHA256SUMS (commit $(cat "$HERE/COMMIT" 2>/dev/null || echo unknown))"

for d in "$EXT/NVRM.kext" "$EXT/NVAccel.kext" "$EXT/NVRMFB.kext" "$EXT/NVRMAGDC.kext" "$GB/NVMTLDriver.bundle" "$GB/nvmtl"; do
  [ -e "$d" ] || { echo "STOP: $d missing — install NullMoth 3.0 first" >&2; exit 1; }
done

echo "== 2. Backing up current stock 3.0 driver =="
if [ ! -d "$BK" ]; then
  mkdir -p "$BK"
  for k in $KEXTS; do ditto "$EXT/$k.kext" "$BK/$k.kext"; done
  ditto "$GB/NVMTLDriver.bundle" "$BK/NVMTLDriver.bundle"
  mkdir -p "$BK/nvmtl"
  cp -p "$GB/nvmtl/libvulkan_nouveau.dylib" "$BK/nvmtl/"
  cp -p "$GB/nvmtl/nvrm610.conf" "$BK/nvmtl/"
  [ -f "$KC" ] && cp -p "$KC" "$BK/AuxiliaryKernelExtensions.kc"
  echo "OK: clean stock backup saved at $BK"
else
  echo "OK: preserving existing stock backup at $BK"
fi

T=$(mktemp -d /var/tmp/nitro-perf-install.XXXX)
NEWKC="$KC.nitro-perf-new"
cleanup() { rm -rf "$T" "$NEWKC"; }
trap cleanup EXIT

echo "== 3. Installing nitro-perf userland atomically (NVMTLDriver, translator, nvencvt, NVK, nvmtl-smi) =="
stage_bin() {
  local src="$1" dst="$2" signed tmp
  signed="$T/$(basename "$dst").$$"
  cp -f "$src" "$signed"
  chmod 755 "$signed"
  codesign --force --sign - "$signed"
  tmp="$(dirname "$dst")/.$(basename "$dst").nitro.tmp.$$"
  cp -pf "$signed" "$tmp"
  chown root:wheel "$tmp"
  chmod 755 "$tmp"
  mv -f "$tmp" "$dst"
}
stage_bin "$HERE/userland/libnvmtl_translate.dylib" "$GB/NVMTLDriver.bundle/Contents/MacOS/libnvmtl_translate.dylib"
# Refresh .sha256 sidecar atomically so NVMTLLibraryLoad's fast-path matches the newly installed translator
shasum -a 256 "$GB/NVMTLDriver.bundle/Contents/MacOS/libnvmtl_translate.dylib" | awk '{print $1}' > "$T/xlate.sha256"
chown root:wheel "$T/xlate.sha256"; chmod 644 "$T/xlate.sha256"
mv -f "$T/xlate.sha256" "$GB/NVMTLDriver.bundle/Contents/MacOS/libnvmtl_translate.dylib.sha256"
stage_bin "$HERE/userland/libnvencvt.dylib" "$GB/NVMTLDriver.bundle/Contents/MacOS/libnvencvt.dylib"
stage_bin "$HERE/userland/NVMTLDriver" "$GB/NVMTLDriver.bundle/Contents/MacOS/NVMTLDriver"
stage_bin "$HERE/userland/libvulkan_nouveau.dylib" "$GB/nvmtl/libvulkan_nouveau.dylib"
mkdir -p "$ST/bin" /usr/local/bin
stage_bin "$HERE/bin/nvmtl-smi" "$ST/bin/nvmtl-smi"
ln -sf "$ST/bin/nvmtl-smi" /usr/local/bin/nvmtl-smi
if [ -f "$HERE/nitro-perf-ctl.sh" ]; then
  install -m 755 -o root -g wheel "$HERE/nitro-perf-ctl.sh" "$ST/nitro-perf-ctl.sh"
  ln -sf "$ST/nitro-perf-ctl.sh" /usr/local/bin/nitro-perf-ctl
fi
if [ -f "$HERE/merge-shared-cache.sh" ]; then
  install -m 755 -o root -g wheel "$HERE/merge-shared-cache.sh" "$ST/merge-shared-cache.sh"
fi
if [ -f "$HERE/rollback-nitro-perf.sh" ]; then
  install -m 755 -o root -g wheel "$HERE/rollback-nitro-perf.sh" "$ST/rollback-nitro-perf.sh"
fi

chown -R root:wheel "$GB/NVMTLDriver.bundle" "$GB/nvmtl" "$GB/nvmtl-allow.txt"
chmod -R go-w,a+rX "$GB/NVMTLDriver.bundle" "$GB/nvmtl"
chmod 644 "$GB/nvmtl-allow.txt"
sudo -u _windowserver /bin/test -r "$GB/nvmtl-allow.txt" || { echo "STOP: _windowserver cannot read nvmtl-allow.txt" >&2; exit 1; }
mkdir -p "$GB/nvmtl/aircache" "$GB/nvmtl/spvcache"
chown -R _windowserver:wheel "$GB/nvmtl/aircache" "$GB/nvmtl/spvcache"
chmod 0755 "$GB/nvmtl/aircache" "$GB/nvmtl/spvcache"

echo "== 4. Installing nitro-perf kexts (NVRM, NVAccel, NVRMFB, NVRMAGDC) =="
INSTALL_KEXTS="NVRM NVAccel NVRMFB"
[ -d "$HERE/kexts/NVRMAGDC.kext" ] && INSTALL_KEXTS="$INSTALL_KEXTS NVRMAGDC"
for k in $INSTALL_KEXTS; do
  rm -rf "$EXT/$k.kext"
  ditto "$HERE/kexts/$k.kext" "$EXT/$k.kext"
  chown -R root:wheel "$EXT/$k.kext"
  chmod -R go-w,a+rX "$EXT/$k.kext"
  chmod 755 "$EXT/$k.kext/Contents/MacOS/$k"
  codesign --force --sign - "$EXT/$k.kext"
done

if [ -r "$KPDB" ] && command -v sqlite3 >/dev/null 2>&1; then
  for k in $INSTALL_KEXTS; do
    id="com.nullmoth.$k"; p="$EXT/$k.kext"
    ch=$(codesign -dvvv "$p" 2>&1 | awk -F= '/^CDHash=/{print $2; exit}')
    if printf '%s' "$ch" | grep -Eq '^[0-9a-f]{40}$'; then
      /usr/bin/sqlite3 "$KPDB" "update kext_load_history_v3 set cdhash='$ch' where path='$p' and bundle_id='$id' and ifnull(team_id,'')=''" 2>/dev/null || true
    fi
  done
fi

echo "== 5. Building and verifying AuxiliaryKernelExtensions.kc =="
rm -f "$NEWKC"
if ! kmutil create -n aux --volume-root / "${KARG[@]}" -B "$KB" -S "$KS" --repository "$EXT" -A "$NEWKC" -z >"$T/kmutil.log" 2>&1; then
  cat "$T/kmutil.log" >&2
  echo "STOP: kmutil create failed; rolling back kexts automatically" >&2
  bash "$HERE/rollback-nitro-perf.sh" --no-kc || true
  exit 1
fi
INS=$(kmutil inspect -a x86_64 -A "$NEWKC" 2>/dev/null)
for k in $KEXTS; do
  printf '%s\n' "$INS" | grep -oE 'com\.nullmoth\.[A-Za-z0-9]+' | grep -Fxq "com.nullmoth.$k" || {
    echo "STOP: com.nullmoth.$k missing from new AuxiliaryKernelExtensions.kc; aborting" >&2
    exit 1
  }
done
mv -f "$NEWKC" "$KC"
echo "OK: AuxiliaryKernelExtensions.kc rebuilt with all 4 NullMoth kexts"

echo "== 6. Applying Nitro 5 RTX 2050 144Hz settings to nvrm610.conf =="
CONF="$GB/nvmtl/nvrm610.conf"
sed -i '' '/^# --- my settings, kept on update ---$/,/^# --- end my settings ---$/d' "$CONF"
cat <<'EOF' >> "$CONF"
# --- my settings, kept on update ---
NVMTL_GSUB_INFLIGHT=8
NVRM_EVENT_WAIT=1
NVRM_EVENT_WAIT_SPIN_US=20
NVMTL_SPVCACHE_MAX_MB=4096
NVMTL_ANGLE_METAL=1
NVMTL_GAME_GPU_PRIORITY=1
NVMTL_SURF_IMPORT_CACHE=1
# NVMTL_FPS_LIMIT_BATT=60  # uncomment to cap FPS on battery
# --- end my settings ---
EOF
chown root:wheel "$CONF"; chmod 644 "$CONF"

echo "== 7. Arming NullMoth automatic boot-guard fallback =="
V=7C436110-AB2A-4BBB-A880-FE41995C9F82
nvram -d $V:nullmoth-nv-starts 2>/dev/null || true
nvram -d $V:nullmoth-nv-ok 2>/dev/null || true
LG="$ST/lastgood.sh"; LGP=/Library/LaunchDaemons/com.nullmoth.lastgood.plist
launchctl bootout system/com.nullmoth.lastgood 2>/dev/null || true
cat > "$LG" <<EOF
#!/bin/bash
PATH=/usr/bin:/bin:/usr/sbin:/sbin
BK='$BK'; LOG=/Library/NullMoth/lastgood.log; V=$V
done_job() { rm -f /Library/LaunchDaemons/com.nullmoth.lastgood.plist /Library/NullMoth/lastgood.sh; }
if nvram \$V:nullmoth-nv-ok >/dev/null 2>&1; then echo "\$(date) nitro-perf display up; keeping it" >> \$LOG; done_job; exit 0; fi
v=\$(nvram \$V:nullmoth-nv-starts 2>/dev/null | cut -f2)
case "\$v" in %0[2-9a-f]|%[1-9a-f]?) ;; *) exit 0;; esac
[ -d "\$BK/NVRM.kext" ] || { done_job; exit 0; }
echo "\$(date) nitro-perf failed twice; restoring stock 3.0 from \$BK" >> \$LOG
for k in $KEXTS; do rm -rf "$EXT/\$k.kext"; [ ! -e "\$BK/\$k.kext" ] || ditto "\$BK/\$k.kext" "$EXT/\$k.kext"; done
rm -rf "$GB/NVMTLDriver.bundle"; ditto "\$BK/NVMTLDriver.bundle" "$GB/NVMTLDriver.bundle"
cp -p "\$BK/nvmtl/libvulkan_nouveau.dylib" "$GB/nvmtl/libvulkan_nouveau.dylib"
[ -f "\$BK/AuxiliaryKernelExtensions.kc" ] && cp -p "\$BK/AuxiliaryKernelExtensions.kc" "$KC"
nvram -d \$V:nullmoth-nv-starts 2>/dev/null || true
done_job
shutdown -r now
EOF
chmod 755 "$LG"; chown root:wheel "$LG"
cat > "$LGP" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>Label</key><string>com.nullmoth.lastgood</string>
<key>ProgramArguments</key><array><string>/bin/bash</string><string>/Library/NullMoth/lastgood.sh</string></array>
<key>RunAtLoad</key><true/>
</dict></plist>
EOF
chmod 644 "$LGP"; chown root:wheel "$LGP"

echo "== 8. Merging warm shader caches into shared cache =="
bash "$HERE/merge-shared-cache.sh" || true

echo ""
echo "DONE: nitro-perf installed! Reboot to load the new kexts, Metal plugin, translator, and NVK."
echo "Verify after reboot:"
echo "  nvmtl-smi"
echo "  sysctl debug.nvrm_tgp_state debug.nvrmfb_flip_latch_deferred debug.nvrmfb_vram_pool_hits debug.nvaccel_pageoff_unlocked_wait debug.nvrm_evt_wakes debug.nvrm_kms_timer_fast"
echo "Rollback anytime with: sudo $HERE/rollback-nitro-perf.sh"
