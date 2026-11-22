"""在宿主上验证 audio_cache 的扇区搬移与回收逻辑。

把 main/audio_cache.c 原样拿来编译（不复制、不修改），用内存模拟 flash，
再按 studio 的回收算法模拟"丢弃已完成会话、把剩下的前移"，检查：
  1. 搬动后每个扇区的 seq 与位置对应，CRC 正确；
  2. 搬动后重新 mount 得到的写指针等于搬移后的扇区数；
  3. 尾部被作废的扇区不会在 mount 时被扫回来；
  4. payload 逐字节不变。

用法：python3 tools/stage4/test_reclaim.py
"""
import ctypes
import os
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
CACHE_C = os.path.join(HERE, "..", "..", "firmware", "passport-app", "main", "audio_cache.c")

SEC, HDR = 4096, 20
MAGIC = 0x31435541


def build_so(tmpdir):
    """把 audio_cache.c 编译成共享库。audio_cache.h 无外部依赖，可直接包含。"""
    inc = os.path.dirname(CACHE_C)
    so = os.path.join(tmpdir, "libcache.so")
    subprocess.run(
        ["cc", "-O1", "-fPIC", "-shared", "-I", inc, CACHE_C, "-o", so],
        check=True,
    )
    return so


class Flash:
    """内存 flash，4 KB 擦除单位。用 ctypes 回调暴露给 C。"""

    def __init__(self, n_sectors):
        self.n = n_sectors
        self.mem = bytearray(b"\xff" * (n_sectors * SEC))
        self.read_cb = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32,
                                        ctypes.c_void_p, ctypes.c_size_t)(self._read)
        self.erase_cb = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32,
                                         ctypes.c_uint32)(self._erase)
        self.write_cb = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32,
                                         ctypes.c_void_p, ctypes.c_size_t)(self._write)

    def _read(self, ctx, off, buf, n):
        seg = self.mem[off:off + n]
        ctypes.memmove(buf, bytes(seg), len(seg))
        return 0 if len(seg) == n else -1

    def _erase(self, ctx, off, n):
        if off % SEC:
            return -1
        self.mem[off:off + n] = b"\xff" * n
        return 0

    def _write(self, ctx, off, buf, n):
        self.mem[off:off + n] = ctypes.string_at(buf, n)
        return 0


class FlashT(ctypes.Structure):
    _fields_ = [("read", ctypes.c_void_p), ("erase", ctypes.c_void_p),
                ("write", ctypes.c_void_p), ("ctx", ctypes.c_void_p)]


class CacheT(ctypes.Structure):
    _fields_ = [("flash", FlashT), ("num_sectors", ctypes.c_uint32),
                ("next_sector", ctypes.c_uint32), ("session", ctypes.c_uint32),
                ("buf_len", ctypes.c_uint32), ("img", ctypes.c_void_p)]


class MountInfo(ctypes.Structure):
    _fields_ = [("sectors", ctypes.c_uint32), ("payload_bytes", ctypes.c_uint32),
                ("last_session", ctypes.c_uint32)]


def check_sectors(mem, n):
    """逐扇区校验头部一致性、序号连续、CRC。返回 (会话列表, 总字节数)。"""
    sessions, total = [], 0
    for i in range(n):
        s = mem[i * SEC:(i + 1) * SEC]
        magic, seq, sess, ln = struct.unpack_from("<IIIH", s, 0)
        crc = struct.unpack_from("<I", s, 16)[0]
        assert magic == MAGIC, f"sector {i}: magic {magic:08x} 应为 {MAGIC:08x}"
        assert seq == i + 1, f"sector {i}: seq {seq} 应为 {i + 1}"
        assert ln <= SEC - HDR, f"sector {i}: len {ln} 超界"
        calc = zlib.crc32(s[0:16] + s[HDR:HDR + ln]) & 0xFFFFFFFF
        assert calc == crc, f"sector {i}: CRC 不符"
        if not sessions or sessions[-1] != sess:
            sessions.append(sess)
        total += ln
    return sessions, total


