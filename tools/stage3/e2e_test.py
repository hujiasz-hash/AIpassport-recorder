#!/usr/bin/env python3
"""NAS 接收服务端到端测试（只用标准库）。

用法：
    python3 tools/stage3/e2e_test.py

脚本会自己起一个 server 子进程（随机端口、临时数据目录、临时 token），
跑完全部用例后清理。不依赖网络、不依赖硬件。

覆盖：正常续传、幂等重复、内容冲突、偏移超前、部分重叠、零长探针、
final 的三种校验结果、重复 final、鉴权三种失败、非法 session_id、
缺 Content-Length、超大单段、GET/HEAD/404/405、并发同段、崩溃恢复。
"""
import http.client
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
SERVER = os.path.join(ROOT, "server", "server.py")

DEV = "0a1b2c3d"
TOKEN = "testtoken-" + "x" * 20
FAILS = []
PASSES = []


def check(name, cond, detail=""):
    if cond:
        PASSES.append(name)
        print(f"  PASS  {name}")
    else:
        FAILS.append(name)
        print(f"  FAIL  {name}  {detail}")


class Client:
    def __init__(self, port):
        self.port = port

    def req(self, method, path, body=None, headers=None, token=TOKEN):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        h = {}
        if token:
            h["Authorization"] = f"Bearer {token}"
        if body is not None:
            h["Content-Length"] = str(len(body))
            h["Content-Type"] = "application/octet-stream"
        if headers:
            h.update(headers)
        conn.request(method, path, body=body, headers=h)
        r = conn.getresponse()
        raw = r.read()
        conn.close()
        try:
            obj = json.loads(raw) if raw else {}
        except json.JSONDecodeError:
            obj = {"_raw": raw[:200]}
        return r.status, obj

    def put(self, sid, offset, data, seq=0, final=False, total=None, **kw):
        h = {"X-Offset": str(offset), "X-Seq": str(seq)}
        if final:
            h["X-Final"] = "1"
            h["X-Total-Len"] = str(total if total is not None else 0)
        return self.req("PUT", f"/v1/upload/{sid}", data, h, **kw)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def start_server(data_dir, tokens_path, port):
    proc = subprocess.Popen(
        [sys.executable, SERVER, "--port", str(port), "--data-dir", data_dir,
         "--tokens", tokens_path, "--host", "127.0.0.1"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    line = proc.stdout.readline()
    if "listening on" not in line:
        err = proc.stderr.read(4000)
        raise RuntimeError(f"server failed to start: {line!r}\n{err}")
    return proc


def make_sid(ts_offset=0, rand="9f3e2a11", dev=DEV):
    ts = int(time.time()) + ts_offset
    return f"{dev}-{ts:010d}-{rand}"


def find_part(data_dir, sid):
    for dirpath, _, files in os.walk(data_dir):
        for f in files:
            if f == f"{sid}.ogg.part":
                return os.path.join(dirpath, f)
    return None


def find_final(data_dir, sid):
    for dirpath, _, files in os.walk(data_dir):
        for f in files:
            if f == f"{sid}.ogg":
                return os.path.join(dirpath, f)
    return None


def make_ogg(payload_len=4000):
    """造一个结构正确的最小 Ogg 页流：BOS 页 + 若干数据页 + EOS 页。"""
    def ogg_crc(b):
        table = []
        for i in range(256):
            r = i << 24
            for _ in range(8):
                r = ((r << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if r & 0x80000000 else (r << 1) & 0xFFFFFFFF
            table.append(r)
        c = 0
        for x in b:
            c = ((c << 8) & 0xFFFFFFFF) ^ table[((c >> 24) ^ x) & 0xFF]
        return c

    out = bytearray()

    def page(seq, granule, body, htype):
        p = bytearray()
        p += b"OggS"
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
        crc = ogg_crc(p)
        p[22:26] = crc.to_bytes(4, "little")
        return bytes(p)

    head = b"OpusHead" + bytes([1, 1]) + (312).to_bytes(2, "little") + \
        (16000).to_bytes(4, "little") + b"\0\0\0"
    tags = b"OpusTags" + (6).to_bytes(4, "little") + b"e2e   " + (0).to_bytes(4, "little")
    out += page(0, 0, head, 0x02)
    out += page(1, 0, tags, 0x00)
    seq = 2
    gran = 0
    remaining = payload_len
    while remaining > 0:
        n = min(300, remaining)
        gran += 960 * max(1, n // 30)
        out += page(seq, gran, bytes([seq & 0xFF]) * n, 0x00)
        seq += 1
        remaining -= n
    out += page(seq, gran, b"", 0x04)
    return bytes(out)


def main():
    tmp = tempfile.mkdtemp(prefix="aipass-e2e-")
    data_dir = os.path.join(tmp, "data")
    tokens_path = os.path.join(tmp, "tokens.json")
    os.makedirs(data_dir, exist_ok=True)
    with open(tokens_path, "w") as f:
        json.dump({"devices": {DEV: TOKEN}}, f)

    port = free_port()
    proc = start_server(data_dir, tokens_path, port)
    c = Client(port)
    try:
        print("\n== 基本续传 ==")
        sid = make_sid(rand="00000001")
        ogg = make_ogg(4000)
        a, b = ogg[:1000], ogg[1000:2200]

        st, r = c.put(sid, 0, a, seq=0)
        check("首次 offset=0", st == 200 and r["status"] == "ok" and r["received"] == 1000, f"{st} {r}")
        part = find_part(data_dir, sid)
        check(".part 已建", part is not None and os.path.getsize(part) == 1000)

        st, r = c.put(sid, 1000, b, seq=1)
        check("续传", st == 200 and r["status"] == "ok" and r["received"] == 2200, f"{st} {r}")

        print("\n== 幂等 ==")
        st, r = c.put(sid, 1000, b, seq=1)
        check("完全重复 -> duplicate", st == 200 and r["status"] == "duplicate" and r["received"] == 2200, f"{st} {r}")
        check("文件未增长", os.path.getsize(part) == 2200)

        st, r = c.put(sid, 1000, b"X" * len(b), seq=1)
        check("同段不同内容 -> 409 conflict", st == 409 and r["status"] == "conflict" and r["received"] == 2200, f"{st} {r}")

        st, r = c.put(sid, 5000, b"zz", seq=2)
        check("offset 超前 -> 409 gap", st == 409 and r["status"] == "gap" and r["received"] == 2200, f"{st} {r}")

        print("\n== 部分重叠 ==")
        overlap = ogg[2100:2600]
        st, r = c.put(sid, 2100, overlap, seq=2)
        check("部分重叠 -> overlap", st == 200 and r["status"] == "overlap" and r["received"] == 2600, f"{st} {r}")
        check("重叠部分未重复写", os.path.getsize(part) == 2600)

        print("\n== 零长探针 ==")
        st, r = c.put(sid, 0, b"", seq=0)
        check("探针返回当前长度", st == 200 and r["status"] == "probe" and r["received"] == 2600, f"{st} {r}")
        check("探针不改文件", os.path.getsize(part) == 2600)

        print("\n== final ==")
        st, r = c.put(sid, 2600, ogg[2600:], seq=3)
        rest = r["received"]
        st, r = c.put(sid, rest, b"", seq=4, final=True, total=rest + 100)
        check("final total 过大 -> 400 incomplete", st == 400 and r["status"] == "incomplete", f"{st} {r}")
        st, r = c.put(sid, rest, b"", seq=4, final=True, total=rest - 10)
        check("final total 过小 -> 400 bad_total", st == 400 and r["status"] == "bad_total", f"{st} {r}")
        st, r = c.put(sid, rest, b"", seq=4, final=True, total=rest)
        check("final 正确 -> finalized", st == 200 and r["status"] == "finalized" and r["finalized"], f"{st} {r}")
        final_path = find_final(data_dir, sid)
        check(".ogg 已出现", final_path is not None and find_part(data_dir, sid) is None)
        if final_path:
            check(".ogg 内容正确", open(final_path, "rb").read() == ogg[:rest])

        st, r = c.put(sid, rest, b"", seq=4, final=True, total=rest)
        check("重复 final 幂等", st == 200 and r["status"] == "finalized", f"{st} {r}")
        st, r = c.put(sid, 0, a, seq=9)
        check("final 后再传 -> finalized", st == 200 and r["status"] == "finalized", f"{st} {r}")

        print("\n== 鉴权 ==")
        sid2 = make_sid(rand="00000002")
        st, r = c.req("PUT", f"/v1/upload/{sid2}", b"x", {"Content-Length": "1"}, token=None)
        check("缺 token -> 401", st == 401 and r["status"] == "unauthorized", f"{st} {r}")
        st, r = c.put(sid2, 0, b"x", token="wrongtoken")
        check("错 token -> 401", st == 401, f"{st} {r}")
        other_sid = make_sid(rand="00000003", dev="deadbeef")
        st, r = c.put(other_sid, 0, b"x")
        check("设备前缀不匹配 -> 403", st == 403 and r["status"] == "forbidden", f"{st} {r}")

        print("\n== 非法输入 ==")
        for bad in ["../etc/passwd", "a/b", "0a1b2c3d-123-9f3e2a11", "0a1b2c3d-9999999999-9f3e2a11",
                    "0A1B2C3D-1763800000-9f3e2a11", "x" * 40]:
            st, r = c.put(bad, 0, b"y")
            check(f"非法 session {bad[:20]!r}", st in (400, 404), f"{st} {r}")

        # 没有 Content-Length、只声明 chunked：服务端不解析 chunked，必须 411。
        conn = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
        conn.putrequest("PUT", f"/v1/upload/{sid2}", skip_host=False)
        conn.putheader("Authorization", f"Bearer {TOKEN}")
        conn.putheader("X-Offset", "0")
        conn.putheader("Transfer-Encoding", "chunked")
        conn.endheaders()
        conn.send(b"3\r\nabc\r\n0\r\n\r\n")
        r = conn.getresponse()
        body = json.loads(r.read() or b"{}")
        check("无 Content-Length -> 411", r.status == 411 and body.get("status") == "length_required",
              f"{r.status} {body}")
        conn.close()

        conn = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
        conn.putrequest("PUT", f"/v1/upload/{sid2}")
        conn.putheader("Authorization", f"Bearer {TOKEN}")
        conn.putheader("X-Offset", "0")
        conn.putheader("Content-Length", str(2 << 20))
        conn.endheaders()
        r = conn.getresponse()
        check("超大单段 -> 413", r.status == 413, f"{r.status}")
        conn.close()

        print("\n== 其他端点 ==")
        st, r = c.req("GET", f"/v1/upload/{sid}", token=TOKEN)
        check("GET 已 finalized 会话", st == 200 and r["status"] == "finalized", f"{st} {r}")
        st, r = c.req("GET", f"/v1/upload/{make_sid(rand='00000009')}", token=TOKEN)
        check("GET 不存在会话 -> probe", st == 200 and r["status"] == "probe", f"{st} {r}")
        st, r = c.req("GET", "/healthz", token=None)
        check("healthz", st == 200 and r["ok"], f"{st} {r}")
        st, r = c.req("GET", "/nope", token=None)
        check("404", st == 404, f"{st} {r}")
        st, r = c.req("POST", f"/v1/upload/{sid}", b"", token=TOKEN)
        check("405", st == 405, f"{st} {r}")
        st, r = c.req("HEAD", f"/v1/upload/{sid}", token=TOKEN)
        check("HEAD 有状态无 body", st in (200,), f"{st}")

        print("\n== 并发 ==")
        sid3 = make_sid(rand="00000005")
        chunk = b"Q" * 4096
        res = {}

        def do(i):
            res[i] = c.put(sid3, 0, chunk, seq=0)

        th = [threading.Thread(target=do, args=(i,)) for i in range(2)]
        [t.start() for t in th]
        [t.join() for t in th]
        statuses = sorted(v[1].get("status") for v in res.values())
        check("两并发同段：一个 ok 一个 duplicate", statuses == ["duplicate", "ok"], str(statuses))
        check("并发后长度只算一次", os.path.getsize(find_part(data_dir, sid3)) == 4096)

        print("\n== 崩溃恢复 ==")
        sid4 = make_sid(rand="00000006")
        c.put(sid4, 0, b"A" * 1024, seq=0)
        p4 = find_part(data_dir, sid4)
        # 模拟"数据写盘后、meta 更新前"崩溃：文件比 committed 长。
        with open(p4, "ab") as f:
            f.write(b"GARBAGE")
        check("注入不一致", os.path.getsize(p4) == 1024 + 7)
        st, r = c.req("GET", f"/v1/upload/{sid4}", token=TOKEN)
        check("恢复后截断到 committed", st == 200 and r["received"] == 1024, f"{st} {r}")
        check("文件已截断", os.path.getsize(p4) == 1024)
        st, r = c.put(sid4, 1024, b"B" * 512, seq=1)
        check("恢复后能继续传", st == 200 and r["received"] == 1536, f"{st} {r}")

        print("\n== 跨零点目录 ==")
        sid5 = make_sid(ts_offset=-86400, rand="00000007")
        c.put(sid5, 0, b"C" * 10, seq=0)
        dirs = [d for d in os.listdir(data_dir) if d[:2] == "20"]
        check("按 session ts 分目录（两天）", len(dirs) >= 2, str(dirs))

        print("\n== 完整 Ogg 上传 ==")
        sid6 = make_sid(rand="00000008")
        big = make_ogg(6000)
        off = 0
        while off < len(big):
            seg = big[off:off + 2048]
            c.put(sid6, off, seg, seq=off)
            off += len(seg)
        st, r = c.put(sid6, off, b"", seq=99, final=True, total=len(big))
        check("完整上传 finalize", st == 200 and r["finalized"], f"{st} {r}")
        fp = find_final(data_dir, sid6)
        check("成品字节一致", fp and open(fp, "rb").read() == big)

    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        if FAILS:
            print(f"\n===== {len(FAILS)} FAILED / {len(PASSES) + len(FAILS)} =====")
            for f in FAILS:
                print("  -", f)
            print(f"临时目录保留：{tmp}")
            return 1
        print(f"\n===== ALL {len(PASSES)} PASSED =====")
        shutil.rmtree(tmp, ignore_errors=True)
        return 0


if __name__ == "__main__":
    sys.exit(main())