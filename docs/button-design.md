---
title: AI Passport 按键交互与多模式开发规范
version: v0.2
author: 胡嘉
date: 2026-11-24
---

# AI Passport 按键交互与多模式开发规范

## 版本

| 版本 | 日期 | 作者 | 改动点 |
| --- | --- | --- | --- |
| v0.1 | 2026-11-24 | 胡嘉 | 基于现有固件基线，定义会议录音、看时间、蓝牙配对、PPT 翻页笔与语音输入（PTT）的统一按键状态机与开发约束。 |
| v0.2 | 2026-11-24 | 胡嘉 | 真机联调定稿：鉴于 ADC 120 ms 抬起锁吞双击，改为长按 `DOWN` 3.0 s 切换 PPT 模式、取消 OK 双击延迟；PPT 翻页采用系统级标准 BLE HID 键盘（`0x1812`）与私有录音服务共存。 |

## 链接

- 库内：[录音同步总体设计](design.md) · [当前固件交付](20261124_AI-Passport_固件交付.md) · [固件环境与构建](firmware-setup.md)
- 源码（本仓库）：[主循环与按键分发](../firmware/passport-app/main/main.c) · [屏幕与背光](../firmware/passport-app/main/app_ui.c) · [BLE 传输与配对](../firmware/passport-app/main/app_ble.c) · [录音采集](../firmware/passport-app/main/app_record.c)
- 源码（BSP 组件）：`components/bsp/include/bsp_pins.h` · `components/bsp/include/bsp_button.h` · `components/bsp/src/bsp_button.c`

---

## 1. 设计目标与硬件边界

**本规范在不增加二级菜单的前提下，用三颗物理按键承载五项能力：会议录音、点亮屏幕看时间、蓝牙配对、PPT 前后翻页（支持与会议录音并发）、按住语音输入（与会议录音隔离）。**

所有交互必须受限于以下已验证的硬件与 BSP 事实：

1. **禁止设计任何双键组合**：`UP`（上，0 mV）、`DOWN`（下，≈300 mV）、`OK`（中，≈595 mV）共用 `GPIO0 / ADC1_CH0` 分压电路。两键同按时电压偏离有效窗口，硬件无法识别。
2. **复用现有底层事件，不改 ADC 解码门限**：`bsp_button.c` 已提供 `PRESS`（按下瞬间，约 40 ms 确认延迟）、`CLICK`（单击，180 ms 判定窗口）、`DOUBLE`（双击）、`LONG`（长按起点，500 ms）和 `RELEASE`（松手瞬间，约 120 ms 保持防抖）。底层 `BSP_BTN_LONG_PRESS_MS = 500` 保持不变，用于支撑按住说话（PTT）的快速响应；1.5 秒停录、3 秒重配对等长时门限统一在应用层（`main.c`）用 `esp_timer_get_time()` 计时判定。
3. **按键回调只置标志位，绝不阻塞**：`on_key` 运行在 `esp_timer` 共享任务上下文中，严禁调用 Flash 读写、I2S 启停、BLE 断连或 LVGL 锁；只能更新原子时间戳或请求标志，由 `app_main` 主循环执行。
4. **解决现版固件的两个误触隐患**：
   - 现版 `main.c` 在 `PRESS` 时亮屏，但紧随其后的 `OK 单击` 会直接开启录音，导致熄屏想看时间时极易误开录；
   - 现版 `OK 长按 500 ms` 直接调用 `app_ble_allow_pairing()` 删除已有绑定并断开连接，口袋挤压或握持稍久即会导致 Mac 掉线且需重输配对码。

---

## 2. 状态模型与并发关系

**翻页笔不是互斥页面，而是可随时叠加在待机或会议录音之上的独立开关；麦克风硬件唯一，会议录音与语音输入严格互斥。**

系统由三个正交状态维度决定按键语义：

| 状态维度 | 取值 | 说明 |
| --- | --- | --- |
| **屏幕背光态** | `SCREEN_OFF` / `SCREEN_ON` | 30 秒无按键输入背光置 0（含录音页，改变现版录音常亮策略以省电防晃眼）。 |
| **音频工作态** | `AUDIO_IDLE` / `AUDIO_MEETING_REC` / `AUDIO_VOICE_PTT` | 单路 ES8311 I2S RX + 单实例定点 Opus 编码器，三态互斥。 |
| **翻页叠加态** | `PPT_OFF`（开机默认） / `PPT_ON` | 长按 `DOWN` 满 3.0 秒随时切换，状态变化不中断正在进行的会议录音。 |

并发矩阵如下：

