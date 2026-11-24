#include "app_store.h"

#include <stdio.h>
#include <string.h>

#include "app_error.h"
#include "audio_cache.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#define TAG "store"

#define CACHE_SUBTYPE 0x40
#define NVS_NS        "sess"

// 扇区头（与 audio_cache.c 一致）：magic/seq/session/len/crc。
#define HDR_MAGIC  0x31435541u
#define OFF_SEQ    4
#define OFF_SESS   8
#define OFF_LEN    12

// 待传会话表上限。一小时的录音约 3 个扇区/分钟，实际远达不到上限；
// 真到了就只取最旧的，保证先进先出。
#define MAX_SESSIONS 96

static const esp_partition_t *s_part;
static audio_cache_t s_cache;
static bool s_mounted;
// 落盘缓冲与读缓冲必须分开：边录边传时，s_sector_buf 里可能正攒着未满一扇区的
// 音频字节（buf_len > 0），若 app_store_read 共用同一块缓冲读已落盘扇区，会把未刷盘字节冲掉。
static SemaphoreHandle_t s_lock;
static uint8_t *s_sector_buf;
static uint8_t *s_read_buf;

static int flash_read(void *ctx, uint32_t off, void *buf, size_t n) {
    (void)ctx;
    return esp_partition_read(s_part, off, buf, n) == ESP_OK ? 0 : -1;
}

static int flash_erase(void *ctx, uint32_t off, uint32_t n) {
    return esp_partition_erase_range((const esp_partition_t *)ctx, off, n) == ESP_OK ? 0 : -1;
}

static int flash_write(void *ctx, uint32_t off, const void *buf, size_t n) {
    return esp_partition_write((const esp_partition_t *)ctx, off, buf, n) == ESP_OK ? 0 : -1;
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// 读第 idx 个扇区的头，成功返回 true。magic/seq/len 任一不符视为无效。
static bool read_header(uint32_t idx, uint32_t *session, uint32_t *len) {
    uint8_t hdr[AUDIO_CACHE_HDR_SIZE];
    if (flash_read(NULL, idx * AUDIO_CACHE_SECTOR, hdr, sizeof hdr) != 0) return false;
    if (rd32(hdr) != HDR_MAGIC) return false;
    if (rd32(hdr + OFF_SEQ) != idx + 1) return false;
    uint32_t n = rd32(hdr + OFF_LEN);
    if (n > AUDIO_CACHE_PAYLOAD_MAX) return false;
    if (session) *session = rd32(hdr + OFF_SESS);
    if (len) *len = n;
    return true;
}

static void ts_key(char *key, size_t cap, const char *pfx, uint32_t session) {
    snprintf(key, cap, "%s%08x", pfx, (unsigned)session);
}

static bool nvs_get_done(uint32_t session) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t v = 0;
    char key[16];
    ts_key(key, sizeof key, "dn", session);
    bool done = (nvs_get_u8(h, key, &v) == ESP_OK && v != 0);
    nvs_close(h);
    return done;
}

static void nvs_set_done(uint32_t session) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        app_error_report(APP_E501_FLASH, APP_SRC_RECORD, "nvs_open for done flag failed");
        return;
    }
    char key[16];
    ts_key(key, sizeof key, "dn", session);
    nvs_set_u8(h, key, 1);
    nvs_commit(h);
    nvs_close(h);
}

static esp_err_t nvs_put_start_ts(uint32_t session, uint32_t ts) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        app_error_report(APP_E501_FLASH, APP_SRC_RECORD, "nvs_open for start_ts failed");
        return ESP_FAIL;
    }
    char key[16];
    ts_key(key, sizeof key, "ts", session);
    esp_err_t err = nvs_set_u32(h, key, ts);
    ts_key(key, sizeof key, "bm", session);
    if (err == ESP_OK) err = nvs_set_u32(h, key, 0); // start marker, with reason=interrupted
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static bool nvs_ble_info(uint32_t session, uint32_t *packed, bool save) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, save ? NVS_READWRITE : NVS_READONLY, &h) != ESP_OK) return false;
    char key[16];
    ts_key(key, sizeof key, "bm", session);
    esp_err_t rc = save ? nvs_set_u32(h, key, *packed) : nvs_get_u32(h, key, packed);
    if (rc == ESP_OK && save) rc = nvs_commit(h);
    nvs_close(h);
    return rc == ESP_OK;
}

