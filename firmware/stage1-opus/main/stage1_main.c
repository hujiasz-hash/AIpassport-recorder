// 阶段 1：Opus 可行性。ES8311 录 16 kHz 单声道 → 定点 libopus 12 kbps → Ogg Opus 写入 voice 分区。
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "bsp_audio.h"
#include "bsp_display.h"
#include "lvgl.h"
#include <stdio.h>
#include "esp_heap_caps.h"
#include "esp_event.h"
#include "esp_log.h"
#include "freertos/event_groups.h"
#include "wifi_test_secret.h"
#include "esp_netif.h"
#include "esp_partition.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "opus.h"

static const char *TAG = "stage1";
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
    lv_label_set_text(s_label, "STAGE1\nboot");
    lv_screen_load(scr);
    bsp_lvgl_unlock();
    bsp_display_backlight(100);
}


#define SAMPLE_RATE       16000
#define FRAME_SAMPLES     320
#define BITRATE_BPS       12000
#define RECORD_SEC        30
#define FRAMES_TOTAL      (RECORD_SEC * 50)
#define PKTS_PER_PAGE     5
#define MAX_PKT_BYTES     400
#define PAGE_MAX          (27 + 255 + PKTS_PER_PAGE * MAX_PKT_BYTES)
#define OGG_SERIAL        0x5A5A1234u
#define GRANULE_PER_FRAME 960
#define VOICE_SUBTYPE     0x40
#define SECTOR            4096

static uint32_t s_crc_tab[256];

typedef struct {
    const esp_partition_t *part;
    uint32_t off;
    uint32_t erased;
    uint32_t seq;
    uint64_t granule;
    uint8_t  lace[255];
    uint8_t  nlace;
    uint8_t  npkt;
    uint32_t dlen;
    uint8_t  data[PKTS_PER_PAGE * MAX_PKT_BYTES];
    uint8_t  page[PAGE_MAX];
} ogg_t;

static void put_le(uint8_t *p, uint32_t v, int n) {
    for (int i = 0; i < n; i++) p[i] = (uint8_t)(v >> (8 * i));
}

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

static esp_err_t flash_write(ogg_t *w, const uint8_t *buf, uint32_t n) {
    if (w->off + n > w->part->size) return ESP_ERR_NO_MEM;
    while (w->off + n > w->erased) {
        esp_err_t err = esp_partition_erase_range(w->part, w->erased, SECTOR);
        if (err != ESP_OK) return err;
        w->erased += SECTOR;
    }
    esp_err_t err = esp_partition_write(w->part, w->off, buf, n);
    if (err == ESP_OK) w->off += n;
    return err;
}

static esp_err_t ogg_flush(ogg_t *w, uint8_t htype) {
    uint8_t *p = w->page;
    memcpy(p, "OggS", 4);
    p[4] = 0;
    p[5] = htype;
    put_le(p + 6, (uint32_t)w->granule, 4);
    put_le(p + 10, (uint32_t)(w->granule >> 32), 4);
    put_le(p + 14, OGG_SERIAL, 4);
    put_le(p + 18, w->seq++, 4);
    put_le(p + 22, 0, 4);
    p[26] = w->nlace;
    memcpy(p + 27, w->lace, w->nlace);
    memcpy(p + 27 + w->nlace, w->data, w->dlen);
    uint32_t total = 27 + w->nlace + w->dlen;
    put_le(p + 22, crc_update(0, p, total), 4);
    esp_err_t err = flash_write(w, p, total);
    w->nlace = 0;
    w->npkt = 0;
    w->dlen = 0;
    return err;
}

static esp_err_t ogg_put(ogg_t *w, const uint8_t *pkt, uint32_t len, uint32_t granule_inc) {
    uint32_t need = len / 255 + 1;
    if (w->npkt == PKTS_PER_PAGE || w->nlace + need > 255 || w->dlen + len > sizeof w->data) {
        esp_err_t err = ogg_flush(w, 0);
        if (err != ESP_OK) return err;
    }
    for (uint32_t r = len; r >= 255; r -= 255) w->lace[w->nlace++] = 255;
    w->lace[w->nlace++] = (uint8_t)(len % 255);
    memcpy(w->data + w->dlen, pkt, len);
    w->dlen += len;
    w->granule += granule_inc;
    w->npkt++;
    return ESP_OK;
}

