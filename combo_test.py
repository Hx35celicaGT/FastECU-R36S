#!/usr/bin/env python3
# Tactrix OpenPort 2.0: SSM K-line logging + 2.5 mm jack at the same time (one USB link).
# Proves the adapter can poll the ECU (channel 3) while streaming the Innovate LM-2 (channel 9),
# and is the reference for the FastECU changes. Close FastECU first. Needs python3-serial.
#
# Usage:  python3 combo_test.py [port] [seconds] [jack_flags] [poll_ms]
#   defaults: /dev/ttyACM0  60  512 (ISO9141_NO_CHECKSUM)  50
# Engine running (or key ON) and LM-2 OUT in the Tactrix jack.
import sys, time, serial

port       = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
secs       = float(sys.argv[2]) if len(sys.argv) > 2 else 60
jack_flags = sys.argv[3] if len(sys.argv) > 3 else "512"
poll_ms    = float(sys.argv[4]) if len(sys.argv) > 4 else 50

s = serial.Serial(port, 115200, timeout=0.02)

def cmd(text, extra=b"", wait=0.15):
    s.write(text.encode() + b"\r\n" + extra)
    time.sleep(wait)
    reply = s.read(s.in_waiting or 1)
    print(f"> {text:34s} < {reply!r}")
    return reply

def ssm(data):                       # 80 10 F0 len data... checksum (sum of all bytes)
    pkt = bytes([0x80, 0x10, 0xF0, len(data)]) + bytes(data)
    return pkt + bytes([sum(pkt) & 0xFF])

def send_ssm(data):
    pkt = ssm(data)
    s.write(f"att3 {len(pkt)} 0\r\n".encode() + pkt)

# ---- open both channels the same way FastECU opens the K-line ----
cmd("ati"); cmd("ata")
cmd("ato3 512 4800 3")                                   # ISO9141, no checksum, 4800 baud
for p, v in ((3, 0), (7, 1), (10, 0), (12, 0), (22, 0), (20, 25)):   # LOOPBACK P1_MAX P3_MIN P4_MIN PARITY TINIL
    cmd(f"ats3 {p} {v}", wait=0.05)
cmd("atf3 1 0 4", bytes(8))                              # pass-all filter, channel 3
cmd(f"ato9 {jack_flags} 19200 0")                         # 2.5 mm jack
cmd("atf9 1 0 4", bytes(8))                              # pass-all filter, channel 9
time.sleep(0.2); s.reset_input_buffer()

# ---- frame parser ----
buf = b""
ssm_msg = b""
stats = dict(ssm_tx=0, ssm_rx=0, jack_frames=0, jack_pkts=0, jack_bytes=0, other=0, bad=0)
jack_sizes = {}
jack_buf = b""
rpm = None
lam_text = "-"

def mts_decode():
    """Pull complete MTS packets out of jack_buf; return text for the first lambda word pair."""
    global jack_buf, lam_text
    while True:
        i = next((k for k in range(len(jack_buf) - 1)
                  if (jack_buf[k] & 0xA2) == 0xA2 and (jack_buf[k + 1] & 0x80) == 0x80), None)
        if i is None:
            jack_buf = jack_buf[-1:]
            return
        jack_buf = jack_buf[i:]
        words = ((jack_buf[0] & 0x01) << 7) | (jack_buf[1] & 0x7F)
        need = 2 + 2 * words
        if len(jack_buf) < need:
            return
        p, jack_buf = jack_buf[:need], jack_buf[need:]
        stats["jack_pkts"] += 1
        if words >= 2:
            func = (p[2] >> 2) & 0x07
            afr_mult = (((p[2] & 0x01) << 7) | (p[3] & 0x7F)) / 10.0
            raw = ((p[4] & 0x3F) << 7) | (p[5] & 0x7F)
            if func == 0:
                lam = (raw + 500) / 1000.0
                lam_text = f"lambda {lam:.3f}  AFR {lam * afr_mult:.2f}"
            elif func == 1:
                lam_text = f"O2 {raw / 10.0:.1f}% (free air)"
            else:
                lam_text = ["ok", "O2", "free-air cal", "need cal", "warming up",
                            "heater cal", "error code", "reserved"][func] + f" ({raw})"

def handle(ch, ftype, payload):
    global ssm_msg, rpm, jack_buf
    if ch == ord("3"):
        if ftype == 0x00:
            ssm_msg += payload
        elif ftype & 0x40:                                 # end of message
            m, ssm_msg = ssm_msg, b""
            if len(m) >= 8 and m[0] == 0x80 and m[4] == 0xE8:
                stats["ssm_rx"] += 1
                rpm = ((m[5] << 8) | m[6]) / 4.0
    elif ch == ord("9"):
        stats["jack_frames"] += 1
        if ftype == 0x00:
            stats["jack_bytes"] += len(payload)
            jack_sizes[len(payload)] = jack_sizes.get(len(payload), 0) + 1
            jack_buf += payload
            mts_decode()
    else:
        stats["other"] += 1

def parse():
    global buf
    while True:
        k = buf.find(b"ar")
        if k < 0:
            buf = buf[-1:]
            return
        if k > 0:
            stats["bad"] += 1
            buf = buf[k:]
        if len(buf) < 3:
            return
        c = buf[2]
        if 0x30 <= c <= 0x39:                              # data frame: ar<ch><len><type><payload>
            if len(buf) < 4 or len(buf) < 4 + buf[3]:
                return
            n = buf[3]
            handle(c, buf[4], buf[5:4 + n])
            buf = buf[4 + n:]
        else:                                              # text reply: aro / are / arf ... \r\n
            e = buf.find(b"\r\n")
            if e < 0:
                return
            if c == ord("e"):
                print("  adapter error:", buf[:e])
            buf = buf[e + 2:]

# ---- run ----
print(f"\nPolling RPM every {poll_ms:.0f} ms and reading the jack for {secs:.0f} s ...\n")
request = [0xA8, 0x00, 0x00, 0x00, 0x0E, 0x00, 0x00, 0x0F]   # RPM, 2 bytes
start = last_tx = last_print = time.time()
pending = False
last = dict(stats)
try:
    while time.time() - start < secs:
        now = time.time()
        if (not pending or now - last_tx > 0.5) and now - last_tx >= poll_ms / 1000.0:
            send_ssm(request); stats["ssm_tx"] += 1; last_tx = now; pending = True
        data = s.read(4096)
        if data:
            buf += data
            before = stats["ssm_rx"]
            parse()
            if stats["ssm_rx"] != before:
                pending = False
        if now - last_print >= 5:
            dt = now - last_print
            r = lambda k: (stats[k] - last[k]) / dt
            print(f"{now - start:5.0f}s  SSM {r('ssm_rx'):5.1f}/s (sent {r('ssm_tx'):5.1f}/s)  "
                  f"RPM {rpm if rpm is not None else '-':>7}  |  jack {r('jack_pkts'):5.1f} pkt/s "
                  f"{r('jack_bytes'):6.0f} B/s  {lam_text}  |  resync {stats['bad']}")
            last, last_print = dict(stats), now
except KeyboardInterrupt:
    pass

el = time.time() - start
print(f"\nTotals in {el:.0f} s: SSM sent {stats['ssm_tx']}, replies {stats['ssm_rx']} "
      f"({100.0 * stats['ssm_rx'] / max(stats['ssm_tx'], 1):.0f}%), "
      f"jack frames {stats['jack_frames']}, MTS packets {stats['jack_pkts']}, resyncs {stats['bad']}")
print("Jack data-frame sizes (bytes: count):", dict(sorted(jack_sizes.items())))
cmd("atz")
s.close()
