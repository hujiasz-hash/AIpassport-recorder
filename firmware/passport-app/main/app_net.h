// Wi-Fi STA + SNTP。录不录音都先把 Wi-Fi 栈初始化好：BluFi 配网也要用它。
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

typedef enum {
    APP_NET_OFF = 0,       // 栈没起来或没配 Wi-Fi
    APP_NET_CONNECTING,
    APP_NET_CONNECTED,     // 有 IP
    APP_NET_FAILED,        // 连续失败，等重试
} app_net_state_t;

// 初始化 netif、事件循环、Wi-Fi STA。不主动连接，等 app_net_apply_credentials()。
void app_net_init(void);

// 用当前 NVS 里的凭据连接。配网成功后调用。会断开旧连接重新连。
void app_net_apply_credentials(void);

// 等拿到 IP。timeout_ms<=0 表示只看当前状态不平。
bool app_net_wait_ip(int timeout_ms);

// 等 SNTP 同步。timeout_ms<=0 只看当前状态。
bool app_net_wait_time(int timeout_ms);

// 当前状态。
app_net_state_t app_net_state(void);

// 最近一次断开的原因（wifi_err_reason_t）。0 表示还没失败过。
// 配网页用它把"连不上"翻译成用户能看懂的话。
int app_net_last_reason(void);

// 本轮失败次数（连上就清零）。
uint32_t app_net_fail_count(void);

// 当前是否已同步过时间（同一次上电内）。
bool app_net_time_valid(void);

// 现在的时间（本地时区）。未同步时返回 0。
time_t app_net_now(void);

// ISO 形式 "2026-11-22T17:20:00"，供日志与 UI。buf 至少 20 字节。
void app_net_now_iso(char *buf, size_t cap);

// 是否把 Wi-Fi 停在省电模式（上传时会临时切到 WIFI_PS_NONE）。
void app_net_set_power_save(bool on);