static uint32_t nvs_get_start_ts(uint32_t session) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return 0;
    uint32_t v = 0;
    char key[16];
    ts_key(key, sizeof key, "ts", session);
    esp_err_t err = nvs_get_u32(h, key, &v);
    nvs_close(h);
    return err == ESP_OK ? v : 0;
}

// 封闭会话：写 NVS 的封闭标记，此后上传侧可以对它 final。
// 没有这个标记的会话都当"可能还在增长"，上传侧只传不 final。
static void nvs_set_sealed(uint32_t session, bool sealed) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        app_error_report(APP_E501_FLASH, APP_SRC_RECORD, "nvs_open for seal failed");
        return;
    }
    char key[16];
    ts_key(key, sizeof key, "sl", session);
    if (sealed) {
        nvs_set_u8(h, key, 1);
    } else {
        nvs_erase_key(h, key);
    }
    nvs_commit(h);
    nvs_close(h);
}

static bool nvs_get_sealed(uint32_t session) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t v = 0;
    char key[16];
    ts_key(key, sizeof key, "sl", session);
    bool sealed = (nvs_get_u8(h, key, &v) == ESP_OK && v != 0);
    nvs_close(h);
    return sealed;
}

// 取锁：录音任务写、上传任务读，两者共用同一块扇区缓冲区，必须串行。
// 2 秒拿不到说明另一方卡在 flash 操作上，报出来并拒绝本次访问，不无限等。
static bool store_lock_take(app_src_t who) {
    if (!s_lock) return true;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(2000)) == pdTRUE) return true;
    app_error_report(APP_E202_TIMEOUT, who, "store lock timeout");
    return false;
}

static void store_lock_give(void) {
    if (s_lock) xSemaphoreGive(s_lock);
}

esp_err_t app_store_init(void) {
    if (s_mounted) return ESP_OK;

    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_sector_buf) {
        s_sector_buf = heap_caps_malloc(AUDIO_CACHE_SECTOR, MALLOC_CAP_8BIT);
        if (!s_sector_buf) {
            app_error_report(APP_E402_NO_SPACE, APP_SRC_RECORD, "sector buffer alloc failed");
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_read_buf) {
        s_read_buf = heap_caps_malloc(AUDIO_CACHE_SECTOR, MALLOC_CAP_8BIT);
        if (!s_read_buf) {
            app_error_report(APP_E402_NO_SPACE, APP_SRC_UPLOAD, "read buffer alloc failed");
            return ESP_ERR_NO_MEM;
        }
    }

    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, CACHE_SUBTYPE, "cache");
    if (!s_part) {
        app_error_report(APP_E501_FLASH, APP_SRC_RECORD, "cache partition not found");
        return ESP_ERR_NOT_FOUND;
    }

    audio_cache_flash_t flash = {
        .read = flash_read,
        .erase = flash_erase,
        .write = flash_write,
        .ctx = (void *)s_part,
    };
    audio_cache_mount_info_t info;
    int r = audio_cache_mount(&s_cache, flash, s_part->size / AUDIO_CACHE_SECTOR, s_sector_buf,
                              &info);
    if (r < 0) {
        app_error_report(APP_E501_FLASH, APP_SRC_RECORD, "mount failed r=%d", r);
        return ESP_FAIL;
    }
    s_mounted = true;

    app_log(TAG, "mounted sectors=%u/%u payload=%u last_session=%08x full=%d pending=%u",
            (unsigned)info.sectors, (unsigned)s_cache.num_sectors,
            (unsigned)info.payload_bytes, (unsigned)info.last_session,
            (int)(s_cache.next_sector >= s_cache.num_sectors),
            (unsigned)app_store_pending_bytes());
    return ESP_OK;
}

bool app_store_full(void) {
    return s_mounted && s_cache.next_sector >= s_cache.num_sectors;
}

uint32_t app_store_used_sectors(void) {
    return s_mounted ? s_cache.next_sector : 0;
}

uint32_t app_store_total_sectors(void) {
    return s_mounted ? s_cache.num_sectors : 0;
}

