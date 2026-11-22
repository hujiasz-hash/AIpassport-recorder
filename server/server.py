#!/usr/bin/env python3
"""AI Passport 录音接收服务（只依赖 Python 标准库）。

设计依据：docs/design.md 第 7 节，阶段 3。
协议要点：
  - 每个录音一个 session_id，格式固定 `<dev8>-<ts10>-<rand8>`（30 字符）。
  - HTTPS/TLS 一般由 FN Connect 终止，本服务默认明文；本机或直连测试可自带证书。
  - 分段 PUT，按字节偏移追加；幂等；偏移超前返回 409 并带上已收长度。
  - 每次响应都带 received，设备只以它为准推进本地读指针。
  - 数据先 fsync 再回 received，保证"确认过的字节一定已落盘"。

运行：
  python3 server.py --port 8000 --data-dir /data --tokens tokens.json
  python3 server.py --port 0        # 随机端口，启动后打印实际端口（测试用）
"""
from __future__ import annotations

import argparse
import datetime
import hmac
import json
import os
import re
import ssl
import sys
import threading
from dataclasses import dataclass, field
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

# ---------------------------------------------------------------------------
# 常量
# ---------------------------------------------------------------------------

MAX_CHUNK = 1 << 20          # 单段上限 1 MiB；设备实际每段 16–32 KB
MAX_SESSION = 32 << 20       # 单会话上限 32 MiB；2 小时录音约 10.8 MB
IDLE_TIMEOUT = 30            # socket 超时（秒）
MAX_THREADS = 16             # 并发请求上限
TS_WINDOW_BEFORE = 90 * 86400
TS_WINDOW_AFTER = 86400

SID_RE = re.compile(r"^([0-9a-f]{8})-(\d{10})-([0-9a-f]{8})$")
SID_LEN = 28                       # 8 + 1 + 10 + 1 + 8

STATUS_OK = "ok"
STATUS_DUPLICATE = "duplicate"
STATUS_OVERLAP = "overlap"
STATUS_CONFLICT = "conflict"
STATUS_GAP = "gap"
STATUS_PROBE = "probe"
STATUS_FINALIZED = "finalized"
STATUS_INCOMPLETE = "incomplete"
STATUS_BAD_TOTAL = "bad_total"
STATUS_UNAUTHORIZED = "unauthorized"
STATUS_FORBIDDEN = "forbidden"
STATUS_BAD_SESSION = "bad_session"
STATUS_BAD_REQUEST = "bad_request"
STATUS_LENGTH_REQUIRED = "length_required"
STATUS_CHUNK_TOO_LARGE = "chunk_too_large"
STATUS_SESSION_TOO_LARGE = "session_too_large"
STATUS_UNSUPPORTED_TYPE = "unsupported_type"
STATUS_NOT_FOUND = "not_found"
STATUS_METHOD_NOT_ALLOWED = "method_not_allowed"
STATUS_BUSY = "busy"


# ---------------------------------------------------------------------------
# 配置
# ---------------------------------------------------------------------------

@dataclass
class Config:
    data_dir: str = "/data"
    tokens_file: str | None = None
    port: int = 8000
    host: str = "0.0.0.0"
    tls_cert: str | None = None
    tls_key: str | None = None
    max_chunk: int = MAX_CHUNK
    max_session: int = MAX_SESSION
    idle_timeout: int = IDLE_TIMEOUT
    # 目录日期按这个时区切；None = 容器本地时区。中国用户应在 compose 里设 TZ=Asia/Shanghai。
    tz_offset_seconds: int | None = None


def date_for_ts(ts: int, tz_offset_seconds: int | None) -> str:
    """由 session_id 里的 ts 推出数据目录名 YYYY-MM-DD。"""
    if tz_offset_seconds is None:
        return datetime.datetime.fromtimestamp(ts).strftime("%Y-%m-%d")
    tz = datetime.timezone(datetime.timedelta(seconds=tz_offset_seconds))
    return datetime.datetime.fromtimestamp(ts, tz).strftime("%Y-%m-%d")