def main():
    tmp = tempfile.mkdtemp()
    lib = ctypes.CDLL(build_so(tmp))

    lib.audio_cache_mount.restype = ctypes.c_int
    lib.audio_cache_mount.argtypes = [ctypes.POINTER(CacheT), FlashT, ctypes.c_uint32,
                                      ctypes.c_void_p, ctypes.POINTER(MountInfo)]
    lib.audio_cache_begin_session.restype = ctypes.c_int
    lib.audio_cache_begin_session.argtypes = [ctypes.POINTER(CacheT), ctypes.c_uint32]
    lib.audio_cache_append.restype = ctypes.c_int
    lib.audio_cache_append.argtypes = [ctypes.POINTER(CacheT), ctypes.c_char_p, ctypes.c_size_t]
    lib.audio_cache_flush.restype = ctypes.c_int
    lib.audio_cache_flush.argtypes = [ctypes.POINTER(CacheT)]
    lib.audio_cache_move_sector.restype = ctypes.c_int
    lib.audio_cache_move_sector.argtypes = [ctypes.POINTER(CacheT), ctypes.c_uint32,
                                            ctypes.c_uint32]

    N = 24
    flash = Flash(N)
    buf = ctypes.create_string_buffer(SEC)
    cache = CacheT()
    flash_t = FlashT(ctypes.cast(flash.read_cb, ctypes.c_void_p).value,
                     ctypes.cast(flash.erase_cb, ctypes.c_void_p).value,
                     ctypes.cast(flash.write_cb, ctypes.c_void_p).value, None)
    info = MountInfo()
    assert lib.audio_cache_mount(ctypes.byref(cache), flash_t, N, buf, ctypes.byref(info)) == 0

    # 造三个会话：A 3 扇区 + 半扇区，B 2 扇区 + 半扇区，C 1 扇区 + 半扇区。
    # 用可辨识的内容，事后逐字节比对。
    def run_session(sess, n_full, tail_bytes, seed):
        lib.audio_cache_begin_session(ctypes.byref(cache), sess)
        for _ in range(n_full):
            # 满扇区必须正好写满 payload，否则会提前落盘、扇区数对不上
            lib.audio_cache_append(ctypes.byref(cache), bytes([seed]) * (SEC - HDR), SEC - HDR)
        lib.audio_cache_append(ctypes.byref(cache), bytes([seed]) * tail_bytes, tail_bytes)
        lib.audio_cache_flush(ctypes.byref(cache))

    A, B, C = 0xAAAAAAAA, 0xBBBBBBBB, 0xCCCCCCCC
    run_session(A, 3, 1000, 0xA1)
    run_session(B, 2, 2000, 0xB2)
    run_session(C, 1, 3000, 0xC3)
    assert cache.next_sector == 9, f"应有 9 个扇区，实为 {cache.next_sector}"

    before = bytes(flash.mem)
    sessions, total_before = check_sectors(before, 9)
    assert sessions == [A, B, C], f"会话序列异常：{sessions}"

    # ---- 场景 1：只保留 B、C（A 已上传），把它们前移到头部 ----
    # 这是 app_store_reclaim 的核心：按索引递增搬，然后作废尾部。
    old_next = cache.next_sector
    dst = 0
    for start, count in ((0, 4), (4, 3), (7, 2)):   # A/b 4 扇区、B 3 扇区、C 2 扇区
        if start == 0:
            continue                                    # A 已完成，丢弃
        for k in range(count):
            r = lib.audio_cache_move_sector(ctypes.byref(cache), start + k, dst)
            assert r == 0, f"move {start + k}->{dst} 失败 r={r}"
            dst += 1
    assert dst == 5, f"应搬 5 个扇区，实为 {dst}"

    # 作废尾部（与固件同一策略：把 magic 写零）
    for i in range(dst, old_next):
        flash.mem[i * SEC:i * SEC + 4] = b"\x00\x00\x00\x00"

    after = bytes(flash.mem)
    sess_after, total_after = check_sectors(after, dst)
    assert sess_after == [B, C], f"搬完的会话序列应为 [B, C]，实为 {sess_after}"
    assert total_after == total_before - (3 * (SEC - HDR) + 1000), \
        f"字节数不符：{total_after}"

    # payload 必须原样：B 和 C 的内容整体前移，一个字节都不能变
    payload_before = b"".join(before[i * SEC + HDR:i * SEC + HDR +
                                    struct.unpack_from("<I", before, i * SEC + 12)[0]]
                              for i in range(4, 9))
    payload_after = b"".join(after[i * SEC + HDR:i * SEC + HDR +
                                   struct.unpack_from("<I", after, i * SEC + 12)[0]]
                             for i in range(0, 5))
    assert payload_before == payload_after, "payload 内容被改动"

    # ---- 场景 2：重新 mount，必须得到写指针 5、会话 C ----
    cache2 = CacheT()
    info2 = MountInfo()
    assert lib.audio_cache_mount(ctypes.byref(cache2), flash_t, N, buf,
                                 ctypes.byref(info2)) == 0
    assert cache2.next_sector == 5, f"重新挂载写指针 {cache2.next_sector} 应为 5"
    assert info2.last_session == C, f"最后会话 {info2.last_session:08x} 应为 {C:08x}"

    # ---- 场景 3：在搬移后继续录，旧尾部数据不能复活 ----
    D = 0xDDDDDDDD
    lib.audio_cache_begin_session(ctypes.byref(cache2), D)
    for _ in range(3):
        lib.audio_cache_append(ctypes.byref(cache2), bytes([0xD4]) * (SEC - HDR), SEC - HDR)
    lib.audio_cache_flush(ctypes.byref(cache2))

    final = bytes(flash.mem)
    sess_final, _ = check_sectors(final, cache2.next_sector)
    assert sess_final == [B, C, D], f"继续录音后的会话序列应为 [B, C, D]，实为 {sess_final}"

    # 尾部仍为 0xff 的扇区必须保持无效头，mount 不能扫过头
    cache3 = CacheT()
    info3 = MountInfo()
    lib.audio_cache_mount(ctypes.byref(cache3), flash_t, N, buf, ctypes.byref(info3))
    assert cache3.next_sector == cache2.next_sector, \
        f"尾部有残留：mount 得到 {cache3.next_sector}，应为 {cache2.next_sector}"

    # ---- 场景 4：搬动方向保护。dst >= src 必须被拒绝，否则会覆盖未处理数据 ----
    r = lib.audio_cache_move_sector(ctypes.byref(cache3), 2, 2)
    assert r != 0, "move(src==dst) 应被拒绝"
    r = lib.audio_cache_move_sector(ctypes.byref(cache3), 1, 2)
    assert r != 0, "move(src<dst) 应被拒绝"

    print("PASS 全部 4 个场景通过")
    print(f"  搬移后 {dst} 扇区，payload {total_after} 字节逐字节一致")
    print(f"  尾部 {old_next - dst} 个扇区已作废，重新挂载未复活")
    print(f"  搬移后继续录音，会话序列 {[f'{s:08x}' for s in sess_final]}")


if __name__ == "__main__":
    main()