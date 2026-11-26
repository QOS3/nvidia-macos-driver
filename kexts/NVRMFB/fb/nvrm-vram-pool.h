/*
 * NullMoth NVIDIA driver for macOS — nitro-perf v11 VRAM recycle pool policy
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * Header-free pure C/C++ policy shared by NVRMFB (kexts/NVRMFB/fb/nvrm-fb.cpp,
 * built with -mkernel -nostdinc) and the host unit test (tools/test_vram_pool.c).
 *
 * On >=4 GiB Resizable BAR, desktop apps (Preview/PDFKit, browsers, Electron)
 * churn variable-size 64 KiB..32 MiB IOSurfaces. Quantizing new allocations to
 * 4-steps-per-octave buckets (<=12.5% slack) and reusing pooled blocks via
 * bounded best-fit (<=25% slack, oldest-first tie-break) raises pool hit rates
 * from ~68% to >95%, avoiding RM/GSP allocate/map/unmap/free RPC storms.
 */
#ifndef NVRM_VRAM_POOL_H
#define NVRM_VRAM_POOL_H

#define NVRM_VRAM_POOL_MAX_SLOTS    512
#define NVRM_VRAM_POOL_MAX_BYTES_4G (256ull * 1024ull * 1024ull)
#define NVRM_VRAM_POOL_MAX_BYTES    (384ull * 1024ull * 1024ull)
#define NVRM_VRAM_POOL_MAX_ENTRY    (32ull * 1024ull * 1024ull)
#define NVRM_VRAM_POOL_COOLDOWN_NS  (10ull * 1000ull * 1000ull)
#define NVRM_VRAM_FREE_Q_CAP        512u

/*
 * Quantize a 64 KiB-aligned request size upward into a 4-steps-per-octave
 * reusable bucket (32 total buckets across 64 KiB .. 32 MiB, <=12.5% overhead).
 * Requests above NVRM_VRAM_POOL_MAX_ENTRY keep their exact 64 KiB-aligned size.
 */
static inline unsigned long long nvrmVramPoolBucket(unsigned long long want)
{
    if (!want || want > NVRM_VRAM_POOL_MAX_ENTRY) return want;
    unsigned long long step = 64ull << 10;
    if (want <= (512ull << 10))      step = 64ull << 10;
    else if (want <= (1ull << 20))   step = 128ull << 10;
    else if (want <= (2ull << 20))   step = 256ull << 10;
    else if (want <= (4ull << 20))   step = 512ull << 10;
    else if (want <= (8ull << 20))   step = 1ull << 20;
    else if (want <= (16ull << 20))  step = 2ull << 20;
    else                             step = 4ull << 20;
    unsigned long long rounded = (want + (step - 1ull)) & ~(step - 1ull);
    return rounded <= NVRM_VRAM_POOL_MAX_ENTRY ? rounded : NVRM_VRAM_POOL_MAX_ENTRY;
}

/*
 * Maximum pooled block size allowed to satisfy a request of `want` bytes:
 * at most 25% slack, with a 64 KiB floor so tiny allocations can still reuse
 * an adjacent granule without wasting large blocks.
 */
static inline unsigned long long nvrmVramPoolMaxFit(unsigned long long want)
{
    unsigned long long slack = want >> 2;
    if (slack < (64ull << 10)) slack = (64ull << 10);
    if (want > (~0ull) - slack) return ~0ull;
    return want + slack;
}

static inline int nvrmVramPoolFits(unsigned long long entrySize, unsigned long long want)
{
    return want > 0 && entrySize >= want && entrySize <= nvrmVramPoolMaxFit(want);
}

typedef struct NVRMVramPoolSlotMeta {
    const void *dev;
    unsigned long long size;
    unsigned long long releasedAbs;
} NVRMVramPoolSlotMeta;

/*
 * Pick the best-fitting cooled slot for `want` on `dev`.
 * - Rejects entries whose size is outside [want, nvrmVramPoolMaxFit(want)].
 * - Skips entries still within `cooldownAbs` of `releasedAbs` and counts them.
 * - Prefers the smallest fitting entry; ties keep the lowest index (oldest).
 * - Breaks immediately on the first cooled exact match (entry.size == want).
 */
static inline int nvrmVramPoolPickBestFit(const NVRMVramPoolSlotMeta *slots, int n,
                                          const void *dev, unsigned long long want,
                                          unsigned long long nowAbs, unsigned long long cooldownAbs,
                                          int *cooldownSkipsOut)
{
    int best = -1;
    unsigned long long bestSize = ~0ull;
    int skips = 0;
    for (int i = 0; i < n; i++) {
        if (slots[i].dev != dev) continue;
        const unsigned long long sz = slots[i].size;
        if (!nvrmVramPoolFits(sz, want)) continue;
        if (nowAbs >= slots[i].releasedAbs && (nowAbs - slots[i].releasedAbs) < cooldownAbs) {
            skips++;
            continue;
        }
        if (best < 0 || sz < bestSize) {
            best = i;
            bestSize = sz;
            if (bestSize == want) break;
        }
    }
    if (cooldownSkipsOut) *cooldownSkipsOut = skips;
    return best;
}

#endif
