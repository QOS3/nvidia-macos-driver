/*
 * NullMoth NVIDIA driver for macOS (nitro-perf v8)
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * Userland lock-free seqlock reader & crash-safe bounded-lock writer for NVRM's
 * shared surface-metadata page (NVRM_SURF_META_MEM_TYPE = 0x7FFFFFFEu).
 * Pure C header (no ObjC/IOKit deps) so tools/test_surf_meta.c proves all invariants.
 */
#ifndef NVMTL_SURF_META_H
#define NVMTL_SURF_META_H
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <unistd.h>

#define NVRM_SURF_META_MEM_TYPE    0x7FFFFFFEu
#define NVRM_SURF_META_MAGIC       0x4E56534Du /* "NVSM" */
#define NVRM_SURF_META_VERSION     1u
#define NVRM_SURF_META_BUCKETS     512u
#define NVRM_SURF_META_WAYS        4u
#define NVRM_SURF_META_SLOTS       (NVRM_SURF_META_BUCKETS * NVRM_SURF_META_WAYS)
#define NVRM_SURF_META_HDR_BYTES   4096u
#define NVRM_SURF_META_BYTES       (NVRM_SURF_META_HDR_BYTES + NVRM_SURF_META_SLOTS * 64u)

#define NVRM_SM_FLAG_VRAM_VALID    (1u << 0)
#define NVRM_SM_FLAG_BUSY_VALID    (1u << 1)
#define NVRM_SM_FLAG_BUSY_DONE     (1u << 2)

typedef struct nvrm_surf_meta_hdr {
    uint32_t magic;
    uint32_t version;
    uint32_t buckets;
    uint32_t ways;
    uint32_t lru_clock;
    uint32_t pageoff_gen;   /* nitro-perf v9: kernel-only monotonic counter (>=1 when active), bumped on page-off/forget */
    uint32_t pad_hdr[10];
    uint32_t bucket_lock[NVRM_SURF_META_BUCKETS];
    uint32_t pad_tail[496];
} nvrm_surf_meta_hdr;

typedef struct nvrm_surf_meta_slot {
    uint32_t seq;
    uint32_t kernel_inval;
    uint32_t sid;
    uint32_t plane;
    uint64_t cookie;
    uint32_t kgen;
    uint32_t vram_seed;
    uint32_t vram_gen;
    uint32_t flags;
    int32_t  busy_pid;
    uint32_t lru_tick;
    uint64_t busy_seq;
    uint64_t busy_stamp_ns;
} nvrm_surf_meta_slot;

typedef struct nvrm_surf_meta_page {
    nvrm_surf_meta_hdr  hdr;
    nvrm_surf_meta_slot slots[NVRM_SURF_META_SLOTS];
} nvrm_surf_meta_page;

_Static_assert(sizeof(nvrm_surf_meta_hdr) == NVRM_SURF_META_HDR_BYTES, "nvrm_surf_meta_hdr must be 4096 bytes");
_Static_assert(sizeof(nvrm_surf_meta_slot) == 64u, "nvrm_surf_meta_slot must be 64 bytes");
_Static_assert(sizeof(nvrm_surf_meta_page) == NVRM_SURF_META_BYTES, "nvrm_surf_meta_page size mismatch");

static inline uint32_t nvmtl_sm_pageoff_gen(const nvrm_surf_meta_page *pg)
{
    return pg ? __atomic_load_n(&pg->hdr.pageoff_gen, __ATOMIC_ACQUIRE) : 0u;
}

static inline uint32_t nvmtl_sm_bucket(uint32_t sid, uint32_t plane)
{
    uint32_t x = sid ^ (plane * 0x9E3779B1u);
    x ^= x >> 16;
    x *= 0x85EBCA6Bu;
    x ^= x >> 13;
    return x & (NVRM_SURF_META_BUCKETS - 1u);
}

static inline void nvmtl_sm_pause(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("pause" ::: "memory");
#else
    __asm__ __volatile__("" ::: "memory");
#endif
}