int app_store_begin(uint32_t session, uint32_t start_ts) {
    if (!s_mounted) return -1;
    if (s_cache.next_sector >= s_cache.num_sectors) return AUDIO_CACHE_ERR_FULL;

    if (!store_lock_take(APP_SRC_RECORD)) return -1;
    int r = audio_cache_begin_session(&s_cache, session);
    store_lock_give();
    if (r < 0) {
        app_error_report(APP_E501_FLASH, APP_SRC_RECORD, "begin_session r=%d", r);
        return r;
    }
    return nvs_put_start_ts(session, start_ts) == ESP_OK ? 0 : -1;
}

int app_store_append(const void *data, size_t n) {
    if (!s_mounted) return -1;
    if (!store_lock_take(APP_SRC_RECORD)) return -1;
    int r = audio_cache_append(&s_cache, (const uint8_t *)data, n);
    store_lock_give();
    if (r == AUDIO_CACHE_ERR_FULL) return r;
    if (r < 0) {
        app_error_report(APP_E501_FLASH, APP_SRC_RECORD, "append r=%d", r);
    }
    return r;
}

void app_store_end(uint8_t reason, uint32_t duration_ms) {
    if (!s_mounted) return;
    if (!store_lock_take(APP_SRC_RECORD)) return;
    int r = audio_cache_flush(&s_cache);
    uint32_t sess = s_cache.session;
    store_lock_give();
    if (r < 0) {
        app_error_report(APP_E501_FLASH, APP_SRC_RECORD, "final flush r=%d", r);
    }
    // 先持久化结束原因，再封闭音频前缀；失败不能宣称正常结束。
    if (sess) {
        uint32_t packed = (duration_ms << 2) | (r < 0 ? 3u : (reason & 3u));
        if (nvs_ble_info(sess, &packed, true)) nvs_set_sealed(sess, true);
    }
}

void app_store_seal(void) {
    if (!s_mounted || s_cache.next_sector == 0) return;

    // 挂载后写指针之前的最后一个扇区就属于上一次运行。它可能因为掉电缺少 EOS 页，
    // 但数据已经落盘且 CRC 校验通过（mount 已排除残缺尾扇区），可以让服务端收尾。
    uint32_t sess = 0, len = 0;
    if (!read_header(s_cache.next_sector - 1, &sess, &len)) return;
    if (nvs_get_done(sess) || nvs_get_sealed(sess)) return;

    nvs_set_sealed(sess, true);
    app_log(TAG, "session=%08x sealed (from previous run, sectors=%u)",
            (unsigned)sess, (unsigned)s_cache.next_sector);
}

// 按扇区顺序把相邻同会话号的扇区归成一组。区间取 [0, next_sector)：
// 掉电残缺的尾扇区在挂载时已经被排除，这里看到的一定是完整扇区。
static int scan_sessions(app_session_t *out, int max) {
    // 扫描只读 flash，但要和写入串行：否则 next_sector 可能在扫描途中被推进。
    if (!store_lock_take(APP_SRC_UPLOAD)) return 0;
    int n = 0;
    app_session_t *cur = NULL;

    for (uint32_t i = 0; i < s_cache.next_sector && n < max; i++) {
        uint32_t sess = 0, len = 0;
        if (!read_header(i, &sess, &len)) {
            app_error_report(APP_E501_FLASH, APP_SRC_RECORD, "sector %u header invalid", (unsigned)i);
            break;
        }
        if (!cur || cur->session != sess) {
            cur = &out[n++];
            memset(cur, 0, sizeof *cur);
            cur->session = sess;
            cur->first_sector = i;
        }
        cur->sectors++;
        cur->bytes += len;
    }
    store_lock_give();
    return n;
}

int app_store_list(app_session_t *out, int max) {
    if (!s_mounted || !out || max <= 0) return 0;

    int raw = scan_sessions(out, max);
    if (raw == 0) return 0;

    int n = 0;
    for (int i = 0; i < raw; i++) {
        if (nvs_get_done(out[i].session)) continue;      // 已上传完，跳过
        out[i].start_ts = nvs_get_start_ts(out[i].session);
        out[i].sealed = nvs_get_sealed(out[i].session);
        uint32_t packed = 0;
        out[i].ble_eligible = nvs_ble_info(out[i].session, &packed, false);
        out[i].end_reason = packed & 3u;
        out[i].duration_ms = packed >> 2;
        if (out[i].start_ts == 0 && !out[i].ble_eligible) {
            // 没有开始时间就拼不出 session_id，传不上去。数据保留不删，等人工处理。
            app_error_report(APP_E203_NO_TIME, APP_SRC_UPLOAD,
                             "session=%08x has no start_ts, kept but not uploaded",
                             (unsigned)out[i].session);
            continue;
        }
        out[n++] = out[i];
    }
    return n;
}

