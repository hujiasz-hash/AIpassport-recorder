// 缓存与会话索引。录音模块往里写，上传模块往外读，两边不直接碰 flash。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint32_t session;       // 32 位会话号，对应 session_id 的 rand8
    uint32_t first_sector;  // 会话在缓存里的第一个扇区
    uint32_t sectors;       // 已落盘扇区数
    uint32_t bytes;         // 已落盘字节数（不含扇区头）
    uint32_t start_ts;      // 录音开始的 Unix 秒；重启后从 NVS 恢复
    bool sealed;            // 已封闭（不再增长），上传侧才允许发 final
} app_session_t;

// 挂载缓存分区、恢复写指针。失败会报 E501。
esp_err_t app_store_init(void);

// 缓存是否已写满。
bool app_store_full(void);
uint32_t app_store_used_sectors(void);
uint32_t app_store_total_sectors(void);

// 开始新会话：先 flush 上一个会话的短扇区，写入 NVS 的 start_ts，再切会话号。
int app_store_begin(uint32_t session, uint32_t start_ts);

// 追加已编码数据。缓存满返回 AUDIO_CACHE_ERR_FULL(-100)。
int app_store_append(const void *data, size_t n);

// 录音结束：把 RAM 缓冲区里的短扇区落盘，会话转为"已封闭"，允许上传侧 final。
void app_store_end(void);

// 开机时调用：把上一轮（掉电中断的）会话封闭，否则它永远不会被 final，
// 文件在 NAS 上一直停在 .ogg.part。
void app_store_seal(void);

// 列出还没标记完成的会话，最旧的在前。返回条数；超过 max 的由下一轮继续。
int app_store_list(app_session_t *out, int max);

// 读会话内逻辑偏移的数据。返回 0 成功，*out 为实际字节数（到末尾时可能小于 n）。
int app_store_read(const app_session_t *s, uint32_t off, void *buf, size_t n, uint32_t *out);

// 服务端已 finalized，不必再传。
void app_store_mark_done(uint32_t session);

// 回收已上传的扇区，把后面的有效数据前移，腾出空间。
// 只在没有录音进行时调用（另一个任务持有写缓冲）。返回搬动的扇区数。
uint32_t app_store_reclaim(void);

// 未上传字节总数，UI 用。
uint32_t app_store_pending_bytes(void);

// 擦除整个缓存分区并清空会话记录（会丢掉所有录音）。
esp_err_t app_store_erase_all(void);