#pragma once
#include <stdbool.h>
#include <stdint.h>

void app_ble_init(void);
bool app_ble_ready(void);
bool app_ble_connected(void);
uint32_t app_ble_passkey(void); // UINT32_MAX if no pairing prompt
void app_ble_allow_pairing(void);
void app_ble_advertise_for(uint32_t seconds); // 立即重试广播（广播一直开着，此接口仅取消节流）
void app_ble_reclaim_if_idle(void);
void app_ble_notify_recording(bool active, uint32_t elapsed_sec);
