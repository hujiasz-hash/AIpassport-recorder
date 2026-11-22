# 固件搭建说明

本仓库只保存我们自己编写或修改的文件。官方固件仓库、ESP-IDF 和 Opus 源码都不入库，需要按下面步骤获取。

## 1. 前置条件

- ESP-IDF **5.5.3**，目标芯片 `esp32c3`。
- 按官方仓库 `skills/passport-setup` 和 `docs/development/engineering/environment-setup.md` 安装环境。
- 硬件：AI Passport 设备（ESP32-C3，8 MB flash，无 PSRAM），USB 数据线。
- 官方仓库：`https://github.com/FoloToy/ai-passport`，本项目使用的版本是 `e85197d56abe92c35a1cfb84fad85fba2545c9cc`。
- Opus：`xiph/opus` v1.5.2（源码包 SHA-256 未与官方页面核对，见设计文档进度记录）。

## 2. 目录布局

```
firmware/
  components/opus/CMakeLists.txt   # 定点 Opus 组件，两个工程共用
  stage1-opus/                     # 阶段 1：Opus 可行性
    main/CMakeLists.txt
    main/stage1_main.c
    main/wifi_test_secret.h.example
    partitions.csv
  stage2-cache/                    # 阶段 2：音频缓存与掉电恢复
    main/CMakeLists.txt
    main/audio_cache.c、audio_cache.h、stage2_main.c
    partitions.csv
tools/stage2/                      # 主机端脚本
  parse_cache.py、check_ogg.py、serial_watch.py、power_loss_test.py
server/                            # 阶段 3 NAS 接收服务（Python 标准库）
  server.py、Dockerfile、docker-compose.yml、tokens.example.json
tools/stage3/                      # 阶段 3 测试与模拟设备
  e2e_test.py、curl_smoke.sh、fake_device.py
```

## 3. 获取官方工程并覆盖我们的文件

```sh
git clone https://github.com/FoloToy/ai-passport.git ai-passport
cd ai-passport
git checkout e85197d56abe92c35a1cfb84fad85fba2545c9cc
```

把本仓库的文件复制到官方工程的副本中（不要改官方仓库本身，建议复制一份工作目录，例如 `stage1-opus`、`stage2-cache`）：

- 阶段 1：`firmware/stage1-opus/` 下的 `main/CMakeLists.txt`、`main/stage1_main.c`、`partitions.csv` 覆盖到副本。
- 阶段 2：`firmware/stage2-cache/` 下的 `main/` 和 `partitions.csv` 覆盖到副本。
- 两者都需要：`firmware/components/opus/` 复制到副本的 `components/opus/`。

## 4. 放置 Opus 源码

`components/opus/CMakeLists.txt` 通过相对路径 `../../../third_party/opus-1.5.2` 找源码。因此需要：

```
<工作目录>/components/opus/CMakeLists.txt
<工作目录>/../third_party/opus-1.5.2/   # 即 third_party 与官方工程同级
```

实际路径取决于目录布局。如果 `third_party` 放在别处，修改该 CMake 中的 `OPUS_DIR`。编译参数已在 CMake 中设定：`FIXED_POINT=1`、`VAR_ARRAYS`，C3 无 FPU 必须定点。

## 5. 配置 Wi-Fi（仅阶段 1 需要）

阶段 1 的 `stage1_main.c` 通过 `wifi_test_secret.h` 读取测试 Wi-Fi：

```sh
cp firmware/stage1-opus/main/wifi_test_secret.h.example <副本>/main/wifi_test_secret.h
```

填入自己的 SSID 和密码。**不要提交此文件**，仓库的 `.gitignore` 已排除它。
注意：ESP32-C3 只支持 2.4 GHz，`_5G` 结尾的 SSID 连不上。

## 6. 编译、烧录

```sh
idf.py set-target esp32c3
idf.py build
idf.py -p <端口> flash monitor
```

- Windows 端口示例：`COM3`；macOS 示例：`/dev/cu.usbmodem*`。
- 阶段 2 烧录后，缓存分区（`cache`）不会被擦除。若要清空，见第 7 节。

## 7. 缓存分区操作（阶段 2）

缓存分区偏移 `0x460000`，大小 `0x3A0000`（928 个 4 KB 扇区）。

```sh
# 擦除（会删除所有已录数据，操作前确认）
esptool.py --chip esp32c3 -p <端口> erase_region 0x460000 0x3A0000

# 读出（约 80 秒）
esptool.py --chip esp32c3 -p <端口> read_flash 0x460000 0x3A0000 cache.bin

# 按会话拆分
python tools/stage2/parse_cache.py cache.bin sessions/

# 校验 Ogg 结构
python tools/stage2/check_ogg.py sessions/*.ogg
```

`esptool.py` 随 ESP-IDF 安装，可以用 IDF 的 Python 环境运行。

## 8. 串口监听

```sh
python tools/stage2/serial_watch.py <端口> serial.log <秒数>
```

脚本会在端口断开后自动重连，适合在复位和掉电测试时使用。

**ESP32-C3 的坑**：它用的是 USB-Serial/JTAG，DTR 接 EN（复位）、RTS 接 GPIO9（下载模式）。pyserial 默认打开端口会把两条线拉高，设备立刻掉进 `waiting for download`，日志只剩一行 `ESP-ROM`。脚本里已显式 `dtr=False`、`rts=False`，自己写脚本时也要这样。

## 8.1 断电测试

`tools/stage2/power_loss_test.py` 负责前后的取证，中间的拔电动作由人做。

```sh
# 1. 擦除缓存（会删所有已录数据，必须加 --yes）
python tools/stage2/power_loss_test.py /dev/cu.usbmodem* ./pl --yes erase

# 2. 抓日志，等你拔电（连续 8 秒无输出即判断已断电）
python tools/stage2/power_loss_test.py /dev/cu.usbmodem* ./pl record

# 3. 重新上电后校验：抓启动日志 + 读回缓存 + 拆分会话
python tools/stage2/power_loss_test.py /dev/cu.usbmodem* ./pl verify
```

真实断电只能用拔 USB 或断电池来做，软件复位（RTS）模拟不了。2026-11-22 已实测通过，结果见设计文档进度记录。

## 9. 已知问题和注意事项

- **屏幕停在 "STAGE2 boot"**：多半是缓存已满。录音循环只在每 10 秒时刷新屏幕，错误（E500）只写串口，不显示在屏幕上。此时先擦除缓存分区。
- **企业文件标记工具（Windows）**：会给新写入的 PNG 注入 tEXt 标签，导致 lvgl 组件 CHECKSUM 校验失败。Windows 上可能需要临时绕过，Mac 上通常不存在此问题。
- **包管理**：内网 pip 源需要认证。需要的主机端工具尽量只用标准库，本仓库的脚本都不依赖第三方包（串口监听除外，需要 `pyserial`，IDF Python 环境已包含）。
- **录音数据**：录音文件属于个人数据，不入库。

## 10. NAS 接收服务

见 [nas-deploy.md](nas-deploy.md)。本机验证不需要 NAS：

```sh
python3 tools/stage3/e2e_test.py                              # 44 条用例
python3 tools/stage3/fake_device.py --url http://127.0.0.1:18081 \
    --token <token> --dev <dev8> --synthetic                  # 模拟设备
```