# ---------------------------------------------------------------------------
# session_id
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class SessionId:
    dev: str
    ts: int
    rand: str

    def __str__(self) -> str:
        return f"{self.dev}-{self.ts:010d}-{self.rand}"

    @classmethod
    def parse(cls, raw: str, now: int) -> "SessionId | None":
        if not raw or len(raw) != SID_LEN:
            return None
        m = SID_RE.match(raw)
        if not m:
            return None
        ts = int(m.group(2))
        # 明显离谱的时间戳直接拒掉，避免 Date 目录被写到 1970 或 3000 年。
        if ts < now - TS_WINDOW_BEFORE or ts > now + TS_WINDOW_AFTER:
            return None
        return cls(m.group(1), ts, m.group(3))


# ---------------------------------------------------------------------------
# 结果
# ---------------------------------------------------------------------------

@dataclass
class Outcome:
    code: int
    status: str
    received: int = 0
    finalized: bool = False
    total: int | None = None
    message: str = ""


# ---------------------------------------------------------------------------
# 存储层：文件系统是唯一事实来源
# ---------------------------------------------------------------------------

@dataclass
class Meta:
    v: int = 1
    session_id: str = ""
    dev: str = ""
    ts: int = 0
    date: str = ""
    committed: int = 0
    total: int | None = None
    finalized: bool = False
    last_seq: int = 0
    ogg_ok: bool | None = None
    created_at: str = ""
    updated_at: str = ""
    src_ip: str = ""

    def to_dict(self) -> dict:
        return dict(self.__dict__)


def _now_iso() -> str:
    return datetime.datetime.now().astimezone().isoformat(timespec="seconds")


