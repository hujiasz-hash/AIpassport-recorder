# AI Passport 蓝牙联调诊断状态（2026-11-23）

## 结论

蓝牙连接、录音开始/停止、数据接收、结束帧都已跑通。目前的问题集中在三处：

1. 第一段录音只有 2.3 秒且几乎无声，所以转写为空。
2. 录音结束后断开，设备停止广播，App 显示「未连接」。这是广播窗口到期的预期行为。
3. 断开期间录的 10 到 20 秒录音，还没有在 Mac 端出现，原因未确认。

## 固件（`firmware/passport-app`）

| 项目 | 状态 |
| --- | --- |
| 当前运行版本 | ota_0 = `passport-app-ble-appearance-20261123.bin`，SHA256 `178049b0…` |
| 上一版本 | `passport-app-ble-upbtn-20261123.bin`，SHA256 `24ca9135…` |
| 原始备份 | `artifacts/passport-ble-backup-20261123/ota0-backup-20261123.bin`，SHA256 `af0a5c29…` |
| 分区表 | 与参考文件一致，未改动 |
| 广播 | 开机后常开广播，不再有 120 秒停止（已修复，见 `app_ble.c` `ensure_advertising`）；长按上键只是立即重试一次广播 |
| 扫描响应 | 名字 `AI Passport`，并带 Appearance `0x0200`（实验，无效果） |
| 配对码与连接状态 | 屏幕已显示，固件无需改动 |

**未解决**：macOS「系统设置 → 蓝牙」仍看不到设备。原因未知，我没有找到 macOS 的过滤规则。改用会议记录 App 的设置页连接，不依赖系统设置页。

## Mac 端（`301-MeetingRecorder`）

| 文件 | 改动 |
| --- | --- |
| `AI-passport/ble_bridge.swift` | 持续扫描并报告 `FOUND`；仅 `CONNECT <id>` 时连接；新增 `DISCONNECT` |
| `AI-passport/ble_receiver.py` | `start_bridge` 改为返回 `PassportLink`，记录设备列表，8 秒无广播即标灰 |
| `meeting_ui.py` | 外部集成页新增「AI Passport 录音设备」卡片，每秒刷新 |
| `AI-passport/launch.py` | 把 `PassportLink` 挂到 UI 类上 |
| `AI-passport/test_ble_receiver.py` | 新增 3 个用例；`AI-passport` 全部 7 个测试通过 |

**需要做**：App 内的 `passport-ble` 助手仍是旧版。需要运行 `bash macos_overlay/build_app_bundle.sh` 重新编译，否则设置页列表一直为空。

改动前的备份在 `/tmp/*.bak-20261123`，这是临时目录，重启后可能丢失。

## 本次会话的数据

| 会话 | 时间 | 内容 | 结果 |
| --- | --- | --- | --- |
| `e1a04548…` | 16:00 | 2306 ms，3576 字节，`reason=normal`；平均音量 -39.3 dB，最大 -3.4 dB | 转写为空，已发布空记录 `2026-11-23-16-00-35.txt` |
| 用户所说的 10 到 20 秒录音 | 约 16:05 之后 | 未出现在接收目录 | 推测留在设备 flash 中，等待下次连接时上传（未证实） |

## 已知问题

1. **空转写仍会发布历史记录。** `ingest.py` 的 `step()` 没有检查转写结果是否为空。
2. **16:01 前后反复断开重连。** 在广播窗口内，trusted 设备每 3 到 5 秒被连接又断开，原因未查清。
3. **设备串口没有输出。** 烧录后复位，串口 0 字节，无法从设备端确认启动和存储状态。
4. **系统蓝牙设置页看不到设备。** 原因未知。
5. **测试产生的文件。** `test_gui_smoke.py` 在 `301-MeetingRecorder/test_tts/` 下生成了两个 txt 文件。

## 下一步

1. 重新编译 App 内的 `passport-ble`，验证设置页列表和连接。
2. 长按上键，确认 10 到 20 秒录音是否随连接上传。若上传成功，问题 3 的预期原因成立。
3. 修复 `ingest.py`：转写为空时不发布空记录，并提示用户重录。
4. 排查 16:01 的断开重连循环。
5. 如需串口日志，检查 USB-JTAG 通道；烧录后复位不稳定，可能需要拔插 USB。
6. 若 appearance 实验无效，撤回 `app_ble.c` 中的 Appearance 字段。

## 仓库卫生

- `firmware/passport-app/` 下构建生成的 `sdkconfig`、`managed_components/`、`dependencies.lock` 已被 `.gitignore` 忽略，未删除。
- `301-MeetingRecorder` 的 `test_tts/` 下新增的两个文件未删除，需要你确认。
