#!/bin/bash
# merge-shared-cache.sh: prunes stale translator-build spvcache files and merges warm aircache,
# current-build spvcache (.c3.spv), and compiled Ampere SASS VkPipelineCache (vkpcache.bin) from
# user caches, sandboxed app Containers, and /private/var/folders (including WindowServer) into
# /Library/GPUBundles/nvmtl/{aircache,spvcache} (owned by _windowserver:wheel, 0644/0755)
# so sandboxed apps, System Settings panes, and Dock extensions hit warm shaders on first launch.
set -euo pipefail
PATH=/usr/bin:/bin:/usr/sbin:/sbin; export PATH
[ "$(id -u)" = 0 ] || { echo "STOP: run with sudo" >&2; exit 2; }

SHARED_AIR=/Library/GPUBundles/nvmtl/aircache
SHARED_SPV=/Library/GPUBundles/nvmtl/spvcache
XLATE=/Library/GPUBundles/NVMTLDriver.bundle/Contents/MacOS/libnvmtl_translate.dylib
mkdir -p "$SHARED_AIR" "$SHARED_SPV"

CUR_TAG=""
if [ -f "$XLATE" ]; then
  CUR_TAG="sha256-$(shasum -a 256 "$XLATE" | awk '{print $1}')"
fi

pruned=0
if [ -n "$CUR_TAG" ]; then
  while IFS= read -r -d '' f; do
    base=${f##*/}
    case "$base" in
      *"$CUR_TAG"*.c3.spv) ;;
      *.c3.spv) rm -f "$f" && pruned=$((pruned + 1)) ;;
    esac
  done < <(find "$SHARED_SPV" -maxdepth 1 -type f -name '*.c3.spv' -print0 2>/dev/null)
fi

copied_air=0
copied_spv=0
merge_dir() {
  local src=$1 dst=$2 mode=$3
  [ -d "$src" ] || return 0
  [ "$src" = "$dst" ] && return 0
  while IFS= read -r -d '' f; do
    local base=${f##*/}
    case "$base" in *.tmp*|.DS_Store|vkpcache.bin) continue ;; esac
    if [ "$mode" = "spv" ] && [ -n "$CUR_TAG" ]; then
      case "$base" in *"$CUR_TAG"*.c3.spv) ;; *) continue ;; esac
    fi
    [ -e "$dst/$base" ] && continue
    if cp -p "$f" "$dst/$base.tmp.$$" && mv -n "$dst/$base.tmp.$$" "$dst/$base"; then
      if [ "$mode" = "air" ]; then copied_air=$((copied_air + 1)); else copied_spv=$((copied_spv + 1)); fi
    else
      rm -f "$dst/$base.tmp.$$"
    fi
  done < <(find "$src" -maxdepth 1 -type f -size +0c -print0 2>/dev/null)
}

scan_root() {
  local root=$1
  [ -d "$root" ] || return 0
  merge_dir "$root/aircache" "$SHARED_AIR" air
  merge_dir "$root/spvcache" "$SHARED_SPV" spv
}

for uhome in /Users/*; do
  scan_root "$uhome/Library/Caches/nvmtl"
  for c in "$uhome"/Library/Containers/*/Data/Library/Caches/nvmtl; do
    scan_root "$c"
  done
done

for d in /private/var/folders/*/*/C/nvmtl /private/var/folders/*/*/C/*/nvmtl /private/var/folders/*/*/T/nvmtl; do
  scan_root "$d"
done

# Seed shared compiled SASS VkPipelineCache (vkpcache.bin) from the richest CoreAnimation/UI cache
# (WindowServer or user cache dir) so newly spawned sandboxed apps & Settings panes load warm SASS immediately.
best=""
best_sz=0
for f in /private/var/folders/*/*/C/WindowServer/nvmtl/spvcache/vkpcache.bin \
         /private/var/folders/*/*/C/nvmtl/spvcache/vkpcache.bin; do
  [ -f "$f" ] || continue
  sz=$(stat -f%z "$f" 2>/dev/null || echo 0)
  if [ "$sz" -gt "$best_sz" ]; then best="$f"; best_sz="$sz"; fi
done
if [ -n "$best" ]; then
  cur_sz=$(stat -f%z "$SHARED_SPV/vkpcache.bin" 2>/dev/null || echo 0)
  if [ "$best_sz" -ge "$cur_sz" ]; then
    cp -p "$best" "$SHARED_SPV/vkpcache.bin.tmp.$$" && mv -f "$SHARED_SPV/vkpcache.bin.tmp.$$" "$SHARED_SPV/vkpcache.bin"
    echo "OK: seeded $SHARED_SPV/vkpcache.bin from $best ($((best_sz / 1024)) KB)"
  fi
fi

chown -R _windowserver:wheel "$SHARED_AIR" "$SHARED_SPV"
chmod 0755 "$SHARED_AIR" "$SHARED_SPV"
find "$SHARED_AIR" "$SHARED_SPV" -type f -exec chmod 0644 {} + 2>/dev/null || true
echo "OK: pruned $pruned stale spvcache file(s), merged $copied_air aircache + $copied_spv current-build spvcache file(s)"
