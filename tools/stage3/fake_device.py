#!/usr/bin/env python3
"""用阶段 2 的缓存 dump 模拟设备，跑完整上传链路。

用途：在没有固件上传代码、或者还没接 NAS 时，先验证服务端和 FN Connect 中继。
它实现和固件完全相同的设备侧状态机：只以服务端返回的 received 为准推进。

用法：
    # 1. 从缓存分区 dump 拆出某会话（可选，不给则随机造 Ogg）
    python3 tools/stage3/fake_device.py \
        --cache cache.bin --session 5891708b \
        --url https://xxx.5ddd.com:8000 --token <token> --dev 77f452f8

    # 2. 不用缓存，随机造一段 Ogg 直接上传（先验证链路）
    python3 tools/stage3/fake_device.py \
        --url http://127.0.0.1:18080 --token testtoken --dev 0a1b2c3d --synthetic

模拟的异常：
    --drop-after N   传完第 N 段后断开（模拟断网），下次重跑会从服务端 received 续传
    --corrupt        故意发一段内容错误的数据，验证 409 conflict 后能重同步
"""
from __future__ import annotations

import argparse
import json
import random
import ssl
import struct
import sys
import time
import urllib.error
import urllib.request
import zlib

SECTOR = 4096
HDR = 20
MAGIC = 0x31435541
CHUNK = 32768


