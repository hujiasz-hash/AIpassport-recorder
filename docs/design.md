---
title: AI Passport 录音固件 — 设计文档
version: v0.6
author: 胡嘉
date: 2026-11-22
---

# AI Passport 录音固件 — 设计文档

## 版本
| 版本 | 日期 | 作者 | 改动点 |
| --- | --- | --- | --- |
| v0.1 | 2026-11-19 | 胡嘉 | 首版草案 |
| v0.2 | 2026-11-19 | 胡嘉 | 录音改为短按切换；缓存改为“先落盘再上传”单一路径，去掉 ring buffer；上传改为长连接；中转方案与第 4 节解耦；补电池、时间同步、RAM 预算、错误码；Opus 验证提为第一个里程碑；列出待输入项 |
| v0.3 | 2026-11-19 | 胡嘉 | 按官方仓库填入硬件参数（8 MB flash、无 PSRAM、520 mAh、三键、IDF 5.5.3）；中转改为只用 FN Connect，去掉 VPS/Cloudflare；NAS 端定为 Docker 接收服务；新分区表预留 OTA；上传改为每 60 s 批量发送以省电；指示间隔定为 10 s；更新待输入项 |
| v0.4 | 2026-11-19 | 胡嘉 | 绿色电源灯为硬件灯、软件不可控，录音指示定为背光；确认无固定公网 IP，FN Connect 走中继，中继验证升为首要阻塞项 |
| v0.5 | 2026-11-22 | 胡嘉 | 阶段 2 真实断电测试通过并补记结果；阶段 3 接收服务实现并定稿 7.2 线格式（session_id 28 字符、409 重同步、零长探针、fsync 顺序、meta 权威长度）；补记固件缺口（会话需存录音开始时间）和中继头透传待验证项 |
| v0.6 | 2026-11-22 | 胡嘉 | 上传链路改为 BLE → 电脑 agent → 本地转写，弃用 NAS 接收服务（`server/` 保留备查）；去掉 Wi-Fi 上传、HTTPS、token 与配网流程（Wi-Fi 直传降级为编译开关）；错误码改为 BLE 语义；时间同步改为 agent 下发；里程碑重排为 3–8 |

