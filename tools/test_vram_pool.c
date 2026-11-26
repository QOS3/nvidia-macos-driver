/*
 * Host unit test for nitro-perf v11 NVRMFB VRAM recycle pool policy
 * (kexts/NVRMFB/fb/nvrm-vram-pool.h): bucket quantization, bounded best-fit
 * matching, cooldown skipping, and variable-size PDF/surface-churn replay.
 *
 * cc -O2 -Wall -Wextra -Werror tools/test_vram_pool.c -o /tmp/test_vram_pool && /tmp/test_vram_pool
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../kexts/NVRMFB/fb/nvrm-vram-pool.h"

static int gFails = 0;
#define CHECK(name, cond) do { \
    int ok_ = !!(cond); \
    printf("%s %s\n", ok_ ? "ok  " : "FAIL", name); \
    if (!ok_) gFails++; \
} while (0)

static uint32_t lcg(uint32_t *s)
{
    *s = *s * 1664525u + 1013904223u;
    return *s >> 8;
}

/* Replay variable-size PDF open/scroll/zoom surface churn under v8 (exact
 * 128-slot/160 MiB/16 MiB) vs v11 (bucketed + 25% best-fit 512-slot/256 MiB/32 MiB). */
static void replay_pdf_workload(int v11, int *hitsOut, int *missesOut, int *evictsOut)
{
    const int maxSlots = v11 ? NVRM_VRAM_POOL_MAX_SLOTS : 128;
    const unsigned long long maxBytes = v11 ? NVRM_VRAM_POOL_MAX_BYTES_4G : (160ull << 20);
    const unsigned long long maxEntry = v11 ? NVRM_VRAM_POOL_MAX_ENTRY : (16ull << 20);
    const unsigned long long cooldown = 10ull; /* abstract ticks */
    const void *dev = (const void *)0x1;

    NVRMVramPoolSlotMeta pool[NVRM_VRAM_POOL_MAX_SLOTS];
    int poolN = 0;
    unsigned long long poolBytes = 0;

    unsigned long long live[128];
    int liveN = 0;

    int hits = 0, misses = 0, evicts = 0;
    uint32_t seed = 1337u;
    unsigned long long now = 100ull;

    for (int step = 0; step < 8000; step++) {
        now += 2ull;
        int allocOp = (liveN < 24) || (liveN < 96 && (lcg(&seed) % 100u) < 52u);
        if (allocOp) {
            uint32_t r = lcg(&seed) % 100u;
            unsigned long long want;
            if (r < 40u)      want = (unsigned long long)((lcg(&seed) % 8u) + 1u) << 16;    /* 64K..512K UI/glyph tiles */
            else if (r < 85u) want = (unsigned long long)((lcg(&seed) % 71u) + 10u) << 16;  /* 640K..5M PDF page tiles */
            else if (r < 95u) want = (unsigned long long)((lcg(&seed) % 21u) + 120u) << 16; /* 7.5M..8.75M window surfaces */
            else              want = (unsigned long long)((lcg(&seed) % 97u) + 272u) << 16; /* 17M..23M 2x Retina PDF pages */

            if (want <= maxEntry) {
                int hit = -1;
                if (v11) {
                    hit = nvrmVramPoolPickBestFit(pool, poolN, dev, want, now, cooldown, NULL);
                } else {
                    for (int i = 0; i < poolN; i++) {
                        if (pool[i].size != want) continue;
                        if (now >= pool[i].releasedAbs && (now - pool[i].releasedAbs) < cooldown) continue;
                        hit = i;
                        break;
                    }
                }
                if (hit >= 0) {
                    unsigned long long got = pool[hit].size;
                    memmove(&pool[hit], &pool[hit + 1], (size_t)(poolN - hit - 1) * sizeof(pool[0]));
                    poolN--;
                    poolBytes -= got;
                    live[liveN++] = got;
                    hits++;
                    continue;
                }
            }
            misses++;
            live[liveN++] = (v11 && want <= maxEntry) ? nvrmVramPoolBucket(want) : want;
        } else {
            int k = (int)(lcg(&seed) % (uint32_t)liveN);
            unsigned long long sz = live[k];
            live[k] = live[--liveN];
            if (sz <= maxEntry) {
                while (poolN >= maxSlots || (poolN > 0 && poolBytes + sz > maxBytes)) {
                    poolBytes -= pool[0].size;
                    memmove(&pool[0], &pool[1], (size_t)(poolN - 1) * sizeof(pool[0]));
                    poolN--;
                    evicts++;
                }
                if (poolN < maxSlots && poolBytes + sz <= maxBytes) {
                    pool[poolN].dev = dev;
                    pool[poolN].size = sz;
                    pool[poolN].releasedAbs = now;
                    poolN++;
                    poolBytes += sz;
                } else {
                    evicts++;
                }
            } else {
                evicts++;
            }
        }
    }
    *hitsOut = hits;
    *missesOut = misses;
    *evictsOut = evicts;
}

