#include "app_ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app_ble.h"
#include "app_config.h"
#include "app_error.h"
#include "app_net.h"
#include "app_record.h"
#include "app_store.h"
#include "app_upload.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "lvgl_private.h"
#include "ui_pixel.h"

// 自带中文字体（lv_font_ui_14.c / lv_font_ui_20.c 生成）。
extern const lv_font_t lv_font_ui_14;
extern const lv_font_t lv_font_ui_20;

#define TAG "ui"

// 屏幕 240×320，UI 按这个尺寸摆（四角有 R=30 圆角裁切）。
#define SCREEN_W 240
#define SCREEN_H 320

// 错误页 10 秒后自动回主页。
#define ERROR_AUTO_BACK_MS 10000

// 30 秒无操作：非录音页熄屏（0%），录音页降为微亮（20%）；按上/下键唤醒回 85%。
#define SCREEN_OFF_US   (30LL * 1000 * 1000)
#define BACKLIGHT_ON    85
#define BACKLIGHT_DIM   20

// 中文字形用自带的 14 号子集字体（lv_font_ui_14.c，由 Hiragino Sans GB 生成）。
#define FONT_CN    &lv_font_ui_14
#define FONT_BIG   &lv_font_montserrat_20
#define FONT_TITLE &lv_font_ui_20

#define HOME_BG       0xF6F1E5
#define HOME_MUTED    0x665648
#define REC_CARD_BG   0xEAE2D0
#define REC_TRACK_BG  0xD5CBB8
#define REC_PILL_BG   0xF0DFDB
#define REC_HALO_HEX  0xFFB23E
#define REC_WAVE_IDLE 0xD8CEBC

#define OCTO_W 184
#define OCTO_H 192
#define OCTO_COUNT 10
#define OCTO_FRAME_BYTES (OCTO_W * OCTO_H * 2)
#define OCTO_HEADER_BYTES 16
#define OCTO_RLE_FLAG LV_IMAGE_FLAGS_USER1

#define REC_BADGE_W 96
#define REC_BADGE_H 96
#define REC_WAVE_BARS 8
#define REC_WAVE_MAX_H 46
#define REC_WAVE_MIN_H 6

extern const uint8_t octobara_frames[];
extern const uint8_t octobara_rec_badge[];

// 帧 0 直接从 Flash 裸 RGB565 绘制；帧 1..9 带 OCTO_RLE_FLAG，走逐行差分解码器（仅占 368B 行缓冲）。
#define OCTO_RLE_FRAME(i) { .header = { .magic = LV_IMAGE_HEADER_MAGIC, \
    .cf = LV_COLOR_FORMAT_RGB565, .flags = OCTO_RLE_FLAG, \
    .w = OCTO_W, .h = OCTO_H, .stride = OCTO_W * 2, .reserved_2 = (i) }, \
    .data_size = OCTO_FRAME_BYTES, .data = octobara_frames }

static const lv_image_dsc_t s_octo_frames[OCTO_COUNT] = {
    {
        .header = {
            .magic = LV_IMAGE_HEADER_MAGIC,
            .cf = LV_COLOR_FORMAT_RGB565,
            .flags = 0,
            .w = OCTO_W,
            .h = OCTO_H,
            .stride = OCTO_W * 2,
        },
        .data_size = OCTO_FRAME_BYTES,
        .data = octobara_frames + OCTO_HEADER_BYTES,
    },
    OCTO_RLE_FRAME(1), OCTO_RLE_FRAME(2), OCTO_RLE_FRAME(3),
    OCTO_RLE_FRAME(4), OCTO_RLE_FRAME(5), OCTO_RLE_FRAME(6),
    OCTO_RLE_FRAME(7), OCTO_RLE_FRAME(8), OCTO_RLE_FRAME(9),
};

static const lv_image_dsc_t s_rec_badge_dsc = {
    .header = {
        .magic = LV_IMAGE_HEADER_MAGIC,
        .cf = LV_COLOR_FORMAT_RGB565,
        .flags = 0,
        .w = REC_BADGE_W,
        .h = REC_BADGE_H,
        .stride = REC_BADGE_W * 2,
    },
    .data_size = REC_BADGE_W * REC_BADGE_H * 2,
    .data = octobara_rec_badge,
};

// 单行解码缓冲（368B + 对齐余量），0 字节堆分配。
LV_DRAW_BUF_DEFINE_STATIC(s_octo_line_buf, OCTO_W, 1, LV_COLOR_FORMAT_RGB565);

