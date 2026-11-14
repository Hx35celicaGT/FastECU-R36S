#!/usr/bin/env python3
# Simulated Tactrix OpenPort 2.0 + SH7058 ECU for testing ram_sniffer.py without a car.
# Speaks the OpenPort text protocol (ata/ato/atf/att/atc, ar3 frames split into chunks,
# TX echo frames, aro acks) and SSM A0 / B0 / BF.
import random, sys, time, glob, os, shutil
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ram_sniffer as rs

class FakeEcu:
    def __init__(self, boot_seed):
        rnd = random.Random(1)
        self.ram = bytearray(rnd.getrandbits(8) for _ in range(0xC000))
        for a in range(0x1310, 0x2000): self.ram[a] = 0x00          # CAND1 truly free
        for a in range(0x50B0, 0x5BDC): self.ram[a] = 0x00          # CAND2 free
        self.ram[0x1800:0x1810] = bytes([boot_seed] * 16)             # start-up written, differs per boot
        self.ram[0x1900:0x1908] = bytes([0x77] * 8)                   # start-up written, SAME every boot
        self.n = 0
    def tick(self):
        self.n += 1
        self.ram[0x0F00 + self.n % 0x400] = self.n & 0xFF             # stack activity
        self.ram[0x1A00] = self.n & 0xFF                              # busy byte inside CAND1
        self.ram[0x4000:0x4010] = bytes([self.n & 0xFF] * 16)
        if self.ram[0x1C00] == 0xA5: self.ram[0x1C00:0x1C04] = b"\x11\x22\x33\x44"   # overwrites a fill
    def handle(self, pkt):
        p = pkt[4:-1]; c = p[0]; self.tick()
        if c == 0xBF: d = bytes([0xFF, 0xA2, 0x10, 0x11, 0x3D, 0x12, 0x58, 0x40, 0x06]) + bytes(40)
        elif c == 0xA0:
            a = (p[2] << 16 | p[3] << 8 | p[4]) - 0xFF0000; n = p[5] + 1
            if a == 0x0040 and random.random() < 0.5: return None      # flaky block -> retry path
            d = bytes([0xE0]) + bytes(self.ram[a:a + n])
        elif c == 0xB0:
            a = (p[1] << 16 | p[2] << 8 | p[3]) - 0xFF0000; data = p[4:]
            self.ram[a:a + len(data)] = data; d = bytes([0xF0]) + data
        else: return None
        m = bytes([0x80, 0xF0, 0x10, len(d)]) + d
        return m + bytes([sum(m) & 0xFF])

class FakeSerial:
    def __init__(self, ecu): self.ecu = ecu; self.out = b""; self.inb = b""
    @property
    def in_waiting(self): return len(self.out)
    def reset_input_buffer(self): self.out = b""
    def close(self): pass
    def read(self, n):
        n = min(n, len(self.out), random.randint(1, 40)); r = self.out[:n]; self.out = self.out[n:]; return r
    def write(self, data):
        self.inb += data
        while True:
            k = self.inb.find(b"\r\n")
            if k < 0: return
            line = self.inb[:k].decode(); rest = self.inb[k + 2:]
            if line.startswith("att3"):
                ln = int(line.split()[1])
                if len(rest) < ln: return
                pkt = rest[:ln]; self.inb = rest[ln:]
                self.out += b"aro\r\n" + b"ar3" + bytes([len(pkt) + 1, 0x01]) + pkt   # ack + TX echo
                resp = self.ecu.handle(pkt)
                if resp:
                    i = 0
                    while i < len(resp):                                   # split reply into frames
                        ch = resp[i:i + random.randint(3, 30)]; i += len(ch)
                        self.out += b"ar3" + bytes([len(ch) + 1, 0x00]) + ch
            elif line.startswith("atf"):
                self.inb = rest[8:]; self.out += b"aro\r\n"
            else:
                self.inb = rest; self.out += b"ari 1.17\r\n" if line == "ata" else b"aro\r\n"

ecu = FakeEcu(0x31)
def fake_tactrix(port, verbose=False): return Tactrix0(port, verbose, ser=FakeSerial(ecu))
Tactrix0 = rs.Tactrix
rs.Tactrix = fake_tactrix

out = "/tmp/rs_test"; shutil.rmtree(out, ignore_errors=True)
F = ["--out", out]
rs.main(["sweep", "--range", "FF0000-FF00FF", "--range", "FF0E00-FF2100", "--range", "FF4000-FF4100", "--candidates", "--passes", "3", "--label", "boot1"] + F)
ecu.ram[0x1800:0x1810] = bytes([0x52] * 16)          # "key cycle": start-up value changes
rs.main(["sweep", "--range", "FF0E00-FF2100", "--range", "FF4000-FF4100", "--candidates", "--passes", "2", "--label", "boot2"] + F)
rs.main(["report"] + sorted(glob.glob(out + "/sessions/*.json")) + F)
rs.main(["fill", "--range", "FF1B00-FF1CFF", "--yes"] + F)
rs.main(["check", "--range", "FF1B00-FF1CFF", "--passes", "3", "--label", "idle"] + F)
try:
    rs.main(["fill", "--range", "FF2000-FF2010", "--yes"] + F)
except SystemExit as e:
    print("protected zone ->", e)
