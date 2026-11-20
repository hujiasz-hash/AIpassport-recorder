// 音频缓存：按 4 KB 扇区顺序写入，每个扇区带头部和 CRC，重启时扫描恢复写指针。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AUDIO_CACHE_SECTOR      4096u
#define AUDIO_CACHE_HDR_SIZE    20u
#define AUDIO_CACHE_PAYLOAD_MAX (AUDIO_CACHE_SECTOR - AUDIO_CACHE_HDR_SIZE)

#define AUDIO_CACHE_ERR_FULL (-100)

typedef struct {
    int (*read)(void *ctx, uint32_t off, void *buf, size_t n);
    int (*erase)(void *ctx, uint32_t off, uint32_t n);
    int (*write)(void *ctx, uint32_t off, const void *buf, size_t n);
    void *ctx;
} audio_cache_flash_t;

typedef struct {
    audio_cache_flash_t flash;
    uint32_t num_sectors;
    uint32_t next_sector;
    uint32_t session;
    uint32_t buf_len;
    uint8_t img[AUDIO_CACHE_SECTOR];
} audio_cache_t;

typedef struct {
    uint32_t sectors;
    uint32_t payload_bytes;
    uint32_t last_session;
} audio_cache_mount_info_t;

// 扫描已有扇区定位写指针。0 成功，负数为 flash 错误码。
int audio_cache_mount(audio_cache_t *c, audio_cache_flash_t flash, uint32_t num_sectors,
                      audio_cache_mount_info_t *info);

// 切换会话前先写完未满的扇区，保证每个扇区只属于一个会话。
int audio_cache_begin_session(audio_cache_t *c, uint32_t session);

// 追加音频字节，满一扇区即写入。缓存已满返回 AUDIO_CACHE_ERR_FULL。
int audio_cache_append(audio_cache_t *c, const uint8_t *data, size_t len);

// 把缓冲区中剩余数据写成一个未满扇区。
int audio_cache_flush(audio_cache_t *c);