static lv_obj_t *s_scr_home, *s_scr_record, *s_scr_prov, *s_scr_error;
static lv_obj_t *s_home_time, *s_home_batt, *s_home_link, *s_home_link_icon;
static lv_obj_t *s_home_message, *s_home_octo;
static lv_timer_t *s_octo_timer;

static lv_obj_t *s_rec_link_icon, *s_rec_link, *s_rec_batt;
static lv_obj_t *s_rec_dot, *s_rec_status_lbl, *s_rec_elapsed;
static lv_obj_t *s_rec_halo, *s_rec_waves[REC_WAVE_BARS];
static lv_obj_t *s_rec_used_lbl, *s_rec_rem_lbl, *s_rec_bar_fill;
static lv_timer_t *s_rec_wave_timer;
static uint8_t s_wave_env[REC_WAVE_BARS / 2];

static lv_obj_t *s_prov_name, *s_prov_state;
static lv_obj_t *s_error_text;
static app_page_t s_page = APP_PAGE_HOME;
static bool s_ui_ok;
static uint8_t s_cur_backlight = BACKLIGHT_ON;
static int64_t s_last_input_us;
static lv_timer_t *s_error_timer;

// ---- OctoBara 帧间差分逐行解码器（Flash 直读，无堆分配） -------------------

static bool is_octo_rle_dsc(const lv_image_decoder_dsc_t *dsc, const lv_image_dsc_t **out_img) {
    if (!dsc || dsc->src_type != LV_IMAGE_SRC_VARIABLE || !dsc->src) return false;
    const lv_image_dsc_t *img = (const lv_image_dsc_t *)dsc->src;
    if ((img->header.flags & OCTO_RLE_FLAG) == 0) return false;
    if (out_img) *out_img = img;
    return true;
}

static lv_result_t octo_rle_info(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc,
                                 lv_image_header_t *header) {
    (void)decoder;
    const lv_image_dsc_t *img = NULL;
    if (!is_octo_rle_dsc(dsc, &img)) return LV_RESULT_INVALID;
    *header = img->header;
    header->flags = 0;
    return LV_RESULT_OK;
}

static lv_result_t octo_rle_open(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc) {
    (void)decoder;
    const lv_image_dsc_t *img = NULL;
    if (!is_octo_rle_dsc(dsc, &img)) return LV_RESULT_INVALID;
    dsc->decoded = NULL;   // 交由 get_area_cb 逐行解码
    return LV_RESULT_OK;
}

static void octo_decode_row(uint16_t frame_idx, uint16_t row, uint16_t *dst) {
    const uint8_t *blob = octobara_frames;
    uint32_t offsets_bytes = (uint32_t)blob[8] | ((uint32_t)blob[9] << 8) |
                             ((uint32_t)blob[10] << 16) | ((uint32_t)blob[11] << 24);
    const uint16_t *f0 = (const uint16_t *)(blob + OCTO_HEADER_BYTES);
    const uint8_t *offsets_base = blob + OCTO_HEADER_BYTES + OCTO_FRAME_BYTES;
    const uint8_t *stream = offsets_base + offsets_bytes;

    uint32_t table_idx = (uint32_t)(frame_idx - 1u) * OCTO_H + row;
    uint32_t pos = (uint32_t)offsets_base[table_idx * 2u] |
                   ((uint32_t)offsets_base[table_idx * 2u + 1u] << 8);

    const uint16_t *f0_row = f0 + (uint32_t)row * OCTO_W;
    uint32_t x = 0;
    while (x < OCTO_W) {
        uint8_t op = stream[pos++];
        if (op & 0x80u) {
            uint32_t cnt = op & 0x7Fu;
            if (x + cnt > OCTO_W) cnt = OCTO_W - x;
            memcpy(dst + x, f0_row + x, cnt * sizeof(uint16_t));
            x += cnt;
        } else if (op & 0x40u) {
            uint32_t cnt = op & 0x3Fu;
            uint16_t color = (uint16_t)stream[pos] | ((uint16_t)stream[pos + 1u] << 8);
            pos += 2u;
            if (x + cnt > OCTO_W) cnt = OCTO_W - x;
            for (uint32_t k = 0; k < cnt; k++) dst[x + k] = color;
            x += cnt;
        } else {
            uint32_t cnt = op & 0x3Fu;
            if (x + cnt > OCTO_W) cnt = OCTO_W - x;
            memcpy(dst + x, stream + pos, cnt * sizeof(uint16_t));
            pos += cnt * 2u;
            x += cnt;
        }
    }
}

