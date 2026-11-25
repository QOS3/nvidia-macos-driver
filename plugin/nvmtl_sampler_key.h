/*
 * NullMoth NVIDIA driver for macOS (nitro-perf v9)
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * Compact binary pixel-sampler signature & canonical stack key packer.
 * Replaces per-draw NSDictionary copies + NSJSONSerialization(SortedKeys) + CFSortIndexes
 * on the 144 Hz WindowServer RunFullDisplayPipe and CoreAnimation hot paths.
 * Pure C header (no ObjC/Foundation deps) so tools/test_sampler_key.c proves all invariants.
 */
#ifndef NVMTL_SAMPLER_KEY_H
#define NVMTL_SAMPLER_KEY_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef struct nvmtl_px_sampler_sig {
    uint32_t minf;
    uint32_t magf;
    uint32_t mipf;
    uint32_t as;
    uint32_t at;
    uint32_t ar;
    uint32_t cmp;
    uint32_t border;
    uint32_t lminb;
    uint32_t lmaxb;
    uint32_t aniso;
    uint32_t pad;
} nvmtl_px_sampler_sig;

_Static_assert(sizeof(nvmtl_px_sampler_sig) == 48u, "nvmtl_px_sampler_sig must be 48 bytes");

#define NVMTL_PXKEY_MAGIC      0x534Bu /* "KS" */
#define NVMTL_PXKEY_HDR_BYTES  8u
#define NVMTL_PXKEY_MAX_SLOTS  16u
#define NVMTL_PXKEY_MAX_BYTES  (NVMTL_PXKEY_HDR_BYTES + 2u * NVMTL_PXKEY_MAX_SLOTS * sizeof(nvmtl_px_sampler_sig))
#define NVMTL_PXKEY_NORM_LMAX  0x477fe000u /* IEEE-754 float 65504.0f */

static inline uint32_t nvmtl_pxkey_float_bits(float f)
{
    uint32_t b = 0;
    memcpy(&b, &f, sizeof b);
    if (b == 0x80000000u) b = 0u; /* canonicalize -0.0f -> +0.0f */
    return b;
}

static inline float nvmtl_pxkey_bits_float(uint32_t b)
{
    float f = 0.0f;
    memcpy(&f, &b, sizeof f);
    return f;
}

static inline uint64_t nvmtl_pxkey_fnv1a(const uint8_t *p, size_t n)
{
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < n; i++) {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ull;
    }
    return h ? h : 1ull;
}

/* Pack bound vertex + fragment pixel-sampler signatures into canonical binary key bytes on the stack.
 * Returns 64-bit FNV-1a hash of the packed key and sets *out_len to exact byte length. */
static inline uint64_t nvmtl_pxkey_pack(uint8_t out[NVMTL_PXKEY_MAX_BYTES], size_t *out_len,
                                        uint16_t vmask, const nvmtl_px_sampler_sig v[NVMTL_PXKEY_MAX_SLOTS],
                                        uint16_t fmask, const nvmtl_px_sampler_sig f[NVMTL_PXKEY_MAX_SLOTS],
                                        int normalize_lmax)
{
    const uint16_t hdr[4] = {
        (uint16_t)NVMTL_PXKEY_MAGIC,
        (uint16_t)(normalize_lmax ? 1u : 0u),
        vmask,
        fmask
    };
    memcpy(out, hdr, NVMTL_PXKEY_HDR_BYTES);
    size_t pos = NVMTL_PXKEY_HDR_BYTES;

    for (unsigned i = 0; i < NVMTL_PXKEY_MAX_SLOTS; i++) {
        if (!(vmask & (1u << i))) continue;
        nvmtl_px_sampler_sig s = v[i];
        s.pad = 0u;
        if (s.lminb == 0x80000000u) s.lminb = 0u;
        if (normalize_lmax) s.lmaxb = NVMTL_PXKEY_NORM_LMAX;
        else if (s.lmaxb == 0x80000000u) s.lmaxb = 0u;
        memcpy(out + pos, &s, sizeof s);
        pos += sizeof s;
    }
    for (unsigned i = 0; i < NVMTL_PXKEY_MAX_SLOTS; i++) {
        if (!(fmask & (1u << i))) continue;
        nvmtl_px_sampler_sig s = f[i];
        s.pad = 0u;
        if (s.lminb == 0x80000000u) s.lminb = 0u;
        if (normalize_lmax) s.lmaxb = NVMTL_PXKEY_NORM_LMAX;
        else if (s.lmaxb == 0x80000000u) s.lmaxb = 0u;
        memcpy(out + pos, &s, sizeof s);
        pos += sizeof s;
    }
    if (out_len) *out_len = pos;
    return nvmtl_pxkey_fnv1a(out, pos);
}

#endif /* NVMTL_SAMPLER_KEY_H */
