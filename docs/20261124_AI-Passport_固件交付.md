---
title: AI Passport 固件交付：OctoBara 主页与 BLE 录音
version: v0.2
author: 胡嘉
date: 2026-11-24
---

# AI Passport 固件交付：OctoBara 主页与 BLE 录音

## 版本

| 版本 | 日期 | 作者 | 改动点 |
| --- | --- | --- | --- |
| v0.1 | 2026-11-24 | 胡嘉 | 记录已烧录版本、验证结果和回滚位置。 |
| v0.2 | 2026-11-24 | 胡嘉 | 主页动画改为逐行差分无损压缩；新增监听耳机 OctoBara 录音页、实时声波与 30s 微亮省电；补齐压缩字库配置。 |

## 链接

- 库内：[固件环境与构建](firmware-setup.md) · [录音同步设计](design.md) · [蓝牙历史诊断](ble-diagnosis-20261123.md)
- 源码：[主页](../firmware/passport-app/main/app_ui.c) · [蓝牙](../firmware/passport-app/main/app_ble.c) · [录音缓存](../firmware/passport-app/main/app_store.c) · [分区表](../firmware/passport-app/partitions.csv) · [动画转换](../tools/stage4/make_octobara_frames.py)

---

## 本版改了什么

| 模块 | 行为 |
| --- | --- |
| 主页 | 正常显示北京时间 `HH:MM`、电量百分比和电池图标。左上角为蓝牙图标和状态文字：已连接为蓝色，未连接为灰色。未校时显示 `--:--`；配对时用六位配对码临时替代时间。 |
| 动画压缩 | 使用 `01_octobara_typing_480.gif`（184×192，10 帧，100 ms/帧），离线编码为帧 0 裸 RGB565 + 帧 1~9 逐行差分 RLE，体积从 706,560 字节降至 107,596 字节（105.1 KiB），像素级无损，LVGL 逐行解码仅占静态行缓冲。 |
| 录音界面 | 与主页统一暖米色底，对齐顶部蓝牙/电量状态栏；显示呼吸红点状态胶囊、48 号大字计时、96×96 监听耳机 OctoBara 徽章、左右对称 8 根实时麦克风声波柱及缓存余量进度卡。录音中 30 秒无按键背光降至 20% 微亮，按上/下键恢复 85%。补齐 `CONFIG_LV_USE_FONT_COMPRESSED=y` 确保压缩中文字库正常渲染。 |
| 录音与同步 | 短按 OK 仍由设备开始或停止录音。低功耗蓝牙（BLE）同步读取已落盘的录音时使用独立扇区缓冲，避免覆盖待写数据。设备向接收端通知开停及录音时长，重连时更新状态。 |

编译直接使用已入库的 [主页差分帧](../firmware/passport-app/main/octobara.rgb565) 与 [录音徽章](../firmware/passport-app/main/octobara_rec.rgb565)。只有要更换图片素材时才需要 Pillow 和原始素材。仓库根目录运行：

```sh
python3 tools/stage4/make_octobara_frames.py \
  /path/to/03_animated_gifs/01_octobara_typing_480.gif \
  firmware/passport-app/main/octobara.rgb565 \
  --rec-avatar /path/to/02_avatars/08_头像_深夜极客_连帽卫衣_512.png \
  --rec-output firmware/passport-app/main/octobara_rec.rgb565
```

## 验证到哪一步

| 检查项 | 结果 |
| --- | --- |
| 编译 | ESP-IDF 5.5.3 / ESP32-C3；镜像 `0x1551c0` 字节，2 MiB 应用槽剩 `0xaae40` 字节（约 683 KiB，33% 空闲）。 |
| 写入 | 核对了设备分区表和槽位；仅写 `ota_0`（`0x20000`），esptool 校验通过。未擦除录音缓存（`0x460000`）。 |
| 真机启动与实屏 | 串口确认从 `0x20000` 启动，ELF SHA-256 前缀 `d48691d75`，BLE 自动重连并完成校时；主页与录音页画面及文字已真机目视确认。 |
| 尚未复测 | 完整的长会录音、断连补传和纪要端到端链路。 |

新镜像 SHA-256：`6df0c25d385b337607a8f7880b96c2dafc2ba2233526b721aeed0b54b6561696`。

## 回滚

当前机器的本地产物目录是 `artifacts/passport-rec-ui-20261124/`（上一版含完整 `ota_0` 备份的目录为 `artifacts/passport-octobara-bt-icon-20261124/`）。回滚前重新核对设备和应用槽，只写应用槽，不写分区表或擦录音缓存。
