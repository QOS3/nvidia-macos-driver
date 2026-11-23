#!/bin/bash
# nitro-perf-ctl.sh: live 144 Hz telemetry dashboard, power/boost mode switcher, and shared shader cache warmer
# for the Acer Nitro 5 nitro-perf v2 driver stack.
set -euo pipefail
PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin; export PATH

cmd="${1:-status}"

show_status() {
  echo "=== nitro-perf v2 Live Telemetry ==="
  echo "-- 144 Hz Micro-Stutter Profiler (NVRMFB) --"
  sysctl -n debug.nvrmfb_frame_stats 2>/dev/null || echo "(reboot into nitro-perf v2 to activate frame profiler)"
  echo "-- Display Flip & VRAM Recycle Pool --"
  sysctl debug.nvrmfb_flip_latch_deferred debug.nvrmfb_flip_latch_waits debug.nvrmfb_flip_latch_late \
         debug.nvrmfb_flip_us_sum debug.nvrmfb_flip_n debug.nvrmfb_flip_us_max \
         debug.nvrmfb_vram_pool_hits debug.nvrmfb_vram_pool_misses debug.nvrmfb_vram_pool_bytes 2>/dev/null || true
  echo "-- GPU Power, Dynamic Boost & Boost Daemon --"
  sysctl debug.nvrm_tgp_enable debug.nvrm_tgp_ac debug.nvrm_tgp_state debug.nvrm_tgp_last_status \
         debug.nvrm_kms_timer_fast debug.nvrm_evt_wakes debug.nvaccel_pageoff_unlocked_wait \
         debug.nvaccel_pageoff_pooled 2>/dev/null || true
  [ -x /Library/NullMoth/boost_daemon.sh ] && echo "nvmtl-boost: $(/Library/NullMoth/boost_daemon.sh status 2>/dev/null || echo unknown)"
  echo "-- Shared Shader & Ampere SASS Cache --"
  printf "aircache files: %s | spvcache files: %s | vkpcache.bin: %s\n" \
    "$(ls /Library/GPUBundles/nvmtl/aircache 2>/dev/null | wc -l | tr -d ' ')" \
    "$(ls /Library/GPUBundles/nvmtl/spvcache/*.c3.spv 2>/dev/null | wc -l | tr -d ' ')" \
    "$(ls -lh /Library/GPUBundles/nvmtl/spvcache/vkpcache.bin 2>/dev/null | awk '{print $5}' || echo missing)"
  echo "-- Memory Pressure (8 GB RAM) --"
  sysctl vm.swapusage 2>/dev/null || true
  memory_pressure -Q 2>/dev/null | tail -n 1 || true
}

case "$cmd" in
  status)
    show_status
    ;;
  perf)
    [ "$(id -u)" = 0 ] || { echo "Run with sudo: sudo nitro-perf-ctl perf" >&2; exit 2; }
    printf 'gpuboost=1\n' > /Library/NullMoth/gpuboost
    chmod 644 /Library/NullMoth/gpuboost
    launchctl kickstart -k system/com.nullmoth.nvmtl-boost 2>/dev/null || true
    echo "OK: nitro-perf switched to PERF mode (gpuboost=1, max 144 Hz boost)"
    ;;
  quiet)
    [ "$(id -u)" = 0 ] || { echo "Run with sudo: sudo nitro-perf-ctl quiet" >&2; exit 2; }
    printf 'gpuboost=0\n' > /Library/NullMoth/gpuboost
    chmod 644 /Library/NullMoth/gpuboost
    launchctl kickstart -k system/com.nullmoth.nvmtl-boost 2>/dev/null || true
    echo "OK: nitro-perf switched to QUIET mode (gpuboost=0, cool/low-power idle)"
    ;;
  warm-cache)
    [ "$(id -u)" = 0 ] || { echo "Run with sudo: sudo nitro-perf-ctl warm-cache" >&2; exit 2; }
    exec /Library/NullMoth/merge-shared-cache.sh
    ;;
  *)
    echo "Usage: nitro-perf-ctl [status | perf | quiet | warm-cache]"
    exit 1
    ;;
esac
