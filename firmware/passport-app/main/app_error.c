#include "app_error.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "app";

static const char *SRC_NAME[] = {
    [APP_SRC_BOOT] = "boot",
    [APP_SRC_CONFIG] = "config",
    [APP_SRC_NET] = "net",
    [APP_SRC_UPLOAD] = "upload",
    [APP_SRC_RECORD] = "record",
    [APP_SRC_PROV] = "prov",
    [APP_SRC_UI] = "ui",
};

typedef struct {
    app_err_t code;
    const char *text;
} err_text_t;

static const err_text_t ERR_TEXT[] = {
    {APP_E100_NO_WIFI_CONFIG, "E100 未配置 Wi-Fi"},
    {APP_E101_WIFI_FAIL, "E101 Wi-Fi 连接失败"},
    {APP_E200_DNS_FAIL, "E200 DNS 解析失败"},
    {APP_E201_TLS_FAIL, "E201 TLS 证书校验失败"},
    {APP_E202_TIMEOUT, "E202 连接或上传超时"},
    {APP_E203_NO_TIME, "E203 时间未同步"},
    {APP_E300_SERVER_REJECT, "E300 服务端拒绝"},
    {APP_E301_TOKEN_INVALID, "E301 Token 无效"},
    {APP_E400_INIT, "E400 初始化失败"},
    {APP_E401_ENCODER, "E401 编码失败"},
    {APP_E402_NO_SPACE, "E402 空间不足"},
    {APP_E500_CACHE_FULL, "E500 缓存已满"},
    {APP_E501_FLASH, "E501 Flash 读写失败"},
    {APP_E600_LOW_BATTERY, "E600 电量低"},
};

#define ERR_COUNT (sizeof(ERR_TEXT) / sizeof(ERR_TEXT[0]))

// 去重：同一错误码在窗口内只打一次完整日志。
#define DEDUP_WINDOW_US (5LL * 1000 * 1000)
static app_err_t s_last_code = APP_OK;
static int64_t s_last_us = 0;
static app_err_t s_last_error = APP_OK;
static volatile uint32_t s_seq;

static const char *src_name(app_src_t src) {
    if ((size_t)src < sizeof(SRC_NAME) / sizeof(SRC_NAME[0]) && SRC_NAME[src]) {
        return SRC_NAME[src];
    }
    return "?";
}

const char *app_error_short(app_err_t err) {
    for (size_t i = 0; i < ERR_COUNT; i++) {
        if (ERR_TEXT[i].code == err) return ERR_TEXT[i].text;
    }
    return "E? 未知错误";
}

void app_log(const char *tag, const char *fmt, ...) {
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    // 用固定前缀，方便 grep 和从串口日志里切出干净的结构化记录。
    ESP_LOGI(tag, "%s", buf);
}

void app_error_report(app_err_t err, app_src_t src, const char *fmt, ...) {
    int64_t now = esp_timer_get_time();

    // 320 而不是 160：上传失败的 detail 要同时放下 session_id（28）、errno 文本、
    // 设备 IP、网关、目标 URL（最长 100+10），160 字节会把 URL 从中间截断，
    // 恰好把最该看的那一段砍掉。
    char detail[320] = "";
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(detail, sizeof detail, fmt, ap);
        va_end(ap);
    }

    s_last_error = err;

    bool dup = (err == s_last_code) && (now - s_last_us < DEDUP_WINDOW_US);
    s_last_code = err;
    s_last_us = now;
    if (dup) return;
    s_seq++;

    ESP_LOGE(TAG, "ERR code=%d src=%s detail=\"%s\" heap=%u min_heap=%u",
             (int)err, src_name(src), detail,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
}

app_err_t app_error_last(void) {
    return s_last_error;
}

uint32_t app_error_seq(void) {
    return s_seq;
}

void app_error_clear(void) {
    s_last_error = APP_OK;
}