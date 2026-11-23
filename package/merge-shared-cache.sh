#!/bin/bash
# merge-shared-cache.sh: copies warm per-user/sandboxed aircache & spvcache entries into
# /Library/GPUBundles/nvmtl/{aircache,spvcache} (owned by _windowserver:wheel, 0644/0755)
# so sandboxed apps & Settings panes hit warm shaders on first use.
set -euo pipefail
PATH=/usr/bin:/bin:/usr/sbin:/sbin; export PATH
[ "$(id -u)" = 0 ] || { echo "STOP: run with sudo" >&2; exit 2; }

SHARED_AIR=/Library/GPUBundles/nvmtl/aircache
SHARED_SPV=/Library/GPUBundles/nvmtl/spvcache
mkdir -p "$SHARED_AIR" "$SHARED_SPV"

copied=0
merge_dir() {
  local src=$1 dst=$2
  [ -d "$src" ] || return 0
  while IFS= read -r -d '' f; do
    local base
    base=$(basename "$f")
    case "$base" in *.tmp*|.DS_Store|vkpcache.bin) continue ;; esac
    [ -e "$dst/$base" ] && continue
    cp -p "$f" "$dst/$base.tmp.$$" && mv -n "$dst/$base.tmp.$$" "$dst/$base" && copied=$((copied + 1)) || rm -f "$dst/$base.tmp.$$"
  done < <(find "$src" -maxdepth 1 -type f -size +0c -print0 2>/dev/null)
}

for uhome in /Users/*; do
  [ -d "$uhome/Library/Caches/nvmtl" ] || continue
  merge_dir "$uhome/Library/Caches/nvmtl/aircache" "$SHARED_AIR"
  merge_dir "$uhome/Library/Caches/nvmtl/spvcache" "$SHARED_SPV"
done

chown -R _windowserver:wheel "$SHARED_AIR" "$SHARED_SPV"
chmod 0755 "$SHARED_AIR" "$SHARED_SPV"
find "$SHARED_AIR" "$SHARED_SPV" -type f -exec chmod 0644 {} + 2>/dev/null || true
echo "OK: merged $copied new shader cache file(s) into shared cache"
