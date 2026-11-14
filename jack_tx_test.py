#!/usr/bin/env python3
# Tactrix OpenPort 2.0 clone: can the 2.5 mm jack TRANSMIT on the tip?
# Close FastECU first. Needs python3-serial. LM-2 must be UNPLUGGED for every mode.
#
# Usage:  python3 jack_tx_test.py <mode> [seconds] [port]
#   idle  - open channel 9 and hold it open so you can meter tip-to-sleeve at rest
#   zeros - send 0x00 bytes nonstop (tip spends most of its time at the "space" level)
#   loop  - send "TACTRIX 0001\n" lines; with tip jumpered to ring, count what comes back
# defaults: 30 seconds, /dev/ttyACM0
import sys, time, serial

mode = sys.argv[1] if len(sys.argv) > 1 else "loop"
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 30
port = sys.argv[3] if len(sys.argv) > 3 else "/dev/ttyACM0"

s = serial.Serial(port, 115200, timeout=0.02)

def cmd(text, extra=b"", wait=0.15):
    s.write(text.encode() + b"\r\n" + extra)
    time.sleep(wait)
    reply = s.read(s.in_waiting or 1)
    print(f"> {text:28s} < {reply!r}")
    return reply

cmd("ati"); cmd("ata")
cmd("ato9 512 19200 0")                 # same open as FastECU uses
cmd("atf9 1 0 4", bytes(8))             # pass-all filter
time.sleep(0.2); s.reset_input_buffer()

buf = b""
echo = b""          # data bytes received back on channel 9
other = b""         # anything that is not an ar9 frame (acks, errors)
sent = 0
n = 0
t0 = time.time()
next_tx = t0
print(f"mode={mode}  running {secs:.0f} s ...")

while time.time() - t0 < secs:
    now = time.time()
    if mode != "idle" and now >= next_tx:
        if mode == "zeros":
            data = bytes(32)
        else:
            n += 1
            data = f"TACTRIX {n:04d}\n".encode()
        s.write(f"att9 {len(data)} 0\r\n".encode() + data)
        sent += len(data)
        next_tx = now + (0.02 if mode == "zeros" else 0.1)   # 32 B / 20 ms = full 19200 rate

    buf += s.read(s.in_waiting or 1)
    while len(buf) >= 2:
        if buf.startswith(b"ar9") and len(buf) >= 5:
            ln = buf[3]
            if len(buf) < 4 + ln:
                break
            frame = buf[4:4 + ln]
            if frame[0] == 0x00:                     # NORM_MSG = data bytes
                echo += frame[1:]
            buf = buf[4 + ln:]
        elif buf.startswith(b"ar") and len(buf) < 5:
            break
        else:
            k = buf.find(b"ar9", 1)
            cut = k if k > 0 else len(buf)
            other += buf[:cut]
            buf = buf[cut:]

cmd("atc9")
s.close()

print()
print(f"bytes sent      : {sent}")
print(f"bytes received  : {len(echo)}")
if echo:
    print(f"first received  : {echo[:60]!r}")
print(f"other replies   : {len(other)} bytes  {other[:120]!r}")
if mode == "loop":
    if b"TACTRIX" in echo:
        print("RESULT: the tip transmits. Lines came back through the jumper.")
    elif echo:
        print("RESULT: something came back but not clean text: check baud/levels.")
    else:
        print("RESULT: nothing came back. Tip did not transmit (or the jumper is not tip-to-ring).")
