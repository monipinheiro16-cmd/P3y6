/*
 * PS3 "DLSS-like" - host PPU (PSL1GHT)
 * Gera um frame 720p com serrilhado, roda o filtro em 6 SPUs,
 * mede o tempo e salva /dev_hdd0/tmp/saida_1080p.ppm.
 * NAO TESTADO em hardware real.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <sys/spu.h>
#include <sys/systime.h>
#include <ppu-types.h>
#include "common.h"

#define NSPU 6

extern const u8 spu_filter_bin[];   /* gerado pelo bin2o a partir do .elf do SPU */

static void make_test_frame(uint32_t *f) {
    for (int y = 0; y < SRC_H; y++) {
        for (int x = 0; x < SRC_W; x++) {
            int g = (x * 255) / SRC_W;
            uint32_t p = 0xFF000000u | (g << 16) | ((y * 255 / SRC_H) << 8) | 64;
            /* linhas diagonais duras (serrilhado) */
            if (((x + y) % 97) < 3 || ((x - y + 2000) % 131) < 3) p = 0xFFFFFFFFu;
            /* circulo duro */
            int dx = x - 640, dy = y - 360;
            int d2 = dx * dx + dy * dy;
            if (d2 > 200 * 200 && d2 < 206 * 206) p = 0xFF000000u;
            f[y * SRC_W + x] = p;
        }
    }
}

static void save_ppm(const char *path, const uint32_t *f, int w, int h) {
    FILE *fp = fopen(path, "wb");
    if (!fp) { printf("nao consegui abrir %s\n", path); return; }
    fprintf(fp, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) {
        unsigned char rgb[3] = { (f[i] >> 16) & 255, (f[i] >> 8) & 255, f[i] & 255 };
        fwrite(rgb, 1, 3, fp);
    }
    fclose(fp);
}

int main(void) {
    uint32_t *src = (uint32_t *)memalign(128, SRC_W * SRC_H * 4);
    uint32_t *dst = (uint32_t *)memalign(128, DST_W * DST_H * 4);
    job_t *jobs = (job_t *)memalign(128, sizeof(job_t) * NSPU);
    if (!src || !dst || !jobs) { printf("sem memoria\n"); return 1; }

    make_test_frame(src);
    memset(dst, 0, DST_W * DST_H * 4);

    sysSpuInitialize(NSPU, 0);

    sysSpuImage image;
    sysSpuImageImport(&image, spu_filter_bin, 0);

    sys_spu_group_t group;
    sysSpuThreadGroupAttribute grpattr;
    sysSpuThreadGroupAttributeInitialize(grpattr);
    sysSpuThreadGroupAttributeName(grpattr, "dlss_ps3");
    sysSpuThreadGroupCreate(&group, NSPU, 100, &grpattr);

    sys_spu_thread_t threads[NSPU];
    for (int i = 0; i < NSPU; i++) {
        jobs[i].src_ea = (uint64_t)(uintptr_t)src;
        jobs[i].dst_ea = (uint64_t)(uintptr_t)dst;
        jobs[i].w = SRC_W; jobs[i].h = SRC_H;
        jobs[i].ow = DST_W; jobs[i].oh = DST_H;
        jobs[i].idx = i; jobs[i].nspu = NSPU;

        sysSpuThreadAttribute attr;
        sysSpuThreadAttributeInitialize(attr);
        sysSpuThreadAttributeName(attr, "filtro");
        sysSpuThreadAttributeOption(attr, SPU_THREAD_ATTR_NONE);

        sysSpuThreadArgument arg;
        sysSpuThreadArgumentInitialize(arg);
        arg.arg0 = (uint64_t)(uintptr_t)&jobs[i];

        sysSpuThreadInitialize(&threads[i], group, i, &image, &attr, &arg);
    }

    u64 t0 = sysGetSystemTime();
    sysSpuThreadGroupStart(group);
    u32 cause, status;
    sysSpuThreadGroupJoin(group, &cause, &status);
    u64 t1 = sysGetSystemTime();

    printf("Filtro 720p -> 1080p em %llu us com %d SPUs\n", (unsigned long long)(t1 - t0), NSPU);

    save_ppm("/dev_hdd0/tmp/saida_1080p.ppm", dst, DST_W, DST_H);
    save_ppm("/dev_hdd0/tmp/entrada_720p.ppm", src, SRC_W, SRC_H);

    sysSpuImageClose(&image);
    free(src); free(dst); free(jobs);
    return 0;
}
