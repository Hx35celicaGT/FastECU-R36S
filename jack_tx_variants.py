#!/usr/bin/env python3
# Tactrix 2.5 mm jack: try a few ways of opening channel 9 and sending, and report what the
# firmware says to each. Read-only risk: a refused command just returns an error.
# Close FastECU first. LM-2 unplugged. Twist red (tip) + black (ring) so any byte that does go
# out comes straight back in.
#
# Usage: python3 jack_tx_variants.py [port]      (default /dev/ttyACM0)
import sys, time, serial

port = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
s = serial.Serial(port, 115200, timeout=0.02)

def cmd(text, extra=b"", wait=0.15):
    s.write(text.encode() + b"\r\n" + extra)
    time.sleep(wait)
    return s.read(s.in_waiting or 1)

print("ati ->", cmd("ati"))
print("ata ->", cmd("ata"))

opens   = [("512", "0"), ("0", "0"), ("512", "9"), ("0", "9")]   # (open flags, last field)
txflags = ["0", "512"]
payload = b"TXTEST\n"
rows = []

for oflags, last in opens:
    cmd("atc9")                                         # make sure it starts closed
    r_open = cmd(f"ato9 {oflags} 19200 {last}")
    r_filt = cmd("atf9 1 0 4", bytes(8))
    for tf in txflags:
        s.reset_input_buffer()
        s.write(f"att9 {len(payload)} {tf}\r\n".encode() + payload)
        time.sleep(0.4)
        reply = s.read(s.in_waiting or 1)
        echo = b"TXTEST" in reply
        rows.append((f"ato9 {oflags} 19200 {last}", r_open.strip(), r_filt.strip(), tf, reply[:60], echo))

cmd("atc9")
s.close()

print()
print(f"{'open command':22s} {'open':10s} {'filter':12s} {'txflags':7s} reply to att9")
for o, ro, rf, tf, rep, echo in rows:
    flag = "   <-- BYTES CAME BACK" if echo else ""
    print(f"{o:22s} {ro.decode(errors='replace'):10s} {rf.decode(errors='replace'):12s} {tf:7s} {rep!r}{flag}")

if any(r[5] for r in rows):
    print("\nRESULT: at least one variant transmitted. Send me this output.")
else:
    print("\nRESULT: no variant transmitted.")