static lv_result_t octo_rle_get_area(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc,
                                     const lv_area_t *full_area, lv_area_t *decoded_area) {
    (void)decoder;
    const lv_image_dsc_t *img = NULL;
    if (!is_octo_rle_dsc(dsc, &img)) return LV_RESULT_INVALID;

    int32_t row;
    if (decoded_area->y1 == LV_COORD_MIN) {
        row = full_area->y1;
    } else {
        row = decoded_area->y1 + 1;
    }
    if (row > full_area->y2 || row < 0 || row >= OCTO_H) return LV_RESULT_INVALID;

    uint16_t frame_idx = img->header.reserved_2;
    if (frame_idx < 1u || frame_idx >= OCTO_COUNT) return LV_RESULT_INVALID;

    uint16_t full_row[OCTO_W];
    octo_decode_row(frame_idx, (uint16_t)row, full_row);

    int32_t x1 = full_area->x1 < 0 ? 0 : full_area->x1;
    int32_t x2 = full_area->x2 >= OCTO_W ? (OCTO_W - 1) : full_area->x2;
    int32_t w_px = x2 - x1 + 1;
    if (w_px <= 0) return LV_RESULT_INVALID;

    s_octo_line_buf.header.w = (uint16_t)w_px;
    s_octo_line_buf.header.h = 1;
    s_octo_line_buf.header.stride = (uint16_t)(w_px * 2);
    memcpy(s_octo_line_buf.data, full_row + x1, (size_t)w_px * sizeof(uint16_t));

    decoded_area->x1 = x1;
    decoded_area->x2 = x2;
    decoded_area->y1 = row;
    decoded_area->y2 = row;
    dsc->decoded = &s_octo_line_buf;
    return LV_RESULT_OK;
}

static void octo_rle_close(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc) {
    (void)decoder;
    (void)dsc;
}

static void register_octo_decoder(void) {
    LV_DRAW_BUF_INIT_STATIC(s_octo_line_buf);
    lv_image_decoder_t *dec = lv_image_decoder_create();
    if (!dec) return;
    lv_image_decoder_set_info_cb(dec, octo_rle_info);
    lv_image_decoder_set_open_cb(dec, octo_rle_open);
    lv_image_decoder_set_get_area_cb(dec, octo_rle_get_area);
    lv_image_decoder_set_close_cb(dec, octo_rle_close);
}

// ---- 共用小工具 ----------------------------------------------------------

static void set_backlight_cached(uint8_t percent) {
    if (s_cur_backlight == percent) return;
    s_cur_backlight = percent;
    bsp_display_backlight(percent);
}

// 内容没变就不碰 LVGL：每秒刷一次时避免无谓重绘。
static void set_label(lv_obj_t *label, const char *text) {
    if (!label) return;
    const char *cur = lv_label_get_text(label);
    if (cur && strcmp(cur, text) == 0) return;
    lv_label_set_text(label, text);
}

static void format_battery(char *out, size_t cap) {
    snprintf(out, cap, "-- " LV_SYMBOL_BATTERY_EMPTY);
    int soc = bsp_battery_soc();
    if (soc >= 0) {
        const char *icon = soc >= 90 ? LV_SYMBOL_BATTERY_FULL :
                           soc >= 65 ? LV_SYMBOL_BATTERY_3 :
                           soc >= 40 ? LV_SYMBOL_BATTERY_2 :
                           soc >= 15 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
        snprintf(out, cap, "%d%% %s", soc, icon);
    }
}

static void query_link_status(bool *connected, const char **status) {
#ifdef CONFIG_PASSPORT_WIFI_LEGACY
    *connected = app_net_state() == APP_NET_CONNECTED;
    *status = *connected ? "网络已连接" : "网络未连接";
#else
    *connected = app_ble_connected();
    *status = *connected ? "蓝牙已连接" : "蓝牙未连接";
#endif
}

static void build_top_bar(lv_obj_t *scr, lv_obj_t **out_icon, lv_obj_t **out_link,
                          lv_obj_t **out_batt) {
#ifdef CONFIG_PASSPORT_WIFI_LEGACY
    const char *link_icon = LV_SYMBOL_WIFI;
    const char *link_text = "网络未连接";
#else
    const char *link_icon = LV_SYMBOL_BLUETOOTH;
    const char *link_text = "蓝牙未连接";
#endif
    *out_icon = ui_pixel_label(scr, link_icon, FONT_BIG, HOME_MUTED);
    lv_obj_align(*out_icon, LV_ALIGN_TOP_LEFT, 22, 9);
    *out_link = ui_pixel_label(scr, link_text, FONT_CN, HOME_MUTED);
    lv_obj_align(*out_link, LV_ALIGN_TOP_LEFT, 47, 13);
    *out_batt = ui_pixel_label(scr, "-- " LV_SYMBOL_BATTERY_EMPTY,
                               &lv_font_montserrat_14, HOME_MUTED);
    lv_obj_align(*out_batt, LV_ALIGN_TOP_RIGHT, -22, 13);
}

