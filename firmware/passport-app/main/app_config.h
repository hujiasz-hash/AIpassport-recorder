// 设备配置：Wi-Fi、NAS 地址、token、设备号，全部存 NVS。
// 配置来源两条路：网页配网写入（app_prov），或编译期 device_secret.h 兜底（仅开发用）。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CFG_SSID_MAX   33      // 32 + 结束符
#define CFG_PASS_MAX   65      // 64 + 结束符
#define CFG_URL_MAX    129     // 带端口的 HTTPS URL
#define CFG_TOKEN_MAX  65
#define CFG_DEV_MAX    9       // dev8 是 8 位十六进制 + 结束符

typedef struct {
    char ssid[CFG_SSID_MAX];
    char pass[CFG_PASS_MAX];
    char url[CFG_URL_MAX];
    char token[CFG_TOKEN_MAX];
    // 设备号 = MAC 低 32 位十六进制，session_id 的前 8 位。首次启动自动生成，不可配。
    char dev[CFG_DEV_MAX];
} app_config_t;

// 初始化 NVS 并加载配置。缺失的项留空，由调用方判断是否已配网。
// 若 device_secret.h 里有编译期值且 NVS 为空，会用编译期值填充（开发期方便）。
void app_config_init(void);

// 读当前配置（返回只读指针，进程内有效）。
const app_config_t *app_config_get(void);

// 是否有足够的配置可以联网：SSID 和 URL 都必须有。
bool app_config_ready(void);

// 是否有 token（没有也能连，但上传会 401）。
bool app_config_has_token(void);

// 写入 Wi-Fi 凭据（BluFi 配网回调调用）。空串表示不修改。
void app_config_set_wifi(const char *ssid, const char *pass);

// 写入 NAS 地址和 token。空串表示不修改。
void app_config_set_upload(const char *url, const char *token);

// 设备号（首次启动自动生成并持久化）。返回只读 8 字符字符串。
const char *app_config_dev(void);

// 清空全部配置（恢复出厂）。
void app_config_erase_all(void);