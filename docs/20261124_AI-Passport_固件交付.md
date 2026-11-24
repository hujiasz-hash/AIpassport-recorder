---
title: AI Passport 固件交付：OctoBara 主页与 BLE 录音
version: v0.1
author: 胡嘉
date: 2026-11-24
---

# AI Passport 固件交付：OctoBara 主页与 BLE 录音

## 版本

| 版本 | 日期 | 作者 | 改动点 |
| --- | --- | --- | --- |
| v0.1 | 2026-11-24 | 胡嘉 | 记录已烧录版本、验证结果和回滚位置。 |

## 链接

- 库内：[固件环境与构建](firmware-setup.md) · [录音同步设计](design.md) · [蓝牙历史诊断](ble-diagnosis-20261123.md)
- 源码：[主页](../firmware/passport-app/main/app_ui.c) · [蓝牙](../firmware/passport-app/main/app_ble.c) · [录音缓存](../firmware/passport-app/main/app_store.c) · [分区表](../firmware/passport-app/partitions.csv) · [动画转换](../tools/stage4/make_octobara_frames.py)

---

## 本版改了什么

| 模块 | 行为 |
| --- | --- |
| 主页 | 正常显示北京时间 `HH:MM`、电量百分比和电池图标。左上角为蓝牙图标和状态文字：已连接为蓝色，未连接为灰色。未校时显示 `--:--`；配对时用六位配对码临时替代时间。 |
| 动画 | 使用 `01_octobara_typing_480.gif`，离线转换为 184×192 的 10 帧 RGB565，保留每帧 100 ms 的节奏。帧数据占 706,560 字节，位于 Flash，只在主页亮屏时播放，设备无需运行 GIF 解码器。 |
| 录音与同步 | 短按 OK 仍由设备开始或停止录音。低功耗蓝牙（BLE）同步读取已落盘的录音时使用独立扇区缓冲，避免覆盖待写数据。设备向接收端通知开停及录音时长，重连时更新状态。 |

编译直接使用已入库的 [RGB565 帧](../firmware/passport-app/main/octobara.rgb565)。只有要更换动画时才需要 Pillow 和原始 GIF。仓库根目录运行：

```sh
python3 tools/stage4/make_octobara_frames.py \
  /path/to/03_animated_gifs/01_octobara_typing_480.gif \
  firmware/passport-app/main/octobara.rgb565
```

## 验证到哪一步

| 检查项 | 结果 |
| --- | --- |
| 编译 | ESP-IDF 5.5.3 / ESP32-C3；镜像 `0x1dc950` 字节，2 MiB 应用槽剩 `0x236b0` 字节（约 142 KiB）。 |
| 写入 | 核对了设备分区表和槽位；仅写 `ota_0`（`0x20000`），esptool 校验通过。未擦除录音缓存（`0x460000`）。 |
| 真机启动与蓝牙 | 串口确认从 `0x20000` 启动，ELF SHA-256 前缀 `3adeabb88`，显示初始化成功，随后 BLE 连接并完成校时。 |
| 尚未复测 | 本版图标和动画的实屏效果，以及完整的录音、断连补传和纪要链路。 |

新镜像 SHA-256：`6b3fb028e8d12f4ebb1c7d84d92ce2922c360b4c08c3c2ca7e245fb15967b7b2`。

## 回滚

当前机器的本地产物目录是 `artifacts/passport-octobara-bt-icon-20261124/`。其中 `boot.log`、`flash.log` 和 `SHA256SUMS` 留作核验，`ota0-before-bt-icon.bak.bin` 是最近一次完整的 `ota_0` 备份，即不带蓝牙图标的 OctoBara 版本。更早的本地备份已清理。回滚前重新核对设备和应用槽，只写应用槽，不写分区表或擦录音缓存。
