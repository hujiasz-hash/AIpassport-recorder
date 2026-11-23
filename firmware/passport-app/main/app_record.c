#include "app_record.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_error.h"
#include "app_net.h"
#include "app_ogg.h"
#include "app_store.h"
#include "audio_cache.h"   // AUDIO_CACHE_ERR_FULL
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "opus.h"

#define TAG "record"

// 单次录音上限 2 小时 = 7200 s × 50 帧/s，约 12.5 MB（实测 13.85 kbit/s）。
// 缓存分区 3.6 MB 只够约 36 分钟，但联网时上传会边录边回收，实际能录多久取决于
// Wi-Fi 可达性。这里只做一个防止无限录下去的硬闸，不按缓存容量算：
// 网络断了缓存先满，由 E500 截断，比按容量提前停录更符合预期。
#define MAX_SEC   7200
#define MAX_FRAMES (MAX_SEC * (1000 / 20))

// 电量低于此值拒绝开录（E600）。
#define MIN_SOC_TO_RECORD 15

// 状态每 10 秒刷一次给 UI，别每帧都碰 LVGL。
#define STATUS_INTERVAL_US (10LL * 1000 * 1000)

static OpusEncoder *s_enc;
static TaskHandle_t s_task;
static volatile bool s_active;
static volatile bool s_stop_req;
static volatile uint32_t s_frames;
static volatile uint32_t s_bytes;
static uint32_t s_session;
static char s_session_hex[9];
static int64_t s_start_us;
static app_record_cb_t s_cb;
static void *s_cb_user;
static ogg_writer_t *s_writer;
static bool s_inited;

static int store_sink(void *ctx, const void *data, size_t n) {
    (void)ctx;
    int r = app_store_append(data, n);
    if (r == 0) s_bytes += (uint32_t)n;
    return r;
}

static void notify(bool active) {
    if (s_cb) s_cb(active, s_cb_user);
}

esp_err_t app_record_init(void) {
    if (s_inited) return ESP_OK;

    ogg_init();

    esp_err_t err = bsp_audio_init();
    if (err != ESP_OK) {
        app_error_report(APP_E400_INIT, APP_SRC_RECORD, "bsp_audio_init: %s",
                         esp_err_to_name(err));
        return err;
    }
    err = bsp_audio_set_format(OGG_SAMPLE_RATE, 16, 1);
    if (err != ESP_OK) {
        app_error_report(APP_E400_INIT, APP_SRC_RECORD, "set_format: %s",
                         esp_err_to_name(err));
        return err;
    }

    s_writer = calloc(1, sizeof *s_writer);
    if (!s_writer) {
        app_error_report(APP_E402_NO_SPACE, APP_SRC_RECORD, "ogg writer alloc failed");
        return ESP_ERR_NO_MEM;
    }
    s_inited = true;
    app_log(TAG, "audio ready %u Hz mono", (unsigned)OGG_SAMPLE_RATE);
    return ESP_OK;
}

