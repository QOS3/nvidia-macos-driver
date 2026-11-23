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
  XLATE_HEX=$(shasum -a 256 "$XLATE" | awk '{print $1}')
  CUR_TAG="sha256-$XLATE_HEX"
  # Write root-owned .sha256 sidecar matched by (size, mtime_sec, mtime_nsec) so NVMTLDriver skips hashing 15.3 MB per app launch.
  PY_BIN=""
  for p in /usr/local/bin/python3 /usr/bin/python3; do [ -x "$p" ] && PY_BIN="$p" && break; done
  if [ -n "$PY_BIN" ]; then
    "$PY_BIN" -c '
import os, sys
p, hx = sys.argv[1], sys.argv[2]
st = os.stat(p)
ns = getattr(st, "st_mtime_ns", int(st.st_mtime * 1e9))
sec, nsec = divmod(ns, 1_000_000_000)
tmp = p + ".sha256.tmp"
with open(tmp, "w") as f: f.write(f"{st.st_size} {sec} {nsec} {hx}\n")
os.chmod(tmp, 0o644)
os.chown(tmp, 0, 0)
os.replace(tmp, p + ".sha256")
' "$XLATE" "$XLATE_HEX" 2>/dev/null || true
  fi
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

# Merge all live-UUID VkPipelineCache files (WindowServer + user + sandboxed apps) using Vulkan vkMergePipelineCaches
# after initializing Metal (so NVMTLDriver loads nvrm610.conf and reports the live pipelineCacheUUID), and purge stale UUIDs.
PY_BIN=""
for p in /usr/local/bin/python3 /usr/bin/python3; do [ -x "$p" ] && PY_BIN="$p" && break; done
if [ -n "$PY_BIN" ] && [ -f /Library/GPUBundles/nvmtl/libvulkan_nouveau.dylib ]; then
  "$PY_BIN" - <<'PYEOF' || true
import ctypes, os, sys

