"""串口监听：把日志原样写到文件，端口断开后自动重连。

用法：
    python tools/stage2/serial_watch.py <端口> <输出文件> <秒数>

为什么不用 serial.Serial(port, 115200) 直接打开：ESP32-C3 用的是 USB-Serial/JTAG，
DTR 接 EN（复位）、RTS 接 GPIO9（下载模式）。pyserial 打开端口时默认把两条控制线
拉高，等于按住 RESET 再拉低 GPIO9 —— 设备会掉进 "waiting for download"，日志只剩
一行 ESP-ROM 就没了。这里显式把 dtr/rts 置为 False（"不激活"），设备保持运行状态。
"""
import sys, time, serial

port, out_path, seconds = sys.argv[1], sys.argv[2], int(sys.argv[3])
end = time.time() + seconds
with open(out_path, "ab") as out:
    while time.time() < end:
        try:
            s = serial.Serial()
            s.port = port
            s.baudrate = 115200
            s.timeout = 0.5
            s.dtr = False     # 不拉 EN，避免复位
            s.rts = False     # 不拉 GPIO9，避免进下载模式
            s.xonxoff = False
            s.rtscts = False
            s.dsrdtr = False
            s.open()
        except Exception:
            time.sleep(0.5)
            continue
        out.write(b"\n=== port opened ===\n")
        out.flush()
        try:
            while time.time() < end:
                d = s.read(4096)
                if d:
                    out.write(d)
                    out.flush()
        except Exception:
            out.write(b"\n=== port lost ===\n")
            out.flush()
        finally:
            try:
                s.close()
            except Exception:
                pass