def read_session_from_cache(path: str, session_hex: str) -> bytes:
    """按阶段 2 扇区格式拆出指定会话的 payload。"""
    want = int(session_hex, 16)
    data = open(path, "rb").read()
    out = bytearray()
    for i in range(len(data) // SECTOR):
        sec = data[i * SECTOR:(i + 1) * SECTOR]
        magic, seq, sess, ln = struct.unpack_from("<IIIH", sec, 0)
        if magic != MAGIC or seq != i + 1 or ln > SECTOR - HDR:
            break
        stored = struct.unpack_from("<I", sec, 16)[0]
        if (zlib.crc32(sec[0:16] + sec[HDR:HDR + ln]) & 0xFFFFFFFF) != stored:
            break
        if sess == want:
            out += sec[HDR:HDR + ln]
    if not out:
        raise SystemExit(f"缓存里没有会话 {session_hex}（可能已被后面的会话覆盖或擦除）")
    return bytes(out)


def make_synthetic_ogg(payload_len: int = 20000) -> bytes:
    table = []
    for i in range(256):
        r = i << 24
        for _ in range(8):
            r = ((r << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if r & 0x80000000 else (r << 1) & 0xFFFFFFFF
        table.append(r)

    def crc(b):
        c = 0
        for x in b:
            c = ((c << 8) & 0xFFFFFFFF) ^ table[((c >> 24) ^ x) & 0xFF]
        return c

    out = bytearray()

    def page(seq, granule, body, htype):
        p = bytearray(b"OggS")
        p.append(0)
        p.append(htype)
        p += granule.to_bytes(8, "little", signed=True)
        p += (0x1234ABCD).to_bytes(4, "little")
        p += seq.to_bytes(4, "little")
        p += b"\0\0\0\0"
        lace = bytearray()
        left = len(body)
        while left >= 255:
            lace.append(255)
            left -= 255
        lace.append(left)
        p.append(len(lace))
        p += lace
        p += body
        p[22:26] = crc(p).to_bytes(4, "little")
        return bytes(p)

    head = b"OpusHead" + bytes([1, 1]) + (312).to_bytes(2, "little") + \
        (16000).to_bytes(4, "little") + b"\0\0\0"
    tags = b"OpusTags" + (6).to_bytes(4, "little") + b"fake  " + (0).to_bytes(4, "little")
    out += page(0, 0, head, 0x02)
    out += page(1, 0, tags, 0x00)
    seq, gran, left = 2, 0, payload_len
    while left > 0:
        n = min(300, left)
        gran += 960
        out += page(seq, gran, bytes([seq & 0xFF]) * n, 0x00)
        seq += 1
        left -= n
    out += page(seq, gran, b"", 0x04)
    return bytes(out)


class Device:
    """和固件同样的状态机：acked 只跟服务端 received 走。"""

    def __init__(self, url: str, token: str, dev: str, insecure: bool):
        self.url = url.rstrip("/")
        self.token = token
        self.dev = dev
        self.acked = 0
        self.seq = 0
        ctx = ssl._create_unverified_context() if insecure else None
        self.opener = urllib.request.build_opener(
            urllib.request.HTTPSHandler(context=ctx) if ctx else urllib.request.HTTPSHandler())

    def put(self, sid: str, offset: int, body: bytes, final: bool = False,
            total: int | None = None) -> tuple[int, dict]:
        url = f"{self.url}/v1/upload/{sid}?offset={offset}&seq={self.seq}"
        if final:
            url += f"&final=1&total={total}"
        req = urllib.request.Request(url, data=body, method="PUT")
        req.add_header("Authorization", f"Bearer {self.token}")
        req.add_header("Content-Type", "application/octet-stream")
        req.add_header("Content-Length", str(len(body)))
        req.add_header("X-Offset", str(offset))
        req.add_header("X-Seq", str(self.seq))
        if final:
            req.add_header("X-Final", "1")
            req.add_header("X-Total-Len", str(total))
        self.seq += 1
        try:
            with self.opener.open(req, timeout=30) as r:
                return r.status, json.loads(r.read() or b"{}")
        except urllib.error.HTTPError as e:
            body = e.read()
            try:
                return e.code, json.loads(body or b"{}")
            except json.JSONDecodeError:
                return e.code, {"_raw": body[:200].decode("utf-8", "replace")}

    def upload(self, sid: str, payload: bytes, drop_after: int = 0, corrupt: bool = False,
               corrupt_mode: str = "rewind"):
        """corrupt_mode:
             rewind  发到第 2 段时后退到 offset=0 并故意改坏内容 → 触发 409 conflict
             forward 正常前进，只破坏第 2 段内容（服务端收下，属于真实数据损坏，检测不到）
        """
        print(f"session={sid} payload={len(payload)} url={self.url}")

        # 开机先用零长探针取权威长度，对应固件"NVS 不可信就重同步"。
        st, r = self.put(sid, 0, b"")
        print(f"  probe -> {st} {r.get('status')} received={r.get('received')}")
        if st >= 400:
            raise SystemExit("探针失败，检查 url/token")
        self.acked = int(r.get("received", 0))
        self.seq = 0

        sent = 0
        corrupted = False          # 破坏只做一次；否则会无限重发同一段
        conflicts = 0              # 连续冲突计数，防止缓存真损坏时死循环
        while self.acked < len(payload):
            if drop_after and sent >= drop_after:
                print(f"  模拟断网：已发 {sent} 段，服务端 received={self.acked}")
                return False
            off = self.acked
            if corrupt and corrupt_mode == "rewind" and not corrupted and sent == 1:
                # 退回到已确认的位置重发，但内容被改坏：
                # 服务端应返回 409 conflict，设备随后从 received 重传正确数据。
                off = 0
                body = bytes([payload[0] ^ 0xFF]) + payload[1:CHUNK]
                corrupted = True
                print("  模拟缓存回退且内容损坏（offset=0 重发不同内容）")
            else:
                body = payload[off:off + CHUNK]
            if corrupt and corrupt_mode == "forward" and not corrupted and sent == 1:
                body = bytes([body[0] ^ 0xFF]) + body[1:] if body else b"X"
                corrupted = True
                print("  故意破坏第 2 段内容")
            st, r = self.put(sid, off, body)
            if st in (200,):
                conflicts = 0
                self.acked = int(r.get("received", self.acked))
                sent += 1
                print(f"  {off}+{len(body)} -> {r.get('status')} received={self.acked}")
            elif st in (409,):
                # 409 是重同步指令，不是错误：把本地指针拉回服务端的 received。
                # 但若连续多次同一个 offset 都被拒，说明本地缓存内容和服务端
                # 那份对不上且无法自愈，必须放弃本轮，交给人工或后续整段覆盖。
                conflicts += 1
                self.acked = int(r.get("received", self.acked))
                print(f"  {off}+{len(body)} -> {st} {r.get('status')} 重同步 received={self.acked}")
                if conflicts >= 3:
                    print("  连续 3 次冲突，本地缓存与服务端不一致，放弃本轮")
                    return False
            elif st in (401, 403):
                raise SystemExit(f"鉴权失败 {st} {r}")
            else:
                print(f"  {off}+{len(body)} -> {st} {r}，本轮停止，稍后从 received 重试")
                return False

        st, r = self.put(sid, self.acked, b"", final=True, total=len(payload))
        print(f"  final -> {st} {r.get('status')} finalized={r.get('finalized')}")
        return st == 200 and r.get("finalized")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", required=True, help="如 https://abc123.5ddd.com:8000")
    ap.add_argument("--token", required=True)
    ap.add_argument("--dev", required=True, help="session_id 里的设备前缀，8 位十六进制")
    ap.add_argument("--cache", help="阶段 2 缓存分区 dump")
    ap.add_argument("--session", help="从缓存里取哪个会话，8 位十六进制")
    ap.add_argument("--synthetic", action="store_true", help="不上缓存，随机造 Ogg")
    ap.add_argument("--size", type=int, default=200000, help="合成 Ogg 的 payload 字节数（要大于 32KB 才能测多段）")
    ap.add_argument("--drop-after", type=int, default=0)
    ap.add_argument("--corrupt", action="store_true", help="干扰重叠内容，验证 409 后能重同步")
    ap.add_argument("--corrupt-mode", choices=["rewind", "forward"], default="rewind")
    ap.add_argument("--insecure", action="store_true", help="跳过证书校验（自签证书测试用）")
    ap.add_argument("--sid", help="直接指定完整 session_id（30→28 字符格式）")
    a = ap.parse_args()

    if a.sid:
        sid = a.sid
    else:
        ts = int(time.time())
        rand = "%08x" % random.getrandbits(32)
        sid = f"{a.dev}-{ts:010d}-{rand}"
        print(f"生成 session_id={sid}")

    if a.synthetic or not a.cache:
        payload = make_synthetic_ogg(a.size)
        print("使用合成 Ogg")
    else:
        if not a.session:
            raise SystemExit("--cache 需要配 --session")
        payload = read_session_from_cache(a.cache, a.session)
        print(f"从缓存取出会话 {a.session}: {len(payload)} 字节")

    d = Device(a.url, a.token, a.dev, a.insecure)
    ok = d.upload(sid, payload, a.drop_after, a.corrupt, a.corrupt_mode)
    print("DONE" if ok else "INCOMPLETE")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())