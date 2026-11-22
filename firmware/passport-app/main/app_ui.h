// 屏幕 UI：主页（时间/电量/待传量）、录音页、配网页、错误页。
// 屏幕显示简短文本；详细原因只在串口日志里。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    APP_PAGE_HOME = 0,
    APP_PAGE_RECORDING,
    APP_PAGE_PROV,     // 网页配网页
    APP_PAGE_ERROR,
} app_page_t;

// 初始化显示、LVGL、电池。失败不阻塞主流程（没有屏幕也能录音）。
esp_err_t app_ui_init(void);

// 切页。线程安全。
void app_ui_set_page(app_page_t page);

// 刷新主页状态（每秒调一次即可）。
void app_ui_refresh_home(void);

// 刷新录音页。
void app_ui_refresh_record(uint32_t elapsed_sec, uint32_t bytes);

// 显示错误页：顶部一行简短错误码文本，下面一行"按 OK 返回"。
// 同时按 10 秒自动回主页，避免一直占着屏幕。
void app_ui_show_error(const char *short_text);

// 背光 0..100。
void app_ui_backlight(uint8_t percent);

// 当前页。
app_page_t app_ui_page(void);

// 最近一次交互的时间戳（微秒），用于 10 秒无操作熄屏。
int64_t app_ui_last_input_us(void);
void app_ui_note_input(void);
void app_ui_idle_tick(void);

// 配网页显示热点名（AIPassport-xxxx）。
void app_ui_set_prov_name(const char *name);

// 刷新配网页的状态行。connect_failed 来自 app_net_state()==APP_NET_FAILED。
void app_ui_refresh_prov(bool client_connected, bool cfg_ready, bool connect_failed);

// 录音指示灯（红灯呼吸）。active=true 开启。
void app_ui_rec_led(bool active);