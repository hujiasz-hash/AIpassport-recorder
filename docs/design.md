---
title: AI Passport 录音上传固件 — 设计文档
version: v0.5
author: 胡嘉
date: 2026-11-22
---

# AI Passport 录音上传固件 — 设计文档

## 版本
| 版本 | 日期 | 作者 | 改动点 |
| --- | --- | --- | --- |
| v0.1 | 2026-11-19 | 胡嘉 | 首版草案 |
| v0.2 | 2026-11-19 | 胡嘉 | 录音改为短按切换；缓存改为“先落盘再上传”单一路径，去掉 ring buffer；上传改为长连接；中转方案与第 4 节解耦；补电池、时间同步、RAM 预算、错误码；Opus 验证提为第一个里程碑；列出待输入项 |
| v0.3 | 2026-11-19 | 胡嘉 | 按官方仓库填入硬件参数（8 MB flash、无 PSRAM、520 mAh、三键、IDF 5.5.3）；中转改为只用 FN Connect，去掉 VPS/Cloudflare；NAS 端定为 Docker 接收服务；新分区表预留 OTA；上传改为每 60 s 批量发送以省电；指示间隔定为 10 s；更新待输入项 |
| v0.4 | 2026-11-19 | 胡嘉 | 绿色电源灯为硬件灯、软件不可控，录音指示定为背光；确认无固定公网 IP，FN Connect 走中继，中继验证升为首要阻塞项 |
| v0.5 | 2026-11-22 | 胡嘉 | 阶段 2 真实断电测试通过并补记结果；阶段 3 接收服务实现并定稿 7.2 线格式（session_id 28 字符、409 重同步、零长探针、fsync 顺序、meta 权威长度）；补记固件缺口（会话需存录音开始时间）和中继头透传待验证项 |

