#pragma once
#include <stdbool.h>
#include <stdint.h>

#define APP_BLE_KEY_NEXT 1u
#define APP_BLE_KEY_PREV 2u

void app_ble_init(void);
bool app_ble_ready(void);
bool app_ble_connected(void);
bool app_ble_pairing_open(void); // 是否处于允许新配对且尚未绑定的窗口期
uint32_t app_ble_passkey(void);  // UINT32_MAX if no pairing prompt
void app_ble_allow_pairing(void);
void app_ble_cancel_pairing(void);
void app_ble_advertise_for(uint32_t seconds); // 立即重试广播（广播一直开着，此接口仅取消节流）
void app_ble_reclaim_if_idle(void);
void app_ble_notify_recording(bool active, uint32_t elapsed_sec);
bool app_ble_notify_key(uint8_t action, uint8_t count);