int app_store_read(const app_session_t *s, uint32_t off, void *buf, size_t n, uint32_t *out_len) {
    if (out_len) *out_len = 0;
    if (!s_mounted || !s || !n) return 0;
    if (off >= s->bytes) return 0;

    if (!store_lock_take(APP_SRC_UPLOAD)) return -1;

    uint32_t got = 0;
    uint32_t logical = 0;
    uint8_t *dst = (uint8_t *)buf;
    int rc = 0;

    for (uint32_t k = 0; k < s->sectors && got < n; k++) {
        uint32_t idx = s->first_sector + k;
        if (flash_read(NULL, idx * AUDIO_CACHE_SECTOR, s_read_buf, AUDIO_CACHE_SECTOR) != 0) {
            app_error_report(APP_E501_FLASH, APP_SRC_UPLOAD, "read sector %u failed", (unsigned)idx);
            rc = -1;
            break;
        }
        if (rd32(s_read_buf) != HDR_MAGIC || rd32(s_read_buf + OFF_SEQ) != idx + 1) {
            app_error_report(APP_E501_FLASH, APP_SRC_UPLOAD, "sector %u header invalid on read",
                             (unsigned)idx);
            rc = -1;
            break;
        }
        uint32_t len = rd32(s_read_buf + OFF_LEN);
        if (len > AUDIO_CACHE_PAYLOAD_MAX) {
            app_error_report(APP_E501_FLASH, APP_SRC_UPLOAD, "sector %u len=%u bad",
                             (unsigned)idx, (unsigned)len);
            rc = -1;
            break;
        }

        if (logical + len <= off) {          // 整扇区在起点之前，跳过
            logical += len;
            continue;
        }
        uint32_t skip = off > logical ? off - logical : 0;
        uint32_t avail = len - skip;
        uint32_t take = n - got < avail ? n - got : avail;
        memcpy(dst + got, s_read_buf + AUDIO_CACHE_HDR_SIZE + skip, take);
        got += take;
        logical += len;
    }

    store_lock_give();
    if (out_len) *out_len = got;
    return rc;
}

void app_store_mark_done(uint32_t session) {
    nvs_set_done(session);
    app_log(TAG, "session=%08x marked done", (unsigned)session);
}

