/*
 * NullMoth NVIDIA driver for macOS (nitro-perf v8)
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * Shared lock-free surface metadata table mapped read/write from NVRMUserClient::clientMemoryForType
 * (type NVRM_SURF_META_MEM_TYPE). Replaces per-frame IOSurfaceCopyValue/IOSurfaceSetValue IOKit traps
 * for single-plane NVMTLVramAt and NVMTLBusyAt tags with ~10 ns seqlock reads/writes.
 * Contains ONLY advisory userspace metadata (no kernel pointers, no physical addresses).
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

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
    uint32_t pad_hdr[11];
    uint32_t bucket_lock[NVRM_SURF_META_BUCKETS];
    uint32_t pad_tail[496];
} nvrm_surf_meta_hdr;

typedef struct nvrm_surf_meta_slot {
    uint32_t seq;           /* userland seqlock: odd = write in progress, even = committed */
    uint32_t kernel_inval;  /* kernel-only atomic counter bumped when sid gets a new hSrcMemory */
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

#if defined(__cplusplus)
static_assert(sizeof(nvrm_surf_meta_hdr) == NVRM_SURF_META_HDR_BYTES, "nvrm_surf_meta_hdr must be 4096 bytes");
static_assert(sizeof(nvrm_surf_meta_slot) == 64u, "nvrm_surf_meta_slot must be 64 bytes");
static_assert(sizeof(nvrm_surf_meta_page) == NVRM_SURF_META_BYTES, "nvrm_surf_meta_page size mismatch");
#else
_Static_assert(sizeof(nvrm_surf_meta_hdr) == NVRM_SURF_META_HDR_BYTES, "nvrm_surf_meta_hdr must be 4096 bytes");
_Static_assert(sizeof(nvrm_surf_meta_slot) == 64u, "nvrm_surf_meta_slot must be 64 bytes");
_Static_assert(sizeof(nvrm_surf_meta_page) == NVRM_SURF_META_BYTES, "nvrm_surf_meta_page size mismatch");
#endif

static inline uint32_t nvrm_surf_meta_bucket(uint32_t sid, uint32_t plane)
{
    uint32_t x = sid ^ (plane * 0x9E3779B1u);
    x ^= x >> 16;
    x *= 0x85EBCA6Bu;
    x ^= x >> 13;
    return x & (NVRM_SURF_META_BUCKETS - 1u);
}

static inline void nvrm_surf_meta_kernel_invalidate_sid(nvrm_surf_meta_page *pg, uint32_t sid, uint32_t plane)
{
    if (!pg || !sid) return;
    const uint32_t base = nvrm_surf_meta_bucket(sid, plane) * NVRM_SURF_META_WAYS;
    for (uint32_t w = 0; w < NVRM_SURF_META_WAYS; w++) {
        nvrm_surf_meta_slot *s = &pg->slots[base + w];
        if (__atomic_load_n(&s->sid, __ATOMIC_RELAXED) == sid) {
            __atomic_fetch_add(&s->kernel_inval, 1u, __ATOMIC_RELEASE);
        }
    }
}
