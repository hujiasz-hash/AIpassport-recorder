// 阶段 2：录音写入音频缓存。开机挂载缓存（恢复写指针），录音中数据按扇区落盘，掉电后重启可恢复。
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "audio_cache.h"
#include "bsp_audio.h"
#include "bsp_display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "opus.h"

static const char *TAG = "stage2";
static lv_obj_t *s_label;

static void status_set(const char *text) {
    if (!s_label || !bsp_lvgl_lock(500)) return;
    lv_label_set_text(s_label, text);
    bsp_lvgl_unlock();
}

static void screen_init(void) {
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "display init failed");
        return;
    }
    if (!bsp_lvgl_lock(1000)) return;
    lv_obj_t *scr = lv_obj_create(NULL);
    s_label = lv_label_create(scr);
    lv_obj_set_style_text_font(s_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(s_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_label);
    lv_label_set_text(s_label, "STAGE2\nboot");
    lv_screen_load(scr);
    bsp_lvgl_unlock();
    bsp_display_backlight(100);
}

#define SAMPLE_RATE       16000
#define FRAME_SAMPLES     320
#define BITRATE_BPS       12000
#define RECORD_SEC        7200
#define FRAMES_TOTAL      (RECORD_SEC * 50)
#define STATUS_EVERY_SEC  10
#define PKTS_PER_PAGE     5
#define MAX_PKT_BYTES     400
#define PAGE_MAX          (27 + 255 + PKTS_PER_PAGE * MAX_PKT_BYTES)
#define GRANULE_PER_FRAME 960
#define VOICE_SUBTYPE     0x40

static const esp_partition_t *s_cache_part;
static audio_cache_t s_cache;

static int cache_read(void *ctx, uint32_t off, void *buf, size_t n) {
    return esp_partition_read((const esp_partition_t *)ctx, off, buf, n) == ESP_OK ? 0 : -1;
}

static int cache_erase(void *ctx, uint32_t off, uint32_t n) {
    return esp_partition_erase_range((const esp_partition_t *)ctx, off, n) == ESP_OK ? 0 : -1;
}

static int cache_write(void *ctx, uint32_t off, const void *buf, size_t n) {
    return esp_partition_write((const esp_partition_t *)ctx, off, buf, n) == ESP_OK ? 0 : -1;
}

static void put_le(uint8_t *p, uint32_t v, int n) {
    for (int i = 0; i < n; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static uint32_t s_crc_tab[256];

static void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t r = i << 24;
        for (int j = 0; j < 8; j++) r = (r & 0x80000000u) ? (r << 1) ^ 0x04C11DB7u : (r << 1);
        s_crc_tab[i] = r;
    }
}

static uint32_t crc_update(uint32_t crc, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) crc = (crc << 8) ^ s_crc_tab[((crc >> 24) ^ p[i]) & 0xFF];
    return crc;
}

typedef struct {
    uint32_t serial;
    uint32_t seq;
    uint64_t granule;
    uint8_t lace[255];
    uint8_t nlace;
    uint8_t npkt;
    uint32_t dlen;
    uint8_t data[PKTS_PER_PAGE * MAX_PKT_BYTES];
    uint8_t page[PAGE_MAX];
} ogg_t;

static int ogg_flush(ogg_t *w, uint8_t htype) {
    uint8_t *p = w->page;
    memcpy(p, "OggS", 4);
    p[4] = 0;
    p[5] = htype;
    put_le(p + 6, (uint32_t)w->granule, 4);
    put_le(p + 10, (uint32_t)(w->granule >> 32), 4);
    put_le(p + 14, w->serial, 4);
    put_le(p + 18, w->seq++, 4);
    put_le(p + 22, 0, 4);
    p[26] = w->nlace;
    memcpy(p + 27, w->lace, w->nlace);
    memcpy(p + 27 + w->nlace, w->data, w->dlen);
    uint32_t total = 27 + w->nlace + w->dlen;
    put_le(p + 22, crc_update(0, p, total), 4);
    int r = audio_cache_append(&s_cache, p, total);
    w->nlace = 0;
    w->npkt = 0;
    w->dlen = 0;
    return r;
}

static int ogg_put(ogg_t *w, const uint8_t *pkt, uint32_t len, uint32_t granule_inc) {
    uint32_t need = len / 255 + 1;
    if (w->npkt == PKTS_PER_PAGE || w->nlace + need > 255 || w->dlen + len > sizeof w->data) {
        int r = ogg_flush(w, 0);
        if (r < 0) return r;
    }
    for (uint32_t r = len; r >= 255; r -= 255) w->lace[w->nlace++] = 255;
    w->lace[w->nlace++] = (uint8_t)(len % 255);
    memcpy(w->data + w->dlen, pkt, len);
    w->dlen += len;
    w->granule += granule_inc;
    w->npkt++;
    return 0;
}