class Store:
    """按 session 分目录落盘，meta 记录已 fsync 的字节数。"""

    def __init__(self, cfg: Config):
        self.cfg = cfg
        self._locks: dict[str, threading.Lock] = {}
        self._locks_guard = threading.Lock()
        os.makedirs(cfg.data_dir, exist_ok=True)
        os.makedirs(os.path.join(cfg.data_dir, "quarantine"), exist_ok=True)

    # -- 锁 ----------------------------------------------------------------

    def lock_for(self, sid: SessionId) -> threading.Lock:
        # 全局锁只在取锁对象时持有，绝不跨 IO。
        with self._locks_guard:
            lk = self._locks.get(str(sid))
            if lk is None:
                lk = threading.Lock()
                self._locks[str(sid)] = lk
            return lk

    # -- 路径 --------------------------------------------------------------

    def _dir(self, sid: SessionId) -> str:
        d = os.path.join(self.cfg.data_dir, date_for_ts(sid.ts, self.cfg.tz_offset_seconds))
        # 兜底：realpath 必须落在 data_dir 内。session_id 已用正则闭合，这里是第二道锁。
        root = os.path.realpath(self.cfg.data_dir)
        if os.path.commonpath([root, os.path.realpath(d)]) != root:
            raise ValueError("path escapes data dir")
        return d

    def _paths(self, sid: SessionId) -> tuple[str, str, str]:
        d = self._dir(sid)
        base = os.path.join(d, str(sid))
        return base + ".ogg.part", base + ".ogg", base + ".meta.json"

    # -- meta --------------------------------------------------------------

    def _read_meta(self, meta_path: str) -> Meta | None:
        try:
            with open(meta_path, "r", encoding="utf-8") as f:
                return Meta(**json.load(f))
        except FileNotFoundError:
            return None
        except (json.JSONDecodeError, TypeError):
            return None

    def _write_meta(self, meta_path: str, meta: Meta) -> None:
        meta.updated_at = _now_iso()
        tmp = meta_path + ".tmp"
        d = os.path.dirname(meta_path)
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(meta.to_dict(), f, ensure_ascii=False, separators=(",", ":"))
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, meta_path)
        self._fsync_dir(d)

    @staticmethod
    def _fsync_dir(path: str) -> None:
        try:
            fd = os.open(path, os.O_RDONLY)
        except OSError:
            return
        try:
            os.fsync(fd)
        except OSError:
            pass
        finally:
            os.close(fd)

    # -- 状态重建 ----------------------------------------------------------

    def _reconcile(self, sid: SessionId, part: str, meta_path: str, src_ip: str) -> Meta | None:
        """在 per-session 锁内调用。修正"文件大小与 meta.committed 不一致"的异常状态。

        正常流程是 写数据 → fsync → 更新 committed，所以两者应当相等。
        崩溃可能留下 size > committed（数据已写、meta 未更新），此时截断到 committed，
        该段由设备重传；size < committed 说明数据丢了，把 committed 降回 size。
        """
        meta = self._read_meta(meta_path)
        part_exists = os.path.exists(part)

        if meta is None:
            if part_exists:
                # .part 存在但 meta 缺失：不猜长度，整个移到 quarantine。
                qdir = os.path.join(self.cfg.data_dir, "quarantine")
                os.makedirs(qdir, exist_ok=True)
                os.replace(part, os.path.join(qdir, os.path.basename(part) + ".orphan"))
            return None

        if not part_exists:
            if meta.committed > 0 and not meta.finalized:
                meta.committed = 0
                self._write_meta(meta_path, meta)
            return meta

        size = os.path.getsize(part)
        if size > meta.committed:
            with open(part, "r+b") as f:
                f.truncate(meta.committed)
                f.flush()
                os.fsync(f.fileno())
        elif size < meta.committed:
            meta.committed = size
            self._write_meta(meta_path, meta)
        return meta

    def status_of(self, sid: SessionId) -> Outcome:
        with self.lock_for(sid):
            part, ogg, meta_path = self._paths(sid)
            if os.path.exists(ogg):
                meta = self._read_meta(meta_path)
                total = meta.total if meta else None
                return Outcome(200, STATUS_FINALIZED, os.path.getsize(ogg), True, total)
            meta = self._reconcile(sid, part, meta_path, "")
            if meta is None:
                return Outcome(200, STATUS_PROBE, 0, False, None)
            return Outcome(200, STATUS_OK, meta.committed, False, meta.total)

    # -- 写入 --------------------------------------------------------------

    def append(self, sid: SessionId, offset: int, data: bytes, seq: int,
               src_ip: str) -> Outcome:
        n = len(data)
        with self.lock_for(sid):
            part, ogg, meta_path = self._paths(sid)

            if os.path.exists(ogg):
                meta = self._read_meta(meta_path)
                total = meta.total if meta else None
                return Outcome(200, STATUS_FINALIZED, os.path.getsize(ogg), True, total)

            meta = self._reconcile(sid, part, meta_path, src_ip)

            # 零长请求：只回报当前长度，不建文件、不改状态。
            if n == 0 and offset == 0:
                return Outcome(200, STATUS_PROBE, meta.committed if meta else 0, False,
                               meta.total if meta else None)

            if meta is None:
                if offset != 0:
                    return Outcome(409, STATUS_GAP, 0, False, None, "offset_ahead")
                # 新建会话：先建 meta（committed=0），再追加。
                os.makedirs(os.path.dirname(part), exist_ok=True)
                meta = Meta(session_id=str(sid), dev=sid.dev, ts=sid.ts,
                            date=os.path.basename(os.path.dirname(part)),
                            committed=0, created_at=_now_iso())
                self._write_meta(meta_path, meta)
                open(part, "ab").close()
                self._fsync_dir(os.path.dirname(part))
                received = 0
            else:
                received = meta.committed

            if meta.finalized:
                return Outcome(200, STATUS_FINALIZED, received, True, meta.total)

            # offset 超前：告诉设备从 received 重传。
            if offset > received:
                return Outcome(409, STATUS_GAP, received, False, meta.total, "offset_ahead")

            # offset 落后或正好：比对重叠部分。
            overlap_len = min(received - offset, n) if offset < received else 0
            if overlap_len > 0 and not self._tail_matches(part, offset, data[:overlap_len]):
                return Outcome(409, STATUS_CONFLICT, received, False, meta.total, "content_mismatch")

            append_from = n - overlap_len      # 只追加 received 之后的部分
            new_len = received + append_from

            if new_len > self.cfg.max_session:
                return Outcome(413, STATUS_SESSION_TOO_LARGE, received, False, meta.total)

            if append_from > 0:
                payload = data[overlap_len:]
                fd = os.open(part, os.O_WRONLY | os.O_APPEND | os.O_CREAT, 0o644)
                try:
                    written = 0
                    while written < len(payload):
                        written += os.write(fd, payload[written:])
                    os.fsync(fd)          # 先落盘
                finally:
                    os.close(fd)
                meta.committed = new_len  # 再记账
                meta.last_seq = seq
                meta.src_ip = src_ip or meta.src_ip
                self._write_meta(meta_path, meta)

            status = STATUS_DUPLICATE if append_from == 0 and overlap_len == n \
                else STATUS_OVERLAP if overlap_len > 0 else STATUS_OK
            return Outcome(200, status, meta.committed, False, meta.total)

    @staticmethod
    def _tail_matches(part: str, offset: int, expected: bytes) -> bool:
        try:
            with open(part, "rb") as f:
                f.seek(offset)
                return f.read(len(expected)) == expected
        except FileNotFoundError:
            return False

    def finalize(self, sid: SessionId, total: int, seq: int, src_ip: str) -> Outcome:
        with self.lock_for(sid):
            part, ogg, meta_path = self._paths(sid)

            if os.path.exists(ogg):
                meta = self._read_meta(meta_path)
                return Outcome(200, STATUS_FINALIZED, os.path.getsize(ogg), True,
                               meta.total if meta else None)

            meta = self._reconcile(sid, part, meta_path, src_ip)
            if meta is None:
                if total != 0:
                    return Outcome(400, STATUS_INCOMPLETE, 0, False, None, "no_data")
                # 空会话：建一个空 .ogg，避免设备卡在 final 重试。
                os.makedirs(os.path.dirname(part), exist_ok=True)
                meta = Meta(session_id=str(sid), dev=sid.dev, ts=sid.ts,
                            date=os.path.basename(os.path.dirname(part)),
                            committed=0, created_at=_now_iso())
                self._write_meta(meta_path, meta)
                open(part, "ab").close()

            received = meta.committed
            if total < received:
                return Outcome(400, STATUS_BAD_TOTAL, received, False, None, "total_lt_received")
            if received < total:
                return Outcome(400, STATUS_INCOMPLETE, received, False, None, "bytes_missing")

            # 内容校验只做最低限度：首 4 字节必须是 OggS。不通过也收，只记在 meta 里。
            ogg_ok = self._looks_like_ogg(part, received)
            if not ogg_ok:
                print(f"[warn] {sid}: finalized but not OggS, kept anyway", file=sys.stderr)

            fd = os.open(part, os.O_RDONLY)
            try:
                os.fsync(fd)
            finally:
                os.close(fd)
            os.replace(part, ogg)
            self._fsync_dir(os.path.dirname(ogg))

            meta.finalized = True
            meta.total = total
            meta.committed = total
            meta.ogg_ok = ogg_ok
            meta.last_seq = seq
            meta.src_ip = src_ip or meta.src_ip
            self._write_meta(meta_path, meta)
            return Outcome(200, STATUS_FINALIZED, total, True, total)

    @staticmethod
    def _looks_like_ogg(part: str, size: int) -> bool:
        if size < 4:
            return False
        try:
            with open(part, "rb") as f:
                return f.read(4) == b"OggS"
        except OSError:
            return False