## 链接
- 库内：[nas-deploy.md](nas-deploy.md) — v0.5 NAS 方案部署说明，已弃用备查
- 库内：[network-handoff.md](network-handoff.md) — v0.5 FN Connect/DDNS 外网验证交接，已弃用备查
- 外部：[FoloToy/ai-passport](https://github.com/FoloToy/ai-passport) — 硬件参数、BSP、开发环境（`skills/passport-setup`）
- 外部：[bleak](https://github.com/hbldh/bleak) — agent 蓝牙库（Win10+/macOS/Linux）
- 外部：[faster-whisper](https://github.com/SYSTRAN/faster-whisper) — 本地转写首选
- 外部：[whisper.cpp](https://github.com/ggml-org/whisper.cpp) — 无 GPU 时的 CPU 转写备选

---

## 1. 硬件（来源：官方仓库 `components/bsp/include/bsp_pins.h`、`partitions.csv`、`docs/CHANGELOG.zh_CN.md`）

| 项 | 参数 |
|---|---|
| 主控 | ESP32-C3，160 MHz RISC-V，无 FPU，约 400 KB SRAM，**无 PSRAM** |
| Flash | 8 MB；出厂分区为 NVS + PHY + 单个 factory 应用（0x7f0000） |
| 屏幕 | ST7789P3 240×320，SPI2；背光 GPIO21，LEDC PWM |
| 按键 | 上 / 下 / OK 三键，共用 GPIO0 的 ADC 分压；BSP 长按阈值 500 ms；GPIO0 低电平可唤醒 deep sleep |
| 音频 | ES8311，I2S0 全双工（录音 DIN=GPIO4） |
| 电池 | 520 mAh 锂电，CW2017 电量计（与 ES8311 共用 I2C0，必须复用 BSP 的总线） |
| 指示灯 | 只有绿色电源/充电灯，由硬件驱动，BSP 中没有 MCU 可控的 LED 引脚，软件不可控 |
| 开发环境 | ESP-IDF **5.5.3**；安装与构建按仓库内 `docs/development/engineering/environment-setup.md` 和 `skills/passport-setup`；交付合并镜像 `full.bin` 烧录到 0x0 |

## 2. 需求

| 项 | 要求 |
|---|---|
| 主页 | 熄屏待机，按任意键亮屏，显示 OctoBara 动画 + 时间日期 |
| 熄屏 | 无操作 30 s 熄屏；录音期间也熄屏 |
| 录音 | 短按 OK 开始，再短按停止；单次最长 2 小时，到时自动停止 |
| 防误触 | 熄屏时第一次按键只亮屏，不触发功能 |
| 同步 | 录音经 BLE 同步到电脑 agent，agent 落盘并本地转写成文本；设备不转写 |
| 配置 | 无需配网：转写设置、输出目录全部在电脑 agent；设备只做一次 BLE 配对 |
| 断连 | 电脑不在范围或 agent 未运行：屏幕报警 + 继续录到本地缓存 + 显示剩余可缓存时间 |
| 错误 | 屏幕显示错误码 + 原因，同时写 log |
| 提示音 | 不要 |
| 录音指示 | 每 10 s 背光短亮一次（约 100 ms） |
| 设备数 | 1 台 |
| OTA | 首版不做，但分区表现在就预留（之后改分区只能 USB 重刷） |

## 3. 编码

**Opus，16 kHz 单声道，12 kbps VBR，20 ms 帧，Ogg Opus 封装。** 实测 13.85 kbit/s（含 Ogg 封装），约 6.2 MB/小时，2 小时约 12.5 MB。

- C3 无 FPU，libopus 必须定点编译（`FIXED_POINT`），complexity 从 0–3 试。
- 候选：Espressif `esp_audio_codec`（是否支持 C3 未核实）或移植 libopus。
- 退路：C3 跑不动则改 IMA-ADPCM（16 kHz 时 64 kbps），缓存时长约降到 1/5。
- 决定（2026-11-20）：保留 Opus 方案，复杂度取 2；只有开 Wi-Fi 复测不过才切 ADPCM。

## 4. 内存预算（无 PSRAM，需实测）

| 项 | 估算 RAM | 说明 |
|---|---|---|
| NimBLE | 约 50–60 KB | BLE host + controller；M3 实测，必须与 Opus、LVGL 并存验证 |
| Opus 编码器 | 未知 | 里程碑 1 实测 |
| LVGL | 小型单 DMA 缓冲（BSP 现状） | 录音熄屏时停掉动画 |
| 释放 | 约 90–110 KB | 弃用 Wi-Fi 直传后不再常驻 Wi-Fi 协议栈、mbedTLS 与 SoftAP |

Wi-Fi 直传代码（`app_net`、`app_upload` 的 HTTP 部分、SoftAP 配网）保留为编译开关 `CONFIG_APP_WIFI_UPLOAD`，默认关；家庭 Wi-Fi 场景可开，NVS 配网流程随之恢复。

## 5. 分区表（8 MB）

| 分区 | 大小 | 说明 |
|---|---|---|
| nvs | 24 KB | 配对信息、缓存读写指针 |
| otadata | 8 KB | OTA 预留 |
| phy_init | 4 KB | |
| ota_0 | 2 MB | 首版只用这一个 |
| ota_1 | 2 MB | OTA 预留 |
| log | 256 KB | 循环日志 |
| cache | 约 3.6 MB | 音频缓存 |

缓存容量：3.6 MB × 8 ÷ 13.85 kbps ≈ **36 分钟**（实测码率 13.85 kbit/s，12 kbps 是编码器目标值，Ogg 封装和 VBR 溢出会多约 15%；换算按扇区扣掉每扇区 20 字节头部）。录满缓存后由 E500 截断，已录部分保留。`MAX_SEC` 硬闸是 2 小时，和"单次最长 2 小时"对齐；连上 agent 时同步边录边回收，实际可录时长一般远大于 36 分钟。前提是应用镜像小于 2 MB，需在里程碑 0 编译后确认；超过则调整 ota 分区大小。

## 6. 缓存与断连

所有音频先写 flash 缓存，同步任务只从缓存读。电脑不在范围或 agent 未运行只是同步暂停，由这条路径兜底；掉电时已落盘的数据也能保住。

- 先进先出、不覆盖：写指针和已确认同步指针存 NVS。
- 按 4 KB 扇区攒满再写，减少擦写。
- 断连：报警 + 错误码 + 剩余可缓存时间（剩余空间 ÷ 码率）。
- 写满：停止录音，已录部分保留，连上 agent 后同步，记 E500。
- 开机：缓存里有未同步数据时，连上 agent 后自动续传。

## 7. 同步与转写（BLE → 电脑 agent）

### 7.1 决定与链路

2026-11-22 决定弃用 NAS 方案：公司环境没有可用的 Wi-Fi 给设备，改为设备经 BLE 把缓存同步到电脑 agent，agent 落盘并本地转写成文本。链路只有一跳，无网络部署、无证书、无 token，录音和转写全程不出本机。

```text
设备（flash 缓存） ── BLE GATT ──> 电脑 agent（bleak） ──> 落盘 .ogg ──> 本地转写 ──> 文本
```

吞吐对账：录音 13.85 kbit/s ≈ 1.7 KB/s，BLE notify 实测普遍 20 KB/s 以上，余量约 10 倍。v0.5 的外网调查（`network-handoff.md`）、NAS 部署（`nas-deploy.md`）、服务端与 e2e（`server/`、`tools/stage3/`，44 用例全过）保留备查，未来恢复网络直传可直接复用。

### 7.2 BLE 传输协议

- **GATT**：自定义服务一个，TX 特征（设备 → agent，notify）+ RX 特征（agent → 设备，write-with-response）。MTU 协商到 512，单包载荷按协商结果分帧；Windows 蓝牙栈协商值可能有差异，帧长度以连接时协商为准。
- **流控**：滑窗累积 ack。设备发完一个窗口即暂停，agent 写 RX 回累计 `received`；设备只以 `received` 为准推进读指针（沿用 v0.5 "以对端确认为准" 的原则）。窗口初始 4 KB，M3 实测后调。
- **帧格式**：`sid(28B ASCII) + offset(4B LE) + len(2B LE) + payload + flags(1B，bit0=final)`。agent 按 `sid` 落盘 `<日期>/<sid>.ogg`；`sid` 沿用 v0.5 的 28 字符三段格式 `<dev8>-<ts10>-<rand8>`（`dev8` 是 MAC 第 3..6 字节的十六进制，等于开机日志里的 `dev=`），agent 侧正则校验。
- **可靠性**：BLE 链路层自带重传，v0.5 的 409/gap/conflict 服务端重同步语义不再需要。断连重连后 agent 以文件实际长度为权威回 `received`，设备从该偏移续传。落盘顺序不可改——写数据 → fsync → 才回 ack，掉电最多丢一个窗口，设备重传即可。
- **final**：`received==total` 时 agent 标记会话完成并触发转写；不一致时回错误由设备重发，语义对齐 v0.5 的 `incomplete`/`bad_total`。
- **时间同步**：连接建立后 agent 先下发当前 Unix 时间；设备用它盖 `start_ts`（见 10.4 固件缺口）。未获得时间时开录报 E203。
- **安全**：BLE 用 Just Works 配对建立加密链路（一跳内、不出本机，暴露面比 NAS 公网入口小；IT 有要求再升级 Passkey）。无网络 token，agent 本机配置文件持有全部设置。
- **省电**：录音时维持连接持续同步（BLE 低占空比），不录音可断开待连；持续连接与每 60 s 批量两种节奏 M3 实测功耗后定。

### 7.3 PC agent 与转写

- Python ≥ 3.10 + bleak（Win10+/macOS/Linux 自带蓝牙栈均可）；单机单设备，不做多机。
- 转写默认 faster-whisper（16 kHz Ogg Opus 直接解码输入；中文优先 large-v3，CPU 弱则 small），无 GPU 电脑用 whisper.cpp；云端 API 只留可选配置，默认本地。
- 输出 `<日期>/<sid>.txt`；转写成功后 `.ogg` 移归档目录或删除，策略可配。
- 配置文件（不入仓库）：输出目录、模型、归档策略、已配对设备过滤。

## 8. 功耗估算（需实测）

520 mAh；录音时 BLE 持续同步 + 编码，BLE 低占空比下平均电流按 30–60 mA 估（推测值，没有实测依据，低于 v0.5 的 Wi-Fi 批量发送预估），2 小时约用 60–120 mAh，满电够用。电量低于阈值时拒绝开始录音（E600）。里程碑 6 用 CW2017 读数实测。

## 9. 错误码（草案）

| 码 | 模块 | 含义 |
|---|---|---|
| E200 | BLE | 扫描不到已配对电脑 |
| E201 | BLE | 连接 / 配对失败 |
| E202 | BLE | 同步超时（ack 连续丢失） |
| E203 | 同步 | 未从 agent 获得时间 |
| E300 | 同步 | agent 协议错误（final 长度不一致等，附细节） |
| E400 | 录音 | ES8311 或显示屏启动失败 |
| E401 | 录音 | 编码器失败 |
| E402 | 缓存 | 空间不足（扫描列表等临时分配失败） |
| E500 | 缓存 | 缓存已满，录音被截断 |
| E501 | 缓存 | flash 读写失败 |
| E600 | 电源 | 电量低 |

固件现有 `app_error` 编码在里程碑 4 按此表重映射。

## 10. 待输入 / 待验证

| # | 问题 | 谁 |
|---|---|---|
| 1 | BLE spike（里程碑 3 前置）：公司电脑蓝牙可用性、bleak 实测吞吐/MTU/RSSI；NimBLE 与 Opus+LVGL 并存的 RAM 峰值 | 胡嘉 |
| 2 | 转写选型：公司电脑 CPU/GPU 跑 faster-whisper 哪档模型，中文听感确认；无 GPU 时 whisper.cpp | 胡嘉 |
| 3 | 配对方式：Just Works 是否满足 IT 要求，必要时 Passkey | 一起定 |
| 4 | **固件缺口（不变，时间来源改为 agent 下发）**：缓存只存 32 位会话号，没有录音开始时间，而 session_id 需要 `ts10`。需在 `audio_cache_begin_session` 时把 `{session32, start_ts, acked}` 写 NVS。`start_ts` 取 agent 下发时间，未获得则报 E203 先同步再开录 | 胡嘉 |
| 5 | 同步节奏：录音中持续连接 vs 每 60 s 批量，M6 实测功耗后定 | M6 定 |

## 11. 里程碑

| # | 内容 | 通过标准 |
|---|---|---|
| 0 | 按官方 `passport-setup` 装 ESP-IDF 5.5.3，跑通主分支 demo | 屏幕亮、三键有响应；记录应用镜像大小 |
| 1 | Opus 可行性：ES8311 16 kHz → 定点 Opus → Ogg 写 flash | ffplay 可播；实测 CPU、RAM 峰值 |
| 2 | 新分区表 + flash 缓存 + 掉电恢复 | 录音中拔电，重启后数据完整 |
| 3 | BLE spike：NimBLE 广播 + bleak 连接，实测吞吐/MTU/RSSI，RAM 含 Opus 并存复测 | 吞吐 ≥ 10 KB/s；RAM 峰值可容纳全链路 |
| 4 | GATT 传输层 + agent 透传回环（滑窗 ack、断连续传） | 断连 10 分钟恢复后数据完整 |
| 5 | agent 落盘 + 本地转写 e2e | 30 s 录音 → 文本落盘，中文可读 |
| 6 | 设备边录边同步 + 功耗实测 | 2 小时录音完整转写；记录电量消耗 |
| 7 | 主页 UI、熄屏、10 s 指示、错误码显示 | |
| 8 | OTA（之后再做） | |

### 进度记录

| 阶段 | 状态 | 结果 |
|---|---|---|
| 0 | 完成（2026-11-20） | 官方主分支 demo 编译烧录到 COM3，屏幕亮、右侧三键有响应。应用镜像 0x174600 字节（约 1.45 MB），合并镜像 0x184600 字节，factory 分区剩余约 82%。 |
| 1 | 完成（2026-11-20） | 测试工程 `esp-work\stage1-opus`（复制自官方仓库，未改动官方仓库）。Opus 源码为 xiph/opus v1.5.2（源码包 SHA-256 `65c1d2f7…7ce1`，未与官方页面核对），定点编译。录音 30 s，16 kHz 单声道，Ogg Opus 12 kbps，OGG 44264 字节，写入 voice 分区（0x200000）。ffmpeg 解码无报错，时长 30.00 s，音量统计 mean -40.8 dB、max -0.1 dB。复杂度 5 时编码平均 19.6 ms、最大 22.4 ms，不可用；复杂度 2 时平均 11.3 ms、最大 12.9 ms（约 57% CPU）。最小剩余堆约 240 KB，最大连续块约 112 KB。录音任务每帧 `vTaskDelay(1)` 让出 CPU，避免看门狗触发。**Wi-Fi 复测（2026-11-20，已连接 2.4 GHz AP，省电 MIN_MODEM，录音中）**：连接后最小剩余堆约 210 KB；录音 30 s 期间最小剩余堆约 136 KB，最大连续块仍为 112 KB；复杂度 2 时编码平均 11.8 ms、最大 15.3 ms（约 59% CPU），仍可用。**未完成**：TLS 握手内存尚未实测（需要 NAS 端点，阶段 3 再测）；听感评价待人工确认。注意 ESP32-C3 不支持 5 GHz，`_5G` 的 SSID 连不上。 |
| 2 | 完成（2026-11-22，待听感确认） | 缓存分区 0x460000 起，0x3A0000 字节，928 个 4 KB 扇区。录音按扇区落盘，每扇区带头部（magic、序号、会话号、长度）和 CRC32；开机扫描恢复写指针，只校验最后一扇区。**真实断电测试（2026-11-22，macOS）**：擦除缓存后录制约 120 s 拔掉 USB。断电前最后状态 `cache mounted sectors=3` 起新会话 `5891708b`；重新上电后 `cache mounted sectors=59 payload=240484 last_session=5891708b`，写指针正确恢复，会话号一致。读回缓存 72 个有效扇区，拆出 3 个会话（`4f4d2385` 12 KB、`5891708b` 228 KB、`904aae77` 53 KB），全部 `crc_bad=0`、`seq_gaps=0`、`gran_monotonic=True`，`truncated_tail=True` 符合断电预期。`5891708b` 断电前已落盘 59 个满扇区（240484 字节），读回 228256 字节，差值来自断电瞬间 RAM 缓冲里未满扇区的内容，符合"只保证已落盘扇区"的设计。另验证 E500：缓存写满时 `E500 cache full at frame 4355` 停止录音，已录部分保留（928/928 扇区）。**未完成**：听感评价待人工确认；屏幕不显示 E500（推迟到阶段 6 UI）。测试音频在 `~/Projects/passport-flash-backup/`，不入库。串口有个坑：pyserial 默认打开端口会拉 DTR/RTS 把 C3 顶进下载模式，`serial_watch.py` 已改为显式 `dtr=False rts=False`。 |
| 3 | 接收服务完成；方案已弃用（2026-11-22，v0.6 改 BLE 同步） | 服务端 `server/server.py`（只用 Python 标准库，约 600 行）已实现并按 v0.5 7.2 定稿协议。`tools/stage3/e2e_test.py` 44 条用例全过，`curl_smoke.sh` 8/8 过，`fake_device.py` 验证断网续传、冲突重同步、死循环保护。NAS 部署与外网验证不再执行，代码保留备查。 |
| 4–8 | 未开始（含义按 v0.6 里程碑表） | |

阻塞项：企业文件标记工具会给新写入的 PNG 注入 tEXt 标签，导致 lvgl 组件的 CHECKSUM 校验失败。当前用 `esp-work\scripts\install_lvgl_verified.py` 绕过，待 IT 决定是否排除 esp-work 目录。
