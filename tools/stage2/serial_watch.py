import sys, time, serial

port, out_path, seconds = sys.argv[1], sys.argv[2], int(sys.argv[3])
end = time.time() + seconds
with open(out_path, "ab") as out:
    while time.time() < end:
        try:
            s = serial.Serial(port, 115200, timeout=1)
        except Exception:
            time.sleep(0.5)
            continue
        out.write(b"\n=== port opened ===\n")
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
