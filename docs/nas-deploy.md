---
title: 录音接收服务部署说明
version: v0.1
author: 胡嘉
date: 2026-11-22
---

# 录音接收服务部署说明

## 版本
| 版本 | 日期 | 作者 | 改动点 |
| --- | --- | --- | --- |
| v0.1 | 2026-11-22 | 胡嘉 | 首版，接收服务在 NAS 上的部署与验证步骤 |

## 链接
- 库内：[docs/design.md](design.md) — 第 7 节协议与链路、第 9 节错误码
- 库内：[docs/firmware-setup.md](firmware-setup.md) — 固件构建、缓存分区操作
- 库内：[server/server.py](../server/server.py) — 接收服务实现
- 库内：[tools/stage3/e2e_test.py](../tools/stage3/e2e_test.py) — 端到端测试
- 外部：[fnOS 远程访问方式](https://help.fnnas.com/articles/v1/access/how-access) — FN Connect 直连/中继说明

---

## 1. 本机验证（不依赖 NAS）

先把服务和协议在本机跑通，再去 NAS 上碰外网。

```sh
# 1. 端到端测试：自己起服务、44 条用例、结束清理
python3 tools/stage3/e2e_test.py

# 2. curl 冒烟：真 HTTP 客户端，8 条用例
python3 server/server.py --port 18081 --data-dir /tmp/data --tokens /tmp/tokens.json &
sh tools/stage3/curl_smoke.sh http://127.0.0.1:18081 <token> <dev8>

# 3. 模拟设备跑完整链路（不用硬件）
python3 tools/stage3/fake_device.py \
    --url http://127.0.0.1:18081 --token <token> --dev <dev8> --synthetic
```

三条都过，再往下走。

## 2. NAS 上部署

### 2.1 准备

1. NAS 上建两个目录：代码目录和**本地文件系统**上的数据目录。
   数据目录不能放在 SMB/NFS 共享上 —— `fsync` 和 `rename` 的原子性在那里不可靠。
2. 生成一台设备的 token（至少 32 字符随机），并确认设备 `dev8`。

```sh
# 举例
python3 -c "import secrets; print(secrets.token_urlsafe(32))"
```

**`dev8` 就是设备屏幕/串口日志里那个设备号**，本机是 `77f452f8`（开机日志
`config: dev=77f452f8`）。它等于 MAC `98:c3:77:f4:52:f8` 的**后** 4 个字节，
即 `f4:52:f8` 加上 MAC 的第 3 字节 `77` —— 直接抄日志里的 `dev` 最稳，别自己截 MAC：

| 写法 | 结果 | 对不对 |
|---|---|---|
| `98:c3:77:f4:52:f8` 去冒号取**前** 8 位 | `98c377f4` | ✗ 错 |
| `98:c3:77:f4:52:f8` 去冒号取**后** 8 位 | `77f452f8` | ✓ 对 |
| 抄开机日志 `dev=` | `77f452f8` | ✓ 对 |

下一步也要用同一个值：配网页 `/info` 里的 `dev` 字段、`tokens.json` 的键。

3. token 的含义：**它是设备的身份，不是 NAS 的密码。** 一机一 token，服务端
   `tokens.json` 里 `{"devices": {"<dev8>": "<token>"}}` 建立"这台设备"和"这个串"
   的绑定。设备每次 PUT 都带 `Authorization: Bearer <token>`，服务端据此判断
   ①串对不对 ②这个串绑的设备是不是 `session_id` 前缀那台。所以：
   - token 填错或没填 → 401，设备报 E301
   - token 是别的设备的 → 403，防止冒用别人的会话

4. 同一份 token 要填两处：NAS 的 `server/tokens.json`，和**设备配网页的 Token 栏**。
   两边必须完全一致。配网页只回显"有没有 token"，不回显内容。

5. 在 NAS 的代码目录里放：

```
server/
  server.py
  Dockerfile
  docker-compose.yml
  tokens.json        # 从 tokens.example.json 复制并填真实值，不要入库
  data/              # 数据目录，建议 symlink 或 bind mount 到 NAS 本地盘
```

### 2.2 启动

```sh
cd <代码目录>/server
docker compose up -d
docker compose ps
docker compose logs -f
```

日志开头应有 `loaded 1 device token(s)` 和 `listening on 0.0.0.0:8000`。

### 2.3 健康检查

```sh
curl -sS http://<NAS内网IP>:8000/healthz
# 期望 {"ok":true,"status":"healthy"}
```

## 3. FN Connect 外网验证

这是设计文档 7.1 的三项阻塞项，必须实测。

### 3.1 确认 FN ID 和端口

在 fnOS 的远程访问设置里查 FN ID，地址形如 `<FN ID>.5ddd.com`。
确认 Docker 的 8000 端口已通过 FN Connect 暴露（用 HTTPS 端口，社区经验是必须）。

### 3.2 用手机热点模拟外网

断开 Mac 的有线/公司 Wi-Fi，连手机热点。然后：

```sh
# 1. 先确认能连上，拿到证书信息（记录签发者，确认不是自签）
curl -sS -o /dev/null -w '%{http_code}\n' https://<FN ID>.5ddd.com:<端口>/healthz

# 2. 三项验证
#    (1) 非浏览器设备能否 HTTPS 访问 Docker 服务端口
#    (2) 端口是否必须 HTTPS
#    (3) 中继是否要求飞牛账号登录态（cookie）

# 带 token 的分段 PUT 全流程
sh tools/stage3/curl_smoke.sh https://<FN ID>.5ddd.com:<端口> <token> <dev8>
```

### 3.3 三项结论怎么判断

| 现象 | 结论 |
| --- | --- |
| `curl` 拿到 200，无需任何 cookie | 中继不要求登录态，设备可直接用。**最理想** |
| `curl` 返回 302 到登录页 / HTML | 中继要求浏览器登录态，设备不可用。走退路（DDNS + 证书或自有 VPS） |
| HTTPS 通、HTTP 不通 | 端口必须是 HTTPS，设备端必须用 TLS（ESP32-C3 已有 mbedTLS 全量证书包） |
| 自定义头被剥离 | 服务端已支持 query 参数兜底；但更稳的是换端口或换链路 |

### 3.4 验证头是否被透传

中继可能改写或剥离 `Authorization`、`X-*` 头。用服务的日志确认：

```sh
# 服务端会打印每个请求；在 NAS 上看有没有收到 X-Offset
docker compose logs -f | grep -E 'X-Offset|Authorization'
```

如果头被剥掉，`fake_device.py` 已经带 query 兜底，会自动走通；但这是隐患，记录结论。

## 4. 验收

| 项 | 通过标准 |
| --- | --- |
| 本机 | `e2e_test.py` 44/44、`curl_smoke.sh` 8/8、`fake_device.py` 两轮（断网续传、冲突重同步）全 DONE |
| NAS 内网 | `healthz` 200；`fake_device.py --synthetic` 上传后 `data/<日期>/<sid>.ogg` 存在且字节一致 |
| FN Connect 外网 | 手机热点下 `curl_smoke.sh` 8/8；确认三项阻塞项的结论 |
| 成品校验 | `python3 tools/stage2/check_ogg.py <ogg>` 输出 `crc_bad=0`、`eos=1` |

## 5. 排错

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| 401 unauthorized | tokens.json 没加载或 token 错 | 看启动日志 `loaded N device token(s)`；确认 `TOKENS_FILE` 挂载路径 |
| 403 forbidden | token 有效但不属于 session 的设备前缀 | 确认 `--dev` 和 tokens.json 里的 dev8 一致 |
| 400 bad_session | session_id 格式错或 ts 超窗 | 格式必须是 `<dev8>-<ts10>-<rand8>`，共 28 字符 |
| 411 length_required | 请求没带 Content-Length | 设备端必须带；不支持 chunked |
| 409 gap | 设备 offset 超前 | 正常重同步，设备会从 received 重传，不是错误 |
| 409 conflict | 重叠内容不一致 | 缓存内容和服务端已收数据对不上；连续 3 次应放弃本轮，人工介入 |
| 文件一直 `.part` | 没收到 final，或 total 对不上 | 看日志；设备端检查 final 的 X-Total-Len 是否等于当前长度 |

## 6. 数据维护

- 每日转写任务只处理 `*.ogg`，并要求兄弟 `<sid>.meta.json` 的 `finalized=true`。
- `quarantine/` 下是孤立文件（`.part` 存在但 meta 丢失），人工确认后删除。
- 备份 `tokens.json` 本身：丢了要重新给设备下发 token。