int main(void)
{
    const unsigned long long K = 1024ull;
    const unsigned long long M = 1024ull * 1024ull;

    /* 1. Bucket quantization properties */
    CHECK("bucket: 64K..512K stay 64K-exact",
          nvrmVramPoolBucket(64 * K) == 64 * K &&
          nvrmVramPoolBucket(192 * K) == 192 * K &&
          nvrmVramPoolBucket(512 * K) == 512 * K);
    CHECK("bucket: 1080p 8323072 -> exact 8 MiB", nvrmVramPoolBucket(8323072ull) == 8 * M);
    CHECK("bucket: exact powers/multiples stay exact",
          nvrmVramPoolBucket(2 * M) == 2 * M &&
          nvrmVramPoolBucket(4 * M) == 4 * M &&
          nvrmVramPoolBucket(12 * M) == 12 * M &&
          nvrmVramPoolBucket(16 * M) == 16 * M &&
          nvrmVramPoolBucket(32 * M) == 32 * M);
    CHECK("bucket: nearby PDF tiles coalesce into one bucket",
          nvrmVramPoolBucket(1179648ull) == nvrmVramPoolBucket(1245184ull));
    CHECK("bucket: >32 MiB passes through unchanged",
          nvrmVramPoolBucket(36 * M) == 36 * M && nvrmVramPoolBucket(64 * M) == 64 * M);

    int bucketOk = 1;
    unsigned long long prev = 0;
    for (unsigned long long sz = 64 * K; sz <= 32 * M; sz += 64 * K) {
        unsigned long long b = nvrmVramPoolBucket(sz);
        if (b < sz || b < prev || (b & 0xFFFFull) != 0 || b > 32 * M) { bucketOk = 0; break; }
        /* Overhead strictly <= 12.5% (1/8) for every 64 KiB-aligned size in 64K..32M */
        if (sz >= 512 * K && (b - sz) * 8ull > sz) { bucketOk = 0; break; }
        prev = b;
    }
    CHECK("bucket: monotonic, 64K-aligned, <=12.5% overhead across 64K..32M", bucketOk);

    /* 2. Best-fit bounds */
    CHECK("fits: exact always fits", nvrmVramPoolFits(4 * M, 4 * M));
    CHECK("fits: undersized never fits", !nvrmVramPoolFits(4 * M - 64 * K, 4 * M));
    CHECK("fits: <=25% slack fits, >25% rejected",
          nvrmVramPoolFits(5 * M, 4 * M) && !nvrmVramPoolFits(5 * M + 64 * K, 4 * M));
    CHECK("fits: 64K floor for tiny blocks without wasting huge blocks",
          nvrmVramPoolFits(128 * K, 64 * K) && !nvrmVramPoolFits(192 * K, 64 * K));
    CHECK("fits: uint64 overflow-safe", nvrmVramPoolMaxFit(~0ull - 1024ull) == ~0ull);

    /* 3. Best-fit picker logic */
    const void *devA = (const void *)0x10;
    const void *devB = (const void *)0x20;
    NVRMVramPoolSlotMeta slots[6] = {
        { devB, 4 * M,           10 }, /* wrong dev */
        { devA, 5 * M,           10 }, /* fits (+25%), older */
        { devA, 4 * M,           95 }, /* exact, cooling down */
        { devA, 4 * M + 512 * K, 20 }, /* tighter fit (+12.5%), cooled */
        { devA, 4 * M + 512 * K, 30 }, /* same tighter fit, newer */
        { devA, 8 * M,           10 }, /* too big (>25%) */
    };
    int skips = 0;
    int idx = nvrmVramPoolPickBestFit(slots, 6, devA, 4 * M, 100, 10, &skips);
    CHECK("picker: skips cooling exact match, ignores wrong dev/oversized, picks oldest tightest best-fit",
          idx == 3 && skips == 1);

    /* Once slot 2 cools down, exact match must win immediately */
    skips = 0;
    idx = nvrmVramPoolPickBestFit(slots, 6, devA, 4 * M, 106, 10, &skips);
    CHECK("picker: cooled exact match wins over larger best-fit", idx == 2 && skips == 0);

    /* 4. Variable-size PDF/surface-churn workload comparison */
    int h8 = 0, m8 = 0, e8 = 0;
    int h11 = 0, m11 = 0, e11 = 0;
    replay_pdf_workload(0, &h8, &m8, &e8);
    replay_pdf_workload(1, &h11, &m11, &e11);
    double r8 = (double)h8 * 100.0 / (double)(h8 + m8);
    double r11 = (double)h11 * 100.0 / (double)(h11 + m11);
    printf("PDF churn replay: v8 %.1f%% hits (%d hits / %d misses / %d evicts) -> v11 %.1f%% hits (%d hits / %d misses / %d evicts)\n",
           r8, h8, m8, e8, r11, h11, m11, e11);
    CHECK("PDF replay: v11 hit rate >= 85% and beats v8 by >= 30 points",
          r11 >= 85.0 && (r11 - r8) >= 30.0);
    CHECK("PDF replay: v11 cuts NVKMS evictions by >= 75%", e11 * 100 <= e8 * 25);

    if (gFails) {
        printf("FAILED (%d)\n", gFails);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