| 组合场景 | 是否允许 | 行为与资源分配 |
| --- | --- | --- |
| 会议录音 + PPT 翻页 | **允许** | 录音写 `app_store`（`0x460000`）并后台 BLE 分块同步；翻页仅发数字节 BLE 键值，互不抢占。 |
| 待机 / PPT 翻页 + 语音输入（PTT） | **允许** | 按住 `OK` 期间临时占用 I2S 与 Opus，编码帧直接走 BLE 实时通道发往电脑上屏，**不写 `app_store`、不触发 MeetingRecorder 纪要**。 |
| 会议录音 + 语音输入（PTT） | **禁止** | 录音已独占麦克风与 Opus 编码器；录音期间长按 `OK` 仅用于满 1.5 秒停止录音。 |
| 蓝牙重配对 + 录音 / 翻页 | **禁止** | 重配对会清除绑定并踢掉当前连接，仅允许在 `AUDIO_IDLE && PPT_OFF` 下触发。 |

---

## 3. 全场景按键映射表

### 3.1 全局最高优先级拦截（先于常规状态分发）

1. **错误弹窗页（`APP_PAGE_ERROR`）**：单击 `OK` 清除错误并返回上一工作页（或 10 秒自动返回）；其余按键仅点亮背光。
2. **蓝牙配对码显示期（`app_ble_passkey() != UINT32_MAX`）**：背光保持常亮；单击任意键主动取消本次配对窗口并恢复常规自动重连广播。
3. **蓝牙未连接时的顺带唤醒**：任意按键触发亮屏时，若 `!app_ble_connected()`，主循环顺带调用一次 `app_ble_advertise_for(0)` 跳过广播节流立即发起快连，不再单独占用物理按键。
4. **旧版 Wi-Fi 兼容宏（`CONFIG_PASSPORT_WIFI_LEGACY`）**：仅在启用该编译宏时保留配网页 `APP_PAGE_PROV` 的长按进退逻辑；默认 BLE 产品构建不编译该分支。

### 3.2 待机状态（`AUDIO_IDLE`，未在会议录音）

| 按键 | 背光状态 | 单击 (`CLICK`) | 双击 (`DOUBLE`) | 长按 / 按住 (`LONG` 500 ms → 持续计时) |
| --- | --- | --- | --- | --- |
| **OK** | 熄屏 | **仅点亮屏幕看时间**（吞掉开录动作） | 同单击 | 点亮屏幕并 **进入语音输入（PTT）**；松手立即结束拾音 |
| **OK** | 亮屏 | **立即开始会议录音**（切入 `AUDIO_MEETING_REC`，零延迟） | 同单击 | **按住语音输入（PTT）**：满 500 ms 起录，松手 `RELEASE` 结束 |
| **DOWN** | 熄屏 / 亮屏 | • `PPT_ON`：**下一页**（`PageDown`，熄屏直发不亮大屏）<br>• `PPT_OFF`：仅点亮屏幕 | • `PPT_ON`：**连发 2 次下一页**<br>• `PPT_OFF`：仅点亮屏幕 | **按住满 3.0 秒：切换 `PPT_ON / PPT_OFF`**（防误触） |
| **UP** | 熄屏 / 亮屏 | • `PPT_ON`：**上一页**（`PageUp`，熄屏直发不亮大屏）<br>• `PPT_OFF`：仅点亮屏幕 | • `PPT_ON`：**连发 2 次上一页**<br>• `PPT_OFF`：仅点亮屏幕 | • `PPT_OFF`：**按住满 3.0 秒触发蓝牙重配对**<br>• `PPT_ON`：无动作（防止演示时误断连） |

### 3.3 会议录音中（`AUDIO_MEETING_REC`，可叠加翻页）

| 按键 | 背光状态 | 单击 (`CLICK`) | 双击 (`DOUBLE`) | 长按 / 按住 (`LONG` → 持续计时) |
| --- | --- | --- | --- | --- |
| **OK** | 熄屏 / 亮屏 | **点亮屏幕查看时间与录音时长**<br>（底部提示“请长按 1.5 秒停止”，单击绝不停录） | 同单击 | **按住满 1.5 秒：停止并保存会议录音**<br>（到 1.5 s 立即停录，无需等松手；未满 1.5 s 松手视为无效，仅保留亮屏并提示“请长按 1.5 秒停止”） |
| **DOWN** | 熄屏 / 亮屏 | • `PPT_ON`：**下一页**（熄屏直发，不亮屏）<br>• `PPT_OFF`：点亮屏幕 | • `PPT_ON`：**连发 2 次下一页**<br>• `PPT_OFF`：点亮屏幕 | **按住满 3.0 秒：切换 `PPT_ON / PPT_OFF`**（录音不中断） |
| **UP** | 熄屏 / 亮屏 | • `PPT_ON`：**上一页**（熄屏直发，不亮屏）<br>• `PPT_OFF`：点亮屏幕 | • `PPT_ON`：**连发 2 次上一页**<br>• `PPT_OFF`：点亮屏幕 | 无动作（锁死蓝牙重配对） |

