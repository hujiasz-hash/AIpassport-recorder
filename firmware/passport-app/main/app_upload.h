// 上传：把缓存里未完成的会话分批 PUT 到 NAS。只跟服务端返回的 received 走。
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    APP_UP_IDLE = 0,     // 没有待传数据或未联网
    APP_UP_RUNNING,      // 正在传
    APP_UP_WAITING,      // 暂时传不了（断网/时间没同步），稍后重试
    APP_UP_DONE,         // 本轮传完
} app_upload_state_t;

// 初始化：创建任务，不立即开始传。
void app_upload_init(void);

// 请求马上跑一轮（按键、配网完成、录音结束时调用）。非阻塞。
void app_upload_kick(void);

// 暂停/恢复：录音期间降低频率，配网期间暂停。
void app_upload_set_enabled(bool on);

app_upload_state_t app_upload_state(void);

// 最近一轮的结果摘要，给 UI 显示。没内容时返回空串。
const char *app_upload_summary(void);

// 已上传累计字节（本次上电以来）。
uint32_t app_upload_total_bytes(void);