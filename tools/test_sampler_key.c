/* test_sampler_key.c - proves the compact binary pixel-sampler key packer (nitro-perf v9)
 * on the host without a GPU: canonical byte packing, -0.0f/+0.0f equivalence, lod_max_clamp
 * normalization (NVMTL_NO_SAMPKEY on/off), vertex vs fragment separation, slot order, and zero padding.
 * Build and run anywhere:  cc -I plugin tools/test_sampler_key.c -o /tmp/test_sampler_key && /tmp/test_sampler_key */
#include "nvmtl_sampler_key.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define ARM(name, cond) do { const int ok = !!(cond); printf("%s  %s\n", ok ? "ok  " : "FAIL", name); if (!ok) fails++; } while (0)

int main(void)
{
    nvmtl_px_sampler_sig v[16], f[16], f2[16];
    memset(v, 0xAA, sizeof v); /* poison unused slots + padding to prove zero garbage leakage */
    memset(f, 0x55, sizeof f);
    memset(f2, 0xBB, sizeof f2);

    nvmtl_px_sampler_sig lin = {
        .minf = 1, .magf = 1, .mipf = 0,
        .as = 0, .at = 0, .ar = 0,
        .cmp = 0, .border = 0,
        .lminb = nvmtl_pxkey_float_bits(0.0f),
        .lmaxb = nvmtl_pxkey_float_bits(1000.0f),
        .aniso = 1, .pad = 0xDEADBEEFu
    };
    nvmtl_px_sampler_sig lin_neg0 = lin;
    lin_neg0.lminb = 0x80000000u; /* -0.0f */
    lin_neg0.lmaxb = nvmtl_pxkey_float_bits(3.4028235e38f); /* FLT_MAX */
    lin_neg0.pad = 0x12345678u;

    f[0] = lin;
    f2[0] = lin_neg0;

    uint8_t k1[NVMTL_PXKEY_MAX_BYTES], k2[NVMTL_PXKEY_MAX_BYTES], k3[NVMTL_PXKEY_MAX_BYTES];
    size_t l1 = 0, l2 = 0, l3 = 0;

    uint64_t h1 = nvmtl_pxkey_pack(k1, &l1, 0, v, 1u << 0, f, 1);
    uint64_t h2 = nvmtl_pxkey_pack(k2, &l2, 0, v, 1u << 0, f2, 1);
    ARM("normalized key ignores lod_max_clamp, -0.0f, pad garbage, and unused slots",
        h1 == h2 && l1 == l2 && l1 == NVMTL_PXKEY_HDR_BYTES + sizeof(nvmtl_px_sampler_sig) &&
        memcmp(k1, k2, l1) == 0);

    uint64_t h3 = nvmtl_pxkey_pack(k3, &l3, 0, v, 1u << 0, f, 0);
    uint64_t h4 = nvmtl_pxkey_pack(k2, &l2, 0, v, 1u << 0, f2, 0);
    ARM("unnormalized mode (NVMTL_NO_SAMPKEY) distinguishes different lod_max_clamp values",
        l3 == l2 && (h3 != h4 || memcmp(k3, k2, l3) != 0));

    v[0] = lin;
    uint64_t hv = nvmtl_pxkey_pack(k1, &l1, 1u << 0, v, 0, f, 1);
    uint64_t hf = nvmtl_pxkey_pack(k2, &l2, 0, v, 1u << 0, f, 1);
    ARM("same sampler bound at vertex[0] vs fragment[0] produces distinct keys",
        l1 == l2 && (hv != hf || memcmp(k1, k2, l1) != 0));

    f[3] = lin;
    f[3].minf = 0; /* Nearest */
    uint64_t hm1 = nvmtl_pxkey_pack(k1, &l1, 0, v, (1u << 0) | (1u << 3), f, 1);
    f[2] = f[3];
    uint64_t hm2 = nvmtl_pxkey_pack(k2, &l2, 0, v, (1u << 0) | (1u << 2), f, 1);
    ARM("different slot index with identical sampler produces distinct keys",
        l1 == NVMTL_PXKEY_HDR_BYTES + 2u * sizeof(nvmtl_px_sampler_sig) &&
        l1 == l2 && (hm1 != hm2 || memcmp(k1, k2, l1) != 0));

    nvmtl_px_sampler_sig f_diff = f[0];
    f_diff.as = 2; /* Repeat */
    f2[0] = f_diff;
    uint64_t hd1 = nvmtl_pxkey_pack(k1, &l1, 0, v, 1u << 0, f, 1);
    uint64_t hd2 = nvmtl_pxkey_pack(k2, &l2, 0, v, 1u << 0, f2, 1);
    ARM("address mode change produces distinct key",
        hd1 != hd2 && memcmp(k1, k2, l1) != 0);

    ARM("float round-trip preserves exact value",
        nvmtl_pxkey_bits_float(nvmtl_pxkey_float_bits(2.5f)) == 2.5f &&
        nvmtl_pxkey_bits_float(NVMTL_PXKEY_NORM_LMAX) == 65504.0f);

    return fails ? 1 : 0;
}