> **连按翻页的技术约束（必读）**：`iot_button` 注册了 `BUTTON_DOUBLE_CLICK` 后，180 ms 窗口内连续按两下只会产生 1 次 `BSP_BTN_DOUBLE`，不会产生 2 次 `BSP_BTN_CLICK`。因此 `PPT_ON` 下的 `DOUBLE` 必须映射为“连续发送两次同向翻页键值”，否则用户在台上快速连按两下只能翻一页（或没反应）。

---

## 4. 按键时序与状态机判定细则

### 4.1 熄屏首按吞键规则

不能再像现版 `on_key` 那样在 `BSP_BTN_PRESS` 里无条件调用 `app_ui_note_input()` 把背光状态改掉，否则后续 `CLICK` 到达时已无法判断按下前是否处于熄屏。

- 在 `app_ui` 暴露只读查询 `bool app_ui_is_screen_on(void)`（判断当前背光占空比是否大于 0）。
- 当 `BSP_BTN_PRESS` 到达时，记录 `s_press_was_screen_off = !app_ui_is_screen_on()`：
  - 若当前为 `PPT_ON` 且按键是 `UP` 或 `DOWN`：**不点亮背光**，保持熄屏盲翻；
  - 其他所有情况：置标志位通知主循环点亮背光并重置 30 秒空闲计时。
- 当 `OK` 的 `BSP_BTN_CLICK` 到达时：若 `s_press_was_screen_off == true` 且处于 `AUDIO_IDLE`，本次 `CLICK` 仅完成亮屏，直接消费返回，不触发 `s_req_start_record`。

### 4.2 三档长按时长在应用层的区分

底层 `BSP_BTN_LONG_PRESS_MS` 固定为 `500 ms`：

1. **语音输入 PTT（0.5 s 起跳，松手即停）**：
   - 条件：`AUDIO_IDLE` 下收到 `OK` 的 `BSP_BTN_LONG`（500 ms）。
   - 动作：置 `s_ptt_wanted = true`，启动实时拾音推流；收到 `OK` 的 `BSP_BTN_RELEASE` 时置 `s_ptt_wanted = false`，发送流结束帧并释放麦克风。
   - 保护：单次按住上限设为 60 秒，超时自动截断收尾，防止书包重物长期压住按键耗尽电量。
2. **停止会议录音（1.5 s 阈值，防翻页误碰）**：
   - 条件：`AUDIO_MEETING_REC` 下收到 `OK` 的 `BSP_BTN_LONG`（此时已按住 500 ms）。
   - 动作：记录 `s_ok_hold_start_us = now - 500ms`，标记 `s_ok_holding = true`；若提前收到 `BSP_BTN_RELEASE` 则清除 `s_ok_holding` 并在录音页短暂提示“请长按停止”。主循环轮询（建议将 `main.c` 循环节拍从 `1000 ms` 缩短至 `50 ms`，UI 仍按 1 秒分频刷新）检测到 `s_ok_holding && (now - s_ok_hold_start_us >= 1500ms)` 时，立即调用 `app_record_stop()`，且忽略随后那次 `RELEASE`。
3. **蓝牙重配对（3.0 s 阈值，防口袋误删绑定）**：
   - 条件：`AUDIO_IDLE && !s_ppt_mode` 下收到 `UP` 的 `BSP_BTN_LONG`。
   - 动作：同上计时，持续按住满 `3000 ms` 未收到 `RELEASE` 时，主循环执行 `app_ble_allow_pairing()`。

---

## 5. UI 反馈规范（240×320 屏幕）

1. **顶栏状态与 `[PPT]` 标记**：
   - 主页与录音页统一保留实时状态显示；开启翻页模式（`PPT_ON`）时，在屏幕顶栏中间或蓝牙状态旁醒目显示 `PPT` 徽标（纯 ASCII 字符可直接用现有 `lv_font_montserrat_14` 渲染，零字库改动即可先行验证）。
   - 录音页需同时显示**当前北京时间 `HH:MM`** 与**录音已持续时长 `MM:SS`**，满足录音中途按一下 `OK` 亮屏同时看时间和录音进度的需求。
2. **语音输入（PTT）浮层 / 页面（`APP_PAGE_PTT`）**：
   - 按住 `OK` 触发 PTT 时切入简洁提示页：显示“正在聆听 · 松开结束”与计时秒数；若当前蓝牙未就绪（`!app_ble_ready()`），立即放弃拾音并弹提示“蓝牙未连接”，不空转麦克风。
3. **背光策略调整**：
   - 取消 `app_ui_idle_tick()` 中 `s_page == APP_PAGE_RECORDING` 永不熄屏的特例。所有常规页（主页、录音页）统一在无操作 30 秒后关闭背光；仅 `APP_PAGE_PROV` 与“正在显示 6 位蓝牙配对码”期间保持常亮。