/* Lock-free seqlock lookup for (sid, plane, cookie). Returns 1 on valid hit, 0 on miss or invalidated slot. */
static inline int nvmtl_sm_read(const nvrm_surf_meta_page *pg, uint32_t sid, uint32_t plane,
                                uint64_t cookie, nvrm_surf_meta_slot *out)
{
    if (!pg || !sid) return 0;
    const uint32_t base = nvmtl_sm_bucket(sid, plane) * NVRM_SURF_META_WAYS;
    for (uint32_t w = 0; w < NVRM_SURF_META_WAYS; w++) {
        const nvrm_surf_meta_slot *s = &pg->slots[base + w];
        for (int tries = 0; tries < 8; tries++) {
            uint32_t s0 = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
            if (s0 & 1u) { nvmtl_sm_pause(); continue; }
            if (__atomic_load_n(&s->sid, __ATOMIC_RELAXED) != sid) break;
            nvrm_surf_meta_slot snap;
            snap.sid           = __atomic_load_n(&s->sid, __ATOMIC_RELAXED);
            snap.plane         = __atomic_load_n(&s->plane, __ATOMIC_RELAXED);
            snap.cookie        = __atomic_load_n(&s->cookie, __ATOMIC_RELAXED);
            snap.kgen          = __atomic_load_n(&s->kgen, __ATOMIC_RELAXED);
            snap.vram_seed     = __atomic_load_n(&s->vram_seed, __ATOMIC_RELAXED);
            snap.vram_gen      = __atomic_load_n(&s->vram_gen, __ATOMIC_RELAXED);
            snap.flags         = __atomic_load_n(&s->flags, __ATOMIC_RELAXED);
            snap.busy_pid      = __atomic_load_n(&s->busy_pid, __ATOMIC_RELAXED);
            snap.lru_tick      = __atomic_load_n(&s->lru_tick, __ATOMIC_RELAXED);
            snap.busy_seq      = __atomic_load_n(&s->busy_seq, __ATOMIC_RELAXED);
            snap.busy_stamp_ns = __atomic_load_n(&s->busy_stamp_ns, __ATOMIC_RELAXED);
            uint32_t kinval    = __atomic_load_n(&s->kernel_inval, __ATOMIC_ACQUIRE);
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            uint32_t s1 = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
            if (s0 != s1) { nvmtl_sm_pause(); continue; }
            if (snap.sid == sid && snap.plane == plane && snap.cookie == cookie && snap.kgen == kinval) {
                snap.seq = s0;
                snap.kernel_inval = kinval;
                if (out) *out = snap;
                return 1;
            }
            break;
        }
    }
    return 0;
}