static void record_task(void *arg) {
    (void)arg;
    if (bsp_audio_init() != ESP_OK || bsp_audio_set_format(SAMPLE_RATE, 16, 1) != ESP_OK) {
        ESP_LOGE(TAG, "audio init failed");
        vTaskDelete(NULL);
    }
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, VOICE_SUBTYPE, "voice");
    if (!part) {
        ESP_LOGE(TAG, "voice partition not found");
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
    ESP_LOGI(TAG, "encoder size=%u lookahead=%d free8=%u", (unsigned)opus_encoder_get_size(1),
             lookahead, (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));

    ogg_t *w = calloc(1, sizeof *w);
    if (!w) {
        ESP_LOGE(TAG, "ogg alloc failed");
        vTaskDelete(NULL);
    }
    w->part = part;

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
    memcpy(tags + 12, "stage1", 6);
    put_le(tags + 18, 0, 4);

    w->granule = 0;
    ogg_put(w, head, sizeof head, 0);
    ogg_flush(w, 0x02);
    ogg_put(w, tags, sizeof tags, 0);
    ogg_flush(w, 0x00);

    int16_t pcm[FRAME_SAMPLES];
    uint8_t pkt[MAX_PKT_BYTES];
    int64_t enc_sum = 0, enc_max = 0, t_start = esp_timer_get_time();
    uint32_t frames = 0;
    int peak = 0;
    for (uint32_t f = 0; f < FRAMES_TOTAL; f++) {
        if (bsp_audio_read(pcm, sizeof pcm) != ESP_OK) {
            ESP_LOGE(TAG, "audio read failed at frame %u", (unsigned)f);
            break;
        }
        for (int i = 0; i < FRAME_SAMPLES; i++) {
            int a = pcm[i] < 0 ? -pcm[i] : pcm[i];
            if (a > peak) peak = a;
        }
        int64_t t0 = esp_timer_get_time();
        int n = opus_encode(enc, pcm, FRAME_SAMPLES, pkt, sizeof pkt);
        int64_t dt = esp_timer_get_time() - t0;
        if (n < 0) {
            ESP_LOGE(TAG, "opus_encode err=%d", n);
            break;
        }
        enc_sum += dt;
        if (dt > enc_max) enc_max = dt;
        frames++;
        vTaskDelay(1);
        if (ogg_put(w, pkt, (uint32_t)n, GRANULE_PER_FRAME) != ESP_OK) {
            ESP_LOGW(TAG, "partition full at frame %u", (unsigned)f);
            break;
        }
        if ((f + 1) % 50 == 0) {
            ESP_LOGI(TAG, "t=%us peak=%d off=%u", (unsigned)((f + 1) / 50), peak, (unsigned)w->off);
            char buf[48];
            snprintf(buf, sizeof buf, "REC %us\n%u bytes", (unsigned)((f + 1) / 50), (unsigned)w->off);
            status_set(buf);
            peak = 0;
        }
    }
    ogg_flush(w, 0x04);
    int64_t wall = esp_timer_get_time() - t_start;

    ESP_LOGI(TAG, "frames=%u enc_avg_us=%lld enc_max_us=%lld wall_ms=%lld",
             (unsigned)frames, frames ? enc_sum / frames : 0, enc_max, wall / 1000);
    ESP_LOGI(TAG, "min_free8=%u largest_free8=%u",
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    ESP_LOGI(TAG, "OGG_BYTES=%u PART_OFF=0x%x", (unsigned)w->off, (unsigned)part->address);
    char done[48];
    snprintf(done, sizeof done, "DONE\n%u bytes", (unsigned)w->off);
    status_set(done);

    opus_encoder_destroy(enc);
    free(w);
    vTaskDelete(NULL);
}

#define WIFI_GOT_IP_BIT BIT0
#define WIFI_FAIL_BIT   BIT1
#define WIFI_MAX_RETRY  5

static EventGroupHandle_t s_wifi_ev;
static int s_retry;

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "wifi disconnected reason=%d", ((wifi_event_sta_disconnected_t *)data)->reason);
        if (s_retry < WIFI_MAX_RETRY) {
            s_retry++;
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_ev, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_retry = 0;
        xEventGroupSetBits(s_wifi_ev, WIFI_GOT_IP_BIT);
    }
}

static esp_err_t wifi_connect_test(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_wifi_ev = xEventGroupCreate();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, WIFI_TEST_SSID, sizeof wc.sta.ssid);
    strlcpy((char *)wc.sta.password, WIFI_TEST_PASSWORD, sizeof wc.sta.password);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
    EventBits_t bits = xEventGroupWaitBits(s_wifi_ev, WIFI_GOT_IP_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(30000));
    if (!(bits & WIFI_GOT_IP_BIT)) {
        ESP_LOGE(TAG, "wifi connect failed bits=0x%x", (unsigned)bits);
        status_set("WIFI FAILED");
        return ESP_FAIL;
    }
    status_set("WIFI OK");
    ESP_LOGI(TAG, "wifi connected free8=%u min_free8=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
    return ESP_OK;
}

void app_main(void) {
    crc_init();
    screen_init();
    vTaskDelay(pdMS_TO_TICKS(3000));
    wifi_connect_test();
    xTaskCreate(record_task, "record", 24576, NULL, 5, NULL);
}
