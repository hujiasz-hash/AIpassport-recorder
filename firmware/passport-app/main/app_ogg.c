#include "app_ogg.h"

#include <stdbool.h>
#include <string.h>

#define OGG_HEADER_BYTES 27

typedef int (*sink_fn)(void *ctx, const void *data, size_t n);

static uint32_t s_crc_tab[256];

void ogg_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t r = i << 24;
        for (int j = 0; j < 8; j++) {
            r = (r & 0x80000000u) ? (r << 1) ^ 0x04C11DB7u : (r << 1);
        }
        s_crc_tab[i] = r;
    }
}

static void put_le(uint8_t *p, uint32_t v, int n) {
    for (int i = 0; i < n; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static uint32_t crc_update(uint32_t crc, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) crc = (crc << 8) ^ s_crc_tab[((crc >> 24) ^ p[i]) & 0xFF];
    return crc;
}

// 落一页。没有待写包时默认不落页；force=true 用于 EOS 补空页。
static int flush_page(ogg_writer_t *w, uint8_t htype, bool force, sink_fn sink, void *ctx) {
    if (w->dlen == 0) {
        if (!force) return 0;
        // EOS 空页：lacing 写一个 0，表示本页不含任何完整包，仅承载流结束标记。
        w->nlace = 1;
        w->lace[0] = 0;
    }

    uint8_t *p = w->page;
    memcpy(p, "OggS", 4);
    p[4] = 0;                                   // 版本
    p[5] = htype;
    put_le(p + 6, (uint32_t)w->granule, 4);     // granule 低 32 位
    put_le(p + 10, (uint32_t)(w->granule >> 32), 4);
    put_le(p + 14, w->serial, 4);
    put_le(p + 18, w->seq++, 4);
    put_le(p + 22, 0, 4);                       // CRC 占位，算完再填
    p[26] = w->nlace;
    memcpy(p + OGG_HEADER_BYTES, w->lace, w->nlace);
    memcpy(p + OGG_HEADER_BYTES + w->nlace, w->data, w->dlen);
    uint32_t total = OGG_HEADER_BYTES + w->nlace + w->dlen;
    put_le(p + 22, crc_update(0, p, total), 4);

    w->nlace = 0;
    w->npkt = 0;
    w->dlen = 0;

    return sink(ctx, p, total);
}

// granule_inc 只在成功把包放进页后累加。触发提前落页时，先落页再放包，
// 落页用的是已经累加过的 granule，语义正确。
static int put_packet(ogg_writer_t *w, const uint8_t *pkt, uint32_t len, uint32_t granule_inc,
                      sink_fn sink, void *ctx) {
    uint32_t need = len / 255 + 1;
    if (w->npkt == OGG_PKTS_PER_PAGE || w->nlace + need > 255 ||
        w->dlen + len > sizeof w->data) {
        int r = flush_page(w, 0x00, false, sink, ctx);
        if (r < 0) return r;
    }
    if (w->nlace + need > 255 || w->dlen + len > sizeof w->data) {
        return -1;   // 单包就超过一页容量（编码器不会产出这么大的包）
    }
    for (uint32_t r = len; r >= 255; r -= 255) w->lace[w->nlace++] = 255;
    w->lace[w->nlace++] = (uint8_t)(len % 255);
    memcpy(w->data + w->dlen, pkt, len);
    w->dlen += len;
    w->granule += granule_inc;
    w->npkt++;
    return 0;
}

int ogg_write_headers(ogg_writer_t *w, uint32_t serial, sink_fn sink, void *ctx) {
    memset(w, 0, sizeof *w);
    w->serial = serial;

    // OpusHead 的 pre-skip 按 48 kHz 计数。1920 = 20 ms @ 16 kHz 换算到 48 kHz 是 2880，
    // 而 pre-skip 取编码器 lookahead（16 kHz 下 312 采样），必须换算成 48 kHz：312 × 3 = 936。
    // 阶段 1、2 用的是同一个值，ffmpeg 解码时长精确到 30.00 s，可沿用。
    uint8_t head[19];
    memcpy(head, "OpusHead", 8);
    head[8] = 1;                            // 版本
    head[9] = 1;                            // 声道数
    put_le(head + 10, 312 * 3, 2);          // pre-skip（48 kHz 计数）
    put_le(head + 12, OGG_SAMPLE_RATE, 4);  // 原始采样率
    put_le(head + 16, 0, 2);                // 输出增益
    head[18] = 0;                           // 声道映射族

    // OpusTags：厂商串 "aipass"，无用户注释。
    uint8_t tags[20];
    memcpy(tags, "OpusTags", 8);
    put_le(tags + 8, 6, 4);
    memcpy(tags + 12, "aipass", 6);
    put_le(tags + 18, 0, 4);

    int r = put_packet(w, head, sizeof head, 0, sink, ctx);
    if (r < 0) return r;
    r = flush_page(w, 0x02, false, sink, ctx);      // BOS：头页必须单独成页
    if (r < 0) return r;
    r = put_packet(w, tags, sizeof tags, 0, sink, ctx);
    if (r < 0) return r;
    return flush_page(w, 0x00, false, sink, ctx);
}

int ogg_put_packet(ogg_writer_t *w, const uint8_t *pkt, uint32_t len,
                   sink_fn sink, void *ctx) {
    return put_packet(w, pkt, len, OGG_GRANULE_PER_FRAME, sink, ctx);
}

int ogg_finish(ogg_writer_t *w, bool eos, sink_fn sink, void *ctx) {
    if (!eos) return flush_page(w, 0x00, false, sink, ctx);
    return flush_page(w, 0x04, true, sink, ctx);
}