static void record_task(void *arg) {
    (void)arg;
    if (bsp_audio_init() != ESP_OK || bsp_audio_set_format(SAMPLE_RATE, 16, 1) != ESP_OK) {
        ESP_LOGE(TAG, "audio init failed");
        vTaskDelete(NULL);
    }

    uint32_t session = esp_random();
    if (audio_cache_begin_session(&s_cache, session) < 0) {
        ESP_LOGE(TAG, "E501 cache session begin failed");
        vTaskDelete(NULL);
    }

    int err = OPUS_OK;
    OpusEncoder *enc = opus_encoder_create(SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, &err);
    if (!enc || err != OPUS_OK) {
        ESP_LOGE(TAG, "opus_encoder_create err=%d", err);
        vTaskDelete(NULL);
    }
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(BITRATE_BPS));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(2));
    opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_int32 lookahead = 0;
    opus_encoder_ctl(enc, OPUS_GET_LOOKAHEAD(&lookahead));

    ogg_t *w = calloc(1, sizeof *w);
    if (!w) {
        ESP_LOGE(TAG, "ogg alloc failed");
        vTaskDelete(NULL);
    }
    w->serial = session;

    uint8_t head[19];
    memcpy(head, "OpusHead", 8);
    head[8] = 1;
    head[9] = 1;
    put_le(head + 10, (uint32_t)lookahead * 3, 2);
    put_le(head + 12, SAMPLE_RATE, 4);
    put_le(head + 16, 0, 2);
    head[18] = 0;

    uint8_t tags[22];
    memcpy(tags, "OpusTags", 8);
    put_le(tags + 8, 6, 4);
    memcpy(tags + 12, "stage2", 6);
    put_le(tags + 18, 0, 4);

    ogg_put(w, head, sizeof head, 0);
    ogg_flush(w, 0x02);
    ogg_put(w, tags, sizeof tags, 0);
    ogg_flush(w, 0x00);

    ESP_LOGI(TAG, "session=%08x start", (unsigned)session);

    int16_t pcm[FRAME_SAMPLES];
    uint8_t pkt[400];
    uint32_t frames = 0;
    for (uint32_t f = 0; f < FRAMES_TOTAL; f++) {
        if (bsp_audio_read(pcm, sizeof pcm) != ESP_OK) {
            ESP_LOGE(TAG, "audio read failed at frame %u", (unsigned)f);
            break;
        }
        int n = opus_encode(enc, pcm, FRAME_SAMPLES, pkt, sizeof pkt);
        if (n < 0) {
            ESP_LOGE(TAG, "opus_encode err=%d", n);
            break;
        }
        frames++;
        vTaskDelay(1);
        int r = ogg_put(w, pkt, (uint32_t)n, GRANULE_PER_FRAME);
        if (r == AUDIO_CACHE_ERR_FULL) {
            ESP_LOGE(TAG, "E500 cache full at frame %u", (unsigned)f);
            break;
        }
        if (r < 0) {
            ESP_LOGE(TAG, "E501 cache write failed r=%d", r);
            break;
        }
        if ((f + 1) % (50 * STATUS_EVERY_SEC) == 0) {
            ESP_LOGI(TAG, "t=%us sectors=%u", (unsigned)((f + 1) / 50), (unsigned)s_cache.next_sector);
            char buf[64];
            snprintf(buf, sizeof buf, "REC %us\n%u KB", (unsigned)((f + 1) / 50),
                     (unsigned)(s_cache.next_sector * 4));
            status_set(buf);
        }
    }
    ogg_flush(w, 0x04);
    audio_cache_flush(&s_cache);
    ESP_LOGI(TAG, "session=%08x done frames=%u sectors=%u", (unsigned)session, (unsigned)frames,
             (unsigned)s_cache.next_sector);

    opus_encoder_destroy(enc);
    free(w);
    vTaskDelete(NULL);
}

static void cache_mount(void) {
    s_cache_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, VOICE_SUBTYPE, "cache");
    if (!s_cache_part) {
        ESP_LOGE(TAG, "cache partition not found");
        return;
    }
    audio_cache_flash_t flash = {
        .read = cache_read,
        .erase = cache_erase,
        .write = cache_write,
        .ctx = (void *)s_cache_part,
    };
    audio_cache_mount_info_t info;
    int r = audio_cache_mount(&s_cache, flash, s_cache_part->size / AUDIO_CACHE_SECTOR, &info);
    if (r < 0) {
        ESP_LOGE(TAG, "E501 cache mount failed r=%d", r);
        return;
    }
    ESP_LOGI(TAG, "cache mounted sectors=%u payload=%u last_session=%08x full=%d",
             (unsigned)info.sectors, (unsigned)info.payload_bytes, (unsigned)info.last_session,
             s_cache.next_sector >= s_cache.num_sectors);
}

void app_main(void) {
    crc_init();
    screen_init();
    cache_mount();
    xTaskCreate(record_task, "record", 24576, NULL, 5, NULL);
}