# ---------------------------------------------------------------------------
# 鉴权
# ---------------------------------------------------------------------------

class TokenAuth:
    """tokens.json: {"devices": {"<dev8>": "<token>"}}。一机一 token，与 session_id 的设备前缀绑定。"""

    def __init__(self, path: str | None):
        self.devices: dict[str, str] = {}
        self.path = path
        if path:
            self.reload()

    def reload(self) -> None:
        if not self.path:
            return
        try:
            with open(self.path, "r", encoding="utf-8") as f:
                data = json.load(f)
        except FileNotFoundError:
            print(f"[warn] tokens file not found: {self.path}", file=sys.stderr)
            return
        except json.JSONDecodeError as e:
            print(f"[error] tokens file invalid: {e}", file=sys.stderr)
            return
        devs = data.get("devices", {})
        self.devices = {str(k): str(v) for k, v in devs.items() if v}
        print(f"[info] loaded {len(self.devices)} device token(s)", file=sys.stderr)

    def check(self, dev: str, header: str | None) -> str | None:
        """返回 None=通过，否则返回错误状态名。

        token 是设备身份，session_id 的设备前缀必须与之对应：
          - token 不匹配任何设备 -> 401（凭据无效）
          - token 有效但不属于这个设备前缀 -> 403（冒用他人 session）
        """
        if not self.devices:
            # 没配 tokens 时拒绝一切上传，避免误开成公网无鉴权服务。
            return STATUS_UNAUTHORIZED
        if not header:
            return STATUS_UNAUTHORIZED
        parts = header.split()
        if len(parts) != 2 or parts[0].lower() != "bearer":
            return STATUS_UNAUTHORIZED
        matched_dev = None
        for d, t in self.devices.items():
            if hmac.compare_digest(parts[1], t):
                matched_dev = d
                break
        if matched_dev is None:
            return STATUS_UNAUTHORIZED
        if matched_dev != dev:
            return STATUS_FORBIDDEN
        return None