// ---- 各页构建 ------------------------------------------------------------

static void octo_tick(lv_timer_t *timer) {
    (void)timer;
    static unsigned frame;
    frame = (frame + 1) % OCTO_COUNT;
    lv_image_set_src(s_home_octo, &s_octo_frames[frame]);
}

static void rec_wave_tick(lv_timer_t *timer) {
    (void)timer;
    uint8_t lvl = app_record_level();
    // 4 档包络由内侧（靠近耳机）向外侧传播，左右镜像对称。
    s_wave_env[3] = (uint8_t)((s_wave_env[2] * 2u + s_wave_env[3]) / 3u);
    s_wave_env[2] = (uint8_t)((s_wave_env[1] * 2u + s_wave_env[2]) / 3u);
    s_wave_env[1] = (uint8_t)((s_wave_env[0] * 2u + s_wave_env[1]) / 3u);
    if (lvl >= s_wave_env[0]) {
        s_wave_env[0] = lvl;
    } else {
        s_wave_env[0] = (uint8_t)((s_wave_env[0] * 2u + lvl) / 3u);
    }

    static const uint8_t scale_pct[4] = { 100, 82, 62, 44 };
    const int center_y = 169;
    for (int i = 0; i < 4; i++) {
        uint32_t v = ((uint32_t)s_wave_env[i] * scale_pct[i]) / 100u;
        int h = REC_WAVE_MIN_H + (int)((v * (REC_WAVE_MAX_H - REC_WAVE_MIN_H)) / 100u);
        if (h & 1) h++;   // 保持偶像素高，上下居中不抖动
        uint32_t col = v >= 55u ? UI_ORANGE : (v >= 14u ? HOME_MUTED : REC_WAVE_IDLE);

        // 左半侧：3(最外)..0(贴耳机)；右半侧：0(贴耳机)..3(最外)
        lv_obj_t *bar_l = s_rec_waves[3 - i];
        lv_obj_t *bar_r = s_rec_waves[4 + i];
        if (lv_obj_get_height(bar_l) != h) {
            lv_obj_set_height(bar_l, h);
            lv_obj_set_y(bar_l, center_y - h / 2);
            lv_obj_set_height(bar_r, h);
            lv_obj_set_y(bar_r, center_y - h / 2);
        }
        lv_obj_set_style_bg_color(bar_l, lv_color_hex(col), 0);
        lv_obj_set_style_bg_color(bar_r, lv_color_hex(col), 0);
    }
}

static void build_home(void) {
    s_scr_home = lv_obj_create(NULL);
    lv_obj_remove_flag(s_scr_home, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_scr_home, lv_color_hex(HOME_BG), 0);
    lv_obj_set_style_border_width(s_scr_home, 0, 0);
    lv_obj_set_style_pad_all(s_scr_home, 0, 0);

    build_top_bar(s_scr_home, &s_home_link_icon, &s_home_link, &s_home_batt);

    s_home_time = ui_pixel_label(s_scr_home, "--:--", &lv_font_montserrat_48, UI_INK);
    lv_obj_align(s_home_time, LV_ALIGN_TOP_MID, 0, 34);
    s_home_message = ui_pixel_label(s_scr_home, "时间未同步", FONT_CN, HOME_MUTED);
    lv_obj_align(s_home_message, LV_ALIGN_TOP_MID, 0, 93);

    s_home_octo = lv_image_create(s_scr_home);
    lv_image_set_src(s_home_octo, &s_octo_frames[0]);
    lv_obj_align(s_home_octo, LV_ALIGN_TOP_MID, 0, 114);
    s_octo_timer = lv_timer_create(octo_tick, 100, NULL);
}

static lv_obj_t *make_round_box(lv_obj_t *parent, int x, int y, int w, int h,
                                int radius, uint32_t bg_hex) {
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(bg_hex), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    return obj;
}

