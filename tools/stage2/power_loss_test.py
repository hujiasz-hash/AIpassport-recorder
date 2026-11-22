"""阶段 2 真实断电测试：擦缓存 → 开始录 → 人工断电 → 重启后校验。

用法：
    python tools/stage2/power_loss_test.py <端口> <工作目录> [步骤]

步骤：
    erase   擦除缓存分区（会删除所有已录数据，需显式确认）
    record  复位设备并抓取串口日志，直到你断电为止（Ctrl-C 结束抓取）
    verify  读回缓存、按会话拆分、校验 Ogg

为什么这么设计：真实断电必须人工拔电或拔 USB，软件无法模拟。脚本负责前后两端
的取证，中间的拔电动作由人完成。

ESP32-C3 用 USB-Serial/JTAG，pyserial 默认会拉 DTR/RTS 把芯片顶进下载模式，
所以打开端口必须显式 dtr=False / rts=False（同 serial_watch.py）。
"""
import os
import struct
import subprocess
import sys
import time

CACHE_OFF = 0x460000
CACHE_SIZE = 0x3A0000
SECTOR = 4096
HDR = 20
MAGIC = 0x31435541


def die(msg):
    print(msg, file=sys.stderr)
    sys.exit(1)


def need(cond, msg):
    if not cond:
        die(msg)


def esptool(args, idf_python):
    cmd = [idf_python, "-m", "esptool", "--chip", "esp32c3"] + args
    print("+", " ".join(cmd))
    subprocess.run(cmd, check=True)


def cmd_erase(port, workdir, idf_python, yes):
    need(yes, "擦除会删除缓存里所有已录数据。确认无误后加 --yes 再执行。")
    esptool(["-p", port, "erase_region", hex(CACHE_OFF), hex(CACHE_SIZE)], idf_python)


def cmd_record(port, workdir, idf_python, seconds):
    import serial

    os.makedirs(workdir, exist_ok=True)
    log_path = os.path.join(workdir, "powerloss_serial.log")
    out = open(log_path, "wb")
    print(f"抓取串口日志到 {log_path}")
    print("现在拔电：拔掉 USB 或断开电池。脚本 8 秒收不到数据就认为已断电。")
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.5
    s.dtr = False
    s.rts = False
    s.open()
    last = time.time()
    deadline = last + seconds
    while time.time() < deadline:
        try:
            d = s.read(4096)
        except Exception:
            d = b""
        if d:
            out.write(d)
            out.flush()
            last = time.time()
        elif time.time() - last > 8:
            print("已连续 8 秒没有输出，判断设备已断电。")
            break
    out.close()
    try:
        s.close()
    except Exception:
        pass
    print("断电前日志已经保存。重新上电后跑 verify 步骤。")


def cmd_verify(port, workdir, idf_python, seconds):
    os.makedirs(workdir, exist_ok=True)

    # 先抓一段重启日志，确认挂载恢复的扇区数。
    import serial

    boot_path = os.path.join(workdir, "powerloss_boot.log")
    out = open(boot_path, "wb")
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.5
    s.dtr = False
    s.rts = False
    s.open()
    end = time.time() + seconds
    while time.time() < end:
        try:
            d = s.read(4096)
        except Exception:
            d = b""
        if d:
            out.write(d)
            out.flush()
    out.close()
    s.close()
    print(f"重启日志 -> {boot_path}")

    for line in open(boot_path, "rb").read().split(b"\n"):
        if b"cache mounted" in line:
            print("  " + line.decode("utf-8", "replace").strip())

    # 读回整片缓存。
    dump = os.path.join(workdir, "powerloss_cache.bin")
    esptool(["-p", port, "-b", "460800", "read_flash", hex(CACHE_OFF), hex(CACHE_SIZE), dump],
            idf_python)

    data = open(dump, "rb").read()
    sessions, order, end_idx = {}, [], 0
    for i in range(len(data) // SECTOR):
        sec = data[i * SECTOR:(i + 1) * SECTOR]
        magic, seq, sess, ln = struct.unpack_from("<IIIH", sec, 0)
        if magic != MAGIC or seq != i + 1 or ln > SECTOR - HDR:
            break
        import zlib
        stored = struct.unpack_from("<I", sec, 16)[0]
        if (zlib.crc32(sec[0:16] + sec[HDR:HDR + ln]) & 0xFFFFFFFF) != stored:
            print(f"  扇区 {i}: CRC 不符，停止")
            break
        if sess not in sessions:
            sessions[sess] = bytearray()
            order.append(sess)
        sessions[sess] += sec[HDR:HDR + ln]
        end_idx = i + 1

    print(f"有效扇区 {end_idx} / {len(data) // SECTOR}")
    listen = os.path.join(workdir, "listen")
    os.makedirs(listen, exist_ok=True)
    for sess in order:
        p = os.path.join(listen, f"powerloss_{sess:08x}.ogg")
        open(p, "wb").write(sessions[sess])
        print(f"  会话 {sess:08x}: {len(sessions[sess])} 字节 -> {p}")
    print("用浏览器或 VLC 打开上面的 ogg 文件听，并跑 check_ogg.py 校验结构。")


def main():
    if len(sys.argv) < 3:
        die(__doc__)
    port, workdir = sys.argv[1], sys.argv[2]
    step = sys.argv[3] if len(sys.argv) > 3 else "verify"
    yes = "--yes" in sys.argv
    seconds = 120
    for a in sys.argv:
        if a.startswith("--seconds="):
            seconds = int(a.split("=", 1)[1])

    idf_python = os.environ.get("IDF_PYTHON_ENV_PATH")
    if not idf_python:
        die("没找到 IDF 环境。先执行：. ~/esp/esp-idf/export.sh")
    idf_python = os.path.join(idf_python, "bin", "python")

    if step == "erase":
        cmd_erase(port, workdir, idf_python, yes)
    elif step == "record":
        cmd_record(port, workdir, idf_python, seconds)
    elif step == "verify":
        cmd_verify(port, workdir, idf_python, seconds)
    else:
        die(f"未知步骤 {step}")


if __name__ == "__main__":
    main()