try:
    metal = ctypes.CDLL("/System/Library/Frameworks/Metal.framework/Metal")
    metal.MTLCreateSystemDefaultDevice.restype = ctypes.c_void_p
    metal.MTLCreateSystemDefaultDevice()
    icd = ctypes.CDLL("/Library/GPUBundles/nvmtl/libvulkan_nouveau.dylib")
    gipa = icd.vk_icdGetInstanceProcAddr
    gipa.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    gipa.restype = ctypes.c_void_p

    class AppInfo(ctypes.Structure):
        _fields_ = [("sType", ctypes.c_uint32), ("pNext", ctypes.c_void_p), ("pApp", ctypes.c_char_p), ("appV", ctypes.c_uint32), ("pEng", ctypes.c_char_p), ("engV", ctypes.c_uint32), ("apiV", ctypes.c_uint32)]
    class InstCI(ctypes.Structure):
        _fields_ = [("sType", ctypes.c_uint32), ("pNext", ctypes.c_void_p), ("flags", ctypes.c_uint32), ("pApp", ctypes.POINTER(AppInfo)), ("elc", ctypes.c_uint32), ("peln", ctypes.c_void_p), ("eec", ctypes.c_uint32), ("peen", ctypes.c_void_p)]
    class QCI(ctypes.Structure):
        _fields_ = [("sType", ctypes.c_uint32), ("pNext", ctypes.c_void_p), ("flags", ctypes.c_uint32), ("qfi", ctypes.c_uint32), ("qc", ctypes.c_uint32), ("pqp", ctypes.POINTER(ctypes.c_float))]
    class DevCI(ctypes.Structure):
        _fields_ = [("sType", ctypes.c_uint32), ("pNext", ctypes.c_void_p), ("flags", ctypes.c_uint32), ("qcic", ctypes.c_uint32), ("pqci", ctypes.POINTER(QCI)), ("elc", ctypes.c_uint32), ("peln", ctypes.c_void_p), ("eec", ctypes.c_uint32), ("peen", ctypes.c_void_p), ("pef", ctypes.c_void_p)]
    class PCCI(ctypes.Structure):
        _fields_ = [("sType", ctypes.c_uint32), ("pNext", ctypes.c_void_p), ("flags", ctypes.c_uint32), ("sz", ctypes.c_size_t), ("p", ctypes.c_void_p)]

    fn = lambda inst, n, rt, at: ctypes.CFUNCTYPE(rt, *at)(gipa(inst, n.encode()))
    app = AppInfo(0, None, b"merge-shared-cache", 1, b"nvmtl", 1, (1<<22)|(3<<12))
    ici = InstCI(1, None, 0, ctypes.pointer(app), 0, None, 0, None)
    inst = ctypes.c_void_p()
    if fn(None, "vkCreateInstance", ctypes.c_int32, [ctypes.POINTER(InstCI), ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)])(ctypes.byref(ici), None, ctypes.byref(inst)) != 0:
        sys.exit(0)
    cnt = ctypes.c_uint32(0)
    enum_pd = fn(inst, "vkEnumeratePhysicalDevices", ctypes.c_int32, [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(ctypes.c_void_p)])
    enum_pd(inst, ctypes.byref(cnt), None)
    if cnt.value == 0: sys.exit(0)
    pdevs = (ctypes.c_void_p * cnt.value)()
    enum_pd(inst, ctypes.byref(cnt), pdevs)
    props = ctypes.create_string_buffer(1024)
    fn(inst, "vkGetPhysicalDeviceProperties", None, [ctypes.c_void_p, ctypes.c_void_p])(pdevs[0], props)
    live_uuid = props.raw[276:292]

    prio = ctypes.c_float(1.0)
    qci = QCI(2, None, 0, 0, 1, ctypes.pointer(prio))
    dci = DevCI(3, None, 0, 1, ctypes.pointer(qci), 0, None, 0, None, None)
    dev = ctypes.c_void_p()
    if fn(inst, "vkCreateDevice", ctypes.c_int32, [ctypes.c_void_p, ctypes.POINTER(DevCI), ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)])(pdevs[0], ctypes.byref(dci), None, ctypes.byref(dev)) != 0:
        sys.exit(0)
    gdpa = fn(inst, "vkGetDeviceProcAddr", ctypes.c_void_p, [ctypes.c_void_p, ctypes.c_char_p])
    dfn = lambda n, rt, at: ctypes.CFUNCTYPE(rt, *at)(gdpa(dev, n.encode()))
    c_pc = dfn("vkCreatePipelineCache", ctypes.c_int32, [ctypes.c_void_p, ctypes.POINTER(PCCI), ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint64)])
    m_pc = dfn("vkMergePipelineCaches", ctypes.c_int32, [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint64)])
    g_pc = dfn("vkGetPipelineCacheData", ctypes.c_int32, [ctypes.c_void_p, ctypes.c_uint64, ctypes.POINTER(ctypes.c_size_t), ctypes.c_void_p])

    shared = "/Library/GPUBundles/nvmtl/spvcache/vkpcache.bin"
    roots = ["/Library/GPUBundles/nvmtl/spvcache", "/private/var/folders"]
    for u in os.listdir("/Users"):
        uh = os.path.join("/Users", u)
        if os.path.isdir(uh):
            roots.extend([os.path.join(uh, "Library/Caches/nvmtl"), os.path.join(uh, "Library/Containers")])
    valid, stale, seen = [], [], set()
    for r in roots:
        if not os.path.exists(r): continue
        for dp, _, fns in os.walk(r):
            if "vkpcache.bin" not in fns: continue
            fp = os.path.join(dp, "vkpcache.bin")
            if fp in seen: continue
            seen.add(fp)
            try: d = open(fp, "rb").read()
            except Exception: continue
            if len(d) < 32: continue
            if d[16:32] == live_uuid: valid.append((fp, d))
            else: stale.append(fp)

    if valid:
        dst = ctypes.c_uint64(0)
        if c_pc(dev, ctypes.byref(PCCI(17, None, 0, 0, None)), None, ctypes.byref(dst)) == 0:
            srcs = []
            for _, d in valid:
                buf = ctypes.create_string_buffer(d, len(d))
                h = ctypes.c_uint64(0)
                if c_pc(dev, ctypes.byref(PCCI(17, None, 0, len(d), ctypes.cast(buf, ctypes.c_void_p))), None, ctypes.byref(h)) == 0:
                    srcs.append(h.value)
            if srcs and m_pc(dev, dst, len(srcs), (ctypes.c_uint64 * len(srcs))(*srcs)) == 0:
                sz = ctypes.c_size_t(0)
                if g_pc(dev, dst, ctypes.byref(sz), None) == 0 and sz.value >= 32:
                    ob = ctypes.create_string_buffer(sz.value)
                    if g_pc(dev, dst, ctypes.byref(sz), ob) == 0:
                        tmp = shared + ".tmp.merge"
                        with open(tmp, "wb") as f: f.write(ob.raw[:sz.value])
                        os.chmod(tmp, 0o644)
                        os.replace(tmp, shared)
                        print(f"OK: merged {len(srcs)} valid vkpcache.bin -> {sz.value // 1024} KB (uuid={live_uuid.hex()[:12]})")
    purged = 0
    for fp in stale:
        if fp == shared: continue
        try: os.remove(fp); purged += 1
        except Exception: pass
    if purged: print(f"OK: purged {purged} stale-UUID vkpcache.bin file(s)")
except Exception as e:
    print(f"WARN: vkpcache merge skipped: {e}", file=sys.stderr)
PYEOF
fi

chown -R _windowserver:wheel "$SHARED_AIR" "$SHARED_SPV"
chmod 0755 "$SHARED_AIR" "$SHARED_SPV"
find "$SHARED_AIR" "$SHARED_SPV" -type f -exec chmod 0644 {} + 2>/dev/null || true
echo "OK: pruned $pruned stale spvcache file(s), merged $copied_air aircache + $copied_spv current-build spvcache file(s)"
