# AI Passport 固件：BLE 短会联调版

- `passport-app.bin`：ESP32-C3 应用镜像，ESP-IDF v5.5.3 编译。
- `partition-table.reference.bin`：参考分区表，仅用于比对，**不写入**。
- `SHA256SUMS`：`shasum -a 256 -c SHA256SUMS` 验证镜像。

已读回当前 USB 设备 `/dev/cu.usbmodem21201` 的分区表并与参考文件逐字节比对，当前运行的是 `0x20000` 的 `ota_0`。缓存位于 `0x460000`，与应用槽互不重叠。本次未烧录。

**确认允许升级后**，先备份当前应用，再只替换应用槽。不要执行 `erase_flash`，也不要写分区表、NVS、缓存或 OTA 选择数据。端口如变更应重新核对设备与分区。

```sh
source ~/esp/esp-idf/export.sh
# 在此交付目录执行；备份文件名要使用尚不存在的路径。
python -m esptool --chip esp32c3 -p /dev/cu.usbmodem21201 read_flash 0x20000 0x200000 previous-ota0.bak.bin
python -m esptool --chip esp32c3 -p /dev/cu.usbmodem21201 -b 460800 write_flash 0x20000 passport-app.bin
```

联调：启动源码版 `macos_overlay/会议记录.app`，首次确认蓝牙权限和配对，设备显示「蓝牙已连接」后短按 OK，讲话约 30 秒，再按 OK 停止。检查文字在停录前写入、最后一句未漏、历史库只有一条。再用一次关 App、离线录音、重开 App 确认续传。旧版缓存不会被自动迁移，现有 Flash 只适合验证短会；尚未实现录制中的环形回收。
