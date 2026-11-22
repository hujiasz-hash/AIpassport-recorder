// 网页配网：设备开热点，手机连上后用浏览器填表，不需要装任何 App。
//
// 流程：
//   1. 未配网时开 SoftAP（SSID 带设备号后 4 位，无密码，IP 192.168.4.1）
//   2. 内置 HTTP server 提供配置页和 /save、/status
//   3. 收到配置写 NVS → 关热点 → 切 STA 连 Wi-Fi → 页面轮询 /status 拿结果
//
// 相比 BluFi 省掉整个蓝牙协议栈（约 50 KB 内部 RAM），C3 无 PSRAM，这笔内存很关键。
#pragma once

#include <stdbool.h>

// 启动热点和 HTTP server。Wi-Fi 栈必须已初始化（app_net_init）。
void app_prov_start(void);

// 关热点、停 server。配网完成后调用。
void app_prov_stop(void);

bool app_prov_active(void);

// 热点名（含设备号后 4 位），UI 用来告诉用户连哪个。
const char *app_prov_ap_name(void);

// 是否收到过完整配置（Wi-Fi + NAS 地址都齐）。
bool app_prov_got_cfg(void);

// 是否有手机正连着热点。配网页用它显示连接状态。
bool app_prov_client_connected(void);

// 主循环每秒调一次：配网完成且手机断开一段时间后自动关热点省电。
void app_prov_tick(void);