static void build_record(void) {
    s_scr_record = lv_obj_create(NULL);
    lv_obj_remove_flag(s_scr_record, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_scr_record, lv_color_hex(HOME_BG), 0);
    lv_obj_set_style_border_width(s_scr_record, 0, 0);
    lv_obj_set_style_pad_all(s_scr_record, 0, 0);

    // 1. 顶栏与主页同坐标对齐
    build_top_bar(s_scr_record, &s_rec_link_icon, &s_rec_link, &s_rec_batt);

    // 2. 正在录音状态胶囊 + 圆形呼吸红点
    lv_obj_t *pill = make_round_box(s_scr_record, 72, 36, 96, 22, 11, REC_PILL_BG);
    s_rec_dot = make_round_box(pill, 12, 6, 10, 10, 5, UI_RED);
    lv_obj_set_style_opa(s_rec_dot, LV_OPA_TRANSP, 0);
    s_rec_status_lbl = ui_pixel_label(pill, "正在录音", FONT_CN, UI_RED);
    lv_obj_set_pos(s_rec_status_lbl, 28, 3);

    // 3. 48 号大字录音计时
    s_rec_elapsed = ui_pixel_label(s_scr_record, "00:00", &lv_font_montserrat_48, UI_INK);
    lv_obj_align(s_rec_elapsed, LV_ALIGN_TOP_MID, 0, 60);

    // 4. 中央：暖橙呼吸圆环边框 + 96×96 戴监听耳机 OctoBara + 左右各 4 根实时声波柱
    lv_obj_t *badge = lv_image_create(s_scr_record);
    lv_image_set_src(badge, &s_rec_badge_dsc);
    lv_obj_set_pos(badge, 72, 121);

    s_rec_halo = lv_obj_create(s_scr_record);
    lv_obj_remove_flag(s_rec_halo, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_rec_halo, 68, 117);
    lv_obj_set_size(s_rec_halo, 104, 104);
    lv_obj_set_style_radius(s_rec_halo, 52, 0);
    lv_obj_set_style_bg_opa(s_rec_halo, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_rec_halo, 3, 0);
    lv_obj_set_style_border_color(s_rec_halo, lv_color_hex(REC_HALO_HEX), 0);
    lv_obj_set_style_border_opa(s_rec_halo, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_rec_halo, 0, 0);

    static const int wave_x[REC_WAVE_BARS] = { 18, 30, 42, 54, 180, 192, 204, 216 };
    for (int i = 0; i < REC_WAVE_BARS; i++) {
        s_rec_waves[i] = make_round_box(s_scr_record, wave_x[i],
                                        169 - REC_WAVE_MIN_H / 2,
                                        6, REC_WAVE_MIN_H, 3, REC_WAVE_IDLE);
    }

    // 5. 缓存余量卡片（已存容量 + 剩余可录分钟 + 进度条）
    lv_obj_t *card = make_round_box(s_scr_record, 20, 230, 200, 46, 12, REC_CARD_BG);
    s_rec_used_lbl = ui_pixel_label(card, "已存 0 KB", FONT_CN, UI_INK);
    lv_obj_set_pos(s_rec_used_lbl, 12, 7);
    s_rec_rem_lbl = ui_pixel_label(card, "还可录 36 分", FONT_CN, HOME_MUTED);
    lv_obj_align(s_rec_rem_lbl, LV_ALIGN_TOP_RIGHT, -12, 7);

    lv_obj_t *track = make_round_box(card, 12, 31, 176, 6, 3, REC_TRACK_BG);
    s_rec_bar_fill = make_round_box(track, 0, 0, 4, 6, 3, UI_SKY_DARK);

    // 6. 底部操作提示
    lv_obj_t *hint = ui_pixel_label(s_scr_record, "按 OK 停止保存", FONT_CN, HOME_MUTED);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 286);

    s_rec_wave_timer = lv_timer_create(rec_wave_tick, 140, NULL);
    lv_timer_pause(s_rec_wave_timer);
}

static void build_prov(void) {
    s_scr_prov = ui_pixel_screen_create("配网", FONT_TITLE);

    lv_obj_t *panel = ui_pixel_panel_create(s_scr_prov, 16, 70, 208, 140, UI_PAPER);
    lv_obj_t *l1 = ui_pixel_label(panel, "手机连接设备", FONT_CN, UI_INK);
    lv_obj_set_pos(l1, 4, 2);

    s_prov_name = ui_pixel_label(panel, "AIPassport", FONT_BIG, UI_SKY_DARK);
    lv_obj_set_pos(s_prov_name, 4, 24);

    lv_obj_t *l3 = ui_pixel_label(panel, "地址 192.168.4.1", FONT_CN, UI_INK);
    lv_obj_set_pos(l3, 4, 56);

    lv_obj_t *l4 = ui_pixel_label(panel, "网页配置", FONT_CN, UI_INK);
    lv_obj_set_pos(l4, 4, 82);

    // 连接状态：手机连上后变成"已连接"，否则用户不知道该不该重试。
    s_prov_state = ui_pixel_label(panel, "等待手机连接", FONT_CN, UI_RED);
    lv_obj_set_pos(s_prov_state, 4, 112);

    lv_obj_t *hint = ui_pixel_label(s_scr_prov, "长按 OK 回主页", FONT_CN, UI_PAPER);
    lv_obj_set_pos(hint, 16, 216);
}

