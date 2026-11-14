#!/usr/bin/env python3
# Does "atc9" really close the Tactrix 2.5 mm jack? Counts the bytes before and after closing it.
import serial, time
s = serial.Serial("/dev/ttyACM0", 115200, timeout=0.2)

def cmd(c, extra=b""):
    s.write(c.encode() + b"\r\n" + extra)
    time.sleep(0.3)
    return s.read(s.in_waiting or 1)

print("ata :", cmd("ata"))
print("ato9:", cmd("ato9 512 19200 0"))
print("atf9:", cmd("atf9 1 0 4", bytes(8))[:12])
time.sleep(2)
print("jack open,   bytes in 2 s:", len(s.read(100000)))

print("atc9:", cmd("atc9")[:40])
s.reset_input_buffer()
print("waiting 5 s - watch the Tactrix light now")
time.sleep(5)
print("jack closed, bytes in 5 s:", len(s.read(100000)))

print("atz :", cmd("atz"))
s.close()