# ---------------------------------------------------------------------------
# HTTP 层
# ---------------------------------------------------------------------------

class UploadHandler(BaseHTTPRequestHandler):
    server_version = "aipass-upload/1"
    sys_version = ""
    protocol_version = "HTTP/1.1"

    # 由 build_server 注入
    cfg: Config
    store: Store
    auth: TokenAuth

    # -- 工具 --------------------------------------------------------------

    def log_message(self, fmt, *args):
        print(f"[{_now_iso()}] {self.address_string()} {fmt % args}", file=sys.stderr)

    def _json(self, code: int, obj: dict) -> None:
        body = json.dumps(obj, ensure_ascii=False, separators=(",", ":")).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        if code == 401:
            self.send_header("WWW-Authenticate", 'Bearer realm="aipass"')
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)
        self.close_connection = True

    def _outcome(self, sid: SessionId, o: Outcome, extra_code_ok: bool = True) -> None:
        obj = {
            "ok": extra_code_ok and o.code < 400,
            "status": o.status,
            "session_id": str(sid),
            "received": o.received,
            "finalized": o.finalized,
            "total": o.total,
        }
        if o.message:
            obj["error"] = o.message
        self._json(o.code, obj)

    def _error(self, code: int, status: str, sid: str = "", received: int = 0,
               message: str = "") -> None:
        obj = {"ok": False, "status": status, "session_id": sid, "received": received,
               "finalized": False, "total": None}
        if message:
            obj["error"] = message
        self._json(code, obj)

    def _read_body(self, n: int) -> bytes:
        chunks: list[bytes] = []
        remaining = n
        while remaining > 0:
            c = self.rfile.read(min(remaining, 65536))
            if not c:
                raise EOFError("short body")
            chunks.append(c)
            remaining -= len(c)
        return b"".join(chunks)

    @staticmethod
    def _int_param(qs: dict, headers, name: str, default=None):
        """头优先，query 兜底。返回 (value, error)。"""
        raw = headers.get(name)
        if raw is None:
            vals = qs.get(name.lower())
            raw = vals[0] if vals else None
        if raw is None:
            return default, None
        try:
            v = int(raw)
        except (TypeError, ValueError):
            return None, f"bad_{name.lower()}"
        if v < 0:
            return None, f"bad_{name.lower()}"
        return v, None

    # -- 路由 --------------------------------------------------------------

    def _route(self, method: str) -> None:
        parsed = urlparse(self.path)
        path = parsed.path
        qs = parse_qs(parsed.query)

        if path == "/healthz":
            if method == "GET":
                self._json(200, {"ok": True, "status": "healthy"})
            else:
                self._error(405, STATUS_METHOD_NOT_ALLOWED)
            return

        if not path.startswith("/v1/upload/"):
            self._error(404, STATUS_NOT_FOUND)
            return

        raw_sid = path[len("/v1/upload/"):]
        now = int(datetime.datetime.now().timestamp())
        sid = SessionId.parse(raw_sid, now)
        if sid is None:
            self._error(400, STATUS_BAD_SESSION, raw_sid, message="invalid_session_id")
            return

        # 鉴权在任何磁盘动作之前。
        auth_err = self.auth.check(sid.dev, self.headers.get("Authorization"))
        if auth_err == STATUS_UNAUTHORIZED:
            self._error(401, STATUS_UNAUTHORIZED, str(sid))
            return
        if auth_err == STATUS_FORBIDDEN:
            self._error(403, STATUS_FORBIDDEN, str(sid), message="token_device_mismatch")
            return

        if method == "GET" or method == "HEAD":
            o = self.store.status_of(sid)
            self._outcome(sid, o)
            return

        if method != "PUT":
            self._error(405, STATUS_METHOD_NOT_ALLOWED, str(sid))
            return

        self._handle_put(sid, qs)

    def _handle_put(self, sid: SessionId, qs: dict) -> None:
        # 1. Content-Length：不支持 chunked。
        raw_len = self.headers.get("Content-Length")
        if raw_len is None:
            self.close_connection = True
            self._error(411, STATUS_LENGTH_REQUIRED, str(sid), message="content_length_required")
            return
        try:
            length = int(raw_len)
        except ValueError:
            self.close_connection = True
            self._error(400, STATUS_BAD_REQUEST, str(sid), message="bad_content_length")
            return
        if length < 0:
            self.close_connection = True
            self._error(400, STATUS_BAD_REQUEST, str(sid), message="bad_content_length")
            return
        if length > self.cfg.max_chunk:
            # 不读 body，直接断开。
            self.close_connection = True
            self._error(413, STATUS_CHUNK_TOO_LARGE, str(sid),
                        message=f"max_chunk={self.cfg.max_chunk}")
            return

        ctype = self.headers.get("Content-Type")
        if ctype and not ctype.lower().startswith("application/octet-stream"):
            # 只记录不拒收：某些中继会改写 Content-Type。
            print(f"[warn] {sid}: unexpected Content-Type {ctype!r}", file=sys.stderr)

        # 2. 协议字段。
        offset, err = self._int_param(qs, self.headers, "X-Offset", 0)
        if err:
            self._error(400, STATUS_BAD_REQUEST, str(sid), message=err)
            return
        seq, _ = self._int_param(qs, self.headers, "X-Seq", 0)
        seq = seq or 0
        final_raw = self.headers.get("X-Final")
        if final_raw is None:
            vals = qs.get("final")
            final_raw = vals[0] if vals else None
        final = str(final_raw).strip().lower() in ("1", "true", "yes") if final_raw else False

        total = None
        if final:
            total, err = self._int_param(qs, self.headers, "X-Total-Len", None)
            if err or total is None:
                self._error(400, STATUS_BAD_REQUEST, str(sid), message="total_len_required")
                return

        # 3. 读 body。
        try:
            body = self._read_body(length)
        except EOFError:
            self.close_connection = True
            self._error(400, STATUS_BAD_REQUEST, str(sid), message="short_body")
            return

        if offset + len(body) > self.cfg.max_session:
            self._error(413, STATUS_SESSION_TOO_LARGE, str(sid))
            return

        # 4. 状态机。
        src = self.client_address[0] if self.client_address else ""
        if len(body) == 0 and not final:
            o = self.store.append(sid, offset, b"", seq, src)
            self._outcome(sid, o)
            return

        if len(body) > 0:
            o = self.store.append(sid, offset, body, seq, src)
            if o.code == 409 or o.code >= 400:
                self._outcome(sid, o)
                return
            if not final:
                self._outcome(sid, o)
                return
            # final 且带数据：先追加，再按 total 校验。
            o = self.store.finalize(sid, total, seq, src)
            self._outcome(sid, o)
            return

        o = self.store.finalize(sid, total, seq, src)
        self._outcome(sid, o)

    def do_PUT(self):
        self._route("PUT")

    def do_GET(self):
        self._route("GET")

    def do_HEAD(self):
        self._route("HEAD")

    def do_POST(self):
        self._route("POST")

    def do_DELETE(self):
        self._route("DELETE")


