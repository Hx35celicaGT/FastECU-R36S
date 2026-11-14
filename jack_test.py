#!/usr/bin/env python3
# Tactrix OpenPort 2.0 2.5 mm jack test (receive-only serial channel).
# Close FastECU first. Needs pyserial:  pip install --break-system-packages pyserial
#
# Usage:  python3 jack_test.py [port] [protocol_id] [baud] [seconds]
#   defaults: /dev/ttyACM0  9 (2.5 mm jack channel)  19200  30
import sys, time, serial

port  = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
proto = sys.argv[2] if len(sys.argv) > 2 else "9"
baud  = sys.argv[3] if len(sys.argv) > 3 else "19200"
secs  = float(sys.argv[4]) if len(sys.argv) > 4 else 30

s = serial.Serial(port, 115200, timeout=0.2)

def cmd(text, wait=0.3):
    s.write((text + "\r\n").encode())
    time.sleep(wait)
    reply = s.read(s.in_waiting or 1)
    print(f"> {text:30s} < {reply!r}")

cmd("ati")                                  # firmware version (clone check)
cmd("ata")                                  # should answer "aro"
cmd(f"ato{proto} 512 {baud} 0", 0.5)          # open the jack channel
s.write(f"atf{proto} 1 0 4\r\n".encode() + bytes(8))   # pass-all filter: 4-byte mask + 4-byte pattern, all zero
time.sleep(0.3)
print(f"> {'atf' + proto + ' 1 0 4 + 8 zero bytes':30s} < {s.read(s.in_waiting or 1)!r}")

print(f"\nListening {secs:.0f} s ... (Ctrl+C to stop)\n")
total, frames, start, buf = 0, 0, time.time(), b""
try:
    while time.time() - start < secs:
        data = s.read(4096)
        if not data:
            continue
        total += len(data)
        buf += data
        frames += data.count(b"ar9")
        text = "".join(chr(b) if 32 <= b < 127 or b == 10 else "." for b in data)
        print(text, end="", flush=True)
except KeyboardInterrupt:
    pass

el = time.time() - start
print(f"\n\n{total} bytes in {el:.1f} s = {total/el:.0f} bytes/s, {frames} 'ar9' headers")
open("jack_test_raw.bin", "wb").write(buf)
print("Raw bytes saved to jack_test_raw.bin")
cmd("atz")                                  # close the adapter cleanly
s.close()
