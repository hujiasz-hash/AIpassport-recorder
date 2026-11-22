// Ogg Opus 页封装。录音时只做封装，数据直接进缓存。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OGG_SAMPLE_RATE     16000
#define OGG_FRAME_SAMPLES   320      // 20 ms
#define OGG_BITRATE_BPS     12000
#define OGG_PKTS_PER_PAGE   5
#define OGG_MAX_PKT_BYTES   400
#define OGG_PAGE_MAX        (27 + 255 + OGG_PKTS_PER_PAGE * OGG_MAX_PKT_BYTES)
#define OGG_GRANULE_PER_FRAME 960    // 48 kHz 计数：20 ms × 48000

typedef struct {
    uint32_t serial;
    uint32_t seq;
    uint64_t granule;
    uint8_t lace[255];
    uint8_t nlace;
    uint8_t npkt;
    uint32_t dlen;
    uint8_t data[OGG_PKTS_PER_PAGE * OGG_MAX_PKT_BYTES];
    uint8_t page[OGG_PAGE_MAX];
} ogg_writer_t;

void ogg_init(void);

// 写 BOS 页（OpusHead）和第一个注释页（OpusTags）。返回 0 成功。
int ogg_write_headers(ogg_writer_t *w, uint32_t serial,
                      int (*sink)(void *ctx, const void *data, size_t n), void *ctx);

// 追加一个 Opus 包。攒够一页或空间不足时自动 flush。
int ogg_put_packet(ogg_writer_t *w, const uint8_t *pkt, uint32_t len,
                   int (*sink)(void *ctx, const void *data, size_t n), void *ctx);

// 把剩余包写成普通页；eos=true 时标记 EOS 并结束流。
int ogg_finish(ogg_writer_t *w, bool eos,
               int (*sink)(void *ctx, const void *data, size_t n), void *ctx);