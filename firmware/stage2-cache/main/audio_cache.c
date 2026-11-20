#include "audio_cache.h"

#include <string.h>

#define HDR_MAGIC 0x31435541u
#define OFF_SEQ   4
#define OFF_SESS  8
#define OFF_LEN   12
#define OFF_CRC   16

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
    }
    return crc;
}

static uint32_t sector_crc(const uint8_t *img, uint32_t len) {
    uint32_t crc = crc32_update(0xFFFFFFFFu, img, OFF_CRC);
    crc = crc32_update(crc, img + AUDIO_CACHE_HDR_SIZE, len);
    return crc ^ 0xFFFFFFFFu;
}

static uint32_t sector_off(uint32_t idx) {
    return idx * AUDIO_CACHE_SECTOR;
}

static int read_header(audio_cache_t *c, uint32_t idx, uint8_t *hdr) {
    return c->flash.read(c->flash.ctx, sector_off(idx), hdr, AUDIO_CACHE_HDR_SIZE);
}

static bool header_ok(const uint8_t *hdr, uint32_t idx) {
    return rd32(hdr) == HDR_MAGIC && rd32(hdr + OFF_SEQ) == idx + 1 &&
           rd32(hdr + OFF_LEN) <= AUDIO_CACHE_PAYLOAD_MAX;
}

int audio_cache_mount(audio_cache_t *c, audio_cache_flash_t flash, uint32_t num_sectors,
                      audio_cache_mount_info_t *info) {
    memset(c, 0, sizeof *c);
    c->flash = flash;
    c->num_sectors = num_sectors;

    uint8_t hdr[AUDIO_CACHE_HDR_SIZE];
    uint32_t n = 0;
    uint32_t total = 0;
    while (n < num_sectors) {
        int r = read_header(c, n, hdr);
        if (r < 0) return r;
        if (!header_ok(hdr, n)) break;
        total += rd32(hdr + OFF_LEN);
        n++;
    }

    // 只有最后一个扇区可能因掉电残缺，整扇区校验它即可。
    if (n > 0) {
        uint32_t last = n - 1;
        uint32_t last_len = 0;
        int r = read_header(c, last, hdr);
        if (r < 0) return r;
        last_len = rd32(hdr + OFF_LEN);
        r = c->flash.read(c->flash.ctx, sector_off(last), c->img, AUDIO_CACHE_HDR_SIZE + last_len);
        if (r < 0) return r;
        if (sector_crc(c->img, last_len) != rd32(c->img + OFF_CRC)) {
            n = last;
            total -= last_len;
        }
    }

    c->next_sector = n;
    if (n > 0) {
        int r = read_header(c, n - 1, hdr);
        if (r < 0) return r;
        c->session = rd32(hdr + OFF_SESS);
    }
    c->buf_len = 0;

    if (info) {
        info->sectors = n;
        info->payload_bytes = total;
        info->last_session = c->session;
    }
    return 0;
}

static int flush_sector(audio_cache_t *c) {
    if (c->buf_len == 0) return 0;
    if (c->next_sector >= c->num_sectors) return AUDIO_CACHE_ERR_FULL;

    uint8_t *img = c->img;
    memset(img, 0, AUDIO_CACHE_HDR_SIZE);
    wr32(img, HDR_MAGIC);
    wr32(img + OFF_SEQ, c->next_sector + 1);
    wr32(img + OFF_SESS, c->session);
    wr32(img + OFF_LEN, c->buf_len);
    wr32(img + OFF_CRC, sector_crc(img, c->buf_len));

    uint32_t off = sector_off(c->next_sector);
    int r = c->flash.erase(c->flash.ctx, off, AUDIO_CACHE_SECTOR);
    if (r < 0) return r;
    r = c->flash.write(c->flash.ctx, off, img, AUDIO_CACHE_HDR_SIZE + c->buf_len);
    if (r < 0) return r;

    c->next_sector++;
    c->buf_len = 0;
    return 0;
}

int audio_cache_flush(audio_cache_t *c) {
    return flush_sector(c);
}

int audio_cache_begin_session(audio_cache_t *c, uint32_t session) {
    int r = flush_sector(c);
    if (r < 0) return r;
    c->session = session;
    return 0;
}

int audio_cache_append(audio_cache_t *c, const uint8_t *data, size_t len) {
    while (len > 0) {
        size_t room = AUDIO_CACHE_PAYLOAD_MAX - c->buf_len;
        size_t n = len < room ? len : room;
        memcpy(c->img + AUDIO_CACHE_HDR_SIZE + c->buf_len, data, n);
        c->buf_len += (uint32_t)n;
        data += n;
        len -= n;
        if (c->buf_len == AUDIO_CACHE_PAYLOAD_MAX) {
            int r = flush_sector(c);
            if (r < 0) return r;
        }
    }
    return 0;
}