static void record_task(void *arg) {
    (void)arg;
    int err = OPUS_OK;
    s_enc = opus_encoder_create(OGG_SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, &err);
    if (!s_enc || err != OPUS_OK) {
        app_error_report(APP_E401_ENCODER, APP_SRC_RECORD, "encoder_create err=%d", err);
        s_active = false;
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    opus_encoder_ctl(s_enc, OPUS_SET_BITRATE(OGG_BITRATE_BPS));
    opus_encoder_ctl(s_enc, OPUS_SET_COMPLEXITY(2));
    opus_encoder_ctl(s_enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

    if (app_store_begin(s_session, (uint32_t)app_net_now()) < 0) {
        // begin 失败只有两种：缓存满或 flash 读写失败，两者都已单独报过码。
        opus_encoder_destroy(s_enc);
        s_enc = NULL;
        s_active = false;
        s_task = NULL;
        notify(false);
        vTaskDelete(NULL);
        return;
    }

    int r = ogg_write_headers(s_writer, s_session, store_sink, NULL);
    if (r < 0) {
        app_error_report(r == -100 ? APP_E500_CACHE_FULL : APP_E501_FLASH, APP_SRC_RECORD,
                         "write ogg headers r=%d", r);
        app_store_end(3, 0);
        opus_encoder_destroy(s_enc);
        s_enc = NULL;
        s_active = false;
        s_task = NULL;
        notify(false);
        vTaskDelete(NULL);
        return;
    }

    char iso[24];
    app_net_now_iso(iso, sizeof iso);
    app_log(TAG, "session=%08x start ts=%s", (unsigned)s_session, iso);
    notify(true);

    int16_t pcm[OGG_FRAME_SAMPLES];
    uint8_t pkt[OGG_MAX_PKT_BYTES];
    int64_t last_status = esp_timer_get_time();
    uint8_t end_reason = 1; // normal, unless recording or final write fails

    for (uint32_t f = 0; f < MAX_FRAMES; f++) {
        if (s_stop_req) break;

        esp_err_t aerr = bsp_audio_read(pcm, sizeof pcm);
        if (aerr != ESP_OK) {
            app_error_report(APP_E400_INIT, APP_SRC_RECORD, "audio read frame %u: %s",
                             (unsigned)f, esp_err_to_name(aerr));
            end_reason = 3;
            break;
        }
        int n = opus_encode(s_enc, pcm, OGG_FRAME_SAMPLES, pkt, sizeof pkt);
        if (n < 0) {
            app_error_report(APP_E401_ENCODER, APP_SRC_RECORD, "opus_encode err=%d at frame %u",
                             n, (unsigned)f);
            end_reason = 3;
            break;
        }
        s_frames++;

        // 每帧让出一次 CPU：单核，编码已占约 60%，不让出会饿死 IDLE 触发看门狗。
        vTaskDelay(1);

        r = ogg_put_packet(s_writer, pkt, (uint32_t)n, store_sink, NULL);
        if (r == AUDIO_CACHE_ERR_FULL) {
            app_error_report(APP_E500_CACHE_FULL, APP_SRC_RECORD, "cache full at frame %u",
                             (unsigned)f);
            end_reason = 2;
            break;
        }
        if (r < 0) {
            app_error_report(APP_E501_FLASH, APP_SRC_RECORD, "cache write r=%d frame %u",
                             r, (unsigned)f);
            end_reason = 3;
            break;
        }

        int64_t now = esp_timer_get_time();
        if (now - last_status >= STATUS_INTERVAL_US) {
            last_status = now;
            app_log(TAG, "t=%us frames=%u bytes=%u sectors=%u/%u",
                    (unsigned)((f + 1) / 50), (unsigned)s_frames, (unsigned)s_bytes,
                    (unsigned)app_store_used_sectors(), (unsigned)app_store_total_sectors());
        }
    }

    // 落尾页。缓存满时尾页也可能写不进去，忽略错误——已落盘的数据仍然可播。
    int tail = ogg_finish(s_writer, end_reason == 1, store_sink, NULL);
    if (tail < 0) end_reason = (tail == AUDIO_CACHE_ERR_FULL) ? 2 : 3;
    app_store_end(end_reason, (uint32_t)((esp_timer_get_time() - s_start_us) / 1000));

    char end_iso[24];
    app_net_now_iso(end_iso, sizeof end_iso);
    app_log(TAG, "session=%08x stop frames=%u bytes=%u elapsed=%llds at %s",
            (unsigned)s_session, (unsigned)s_frames, (unsigned)s_bytes,
            (long long)((esp_timer_get_time() - s_start_us) / 1000000), end_iso);

    opus_encoder_destroy(s_enc);
    s_enc = NULL;
    s_active = false;
    s_task = NULL;
    notify(false);
    vTaskDelete(NULL);
}

esp_err_t app_record_start(void) {
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    if (s_active) return ESP_ERR_INVALID_STATE;

    const char *why = NULL;
    if (app_store_full()) {
        app_error_report(APP_E500_CACHE_FULL, APP_SRC_RECORD, "start refused, cache full");
        why = "E500";
    }
    int soc = bsp_battery_soc();
    if (!why && soc >= 0 && soc < MIN_SOC_TO_RECORD) {
        app_error_report(APP_E600_LOW_BATTERY, APP_SRC_RECORD, "soc=%d < %d", soc,
                         MIN_SOC_TO_RECORD);
        why = "E600";
    }
    if (why) return ESP_ERR_INVALID_STATE;

    s_session = esp_random();
    if (s_session == 0) s_session = 1;
    snprintf(s_session_hex, sizeof s_session_hex, "%08x", (unsigned)s_session);
    s_frames = 0;
    s_bytes = 0;
    s_stop_req = false;
    s_start_us = esp_timer_get_time();
    s_active = true;

    // 栈 24 KB + 优先级 5，与阶段 2 实测通过的配置一致。编码期是 CPU 热点，
    // 上传任务优先级更低（3），不会抢它。
    if (xTaskCreate(record_task, "record", 24576, NULL, 5, &s_task) != pdPASS) {
        s_active = false;
        app_error_report(APP_E402_NO_SPACE, APP_SRC_RECORD, "task create failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void app_record_stop(void) {
    if (!s_active || !s_task) return;
    s_stop_req = true;
    // 等任务收尾：最后一个包 + 尾页 + flush 扇区，通常 100 ms 内完成。
    for (int i = 0; i < 20 && s_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (s_task) {
        app_error_report(APP_E202_TIMEOUT, APP_SRC_RECORD, "record task did not stop in 2s");
    }
}

bool app_record_active(void) {
    return s_active;
}

uint32_t app_record_elapsed_sec(void) {
    if (!s_active) return 0;
    return (uint32_t)((esp_timer_get_time() - s_start_us) / 1000000);
}

uint32_t app_record_bytes(void) {
    return s_bytes;
}

const char *app_record_session_hex(void) {
    return s_session_hex;
}

void app_record_set_callback(app_record_cb_t cb, void *user) {
    s_cb = cb;
    s_cb_user = user;
}