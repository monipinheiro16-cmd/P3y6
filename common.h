#ifndef COMMON_H
#define COMMON_H
#include <stdint.h>

#define SRC_W 1280
#define SRC_H 720
#define DST_W 1920
#define DST_H 1080

/* Job enviado para cada SPU (64 bytes, alinhado em 16) */
typedef struct {
    uint64_t src_ea;   /* frame 1280x720 ARGB */
    uint64_t dst_ea;   /* frame 1920x1080 ARGB */
    uint32_t w, h, ow, oh;
    uint32_t idx, nspu;
    uint32_t pad[6];
} __attribute__((aligned(16))) job_t;

#endif