/* Crash-safe per-bucket writer lock (stores writer pid; steals if owner died or bounded spin expires). */
static inline int nvmtl_sm_bucket_lock(nvrm_surf_meta_page *pg, uint32_t b, uint32_t my_pid)
{
    uint32_t *lk = &pg->hdr.bucket_lock[b];
    uint32_t tag = my_pid ? my_pid : 1u;
    for (int spin = 0; spin < 512; spin++) {
        uint32_t exp = 0;
        if (__atomic_compare_exchange_n(lk, &exp, tag, 1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 1;
        nvmtl_sm_pause();
    }
    for (int y = 0; y < 128; y++) {
        uint32_t exp = 0;
        if (__atomic_compare_exchange_n(lk, &exp, tag, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 1;
        if (y >= 4 && exp > 1u && exp != tag && kill((pid_t)exp, 0) != 0 && errno == ESRCH) {
            if (__atomic_compare_exchange_n(lk, &exp, tag, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 1;
        }
        sched_yield();
    }
    /* Bounded fallback: force-steal so a wedged process can never deadlock writers. */
    __atomic_store_n(lk, tag, __ATOMIC_ACQUIRE);
    return 1;
}

static inline void nvmtl_sm_bucket_unlock(nvrm_surf_meta_page *pg, uint32_t b, uint32_t my_pid)
{
    uint32_t *lk = &pg->hdr.bucket_lock[b];
    uint32_t tag = my_pid ? my_pid : 1u;
    __atomic_compare_exchange_n(lk, &tag, 0u, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
}

/* Locate matching way or choose empty / invalidated / LRU way while bucket lock is held. */
static inline nvrm_surf_meta_slot *nvmtl_sm_pick_way(nvrm_surf_meta_page *pg, uint32_t b,
                                                     uint32_t sid, uint32_t plane, uint64_t cookie,
                                                     int *matched_valid)
{
    const uint32_t base = b * NVRM_SURF_META_WAYS;
    int empty = -1, stale = -1, lru = 0;
    uint32_t lru_min = UINT32_MAX;
    *matched_valid = 0;
    for (uint32_t w = 0; w < NVRM_SURF_META_WAYS; w++) {
        nvrm_surf_meta_slot *s = &pg->slots[base + w];
        const uint32_t kinval = __atomic_load_n(&s->kernel_inval, __ATOMIC_ACQUIRE);
        if (s->sid == sid && s->plane == plane) {
            *matched_valid = (s->cookie == cookie && s->kgen == kinval);
            return s;
        }
        if (s->sid == 0 && empty < 0) empty = (int)w;
        else if (s->sid != 0 && s->kgen != kinval && stale < 0) stale = (int)w;
        if (s->lru_tick < lru_min) { lru_min = s->lru_tick; lru = (int)w; }
    }
    if (empty >= 0) return &pg->slots[base + (uint32_t)empty];
    if (stale >= 0) return &pg->slots[base + (uint32_t)stale];
    return &pg->slots[base + (uint32_t)lru];
}

/* Update VRAM tag (set or bump gen). Returns resulting vram_gen. */
static inline uint32_t nvmtl_sm_vram_update(nvrm_surf_meta_page *pg, uint32_t sid, uint32_t plane,
                                            uint64_t cookie, uint32_t seed, uint32_t explicit_gen,
                                            int mode /* 0=keep gen, 1=bump gen, 2=explicit gen */, uint32_t my_pid)
{
    if (!pg || !sid) return explicit_gen;
    const uint32_t b = nvmtl_sm_bucket(sid, plane);
    nvmtl_sm_bucket_lock(pg, b, my_pid);
    int same = 0;
    nvrm_surf_meta_slot *s = nvmtl_sm_pick_way(pg, b, sid, plane, cookie, &same);
    uint32_t seq = __atomic_load_n(&s->seq, __ATOMIC_RELAXED);
    if (seq & 1u) seq++;
    __atomic_store_n(&s->seq, seq + 1u, __ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_RELEASE);

    const uint32_t kinval = __atomic_load_n(&s->kernel_inval, __ATOMIC_ACQUIRE);
    uint32_t gen;
    if (mode == 2) gen = explicit_gen;
    else if (mode == 1) gen = (same && (s->flags & NVRM_SM_FLAG_VRAM_VALID)) ? (s->vram_gen + 1u) : 1u;
    else gen = (same && (s->flags & NVRM_SM_FLAG_VRAM_VALID)) ? s->vram_gen : 0u;

    if (!same) {
        s->flags = NVRM_SM_FLAG_VRAM_VALID;
        s->busy_pid = 0;
        s->busy_seq = 0;
        s->busy_stamp_ns = 0;
    } else {
        s->flags |= NVRM_SM_FLAG_VRAM_VALID;
    }
    s->sid       = sid;
    s->plane     = plane;
    s->cookie    = cookie;
    s->kgen      = kinval;
    s->vram_seed = seed;
    s->vram_gen  = gen;
    s->lru_tick  = __atomic_fetch_add(&pg->hdr.lru_clock, 1u, __ATOMIC_RELAXED) + 1u;

    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&s->seq, seq + 2u, __ATOMIC_RELEASE);
    nvmtl_sm_bucket_unlock(pg, b, my_pid);
    return gen;
}

/* Update busy tag for (sid, plane, cookie) without disturbing valid VRAM tag fields. */
static inline void nvmtl_sm_busy_update(nvrm_surf_meta_page *pg, uint32_t sid, uint32_t plane,
                                        uint64_t cookie, int32_t writer_pid, uint64_t bseq,
                                        uint64_t stamp_ns, int done)
{
    if (!pg || !sid) return;
    const uint32_t b = nvmtl_sm_bucket(sid, plane);
    const uint32_t my_pid = writer_pid > 0 ? (uint32_t)writer_pid : 1u;
    nvmtl_sm_bucket_lock(pg, b, my_pid);
    int same = 0;
    nvrm_surf_meta_slot *s = nvmtl_sm_pick_way(pg, b, sid, plane, cookie, &same);
    uint32_t seq = __atomic_load_n(&s->seq, __ATOMIC_RELAXED);
    if (seq & 1u) seq++;
    __atomic_store_n(&s->seq, seq + 1u, __ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_RELEASE);

    const uint32_t kinval = __atomic_load_n(&s->kernel_inval, __ATOMIC_ACQUIRE);
    if (!same) {
        s->vram_seed = 0;
        s->vram_gen = 0;
        s->flags = NVRM_SM_FLAG_BUSY_VALID | (done ? NVRM_SM_FLAG_BUSY_DONE : 0u);
    } else {
        s->flags = (s->flags & ~NVRM_SM_FLAG_BUSY_DONE) | NVRM_SM_FLAG_BUSY_VALID | (done ? NVRM_SM_FLAG_BUSY_DONE : 0u);
    }
    s->sid           = sid;
    s->plane         = plane;
    s->cookie        = cookie;
    s->kgen          = kinval;
    s->busy_pid      = writer_pid;
    s->busy_seq      = bseq;
    s->busy_stamp_ns = stamp_ns;
    s->lru_tick      = __atomic_fetch_add(&pg->hdr.lru_clock, 1u, __ATOMIC_RELAXED) + 1u;

    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&s->seq, seq + 2u, __ATOMIC_RELEASE);
    nvmtl_sm_bucket_unlock(pg, b, my_pid);
}

#endif /* NVMTL_SURF_META_H */