4. **中文字库补字清单**：
   - `lv_font_ui_14.c` 为子集字体，新增文案前须重新生成字库，确保包含：`翻页演示模式语音输入聆听松开长按结束秒`。

---

## 6. 通信协议与电脑端职责边界

### 6.1 PPT 翻页通道（二选一，推荐先走私有帧、后演进 HID）

| 方案 | 固件侧实现 | Mac 端实现 | 优缺点 |
| --- | --- | --- | --- |
| **方案 A（推荐首版）：复用现有私有 GATT 控制帧** | 在 `tx_uuid` Notify 中新增 1 类短消息（如首字节 `'K'` + 键值 `NEXT=1 / PREV=2`），与录音分块复用同一加密连接。 | 现有 `AI-passport` 接收守护进程收到 `'K'` 帧后，通过 macOS Quartz `CGEventCreateKeyboardEvent` 注入 `kVK_DownArrow` / `kVK_UpArrow`（或 `PageDown` / `PageUp`）。 | 固件改动极小（十余行），不改 GATT 表结构，不会触发 macOS 蓝牙 GATT 缓存失效导致需要删设备重配。 |
| **方案 B（独立配件态）：标准 BLE HOGP (HID Keyboard)** | 新增 `0x1812` HID Service，上报标准键盘 `PageDown (0x4E)` / `PageUp (0x4B)`。 | 零依赖，macOS / Windows / iPad 系统原生识别为蓝牙翻页器。 | 改变 GATT 属性表后已绑定主机必须重新配对一次；适合需要脱离个人 Mac 给其他电脑演示时启用。 |

### 6.2 语音输入（PTT）与会议录音的严格分流

| 对比项 | 会议录音（Meeting Record） | 语音输入麦克风（Voice PTT） |
| --- | --- | --- |
| 触发方式 | 亮屏单击 `OK` 开 / 长按 `OK` 1.5 s 停 | 待机下按住 `OK` 说话 / 松手即停 |
| 音频编码 | 16 kHz 单声道，20 ms 帧，Ogg Opus 封装 | 复用同一 16 kHz 单声道 20 ms 裸 Opus 包（带序号，无需 Ogg 文件封装） |
| 设备 Flash 缓存 | **写入 `app_store`（`0x460000`）**，支持断连补传与掉电恢复 | **绝不写 Flash**，纯 RAM 环形缓冲直推 BLE Notify；断连即丢弃并报错 |
| BLE 消息通道 | 现有会话同步协议（`session` / `data` / `end` + 磁盘偏移 ACK） | 独立帧类型（`ptt_start` / `ptt_opus_frame` / `ptt_stop`），低延迟无落盘 ACK 阻塞 |
| 电脑端消费方 | 落盘 `receipt.json` + `audio.ogg` → `MeetingRecorder` 生成长转写 TXT 与会议纪要 MD | 接收端内存攒完单句音频（或流式送本地 ASR）→ 识别成单段文本 → 粘贴/注入当前系统焦点光标处，**不进 `~/DualRecorderMeetings/`、不进历史会议库** |

---

## 7. 分阶段实施建议与验收标准

1. **阶段 1：按键状态机、防误触与看时间优化（纯固件 UI/按键层）**
   - `main.c` 主循环节拍改为 `50 ms`；落地熄屏首按只亮屏、录音页 30 秒熄屏、录音页补齐北京时间、录音中长按 `OK` 1.5 秒停录、`UP` 长按 3 秒重配对、`OK` 双击切换 `PPT` 顶栏标记。
   - **验收**：熄屏下按任意键包括 `OK` 均只亮屏不误录；录音中单击 `OK` 只亮屏不停录，按住 1.5 秒稳定停录；口袋误碰 `OK` 0.5~1 秒不再丢失蓝牙配对。
2. **阶段 2：PPT 翻页笔打通（录音并发验证）**
   - 跑通 `UP` / `DOWN` 单击翻 1 页、双击翻 2 页、熄屏盲翻不亮屏。
   - **验收**：开启一场 10 分钟会议录音并开启 `PPT` 模式，期间连续前后翻页 50 次（含快速双击），Keynote / PowerPoint / PDF 翻页无吞键，录音结束后校验 Ogg 文件无丢帧、无破音。
3. **阶段 3：按住语音输入（PTT）通路打通**
   - 抽离 `bsp_audio` + `opus` 的内存直推模式供 PTT 与 `app_record` 分时复用；电脑端接入本地 ASR 与光标文本上屏。
   - **验收**：待机按住 `OK` 说一句话松手，2 秒内文字出现在当前光标输入框，且 `app_store_used_sectors()` 扇区占用数为 0 增长，`MeetingRecorder` 历史列表不新增记录；会议录音进行中按住 `OK` 不会切入 PTT。