## 链接
- 库内：无
- 外部：[FoloToy/ai-passport](https://github.com/FoloToy/ai-passport) — 硬件参数、BSP、开发环境（`skills/passport-setup`）
- 外部：[fnOS 远程访问方式](https://help.fnnas.com/articles/v1/access/how-access) — FN Connect 直连/中继说明

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
| 上传 | 边录边传到 NAS，设备不转写；NAS 每天处理，转写引擎后定 |
| 配网 | 设备开热点，手机连上后用浏览器填表（不装 App） |
| 断网 | 屏幕报警 + 继续录到本地缓存 + 显示剩余可缓存时间 |
| 错误 | 屏幕显示错误码 + 原因，同时写 log |
| 提示音 | 不要 |
| 录音指示 | 每 10 s 背光短亮一次（约 100 ms） |
| 设备数 | 1 台 |
| OTA | 首版不做，但分区表现在就预留（之后改分区只能 USB 重刷） |

## 3. 编码

**Opus，16 kHz 单声道，12 kbps VBR，20 ms 帧，Ogg Opus 封装。** 12 kbps × 3600 s ÷ 8 ≈ 5.4 MB/小时，2 小时约 10.8 MB。

- C3 无 FPU，libopus 必须定点编译（`FIXED_POINT`），complexity 从 0–3 试。
- 候选：Espressif `esp_audio_codec`（是否支持 C3 未核实）或移植 libopus。
- 退路：C3 跑不动则改 IMA-ADPCM（16 kHz 时 64 kbps），缓存时长约降到 1/5。
- 决定（2026-11-20）：保留 Opus 方案，复杂度取 2；只有开 Wi-Fi 复测不过才切 ADPCM。

## 4. 内存预算（无 PSRAM，需实测）

| 项 | 估算 RAM | 说明 |
|---|---|---|
| Wi-Fi | 约 50–60 KB | ESP-IDF 典型值 |
| mbedTLS 单连接 | 约 40 KB | 可缩小 `MBEDTLS_SSL_IN/OUT_CONTENT_LEN` |
| Opus 编码器 | 未知 | 里程碑 1 实测 |
| LVGL | 小型单 DMA 缓冲（BSP 现状） | 录音熄屏时停掉动画 |
| 配网 SoftAP + HTTP server | 约 15–19 KB | 只在配网时启用，配完释放；蓝牙整块不编，反而比 BluFi 方案省约 35 KB |

## 5. 分区表（8 MB）

| 分区 | 大小 | 说明 |
|---|---|---|
| nvs | 24 KB | Wi-Fi、token、缓存读写指针 |
| otadata | 8 KB | OTA 预留 |
| phy_init | 4 KB | |
| ota_0 | 2 MB | 首版只用这一个 |
| ota_1 | 2 MB | OTA 预留 |
| log | 256 KB | 循环日志 |
| cache | 约 3.6 MB | 音频缓存 |

缓存容量：3.6 MB × 8 ÷ 12 kbps ≈ 2400 s ≈ **40 分钟**。前提是应用镜像小于 2 MB，需在里程碑 0 编译后确认；超过则调整 ota 分区大小。

## 6. 缓存与断网

所有音频先写 flash 缓存，上传任务只从缓存读。断网只是上传暂停，NAS 或网络不稳定都由这条路径兜底；掉电时已落盘的数据也能保住。

- 先进先出、不覆盖：写指针和已确认上传指针存 NVS。
- 按 4 KB 扇区攒满再写，减少擦写。
- 断网：报警 + 错误码 + 剩余可缓存时间（剩余空间 ÷ 码率）。
- 写满：停止录音，已录部分保留，恢复网络后上传，记 E500。
- 开机：缓存里有未上传数据时，联网后自动续传。

## 7. 上传与网络

### 7.1 链路：设备 → FN Connect → NAS 上的接收服务

不另外架中转。FN Connect 已开启，宽带无固定公网 IP，因此设备流量会走飞牛中继。设备直接访问 FN Connect 分配的地址（`<FN ID>.5ddd.com:<端口>`）。FN ID 不写死在固件里，做成设备配置项，在用户界面上输入，存 NVS，不入仓库。FN Connect 在有公网 IP 时直连，否则走飞牛自己的中继（限速）。12 kbps 只有约 1.5 KB/s，限速影响不大。

**待验证**（阻塞项，见里程碑 3）：
1. 无公网 IP、走飞牛中继时，非浏览器设备能否用 HTTPS PUT 访问 Docker 服务的端口。目前查到的资料只说明浏览器和飞牛 App 可以访问。
2. Docker 服务端口是否必须是 HTTPS。社区经验是要用 HTTPS 端口；必要时在 NAS 上加 nginx 反代。
3. 中继是否要求飞牛账号登录态（cookie）。如果要求，设备无法使用，只能改为 DDNS + 证书或自有 VPS。

### 7.2 协议

线格式已按实现定稿（`server/server.py`）：

- **session_id 固定 28 字符**：`<dev8>-<ts10>-<rand8>`，如 `98c377f4-1763800000-9f3e2a11`。`dev8` 是 MAC 低 32 位十六进制，`ts10` 是录音开始的 Unix 秒，`rand8` 是缓存里的 32 位会话号。服务端正则闭合校验，路径只由它和它推导的日期拼成，不做字符串清洗。`ts` 超出 `[now-90d, now+1d]` 拒绝。
- **端点**：`PUT /v1/upload/<session_id>` 分段上传；`GET` 同路径查状态；`GET /healthz` 健康检查。
- **请求头**（也接受 query 参数兜底，防止中继改写）：`Authorization: Bearer <token>`、`Content-Length`（必需，不支持 chunked）、`X-Offset`、`X-Seq`（仅日志）、`X-Final`、`X-Total-Len`（final 时必需）。
- **每个响应都带 `received`**，设备只以它为准推进本地读指针；不跟"已发送"走。这样 offset 落后和超前用同一条赋值语句收敛。
- 省电：录音时每 60 s 开一次连接批量发完缓存数据，其余时间 Wi-Fi 进入省电模式；不做持续长连接。
- **状态机**：首次 `offset=0` → 200；`offset==received` → 200；完全重复且字节一致 → 200 `duplicate`；部分重叠一致 → 200 `overlap` 只追加尾部；重叠内容不一致 → 409 `conflict`；`offset>received` → 409 `gap`；零长 `offset=0` 探针 → 200 `probe` 不建文件。409 是重同步指令，不是错误码 E300。
- **final**：`received==total` 才改名完成；`received<total` → 400 `incomplete`；`total<received` → 400 `bad_total`。已 finalized 的会话收到任何 PUT 都返回 200 `finalized`（幂等）。
- **落盘顺序不可改**：写数据 → fsync → 更新 meta 的 `committed` → 才回 200。保证"回给设备的 received 一定已落盘"，掉电最多丢一个 32 KB 段，设备重传即可。重启后以 meta 的 `committed` 为权威，文件更长则截断，更短则降回实际大小。
- **鉴权**：一机一 token，存服务端 `tokens.json`，不入仓库。token 与 `dev8` 绑定：token 无效 → 401，token 有效但不属于这个设备前缀 → 403。比较用 `hmac.compare_digest`。
- **TLS**：FN Connect 一般自己终止 TLS，服务端默认明文；本机或直连测试可自带证书。设备侧 CA 校验保护的是设备到 FN Connect 那一段。

### 7.3 NAS 端接收服务

- Docker 容器，单个 Python 标准库 HTTP 服务（`server/server.py`，无第三方依赖）。源码和 compose 在仓库 `server/`。
- 文件落到 `<日期>/<session_id>.ogg`，未完成是兄弟文件 `.ogg.part`，元数据是同目录的 `<session_id>.meta.json`。日期由 session_id 里的 ts 推导，跨零点的会话落在同一目录。
- 数据目录必须落在 NAS **本地文件系统**（ext4/btrfs）的 bind mount，不要放 SMB/NFS 共享：`fsync` 和 `rename` 的原子性在那里不可靠。
- 每日转写任务只处理 `*.ogg`，并额外要求兄弟 meta 的 `finalized=true`（防半成品或人工拷入的文件）。
- 测试：`tools/stage3/e2e_test.py`（44 用例）、`curl_smoke.sh`（8 用例）、`fake_device.py`（无硬件跑完整链路）。

## 8. 功耗估算（需实测）

520 mAh；录音时 Wi-Fi 间歇发送 + 编码，平均电流按 50–80 mA 估（推测值，没有实测依据），2 小时约用 100–160 mAh，满电够用。电量低于阈值时拒绝开始录音（E600）。里程碑 4 用 CW2017 读数实测。

## 9. 错误码（草案）

| 码 | 模块 | 含义 |
|---|---|---|
| E100 | 配网 | 未配置 Wi-Fi |
| E101 | 配网 | Wi-Fi 连接失败 |
| E200 | 网络 | DNS 解析失败 |
| E201 | 网络 | TLS 握手 / 证书校验失败 |
| E202 | 网络 | 连接或上传超时 |
| E203 | 网络 | SNTP 时间未同步 |
| E300 | 上传 | 服务端返回非 2xx（附状态码） |
| E301 | 上传 | 401/403，token 无效 |
| E400 | 录音 / 配网 | ES8311、显示屏或配网（热点 / 内置网页服务）启动失败 |
| E401 | 录音 | 编码器失败 |
| E402 | 缓存 | 空间不足（扫描列表等临时分配失败） |
| E500 | 缓存 | 缓存已满，录音被截断 |
| E501 | 缓存 | flash 读写失败 |
| E600 | 电源 | 电量低 |

## 10. 待输入 / 待验证

| # | 问题 | 谁 |
|---|---|---|
| 1 | FN ID：做成用户界面可配置项，存 NVS，不入仓库。开发阶段先用编译期占位值，界面配置在阶段 6 实现 | 胡嘉 |
| 2 | 宽带是否有 IPv6（可能有，但目前无法确认，也没有配置）。暂时绕过，按走中继处理，不影响阶段 2–3 的设计 | 胡嘉 |
| 3 | 7.1 的三项验证：在 NAS 上起一个测试容器，用手机热点（模拟外网）`curl -X PUT` 测 | 一起做 |
| 4 | 第 7.1 第 3 项不通过时的退路：DDNS + 证书（需要公网 IP），或自有 VPS | 验证后再定 |
| 5 | **固件缺口（阶段 4 前置）**：阶段 2 缓存只存 32 位会话号，没有录音开始时间，而 session_id 需要 `ts10`。需在 `audio_cache_begin_session` 时把 `{session32, start_ts, acked}` 写 NVS。`start_ts` 取 SNTP 时间，未同步则报 E203 先同步再开录 | 胡嘉 |
| 6 | FN Connect 中继是否原样透传 `Authorization` 和 `X-*` 头。服务端头和 query 两种形式都接受，接入真机前先用真实请求确认 | 验证 7.1 时一并做 |

## 11. 里程碑

| # | 内容 | 通过标准 |
|---|---|---|
| 0 | 按官方 `passport-setup` 装 ESP-IDF 5.5.3，跑通主分支 demo | 屏幕亮、三键有响应；记录应用镜像大小 |
| 1 | Opus 可行性：ES8311 16 kHz → 定点 Opus → Ogg 写 flash | ffplay 可播；实测 CPU、RAM 峰值，并在开 Wi-Fi 时复测 |
| 2 | 新分区表 + flash 缓存 + 掉电恢复 | 录音中拔电，重启后数据完整 |
| 3 | NAS 接收容器 + FN Connect 外网验证 | 外网 curl 分段 PUT，NAS 收到完整文件 |
| 4 | 设备边录边传 | 2 小时录音完整到 NAS；记录电量消耗 |
| 5 | 断网续传 | 中途断网 10 分钟，恢复后文件完整 |
| 6 | 主页 UI、熄屏、10 s 指示 | |
| 7 | 网页配网与错误码显示 | |
| 8 | OTA、转写（之后再做） | |

### 进度记录

| 阶段 | 状态 | 结果 |
|---|---|---|
| 0 | 完成（2026-11-20） | 官方主分支 demo 编译烧录到 COM3，屏幕亮、右侧三键有响应。应用镜像 0x174600 字节（约 1.45 MB），合并镜像 0x184600 字节，factory 分区剩余约 82%。 |
| 1 | 完成（2026-11-20） | 测试工程 `esp-work\stage1-opus`（复制自官方仓库，未改动官方仓库）。Opus 源码为 xiph/opus v1.5.2（源码包 SHA-256 `65c1d2f7…7ce1`，未与官方页面核对），定点编译。录音 30 s，16 kHz 单声道，Ogg Opus 12 kbps，OGG 44264 字节，写入 voice 分区（0x200000）。ffmpeg 解码无报错，时长 30.00 s，音量统计 mean -40.8 dB、max -0.1 dB。复杂度 5 时编码平均 19.6 ms、最大 22.4 ms，不可用；复杂度 2 时平均 11.3 ms、最大 12.9 ms（约 57% CPU）。最小剩余堆约 240 KB，最大连续块约 112 KB。录音任务每帧 `vTaskDelay(1)` 让出 CPU，避免看门狗触发。**Wi-Fi 复测（2026-11-20，已连接 2.4 GHz AP，省电 MIN_MODEM，录音中）**：连接后最小剩余堆约 210 KB；录音 30 s 期间最小剩余堆约 136 KB，最大连续块仍为 112 KB；复杂度 2 时编码平均 11.8 ms、最大 15.3 ms（约 59% CPU），仍可用。**未完成**：TLS 握手内存尚未实测（需要 NAS 端点，阶段 3 再测）；听感评价待人工确认。注意 ESP32-C3 不支持 5 GHz，`_5G` 的 SSID 连不上。 |
| 2 | 完成（2026-11-22，待听感确认） | 缓存分区 0x460000 起，0x3A0000 字节，928 个 4 KB 扇区。录音按扇区落盘，每扇区带头部（magic、序号、会话号、长度）和 CRC32；开机扫描恢复写指针，只校验最后一扇区。**真实断电测试（2026-11-22，macOS）**：擦除缓存后录制约 120 s 拔掉 USB。断电前最后状态 `cache mounted sectors=3` 起新会话 `5891708b`；重新上电后 `cache mounted sectors=59 payload=240484 last_session=5891708b`，写指针正确恢复，会话号一致。读回缓存 72 个有效扇区，拆出 3 个会话（`4f4d2385` 12 KB、`5891708b` 228 KB、`904aae77` 53 KB），全部 `crc_bad=0`、`seq_gaps=0`、`gran_monotonic=True`，`truncated_tail=True` 符合断电预期。`5891708b` 断电前已落盘 59 个满扇区（240484 字节），读回 228256 字节，差值来自断电瞬间 RAM 缓冲里未满扇区的内容，符合"只保证已落盘扇区"的设计。另验证 E500：缓存写满时 `E500 cache full at frame 4355` 停止录音，已录部分保留（928/928 扇区）。**未完成**：听感评价待人工确认；屏幕不显示 E500（推迟到阶段 6 UI）。测试音频在 `~/Projects/passport-flash-backup/`，不入库。串口有个坑：pyserial 默认打开端口会拉 DTR/RTS 把 C3 顶进下载模式，`serial_watch.py` 已改为显式 `dtr=False rts=False`。 |
| 3 | 接收服务完成，外网验证待做（2026-11-22） | 服务端 `server/server.py`（只用 Python 标准库，约 600 行）已实现并按设计文档 7.2 扩展了协议细节。`tools/stage3/e2e_test.py` 44 条用例全过，`curl_smoke.sh` 8/8 过。`tools/stage3/fake_device.py` 用缓存 dump 或合成 Ogg 模拟设备跑完整链路，断网续传、冲突重同步、死循环保护均已验证。**未完成**：部署到 NAS；经 FN Connect 中继的 HTTPS 外网验证（设计文档 7.1 三项阻塞项仍待实测）。 |
| 4–8 | 未开始 | |

阻塞项：企业文件标记工具会给新写入的 PNG 注入 tEXt 标签，导致 lvgl 组件的 CHECKSUM 校验失败。当前用 `esp-work\scripts\install_lvgl_verified.py` 绕过，待 IT 决定是否排除 esp-work 目录。
