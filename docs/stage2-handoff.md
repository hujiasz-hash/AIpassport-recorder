# 阶段 2 交接说明（Windows → Mac）

最后更新：2026-11-20

## 1. 当前状态

| 阶段 | 状态 |
|---|---|
| 0 官方 demo 烧录 | 完成 |
| 1 Opus 可行性 | 完成（听感待确认） |
| 2 缓存 + 掉电恢复 | 完成（真实断电未测，听感待确认） |
| 3 NAS 接收 + FN Connect 外网验证 | 未开始，阻塞 |

阶段 2 已验证：
- 扇区缓存格式：4 KB 扇区，20 字节头（magic、序号、会话号、长度、CRC32）。
- 挂载时只校验最后一扇区，恢复写指针。
- 两次 RTS 硬复位模拟掉电：每次开新会话，挂载正确（`cache mounted sectors=94`）。
- 读出后按会话拆分，得到 4 个 Ogg 流，CRC 全部通过，页序号连续，granule 单调。
- 缓存满时报 E500 并停止（仅写串口，屏幕无提示）。

阶段 2 未完成：
- 真实断电测试（拔电池或 USB 后重启），目前只是复位模拟。
- 解码播放未在本机验证。

待听的文件（Ogg Opus，用浏览器或 VLC 直接打开）：
- `esp-work\stage2-tools\listen\stage2_session_A_104s.ogg`（约 104 秒）
- `esp-work\stage2-tools\listen\stage2_session_B_45s.ogg`（约 45 秒）

## 2. 下一步

1. 听 A、B 两段录音，评价音质。若不可接受，回到阶段 1 调复杂度（当前 2），或切换 IMA-ADPCM。
2. 做真实断电测试：录音中拔电，重启后校验缓存。
3. 处理 E500 的屏幕提示（可以放到阶段 6 UI 中）。
4. 删除误输出文件 `esp-work\stage2-tools\--help`（与 `cut.ogg` 同大小，无用）。
5. 决定是否提交 `docs/design.md` 的进度记录，以及本文件。
6. 进入阶段 3：在 NAS 上起测试容器，用手机热点（模拟外网）验证 `curl -X PUT` 经 FN Connect 中继的三项内容（见设计文档 7.1）。这一步依赖 NAS 访问权限和 FN ID。

## 3. 环境（Windows 当前环境，Mac 需要重建）

固件和工具已入库，搭建步骤见 `docs/firmware-setup.md`：

- 固件源码：`firmware/`（阶段 1、阶段 2、共用的 Opus 组件 CMake）。
- 主机工具：`tools/stage2/`（`parse_cache.py`、`check_ogg.py`、`serial_watch.py`）。
- 官方仓库和 ESP-IDF 不入库，按搭建说明获取。
- 录音音频和 dump 文件属于个人数据，未入库，仍在 Windows 的 `esp-work\stage2-tools\` 中。
- Windows 上的 ESP-IDF 环境：`esp-work\esp-idf-v5.5.3`，IDF Python 环境 `esp-work\.espressif\python_env\idf5.5_py3.14_env`。系统 Python 没有 esptool。
- 官方仓库：`https://github.com/FoloToy/ai-passport`，版本 `e85197d`。

Windows 上的注意事项：
- 串口：COM3。Mac 上的端口通常是 `/dev/cu.usbmodem*`。
- 企业文件标记工具会给新写入的 PNG 注入 tEXt 标签，导致 lvgl 组件 CHECKSUM 校验失败。Windows 上用 `esp-work\scripts\install_lvgl_verified.py` 绕过。Mac 上可能不存在该问题，先不要照搬。
- 系统 Python（`C:\Program Files\Python39`）没有 esptool，必须用 IDF Python 环境运行。
- 内网 pip 源需要认证，无法直接安装 PyAV 等包。
- ESP32-C3 不支持 5 GHz Wi-Fi，只能连 2.4 GHz。
- Wi-Fi 凭据在 `stage1-opus\main\wifi_test_secret.h`，不要提交。

Mac 上需要做的：
1. 按官方 `passport-setup` 安装 ESP-IDF 5.5.3。
2. 把 `stage2-cache` 目录拷到 Mac 并编译。
3. 用 esptool 操作缓存分区（偏移 `0x460000`，大小 `0x3A0000`）：
   - 擦除：`esptool.py --chip esp32c3 -p <port> erase_region 0x460000 0x3A0000`
   - 读出：`esptool.py --chip esp32c3 -p <port> read_flash 0x460000 0x3A0000 cache.bin`
4. 播放和校验用 `brew install ffmpeg`（若需要转换），或直接用浏览器播放 Ogg。

## 4. 注意事项

- 测试期间缓存被写满会导致屏幕停在 "STAGE2 boot"（录音循环只在每 10 秒时刷新屏幕）。遇到这种情况先擦除缓存分区。
- 擦除缓存会删除所有已录数据，操作前要确认。
- `docs/design.md` 当前有未提交改动（阶段 1 决定、FN ID 处理、阶段 2 进度），本文件是新增的未跟踪文件。
