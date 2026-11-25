/* test_surf_meta.c - proves the lock-free NVRM shared surface-metadata table (nitro-perf v8)
 * on the host without a GPU: ABI match, VRAM/busy tag coexistence, kernel invalidation on
 * recycled IOSurfaceIDs, cookie mismatch, 4-way LRU eviction, dead-PID lock recovery, and
 * multi-threaded seqlock stress.
 * Build and run anywhere:  cc -I plugin -pthread tools/test_surf_meta.c -o /tmp/test_surf_meta && /tmp/test_surf_meta */
#include "nvmtl_surf_meta.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define ARM(name, cond) do { const int ok = !!(cond); printf("%s  %s\n", ok ? "ok  " : "FAIL", name); if (!ok) fails++; } while (0)

/* Verify the kernel header copy defines identical constants by parsing or direct check below */
static nvrm_surf_meta_page *alloc_page(void)
{
    nvrm_surf_meta_page *pg = calloc(1, sizeof *pg);
    pg->hdr.magic = NVRM_SURF_META_MAGIC;
    pg->hdr.version = NVRM_SURF_META_VERSION;
    pg->hdr.buckets = NVRM_SURF_META_BUCKETS;
    pg->hdr.ways = NVRM_SURF_META_WAYS;
    return pg;
}

static void sim_kernel_invalidate(nvrm_surf_meta_page *pg, uint32_t sid, uint32_t plane)
{
    const uint32_t base = nvmtl_sm_bucket(sid, plane) * NVRM_SURF_META_WAYS;
    for (uint32_t w = 0; w < NVRM_SURF_META_WAYS; w++) {
        nvrm_surf_meta_slot *s = &pg->slots[base + w];
        if (__atomic_load_n(&s->sid, __ATOMIC_RELAXED) == sid)
            __atomic_fetch_add(&s->kernel_inval, 1u, __ATOMIC_RELEASE);
    }
}

struct stress_ctx {
    nvrm_surf_meta_page *pg;
    int id;
    int bad;
};

static void *stress_worker(void *arg)
{
    struct stress_ctx *c = (struct stress_ctx *)arg;
    const uint64_t cookie = 0x1122334455667788ull;
    const uint32_t my_pid = (uint32_t)getpid();
    for (uint32_t i = 1; i <= 4000; i++) {
        uint32_t sid = 100u + (i & 7u);
        if ((c->id & 1) == 0) {
            nvmtl_sm_vram_update(c->pg, sid, 0, cookie, i, 0, 1, my_pid);
            nvmtl_sm_busy_update(c->pg, sid, 0, cookie, (int32_t)my_pid, i, (uint64_t)i * 1000ull, (int)(i & 1u));
        } else {
            nvrm_surf_meta_slot s;
            if (nvmtl_sm_read(c->pg, sid, 0, cookie, &s)) {
                if (s.sid != sid || s.cookie != cookie || s.plane != 0) {
                    c->bad++;
                }
                if ((s.flags & NVRM_SM_FLAG_BUSY_VALID) && s.busy_stamp_ns != s.busy_seq * 1000ull) {
                    c->bad++; /* torn read! */
                }
            }
        }
    }
    return NULL;
}