# ---------------------------------------------------------------------------
# 服务器
# ---------------------------------------------------------------------------

class BoundedThreadingHTTPServer(ThreadingHTTPServer):
    """限制并发线程数，超出的请求直接 503，避免被慢连接拖垮。"""

    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, addr, handler, cfg: Config):
        self._slots = threading.BoundedSemaphore(MAX_THREADS)
        self.cfg = cfg
        super().__init__(addr, handler)

    def process_request(self, request, client_address):
        if not self._slots.acquire(blocking=False):
            try:
                request.sendall(b"HTTP/1.1 503 Service Unavailable\r\n"
                                b"Content-Length: 0\r\nConnection: close\r\n\r\n")
            except OSError:
                pass
            self.shutdown_request(request)
            return
        try:
            super().process_request(request, client_address)
        except Exception:
            self._slots.release()
            raise

    def process_request_thread(self, request, client_address):
        try:
            super().process_request_thread(request, client_address)
        finally:
            self._slots.release()


def build_server(cfg: Config) -> BoundedThreadingHTTPServer:
    store = Store(cfg)
    auth = TokenAuth(cfg.tokens_file)

    handler = type("BoundUploadHandler", (UploadHandler,), {
        "cfg": cfg, "store": store, "auth": auth,
    })
    httpd = BoundedThreadingHTTPServer((cfg.host, cfg.port), handler, cfg)
    if cfg.tls_cert and cfg.tls_key:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(cfg.tls_cert, cfg.tls_key)
        httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)
    return httpd


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="AI Passport upload receiver")
    ap.add_argument("--host", default=os.environ.get("HOST", "0.0.0.0"))
    ap.add_argument("--port", type=int, default=int(os.environ.get("PORT", "8000")))
    ap.add_argument("--data-dir", default=os.environ.get("DATA_DIR", "/data"))
    ap.add_argument("--tokens", default=os.environ.get("TOKENS_FILE"))
    ap.add_argument("--tls-cert", default=os.environ.get("TLS_CERT"))
    ap.add_argument("--tls-key", default=os.environ.get("TLS_KEY"))
    ap.add_argument("--tz-offset", type=int, default=None,
                    help="目录日期的时区偏移秒数，如 28800 = UTC+8；缺省用容器本地时区")
    args = ap.parse_args(argv)

    cfg = Config(
        data_dir=args.data_dir, tokens_file=args.tokens, port=args.port, host=args.host,
        tls_cert=args.tls_cert, tls_key=args.tls_key, tz_offset_seconds=args.tz_offset,
    )
    httpd = build_server(cfg)
    actual_port = httpd.server_address[1]
    # 测试脚本靠这一行拿端口。
    print(f"listening on {cfg.host}:{actual_port}", flush=True)
    print(f"data dir: {os.path.realpath(cfg.data_dir)}", file=sys.stderr, flush=True)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("shutting down", file=sys.stderr)
    finally:
        httpd.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))