// 回收：把未完成会话的完整扇区搬到分区头部，已完成的会话就此丢弃。
// 只能在录音停止时调用：搬动期间不能有新的写入，否则 next_sector 会错位。
uint32_t app_store_reclaim(void) {
    if (!s_mounted) return 0;

    app_session_t list[MAX_SESSIONS];
    // 单独存 done 标志，不塞进 app_session_t：那个结构是给上传侧用的，字段语义别混。
    bool done[MAX_SESSIONS];
    int n = scan_sessions(list, MAX_SESSIONS);
    if (n == 0) return 0;

    // 先算总保留扇区数，决定把哪个位置的扇区搬到哪儿。
    uint32_t keep = 0;
    for (int i = 0; i < n; i++) {
        done[i] = nvs_get_done(list[i].session);
        if (!done[i]) keep += list[i].sectors;
    }
    if (keep == s_cache.next_sector) {
        // 没有可回收的；只剩已完成的会话时也只能等（它们理论上都标了 done）。
        return 0;
    }

    if (!store_lock_take(APP_SRC_UPLOAD)) return 0;

    // 缓冲区里若有未落盘数据，搬动会覆盖它。录音停止时 end() 已 flush，这里只做防御。
    if (s_cache.buf_len != 0) {
        app_log(TAG, "reclaim skipped, %u bytes buffered", (unsigned)s_cache.buf_len);
        store_lock_give();
        return 0;
    }
    uint32_t old_next = s_cache.next_sector;

    // 按索引递增搬：dst 只增不减且总在 src 之前，未处理的源扇区不会被覆盖。
    uint32_t dst = 0;
    uint32_t moved = 0;
    bool moved_ok = true;
    for (int i = 0; i < n && moved_ok; i++) {
        uint32_t base = list[i].first_sector;
        for (uint32_t k = 0; k < list[i].sectors; k++) {
            uint32_t src = base + k;
            if (done[i]) continue;
            if (src != dst) {
                int r = audio_cache_move_sector(&s_cache, src, dst);
                if (r < 0) {
                    app_error_report(APP_E501_FLASH, APP_SRC_UPLOAD,
                                     "reclaim move %u->%u failed r=%d", (unsigned)src,
                                     (unsigned)dst, r);
                    moved_ok = false;
                    break;
                }
                moved++;
            }
            dst++;
        }
    }

    // 尾部残留的旧扇区必须逐个作废。它们头部里的 seq 恰好等于"新位置 + 1"时，
    // 挂载扫描会把它当成有效数据接着往后读，把已上传的旧录音又扫回来。
    // 只擦第一个不够：以后新录音填到那个位置，后面的旧扇区就重新连上了。
    // NOR 写只能把 1 变 0，把 magic 写零即可作废，不必擦除，代价是每个扇区一次小写。
    if (dst < old_next) {
        static const uint8_t zero_magic[4] = {0, 0, 0, 0};
        uint32_t bad = 0;
        for (uint32_t i = dst; i < old_next; i++) {
            if (esp_partition_write(s_part, i * AUDIO_CACHE_SECTOR, zero_magic,
                                    sizeof zero_magic) != ESP_OK) {
                bad++;
            }
        }
        if (bad) {
            app_error_report(APP_E501_FLASH, APP_SRC_UPLOAD,
                             "%u of %u stale sector headers not invalidated", (unsigned)bad,
                             (unsigned)(old_next - dst));
        }
    }

    // 最后一个保留扇区所属的会话就是"当前会话"：下次 begin_session 会先 flush 短扇区，
    // 靠它找回会话号，否则新扇区会被写成会话 0。
    uint32_t tail_sess = 0;
    if (dst > 0) {
        uint8_t hdr[AUDIO_CACHE_HDR_SIZE];
        if (flash_read(NULL, (dst - 1) * AUDIO_CACHE_SECTOR, hdr, sizeof hdr) == 0) {
            tail_sess = rd32(hdr + OFF_SESS);
        }
    }
    s_cache.next_sector = dst;
    s_cache.session = tail_sess;
    store_lock_give();

    if (moved) {
        app_log(TAG, "reclaimed %u sectors, %u free", (unsigned)moved,
                (unsigned)(s_cache.num_sectors - s_cache.next_sector));
    }
    return moved;
}

uint32_t app_store_pending_bytes(void) {
    if (!s_mounted) return 0;
    app_session_t list[MAX_SESSIONS];
    int n = scan_sessions(list, MAX_SESSIONS);
    uint32_t total = 0;
    for (int i = 0; i < n; i++) {
        if (!nvs_get_done(list[i].session)) total += list[i].bytes;
    }
    return total;
}

esp_err_t app_store_erase_all(void) {
    if (!s_part) return ESP_ERR_INVALID_STATE;
    if (!store_lock_take(APP_SRC_RECORD)) return ESP_ERR_TIMEOUT;

    // 先清会话记录再擦扇区：万一中途掉电，剩下的旧记录对不上有效扇区，
    // 挂载扫描时会因为 header 校验失败而自然跳过。
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    } else {
        app_error_report(APP_E501_FLASH, APP_SRC_RECORD, "nvs_open for erase failed");
    }

    esp_err_t err = esp_partition_erase_range(s_part, 0, s_part->size);
    if (err != ESP_OK) {
        app_error_report(APP_E501_FLASH, APP_SRC_RECORD, "erase partition: %s", esp_err_to_name(err));
        store_lock_give();
        return err;
    }

    memset(&s_cache, 0, sizeof s_cache);
    s_cache.flash.read = flash_read;
    s_cache.flash.erase = flash_erase;
    s_cache.flash.write = flash_write;
    s_cache.flash.ctx = (void *)s_part;
    s_cache.num_sectors = s_part->size / AUDIO_CACHE_SECTOR;
    s_cache.next_sector = 0;
    s_cache.img = s_sector_buf;

    store_lock_give();
    app_log(TAG, "cache erased, %u sectors free", (unsigned)s_cache.num_sectors);
    return ESP_OK;
}