static void build_error(void) {
    s_scr_error = ui_pixel_screen_create("出错", FONT_TITLE);

    lv_obj_t *panel = ui_pixel_panel_create(s_scr_error, 16, 80, 208, 120, UI_PAPER);
    s_error_text = ui_pixel_label(panel, "E?", FONT_CN, UI_RED);
    lv_obj_set_pos(s_error_text, 4, 4);
    lv_label_set_long_mode(s_error_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_error_text, 176);

    lv_obj_t *hint = ui_pixel_label(s_scr_error, "按 OK 返回", FONT_CN, UI_PAPER);
    lv_obj_set_pos(hint, 16, 214);
}

// ---- 公共 ----------------------------------------------------------------

static void error_back_cb(lv_timer_t *t) {
    (void)t;
    app_ui_set_page(APP_PAGE_HOME);
}

esp_err_t app_ui_init(void) {
    if (bsp_display_init() != ESP_OK) {
        app_error_report(APP_E400_INIT, APP_SRC_UI, "display init failed");
        return ESP_FAIL;
    }
    if (!bsp_lvgl_init()) {
        app_error_report(APP_E400_INIT, APP_SRC_UI, "lvgl init failed");
        return ESP_FAIL;
    }
    if (!bsp_lvgl_lock(2000)) {
        app_error_report(APP_E202_TIMEOUT, APP_SRC_UI, "lvgl lock timeout at init");
        return ESP_ERR_TIMEOUT;
    }
    register_octo_decoder();
    build_home();
    build_record();
    build_prov();
    build_error();
    lv_screen_load(s_scr_home);

    s_error_timer = lv_timer_create(error_back_cb, ERROR_AUTO_BACK_MS, NULL);
    lv_timer_pause(s_error_timer);
    bsp_lvgl_unlock();

    s_cur_backlight = BACKLIGHT_ON;
    bsp_display_backlight(BACKLIGHT_ON);
    s_last_input_us = esp_timer_get_time();
    s_page = APP_PAGE_HOME;
    s_ui_ok = true;
    app_log(TAG, "ui ready %dx%d", SCREEN_W, SCREEN_H);
    return ESP_OK;
}

void app_ui_set_page(app_page_t page) {
    if (!s_ui_ok) return;
    if (!bsp_lvgl_lock(500)) return;
    lv_obj_t *scr = s_scr_home;
    switch (page) {
    case APP_PAGE_RECORDING: scr = s_scr_record; break;
    case APP_PAGE_PROV: scr = s_scr_prov; break;
    case APP_PAGE_ERROR: scr = s_scr_error; break;
    default: scr = s_scr_home; break;
    }
    lv_screen_load(scr);
    s_page = page;
    if (s_octo_timer) {
        if (page == APP_PAGE_HOME) lv_timer_resume(s_octo_timer);
        else lv_timer_pause(s_octo_timer);
    }
    if (s_rec_wave_timer) {
        if (page == APP_PAGE_RECORDING) {
            memset(s_wave_env, 0, sizeof s_wave_env);
            lv_timer_resume(s_rec_wave_timer);
        } else {
            lv_timer_pause(s_rec_wave_timer);
        }
    }
    if (page == APP_PAGE_ERROR && s_error_timer) {
        lv_timer_reset(s_error_timer);
        lv_timer_resume(s_error_timer);
    } else if (s_error_timer) {
        lv_timer_pause(s_error_timer);
    }
    bsp_lvgl_unlock();
    app_ui_note_input();
    if (page == APP_PAGE_RECORDING) {
        app_ui_refresh_record(app_record_elapsed_sec(), app_record_bytes());
    } else if (page == APP_PAGE_HOME) {
        app_ui_refresh_home();
    }
}

app_page_t app_ui_page(void) {
    return s_page;
}

