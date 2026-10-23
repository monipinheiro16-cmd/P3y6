/*
 * PS3 "DLSS-like" - kernel SPU
 * Por faixa de 8 linhas: AA morfologico leve (estilo MLAA/FXAA) ->
 * nitidez adaptativa (so onde nao ha borda) -> upscale bilinear 1280x720 -> 1920x1080.
 * Formato de pixel: 0xAARRGGBB.
 * NAO TESTADO em hardware real. Versao escalar, otimizar com SIMD depois.
 */
#include <spu_mfcio.h>
#include <stdint.h>
#include "common.h"

#define BH       8            /* linhas de origem por faixa */
#define OBH      12           /* linhas de saida por faixa (8 * 3/2) */
#define THR      24           /* limiar de borda */
#define SHARP    6            /* forca da nitidez (6/16) */

static job_t job;
static uint32_t in_rows[BH + 3][SRC_W] __attribute__((aligned(128)));
static uint32_t aa_rows[BH + 1][SRC_W] __attribute__((aligned(128)));
static uint32_t out_rows[OBH][DST_W]   __attribute__((aligned(128)));

static inline int iabs(int v) { return v < 0 ? -v : v; }

static inline int luma(uint32_t p) {
    return ((((p >> 16) & 255) * 77) + (((p >> 8) & 255) * 150) + ((p & 255) * 29)) >> 8;
}

/* mistura a (256-w) e b (w), w em 0..256 */
static inline uint32_t mix(uint32_t a, uint32_t b, int w) {
    uint32_t rb = (((a & 0xFF00FF) * (256 - w) + (b & 0xFF00FF) * w) >> 8) & 0xFF00FF;
    uint32_t g  = (((a & 0x00FF00) * (256 - w) + (b & 0x00FF00) * w) >> 8) & 0x00FF00;
    return 0xFF000000u | rb | g;
}

static inline int clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

static inline uint32_t sharpen(uint32_t o, uint32_t blur) {
    int r = (o >> 16) & 255, g = (o >> 8) & 255, b = o & 255;
    int br = (blur >> 16) & 255, bg = (blur >> 8) & 255, bb = blur & 255;
    r = clamp8(r + (((r - br) * SHARP) >> 4));
    g = clamp8(g + (((g - bg) * SHARP) >> 4));
    b = clamp8(b + (((b - bb) * SHARP) >> 4));
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

/* AA + nitidez de uma linha: up/cur/dn sao linhas cruas */
static void filter_row(const uint32_t *up, const uint32_t *cur, const uint32_t *dn,
                       uint32_t *out, int w) {
    for (int x = 0; x < w; x++) {
        uint32_t c = cur[x];
        uint32_t l = cur[x > 0 ? x - 1 : 0];
        uint32_t r = cur[x < w - 1 ? x + 1 : x];
        uint32_t n = up[x], s = dn[x];

        int gx = luma(r) - luma(l);
        int gy = luma(s) - luma(n);
        int mag = iabs(gx) + iabs(gy);

        if (mag > THR) {
            int wt = mag - THR;
            if (wt > 96) wt = 96;
            /* suaviza ao longo da borda */
            uint32_t nb = (iabs(gx) > iabs(gy)) ? mix(n, s, 128) : mix(l, r, 128);
            out[x] = mix(c, nb, wt);
        } else {
            uint32_t blur = mix(mix(n, s, 128), mix(l, r, 128), 128);
            out[x] = sharpen(c, blur);
        }
    }
}

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

int main(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    (void)arg1; (void)arg2; (void)arg3;

    /* le o job */
    mfc_get(&job, arg0, sizeof(job_t), 0, 0, 0);
    mfc_write_tag_mask(1 << 0);
    mfc_read_tag_status_all();

    const int w = job.w, h = job.h, ow = job.ow, oh = job.oh;
    const int nbands = (h + BH - 1) / BH;

    for (int band = job.idx; band < nbands; band += job.nspu) {
        int y0 = band * BH;

        /* DMA de entrada: linhas y0-1 .. y0+BH+1 (com clamp nas bordas) */
        for (int k = 0; k < BH + 3; k++) {
            int y = clampi(y0 - 1 + k, 0, h - 1);
            mfc_get(in_rows[k], job.src_ea + (uint64_t)y * w * 4, w * 4, 1, 0, 0);
        }
        mfc_write_tag_mask(1 << 1);
        mfc_read_tag_status_all();

        /* AA + nitidez nas linhas y0 .. y0+BH (BH+1 linhas) */
        for (int k = 0; k < BH + 1; k++) {
            /* linha y0+k corresponde a in_rows[k+1] */
            filter_row(in_rows[k], in_rows[k + 1], in_rows[k + 2], aa_rows[k], w);
        }

        /* upscale bilinear 2/3 */
        for (int j = 0; j < OBH; j++) {
            int oy = band * OBH + j;
            if (oy >= oh) break;

            int sy = (oy * 2) / 3;
            int fy = ((oy * 2) % 3) * 85;
            int k0 = sy - y0;
            int k1 = (sy + 1 < h) ? k0 + 1 : k0;
            const uint32_t *r0 = aa_rows[k0];
            const uint32_t *r1 = aa_rows[k1];

            for (int ox = 0; ox < ow; ox++) {
                int sx = (ox * 2) / 3;
                int fx = ((ox * 2) % 3) * 85;
                int x1 = (sx + 1 < w) ? sx + 1 : sx;
                uint32_t top = mix(r0[sx], r0[x1], fx);
                uint32_t bot = mix(r1[sx], r1[x1], fx);
                out_rows[j][ox] = mix(top, bot, fy);
            }
            mfc_put(out_rows[j], job.dst_ea + (uint64_t)oy * ow * 4, ow * 4, 2, 0, 0);
        }
        mfc_write_tag_mask(1 << 2);
        mfc_read_tag_status_all();
    }
    return 0;
}