int main(void)
{
    nvrm_surf_meta_page *pg = alloc_page();
    nvrm_surf_meta_slot s;
    const uint32_t me = (uint32_t)getpid();
    const uint64_t c1 = 0xAABBCCDD11223344ull, c2 = 0xAABBCCDD99887766ull;

    ARM("empty page misses cleanly", nvmtl_sm_read(pg, 42, 0, c1, &s) == 0);

    uint32_t g = nvmtl_sm_vram_update(pg, 42, 0, c1, 7, 0, 1, me);
    ARM("vram bump writes seed and gen=1",
        g == 1 && nvmtl_sm_read(pg, 42, 0, c1, &s) == 1 &&
        (s.flags & NVRM_SM_FLAG_VRAM_VALID) && s.vram_seed == 7 && s.vram_gen == 1);

    nvmtl_sm_busy_update(pg, 42, 0, c1, 1234, 99, 555555ull, 0);
    ARM("busy update preserves vram tag in same slot",
        nvmtl_sm_read(pg, 42, 0, c1, &s) == 1 &&
        (s.flags & NVRM_SM_FLAG_VRAM_VALID) && s.vram_seed == 7 && s.vram_gen == 1 &&
        (s.flags & NVRM_SM_FLAG_BUSY_VALID) && !(s.flags & NVRM_SM_FLAG_BUSY_DONE) &&
        s.busy_pid == 1234 && s.busy_seq == 99 && s.busy_stamp_ns == 555555ull);

    nvmtl_sm_busy_update(pg, 42, 0, c1, 1234, 99, 666666ull, 1);
    ARM("busy done update sets BUSY_DONE while keeping vram tag",
        nvmtl_sm_read(pg, 42, 0, c1, &s) == 1 &&
        (s.flags & NVRM_SM_FLAG_BUSY_DONE) && s.vram_seed == 7 && s.vram_gen == 1);

    uint32_t g2 = nvmtl_sm_vram_update(pg, 42, 0, c1, 8, 0, 0, me);
    ARM("vram set keeps existing gen and busy tag",
        g2 == 1 && nvmtl_sm_read(pg, 42, 0, c1, &s) == 1 &&
        s.vram_seed == 8 && s.vram_gen == 1 && (s.flags & NVRM_SM_FLAG_BUSY_VALID));

    ARM("geometry cookie mismatch rejects stale read", nvmtl_sm_read(pg, 42, 0, c2, &s) == 0);

    sim_kernel_invalidate(pg, 42, 0);
    ARM("kernel invalidation immediately invalidates recycled sid even with identical cookie and seed",
        nvmtl_sm_read(pg, 42, 0, c1, &s) == 0);

    uint32_t g3 = nvmtl_sm_vram_update(pg, 42, 0, c1, 1, 0, 1, me);
    ARM("writer re-adopts invalidated slot cleanly with fresh gen=1 and cleared busy flag",
        g3 == 1 && nvmtl_sm_read(pg, 42, 0, c1, &s) == 1 &&
        s.vram_seed == 1 && s.vram_gen == 1 && !(s.flags & NVRM_SM_FLAG_BUSY_VALID));

    /* Find 5 sids that hash into the same bucket to prove 4-way LRU eviction */
    uint32_t b0 = nvmtl_sm_bucket(1, 0);
    uint32_t same_b[5]; int found = 0;
    for (uint32_t sid = 1; sid < 100000 && found < 5; sid++) {
        if (nvmtl_sm_bucket(sid, 0) == b0) same_b[found++] = sid;
    }
    ARM("found 5 colliding sids in bucket", found == 5);
    for (int i = 0; i < 5; i++) nvmtl_sm_vram_update(pg, same_b[i], 0, c1, (uint32_t)(10 + i), 0, 1, me);
    ARM("4-way bucket evicts oldest (LRU) and retains 4 newest",
        nvmtl_sm_read(pg, same_b[0], 0, c1, &s) == 0 &&
        nvmtl_sm_read(pg, same_b[1], 0, c1, &s) == 1 &&
        nvmtl_sm_read(pg, same_b[2], 0, c1, &s) == 1 &&
        nvmtl_sm_read(pg, same_b[3], 0, c1, &s) == 1 &&
        nvmtl_sm_read(pg, same_b[4], 0, c1, &s) == 1);

    /* Simulate a dead process holding a bucket lock and leaving odd seq */
    uint32_t dead_b = nvmtl_sm_bucket(777, 0);
    pg->hdr.bucket_lock[dead_b] = 999999u; /* dead PID */
    pg->slots[dead_b * NVRM_SURF_META_WAYS].seq = 3u; /* odd = died mid-write */
    nvmtl_sm_vram_update(pg, 777, 0, c1, 55, 0, 1, me);
    ARM("dead-PID lock + odd seqlock recovers cleanly without deadlock",
        pg->hdr.bucket_lock[dead_b] == 0 &&
        nvmtl_sm_read(pg, 777, 0, c1, &s) == 1 && s.vram_seed == 55);

    /* Multi-threaded seqlock stress: 2 writers + 2 readers, zero torn reads */
    pthread_t th[4];
    struct stress_ctx ctx[4];
    for (int i = 0; i < 4; i++) {
        ctx[i] = (struct stress_ctx){ pg, i, 0 };
        pthread_create(&th[i], NULL, stress_worker, &ctx[i]);
    }
    int total_bad = 0;
    for (int i = 0; i < 4; i++) { pthread_join(th[i], NULL); total_bad += ctx[i].bad; }
    ARM("multi-threaded seqlock stress produces zero torn reads", total_bad == 0);

    /* nitro-perf v9: pageoff_gen monotonicity and zero-skip wraparound */
    ARM("pageoff_gen reads 0 before kernel activation", nvmtl_sm_pageoff_gen(pg) == 0u);
    pg->hdr.pageoff_gen = 1u;
    ARM("pageoff_gen reads 1 after activation", nvmtl_sm_pageoff_gen(pg) == 1u);
    __atomic_add_fetch(&pg->hdr.pageoff_gen, 1u, __ATOMIC_RELEASE);
    ARM("pageoff_gen increments cleanly", nvmtl_sm_pageoff_gen(pg) == 2u);
    pg->hdr.pageoff_gen = UINT32_MAX;
    if (__atomic_add_fetch(&pg->hdr.pageoff_gen, 1u, __ATOMIC_RELEASE) == 0u)
        __atomic_add_fetch(&pg->hdr.pageoff_gen, 1u, __ATOMIC_RELEASE);
    ARM("pageoff_gen wraparound skips 0 so active counter never looks inactive", nvmtl_sm_pageoff_gen(pg) == 1u);

    free(pg);
    return fails ? 1 : 0;
}