void app_ui_refresh_home(void) {
    if (!s_ui_ok) return;
    char time_s[12] = "--:--";
    time_t now = app_net_now();
    const char *message = now ? "" : "时间未同步";
    if (now) {
        // 主页显示北京时间；不改变录音和蓝牙协议使用的 Unix 时间戳。
        now += 8 * 60 * 60;
        struct tm tm;
        gmtime_r(&now, &tm);
        strftime(time_s, sizeof time_s, "%H:%M", &tm);
    }
    if (app_store_full()) message = "缓存已满 等上传";

    char batt[24];
    format_battery(batt, sizeof batt);

    bool connected;
    const char *status;
    query_link_status(&connected, &status);
#ifdef CONFIG_PASSPORT_WIFI_LEGACY
    if (!app_config_ready()) message = "长按 OK 进入配网";
#else
    uint32_t key = app_ble_passkey();
    if (key != UINT32_MAX) {
        snprintf(time_s, sizeof time_s, "%06u", (unsigned)key);
        message = "配对码 请在 Mac 输入";
    }
#endif

    if (!bsp_lvgl_lock(200)) return;
    set_label(s_home_time, time_s);
    set_label(s_home_batt, batt);
    if (strcmp(lv_label_get_text(s_home_link), status) != 0) {
        set_label(s_home_link, status);
        lv_obj_set_style_text_color(s_home_link_icon,
                                    lv_color_hex(connected ? UI_SKY_DARK : HOME_MUTED), 0);
    }
    set_label(s_home_message, message);
    if (s_octo_timer && s_page == APP_PAGE_HOME &&
        esp_timer_get_time() - s_last_input_us < SCREEN_OFF_US) {
        lv_timer_resume(s_octo_timer);
    }
    bsp_lvgl_unlock();
}

void app_ui_refresh_record(uint32_t elapsed_sec, uint32_t bytes) {
    if (!s_ui_ok) return;

    char t[16];
    const lv_font_t *elapsed_font = &lv_font_montserrat_48;
    int elapsed_y = 60;
    if (elapsed_sec >= 3600u) {
        snprintf(t, sizeof t, "%u:%02u:%02u",
                 (unsigned)(elapsed_sec / 3600u),
                 (unsigned)((elapsed_sec % 3600u) / 60u),
                 (unsigned)(elapsed_sec % 60u));
        elapsed_font = FONT_BIG;
        elapsed_y = 74;
    } else {
        snprintf(t, sizeof t, "%02u:%02u",
                 (unsigned)(elapsed_sec / 60u),
                 (unsigned)(elapsed_sec % 60u));
    }

    char batt[24];
    format_battery(batt, sizeof batt);

    bool connected;
    const char *status;
    query_link_status(&connected, &status);

    char used_s[32];
    if (bytes >= 1024u * 1024u) {
        uint32_t tenths = (bytes * 10u) / (1024u * 1024u);
        snprintf(used_s, sizeof used_s, "已存 %u.%u MB",
                 (unsigned)(tenths / 10u), (unsigned)(tenths % 10u));
    } else {
        snprintf(used_s, sizeof used_s, "已存 %u KB", (unsigned)(bytes / 1024u));
    }

    uint32_t used_sec = app_store_used_sectors();
    uint32_t total_sec = app_store_total_sectors();
    if (total_sec == 0) total_sec = 928u;
    if (used_sec > total_sec) used_sec = total_sec;
    uint32_t pct = (used_sec * 100u) / total_sec;

    // 实测 13.85 kbps Ogg Opus + 扇区头：约 25.8 个 4KB 扇区 / 分钟（928 扇区 ≈ 36 分钟）。
    uint32_t free_sec = total_sec - used_sec;
    uint32_t rem_min = (free_sec * 36u) / total_sec;
    char rem_s[32];
    bool warn_cache = (pct >= 85u) || app_store_full();
    if (app_store_full()) {
        snprintf(rem_s, sizeof rem_s, "缓存已满");
    } else if (rem_min == 0) {
        snprintf(rem_s, sizeof rem_s, "还可录 <1 分");
    } else {
        snprintf(rem_s, sizeof rem_s, "还可录 %u 分", (unsigned)rem_min);
    }

#ifndef CONFIG_PASSPORT_WIFI_LEGACY
    uint32_t key = app_ble_passkey();
    if (key != UINT32_MAX) {
        snprintf(used_s, sizeof used_s, "配对码 %06u", (unsigned)key);
    }
#endif

    int bar_w = (int)((176u * pct) / 100u);
    if (bar_w < 4) bar_w = 4;
    if (bar_w > 176) bar_w = 176;

    if (!bsp_lvgl_lock(200)) return;
    set_label(s_rec_batt, batt);
    if (strcmp(lv_label_get_text(s_rec_link), status) != 0) {
        set_label(s_rec_link, status);
        lv_obj_set_style_text_color(s_rec_link_icon,
                                    lv_color_hex(connected ? UI_SKY_DARK : HOME_MUTED), 0);
    }
    if (lv_obj_get_style_text_font(s_rec_elapsed, 0) != elapsed_font) {
        lv_obj_set_style_text_font(s_rec_elapsed, elapsed_font, 0);
        lv_obj_align(s_rec_elapsed, LV_ALIGN_TOP_MID, 0, elapsed_y);
    }
    set_label(s_rec_elapsed, t);
    set_label(s_rec_used_lbl, used_s);
    set_label(s_rec_rem_lbl, rem_s);
    lv_obj_set_style_text_color(s_rec_rem_lbl,
                                lv_color_hex(warn_cache ? UI_RED : HOME_MUTED), 0);
    lv_obj_set_width(s_rec_bar_fill, bar_w);
    lv_obj_set_style_bg_color(s_rec_bar_fill,
                              lv_color_hex(warn_cache ? UI_RED : UI_SKY_DARK), 0);
    bsp_lvgl_unlock();
}

