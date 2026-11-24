// 录音：Opus 编码后写缓存。按键开关，单次最长 2 小时，缓存满自动停。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// 初始化音频 codec 与编码器。失败报 E400/E401。
esp_err_t app_record_init(void);

// 开始录音。单次最长 MAX_SEC，缓存满时提前结束。
// 返回 ESP_ERR_INVALID_STATE 表示已在录，ESP_ERR_NO_MEM 表示缓存满。
esp_err_t app_record_start(void);

// 请求停止录音，等编码任务收尾（最多 2 秒）。
void app_record_stop(void);

bool app_record_active(void);

// 本次录音已进行的秒数。
uint32_t app_record_elapsed_sec(void);

// 本次录音已写入的字节数。
uint32_t app_record_bytes(void);

// 最近一帧麦克风音量（0..100），未在录时返回 0。
uint8_t app_record_level(void);

// 当前会话号的 8 位十六进制串，供 UI/日志。未在录时返回上一次的。
const char *app_record_session_hex(void);

// 录音状态变化回调（开始/停止/自动结束）。在录音任务上下文调用，勿阻塞。
typedef void (*app_record_cb_t)(bool active, void *user);
void app_record_set_callback(app_record_cb_t cb, void *user);