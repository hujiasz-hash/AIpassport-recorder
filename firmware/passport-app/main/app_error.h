// 统一错误码与日志。所有模块只通过这里报错，保证串口和屏幕口径一致。
#pragma once

#include <stdint.h>

// 错误码定义见 docs/design.md 第 9 节。数值稳定，不要重排。
typedef enum {
    APP_OK = 0,
    APP_E100_NO_WIFI_CONFIG = 100,
    APP_E101_WIFI_FAIL = 101,
    APP_E200_DNS_FAIL = 200,
    APP_E201_TLS_FAIL = 201,
    APP_E202_TIMEOUT = 202,
    APP_E203_NO_TIME = 203,
    APP_E300_SERVER_REJECT = 300,
    APP_E301_TOKEN_INVALID = 301,
    APP_E400_INIT = 400,        // 音频 / 显示 / 按键 / 配网等初始化失败，屏幕统一显示"E400 初始化失败"
    APP_E400_CODEC_INIT = APP_E400_INIT,   // 兼容旧名，新代码用 APP_E400_INIT
    APP_E401_ENCODER = 401,
    APP_E402_NO_SPACE = 402,
    APP_E500_CACHE_FULL = 500,
    APP_E501_FLASH = 501,
    APP_E600_LOW_BATTERY = 600,
} app_err_t;

// 错误发生的位置，用于日志定位。
typedef enum {
    APP_SRC_BOOT = 0,
    APP_SRC_CONFIG,
    APP_SRC_NET,
    APP_SRC_UPLOAD,
    APP_SRC_RECORD,
    APP_SRC_PROV,
    APP_SRC_UI,
} app_src_t;

// 上报一个错误：写串口（含堆、时间、来源），并更新屏幕错误页。
// 同一错误码 5 秒内重复只记一次，避免网络抖动刷屏。
void app_error_report(app_err_t err, app_src_t src, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

// 屏幕上的简短文本，如 "E201 TLS 失败"。未知码返回 "E? 未知错误"。
const char *app_error_short(app_err_t err);

// 打印一条结构化状态日志（非错误），格式统一便于 grep。
void app_log(const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

// 最近一次错误，供 UI 查询。
app_err_t app_error_last(void);

// 错误发生序号：每记录一条新错误（未被 5 秒去重吞掉）加一。
// UI 用它判断"有没有新错误要显示"，避免每秒都弹一次错误页。
uint32_t app_error_seq(void);

void app_error_clear(void);