void app_ui_show_error(const char *short_text) {
    if (!s_ui_ok) return;
    if (!bsp_lvgl_lock(500)) return;
    lv_label_set_text(s_error_text, short_text ? short_text : "E? 未知错误");
    bsp_lvgl_unlock();
    app_ui_set_page(APP_PAGE_ERROR);
}

void app_ui_backlight(uint8_t percent) {
    set_backlight_cached(percent);
}

void app_ui_note_input(void) {
    s_last_input_us = esp_timer_get_time();
    set_backlight_cached(BACKLIGHT_ON);
}

int64_t app_ui_last_input_us(void) {
    return s_last_input_us;
}

void app_ui_idle_tick(void) {
    if (!s_ui_ok) return;
    int64_t idle_us = esp_timer_get_time() - s_last_input_us;
    if (idle_us < SCREEN_OFF_US) return;

    if (s_page == APP_PAGE_RECORDING) {
        // 录音中无按键满 30 秒降为微亮省电，保留计时与声波常显。
        set_backlight_cached(BACKLIGHT_DIM);
        return;
    }
    if (s_octo_timer && bsp_lvgl_lock(200)) {
        lv_timer_pause(s_octo_timer);
        bsp_lvgl_unlock();
    }
    set_backlight_cached(0);
}

static void rec_dot_opa(void *obj, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void rec_halo_opa(void *obj, int32_t v) {
    lv_obj_set_style_border_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

void app_ui_rec_led(bool active) {
    if (!s_ui_ok) return;
    if (!bsp_lvgl_lock(200)) return;
    if (active) {
        lv_anim_del(s_rec_dot, rec_dot_opa);
        lv_obj_set_style_opa(s_rec_dot, LV_OPA_COVER, 0);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_rec_dot);
        lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_20);
        lv_anim_set_duration(&a, 650);
        lv_anim_set_playback_duration(&a, 650);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_exec_cb(&a, rec_dot_opa);
        lv_anim_start(&a);

        lv_anim_del(s_rec_halo, rec_halo_opa);
        lv_obj_set_style_border_opa(s_rec_halo, LV_OPA_40, 0);
        lv_anim_t h;
        lv_anim_init(&h);
        lv_anim_set_var(&h, s_rec_halo);
        lv_anim_set_values(&h, LV_OPA_10, LV_OPA_70);
        lv_anim_set_duration(&h, 1100);
        lv_anim_set_playback_duration(&h, 1100);
        lv_anim_set_repeat_count(&h, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_exec_cb(&h, rec_halo_opa);
        lv_anim_start(&h);
    } else {
        lv_anim_del(s_rec_dot, rec_dot_opa);
        lv_obj_set_style_opa(s_rec_dot, LV_OPA_TRANSP, 0);
        lv_anim_del(s_rec_halo, rec_halo_opa);
        lv_obj_set_style_border_opa(s_rec_halo, LV_OPA_TRANSP, 0);
    }
    bsp_lvgl_unlock();
}

void app_ui_set_prov_name(const char *name) {
    if (!s_ui_ok || !name) return;
    if (!bsp_lvgl_lock(200)) return;
    set_label(s_prov_name, name);
    bsp_lvgl_unlock();
}

// 三种情况按用户下一步该做什么排序：连上并配好 > 连上了等填表 > 还没连 > 连 Wi-Fi 失败。
void app_ui_refresh_prov(bool client_connected, bool cfg_ready, bool connect_failed) {
    if (!s_ui_ok) return;
    const char *text;
    uint32_t color;
    if (connect_failed) {
        text = "网络连接失败 请重试";
        color = UI_RED;
    } else if (cfg_ready) {
        text = "配置完成";
        color = UI_SKY_DARK;
    } else if (client_connected) {
        text = "已连接 请设置";
        color = UI_SKY_DARK;
    } else {
        text = "等待手机连接";
        color = UI_RED;
    }
    if (!bsp_lvgl_lock(200)) return;
    if (s_prov_state) {
        set_label(s_prov_state, text);
        lv_obj_set_style_text_color(s_prov_state, lv_color_hex(color), 0);
    }
    bsp_lvgl_unlock();
}
