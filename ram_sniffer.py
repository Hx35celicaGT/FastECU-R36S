#!/usr/bin/env python3
# ram_sniffer.py - map which SH7058 RAM bytes the ECU uses, over SSM K-line
# through a Tactrix OpenPort 2.0 (same channel setup FastECU uses).
#
# Close FastECU first. Needs python3-serial (pip install pyserial).
#
# Commands
#   id      Connect and print the ECU ID.
#   sweep   Read RAM over and over; record each byte's first value, min/max and
#           how often it changed. Saves a session file (.json) per run.
#   report  Combine session files (no car needed) into report.txt + bytes.csv.
#   fill    Write a test pattern (default A5) into a block. Asks first.
#   check   Read a filled block over and over; log every byte that stops
#           matching the pattern, when, and what it was changed to.
#
# Address ranges: FF1310-FF1FFF or 0xFFFF1310-0xFFFF1FFF (end is inclusive).
#   --full        whole RAM 0xFFFF0000-0xFFFFBFFF (about 3 min per pass at 4800 baud)
#   --candidates  the four free-block candidates from the E2UE102I RAM map
#
# Examples
#   python3 ram_sniffer.py id
#   python3 ram_sniffer.py sweep --full --passes 3 --label keyon
#   python3 ram_sniffer.py sweep --full --secs 900 --label drive
#   python3 ram_sniffer.py report sessions/*.json
#   python3 ram_sniffer.py fill --range FF50B0-FF5BDB
#   python3 ram_sniffer.py check --range FF50B0-FF5BDB --secs 1200 --label drive
#
# Version 1.2  2026-11-09  This ECU answers every SSM read (A0 and A8) of 0xFFFF0000-0xFFFF1FFF
#              with FF, so that area can't be tested from outside: sweeps skip it, reports leave
#              it out, and fill refuses it.
import argparse, csv, json, os, sys, time
from array import array

VERSION = "1.2"
RAM_START, RAM_END = 0xFF0000, 0xFFBFFF       # SH7058 48 KB RAM (SSM uses the low 24 bits)
HIDDEN_END = 0xFF1FFF     # this ECU returns FF for all SSM reads of 0xFFFF0000-0xFFFF1FFF

# Zones from the E2UE102I static analysis (claude/RAM_MAP_E2UE102I.md).
# nofill = fill refuses to write there.  Edit this table for another ROM.
ZONES = [
    # start,    end,      name,          nofill, note
    (0xFF0000, 0xFF130F, "STACK",        True,  "runtime stack, grows down from 0xFFFF1304; deep bytes look unused but are not"),
    (0xFF1310, 0xFF1FFF, "CAND1",        True,  "static-analysis candidate, but SSM reads return FF here: can't be tested"),
    (0xFF2000, 0xFF3FFF, "LEARNED",      True,  "learned values, 0xAA55 signature; never write"),
    (0xFF50B0, 0xFF5BDB, "CAND2",        False, "free candidate, high confidence; cleared at boot"),
    (0xFFA490, 0xFFB4DF, "CAND3",        False, "free candidate, medium (array before it has unknown size)"),
    (0xFFB4E0, 0xFFB7FF, "RINGBUF",      True,  "100-entry ring buffer"),
    (0xFFB800, 0xFFBF8F, "BOOTSTACK",    False, "low confidence: boot stack, overwritten every reset"),
    (0xFFBFA0, 0xFFBFFF, "TOP",          True,  "above reset stack pointer"),
]
CANDIDATES = [(z[0], z[1]) for z in ZONES if z[2] in ("CAND2", "CAND3")] + [(0xFFB800, 0xFFBF8F)]


def zone_of(a):
    for s, e, name, nofill, note in ZONES:
        if s <= a <= e:
            return name, nofill
    return "", False


def fmt(a):
    return f"0xFF{a & 0xFFFFFF:06X}"   # print as 0xFFFFxxxx like Ghidra / the RAM map


def parse_addr(t):
    v = int(t.strip().lower().replace("0x", ""), 16)
    return v & 0xFFFFFF


def parse_range(t, any_addr=False):
    if "-" not in t:
        raise argparse.ArgumentTypeError("range must be START-END, e.g. FF1310-FF1FFF")
    a, b = (parse_addr(x) for x in t.split("-", 1))
    if b < a:
        a, b = b, a
    if not any_addr and (a < RAM_START or b > RAM_END):
        raise argparse.ArgumentTypeError(f"{t} is outside RAM 0xFFFF0000-0xFFFFBFFF (use --any-address to force)")
    return (a, b)


def merge_ranges(rs):
    out = []
    for a, b in sorted(rs):
        if out and a <= out[-1][1] + 1:
            out[-1] = (out[-1][0], max(out[-1][1], b))
        else:
            out.append((a, b))
    return out


# ---------------------------------------------------------------- Tactrix link
class Tactrix:
    """OpenPort 2.0 text protocol, ISO9141 channel 3, 4800 baud, no checksum (SSM K-line)."""

    def __init__(self, port, verbose=False, ser=None):
        if ser is None:
            import serial
            ser = serial.Serial(port, 115200, timeout=0.02)
        self.s = ser
        self.verbose = verbose
        self.buf = b""          # raw bytes from the Tactrix
        self.rx = b""           # K-line data bytes received
        self.errors = []
        self.raw = b""          # everything the Tactrix sent (for diagnostics)
        self.s.reset_input_buffer()
        self.cmd("ata")
        self.cmd("ato3 512 4800 3")                 # protocol ISO9141, ISO9141_NO_CHECKSUM, 4800
        # same SET_CONFIG as FastECU set_j2534_iso9141_timings():
        # LOOPBACK 0, P1_MAX 1, P3_MIN 0, P4_MIN 0, PARITY none, TINIL 25
        for par, val in ((3, 0), (7, 1), (10, 0), (12, 0), (22, 0), (20, 25)):
            self.cmd(f"ats3 {par} {val}", wait=0.05)
        self.cmd("atf3 1 0 4", bytes(8))            # pass-all filter, as FastECU
        self.vbatt = self.read_vbatt()
        time.sleep(0.1)
        self.pump()
        self.rx = b""

    def cmd(self, text, extra=b"", wait=0.15):
        mark = len(self.raw)
        self.s.write(text.encode() + b"\r\n" + extra)
        time.sleep(wait)
        self.pump()
        if self.verbose:
            print(f"> {text:22s} < {self.raw[mark:]!r}")

    def read_vbatt(self):
        """OBD pin 16 voltage in volts, or None. Reply looks like 'arr 16 12345'."""
        mark = len(self.raw)
        self.s.write(b"atr 16\r\n")
        time.sleep(0.2)
        self.pump()
        txt = self.raw[mark:].decode(errors="replace")
        for line in txt.splitlines():
            parts = line.strip().split()
            if parts and parts[0] == "arr" and parts[-1].isdigit():
                v = int(parts[-1]) / 1000
                if self.verbose:
                    print(f"> atr 16                 < {line.strip()!r}  = {v:.2f} V")
                return v
        if self.verbose:
            print(f"> atr 16                 < {txt!r}")
        return None

    def close(self):
        try:
            self.cmd("atc3", wait=0.05)
        finally:
            self.s.close()

    def pump(self):
        n = self.s.in_waiting
        data = self.s.read(n or 1)
        if data:
            self.buf += data
            self.raw = (self.raw + data)[-4000:]
        b = self.buf
        while len(b) >= 3:
            if b.startswith(b"ar3"):
                if len(b) < 5:
                    break
                ln = b[3]
                if len(b) < 4 + ln:
                    break
                frame = b[4:4 + ln]
                if ln and frame[0] in (0x00, 0x02):       # NORM_MSG / START_OF_MESSAGE: data
                    self.rx += frame[1:]
                b = b[4 + ln:]                             # TX echo / indications dropped
            elif b.startswith(b"aro"):
                k = b.find(b"\n")
                if k < 0:
                    break
                b = b[k + 1:]
            elif b.startswith(b"are"):
                k = b.find(b"\n")
                if k < 0:
                    break
                self.errors.append(b[:k].decode(errors="replace").strip())
                b = b[k + 1:]
            elif b.startswith(b"ar"):
                k = b.find(b"\n")
                if k < 0:
                    if len(b) > 200:            # never stall on garbage
                        b = b[2:]
                        continue
                    break
                b = b[k + 1:]                   # ari / other text replies
            else:
                k = b.find(b"ar", 1)
                b = b[k:] if k > 0 else b[-1:]
        self.buf = b

    def ssm(self, payload, timeout=1.5):
        pkt = bytes([0x80, 0x10, 0xF0, len(payload)]) + bytes(payload)
        pkt += bytes([sum(pkt) & 0xFF])
        self.pump()
        self.rx = b""
        self.s.write(f"att3 {len(pkt)} 0\r\n".encode() + pkt)
        if self.verbose:
            print(f"> SSM  {pkt.hex(' ')}")
        mark = len(self.raw)
        t_end = time.time() + timeout
        while time.time() < t_end:
            self.pump()
            i = self.rx.find(b"\x80\xF0\x10")
            if i >= 0 and len(self.rx) >= i + 4:
                ln = self.rx[i + 3]
                total = 4 + ln + 1
                if len(self.rx) >= i + total:
                    msg = self.rx[i:i + total]
                    if sum(msg[:-1]) & 0xFF != msg[-1]:
                        if self.verbose:
                            print(f"< bad checksum {msg.hex(' ')}")
                        return None
                    if self.verbose:
                        print(f"< SSM  {msg.hex(' ')}")
                    return msg[4:4 + ln]
            time.sleep(0.002)
        if self.verbose:
            print(f"< no SSM reply. K-line bytes: {self.rx.hex(' ') or '(none)'}")
            print(f"  raw from Tactrix: {self.raw[mark:][-300:]!r}")
            if self.errors:
                print(f"  Tactrix errors: {self.errors}")
        return None


class Ecu:
    def __init__(self, link, method="a0"):
        self.link = link
        self.method = method    # a0 = block read, a8 = list-of-addresses read (what loggers use)
        self.requests = 0
        self.failures = 0

    def ecu_id(self):
        r = self.link.ssm([0xBF], timeout=2.0)
        if not r or r[0] != 0xFF or len(r) < 9:
            return None
        return r[4:9].hex().upper()

    def read_block(self, a, n):
        self.requests += 1
        if self.method == "a8":
            req = [0xA8, 0x00]
            for x in range(a, a + n):
                req += [(x >> 16) & 0xFF, (x >> 8) & 0xFF, x & 0xFF]
            r = self.link.ssm(req)
            ok = r and r[0] == 0xE8 and len(r) == n + 1
        else:
            r = self.link.ssm([0xA0, 0x00, (a >> 16) & 0xFF, (a >> 8) & 0xFF, a & 0xFF, n - 1])
            ok = r and r[0] == 0xE0 and len(r) == n + 1
        if ok:
            return r[1:]
        self.failures += 1
        return None

    def write_block(self, a, data):
        r = self.link.ssm([0xB0, (a >> 16) & 0xFF, (a >> 8) & 0xFF, a & 0xFF] + list(data))
        return bool(r) and r[0] == 0xF0

    def read(self, a, n, block):
        """Read n bytes. Returns (bytearray, okmask bytearray). Blocks that fail twice are split."""
        data = bytearray(n)
        ok = bytearray(n)
        pos = 0
        while pos < n:
            ln = min(block, n - pos)
            self._read_into(a + pos, ln, data, ok, pos)
            pos += ln
        return data, ok

    def _read_into(self, a, ln, data, ok, off):
        for _ in range(2):
            r = self.read_block(a, ln)
            if r is not None:
                data[off:off + ln] = r
                ok[off:off + ln] = b"\x01" * ln
                return
        if ln > 1:                                  # split and try the halves
            h = ln // 2
            self._read_into(a, h, data, ok, off)
            self._read_into(a + h, ln - h, data, ok, off + h)


def connect(args):
    link = Tactrix(args.port, verbose=args.verbose)
    ecu = Ecu(link, args.method)
    if args.method == "a8" and args.block > 32:
        args.block = 32                         # A8 request carries 3 bytes per address

    if link.vbatt is not None:
        print(f"OBD battery voltage {link.vbatt:.2f} V" +
              ("  <- too low: key on? cable seated?" if link.vbatt < 11 else ""))
    eid = ecu.ecu_id()
    if not eid:
        link.close()
        print(f"Tactrix sent: {link.raw[-300:]!r}")
        print(f"K-line bytes: {link.rx.hex(' ') or '(none)'}")
        sys.exit("No reply from the ECU. Key on? FastECU closed? Right port (--port)? "
                 "Run again with --verbose and send the output.")
    print(f"ECU ID {eid}")
    return link, ecu, eid


# ---------------------------------------------------------------- helpers
def ranges_from_args(args):
    rs = list(args.range or [])
    if getattr(args, "full", False):
        rs.append((RAM_START, RAM_END))
    if getattr(args, "candidates", False):
        rs += CANDIDATES
    if not rs:
        sys.exit("Give --range START-END, --full or --candidates")
    rs = merge_ranges(rs)
    if any(a <= HIDDEN_END for a, b in rs):
        print("Skipping 0xFFFF0000-0xFFFF1FFF: this ECU returns FF for every SSM read there.")
        rs = [(max(a, HIDDEN_END + 1), b) for a, b in rs if b > HIDDEN_END]
        if not rs:
            sys.exit("Nothing left: that whole range is in 0xFFFF0000-0xFFFF1FFF, which can't be read or tested.")
    return rs


def outdir(args, sub):
    d = os.path.join(args.out, sub)
    os.makedirs(d, exist_ok=True)
    return d


def stamp():
    return time.strftime("%Y%m%d-%H%M%S")


def runs(addrs_flags, min_len):
    """addrs_flags: sorted list of (addr, flag). Returns [(start, end)] runs of flag True >= min_len."""
    out, start, prev = [], None, None
    for a, f in addrs_flags:
        if f and start is not None and a == prev + 1:
            prev = a
            continue
        if start is not None and prev - start + 1 >= min_len:
            out.append((start, prev))
        start, prev = (a, a) if f else (None, None)
    if start is not None and prev - start + 1 >= min_len:
        out.append((start, prev))
    return out


def describe_values(vals):
    s = set(vals)
    if len(s) == 1:
        return f"all {vals[0]:02X}"
    zeros = sum(1 for v in vals if v == 0)
    return f"mixed ({zeros * 100 // len(vals)}% 00)"


# ---------------------------------------------------------------- sweep
def cmd_sweep(args):
    rs = ranges_from_args(args)
    total = sum(b - a + 1 for a, b in rs)
    link, ecu, eid = connect(args)
    st = []
    for a, b in rs:
        n = b - a + 1
        st.append(dict(start=a, n=n, first=None, last=None, mn=bytearray(b"\xff" * n),
                       mx=bytearray(n), chg=array("H", [0] * n), seen=bytearray(n)))
    t0 = time.time()
    passes = 0
    print(f"Sweeping {total} bytes in {len(rs)} range(s), block {args.block}. Ctrl+C stops and saves.")
    try:
        while True:
            tp = time.time()
            for r in st:
                data, ok = ecu.read(r["start"], r["n"], args.block)
                if r["first"] is None:
                    r["first"] = bytearray(data)
                    r["last"] = bytearray(data)
                last, mn, mx, chg, seen = r["last"], r["mn"], r["mx"], r["chg"], r["seen"]
                for i in range(r["n"]):
                    if not ok[i]:
                        continue
                    v = data[i]
                    if not seen[i]:
                        seen[i] = 1
                        r["first"][i] = v
                    elif v != last[i] and chg[i] < 65535:
                        chg[i] += 1
                    last[i] = v
                    if v < mn[i]: mn[i] = v
                    if v > mx[i]: mx[i] = v
            passes += 1
            el = time.time() - t0
            busy = sum(1 for r in st for c in r["chg"] if c)
            print(f"pass {passes}  {time.time() - tp:5.1f} s  elapsed {el:6.0f} s  bytes changed so far {busy}  "
                  f"failed reads {ecu.failures}", flush=True)
            if args.passes and passes >= args.passes:
                break
            if args.secs and el >= args.secs:
                break
    except KeyboardInterrupt:
        print("\nStopped, saving.")
    finally:
        link.close()
    if passes == 0 and st[0]["first"] is None:
        sys.exit("Nothing read.")
    sess = dict(tool="ram_sniffer", version=VERSION, kind="sweep", label=args.label, ecu_id=eid, method=args.method,
                started=time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(t0)),
                seconds=round(time.time() - t0, 1), passes=passes, block=args.block,
                ranges=[dict(start=r["start"], n=r["n"], first=r["first"].hex(), last=r["last"].hex(),
                             min=r["mn"].hex(), max=r["mx"].hex(), changes=list(r["chg"]),
                             seen=r["seen"].hex()) for r in st])
    path = os.path.join(outdir(args, "sessions"), f"{stamp()}_{args.label}.json")
    with open(path, "w") as f:
        json.dump(sess, f)
    print(f"Saved {path}")
    print("Make a report with:  python3 ram_sniffer.py report " + os.path.join(args.out, "sessions", "*.json"))


# ---------------------------------------------------------------- report
def load_session(p):
    with open(p) as f:
        s = json.load(f)
    if s.get("tool") != "ram_sniffer" or s.get("kind") != "sweep":
        sys.exit(f"{p} is not a ram_sniffer sweep session")
    byte = {}
    s["blind"] = 0
    for r in s["ranges"]:
        first = bytes.fromhex(r["first"]); seen = bytes.fromhex(r["seen"])
        mn = bytes.fromhex(r["min"]); mx = bytes.fromhex(r["max"])
        for i in range(r["n"]):
            if seen[i]:
                a = r["start"] + i
                if a <= HIDDEN_END:             # ECU returns FF here for every read: no information
                    s["blind"] += 1
                    continue
                byte[a] = (first[i], r["changes"][i], mn[i], mx[i])
    s["byte"] = byte
    s["path"] = p
    lo = HIDDEN_END + 1
    s["covers"] = [(max(r["start"], lo), r["start"] + r["n"] - 1) for r in s["ranges"]]
    return s


def cmd_report(args):
    sessions = [load_session(p) for p in args.sessions]
    ids = {s["ecu_id"] for s in sessions}
    allre = sorted(set().union(*[s["byte"].keys() for s in sessions]))

    if not allre:
        sys.exit("No bytes in these sessions.")
    verdict = {}
    for a in allre:
        recs = [s["byte"].get(a) for s in sessions]
        got = [r for r in recs if r]
        zname, nofill = zone_of(a)
        if any(r[1] for r in got):
            v = "BUSY"                                  # changed while we watched
        elif len({r[0] for r in got}) > 1:
            v = "SET-AT-START"                          # steady within a session, differs between sessions
        else:
            v = "STATIC"
        covering = sum(1 for s in sessions if any(c0 <= a <= c1 for c0, c1 in s["covers"]))
        if len(got) < covering:
            v += "?"                                    # a read failed in a session that covered it
        verdict[a] = v

    d = outdir(args, "reports")
    base = os.path.join(d, f"{stamp()}_report")
    with open(base + "_bytes.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["address", "zone", "verdict"] +
                   [h for s in sessions for h in (f"{s['label']}_value", f"{s['label']}_changes",
                                                   f"{s['label']}_min", f"{s['label']}_max")])
        for a in allre:
            row = [fmt(a), zone_of(a)[0], verdict[a]]
            for s in sessions:
                r = s["byte"].get(a)
                row += [f"{r[0]:02X}", r[1], f"{r[2]:02X}", f"{r[3]:02X}"] if r else ["", "", "", ""]
            w.writerow(row)

    def table(title, flag, min_len, explain):
        lines = [title, explain, f"  {'start':12s}{'end':12s}{'bytes':>7s}  {'values':16s}zone"]
        found = runs([(a, flag(a)) for a in allre], min_len)
        for s0, e0 in found:
            vals = [sessions[0]["byte"].get(a, (0,))[0] for a in range(s0, e0 + 1)]
            zs = sorted({zone_of(a)[0] or "-" for a in range(s0, e0 + 1)})
            lines.append(f"  {fmt(s0):12s}{fmt(e0):12s}{e0 - s0 + 1:7d}  {describe_values(vals):16s}{','.join(zs)}")
        if not found:
            lines.append("  (none)")
        return lines + [""]

    def usable(a):
        return verdict[a] == "STATIC" and not zone_of(a)[1]

    out = [f"ram_sniffer {VERSION} report  {time.strftime('%Y-%m-%d %H:%M')}", ""]
    if len(ids) > 1:
        out += [f"WARNING: sessions come from different ECU IDs: {', '.join(sorted(ids))}", ""]
    out.append("Sessions")
    for i, s in enumerate(sessions, 1):
        out.append(f"  {i}. {s['label']:14s} {s.get('method', 'a0 (v1.0)'):9s} {s['started']}  ECU {s['ecu_id']}  {s['passes']} passes  "
                   f"{s['seconds']:.0f} s  {len(s['byte'])} bytes  ({os.path.basename(s['path'])})")
    blind = sum(s["blind"] for s in sessions)
    if blind:
        out.append(f"  Left out {blind} byte reads of 0xFFFF0000-0xFFFF1FFF: this ECU returns FF there"
                   " for every SSM read, so they say nothing about use.")
    out.append("")
    counts = {}
    for v in verdict.values():
        counts[v] = counts.get(v, 0) + 1
    out.append("Verdicts  " + "  ".join(f"{k} {v}" for k, v in sorted(counts.items())))
    out += ["  BUSY          changed while being read",
            "  SET-AT-START  never changed during a session but had a different value in another",
            "                session (written at start-up, or rarely). Not safe.",
            "  STATIC        same value in every read of every session. Only a candidate: code that",
            "                writes the same value each start-up looks the same. Confirm with fill/check.",
            "  ?             not read in every session", ""]
    if len(sessions) < 2:
        out += ["NOTE: only one session. Do at least one more after a key off / key on so",
                "start-up writes show up as SET-AT-START.", ""]
    out += table(f"Candidate blocks (STATIC in every session, >= {args.min_run} bytes, outside protected zones)",
                 usable, args.min_run, "Fill and check these next.")
    out += table("Start-up / occasional writes (SET-AT-START runs >= 4 bytes)",
                 lambda a: verdict[a].startswith("SET-AT-START"), 4, "Avoid these.")
    out.append("By zone (bytes read: BUSY / SET-AT-START / STATIC)")
    for s0, e0, name, nofill, note in ZONES:
        zs = [verdict[a] for a in allre if s0 <= a <= e0]
        if zs:
            b = sum(v.startswith("BUSY") for v in zs); ss = sum(v.startswith("SET") for v in zs)
            out.append(f"  {name:10s} {fmt(s0)}-{fmt(e0)}  {len(zs):6d}: {b:5d} / {ss:5d} / {len(zs) - b - ss:5d}"
                       f"   {'[no fill] ' if nofill else ''}{note}")
    out += ["", "STACK zone: reading it is harmless. Its deep bytes look STATIC only because the stack",
            "has not grown that far yet, so it is never offered as a candidate."]
    with open(base + ".txt", "w") as f:
        f.write("\n".join(out) + "\n")
    print("\n".join(out))
    print(f"Saved {base}.txt and {base}_bytes.csv")


# ---------------------------------------------------------------- fill / check
def pattern_bytes(args, n):
    p = bytes.fromhex(args.pattern)
    return (p * (n // len(p) + 1))[:n]


def check_fill_allowed(a, b, force):
    bad = sorted({zone_of(x)[0] for x in range(a, b + 1) if zone_of(x)[1]})
    if bad:
        sys.exit(f"Refusing: {fmt(a)}-{fmt(b)} overlaps protected zone(s) {', '.join(bad)}.")
    if not force:
        warn = sorted({zone_of(x)[0] or "unmapped" for x in range(a, b + 1)} - {"CAND2", "CAND3"})
        if warn:
            print(f"Note: range touches {', '.join(warn)} (not a high/medium candidate).")


def cmd_fill(args):
    rs = ranges_from_args(args)
    for a, b in rs:
        check_fill_allowed(a, b, args.force)
    print("This WRITES to ECU RAM. Do it key on, engine off. A key off with the battery")
    print("connected keeps RAM; disconnecting the battery clears it. A backup of the")
    print("current contents is saved first.")
    for a, b in rs:
        print(f"  {fmt(a)}-{fmt(b)}  {b - a + 1} bytes  pattern {args.pattern.upper()}")
    if not args.yes and input("Type YES to write: ").strip() != "YES":
        sys.exit("Cancelled.")
    link, ecu, eid = connect(args)
    d = outdir(args, "fills")
    try:
        for a, b in rs:
            n = b - a + 1
            old, ok = ecu.read(a, n, args.block)
            if not all(ok):
                sys.exit(f"Could not read all of {fmt(a)}-{fmt(b)} for the backup; nothing written.")
            bk = os.path.join(d, f"{stamp()}_backup_{fmt(a)}_{fmt(b)}.bin")
            with open(bk, "wb") as f:
                f.write(old)
            print(f"Backup {bk}  ({describe_values(list(old))})")
            pat = pattern_bytes(args, n)
            failed = 0
            for off in range(0, n, args.wblock):
                chunk = pat[off:off + args.wblock]
                if not ecu.write_block(a + off, chunk) and not ecu.write_block(a + off, chunk):
                    failed += 1
            back, ok = ecu.read(a, n, args.block)
            bad = [i for i in range(n) if not ok[i] or back[i] != pat[i]]
            if failed or bad:
                print(f"  {fmt(a)}-{fmt(b)}: {failed} write blocks failed, {len(bad)} bytes don't read back "
                      f"(first {fmt(a + bad[0]) if bad else '-'}). Bytes the ECU rewrote within ~1 s are already in use.")
            else:
                print(f"  {fmt(a)}-{fmt(b)}: written and read back OK")
            meta = dict(tool="ram_sniffer", kind="fill", ecu_id=eid, start=a, end=b, pattern=args.pattern.upper(),
                        time=time.strftime("%Y-%m-%d %H:%M:%S"), backup=bk, readback_bad=[fmt(a + i) for i in bad[:200]])
            with open(os.path.join(d, f"fill_{fmt(a)}_{fmt(b)}.json"), "w") as f:
                json.dump(meta, f, indent=1)
    finally:
        link.close()
    print("Next: python3 ram_sniffer.py check --range ... --label <condition>  (cold start, idle, drive, A/C)")


def cmd_restore(args):
    a = args.at
    with open(args.file, "rb") as f:
        data = f.read()
    b = a + len(data) - 1
    check_fill_allowed(a, b, True)
    if not args.yes and input(f"Write {len(data)} bytes from {args.file} to {fmt(a)}? Type YES: ").strip() != "YES":
        sys.exit("Cancelled.")
    link, ecu, eid = connect(args)
    try:
        for off in range(0, len(data), args.wblock):
            ecu.write_block(a + off, data[off:off + args.wblock])
        back, ok = ecu.read(a, len(data), args.block)
        print("Restored OK" if bytes(back) == data else "Restore read-back differs (bytes in use?)")
    finally:
        link.close()


def cmd_check(args):
    rs = ranges_from_args(args)
    link, ecu, eid = connect(args)
    st = [dict(a=a, n=b - a + 1, pat=pattern_bytes(args, b - a + 1), broke=[None] * (b - a + 1)) for a, b in rs]
    t0 = time.time()
    passes = 0
    print(f"Checking for pattern {args.pattern.upper()}. Ctrl+C stops and saves.")
    try:
        while True:
            for r in st:
                data, ok = ecu.read(r["a"], r["n"], args.block)
                for i in range(r["n"]):
                    if ok[i] and data[i] != r["pat"][i] and r["broke"][i] is None:
                        r["broke"][i] = (round(time.time() - t0, 1), passes + 1, data[i])
            passes += 1
            nb = sum(1 for r in st for x in r["broke"] if x)
            print(f"pass {passes}  elapsed {time.time() - t0:6.0f} s  bytes overwritten {nb}", flush=True)
            if args.passes and passes >= args.passes:
                break
            if args.secs and time.time() - t0 >= args.secs:
                break
    except KeyboardInterrupt:
        print("\nStopped, saving.")
    finally:
        link.close()
    d = outdir(args, "checks")
    base = os.path.join(d, f"{stamp()}_check_{args.label}")
    with open(base + ".csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["address", "zone", "seconds", "pass", "new_value"])
        for r in st:
            for i, x in enumerate(r["broke"]):
                if x:
                    w.writerow([fmt(r["a"] + i), zone_of(r["a"] + i)[0], x[0], x[1], f"{x[2]:02X}"])
    out = [f"ram_sniffer {VERSION} check '{args.label}'  ECU {eid}  {passes} passes  {time.time() - t0:.0f} s  "
           f"pattern {args.pattern.upper()}", ""]
    for r in st:
        a, n = r["a"], r["n"]
        nb = sum(1 for x in r["broke"] if x)
        out.append(f"{fmt(a)}-{fmt(a + n - 1)}  {n} bytes  overwritten {nb}")
        for s0, e0 in runs([(a + i, r["broke"][i] is None) for i in range(n)], args.min_run):
            out.append(f"  intact  {fmt(s0)}-{fmt(e0)}  {e0 - s0 + 1} bytes")
        for s0, e0 in runs([(a + i, r["broke"][i] is not None) for i in range(n)], 1):
            x = r["broke"][s0 - a]
            out.append(f"  hit     {fmt(s0)}-{fmt(e0)}  {e0 - s0 + 1} bytes  first at {x[0]} s (pass {x[1]})")
    out += ["", "If a whole block reads 00 right after a key cycle, boot clears it (CAND2 is expected to)."]
    with open(base + ".txt", "w") as f:
        f.write("\n".join(out) + "\n")
    print("\n".join(out))
    print(f"Saved {base}.txt and {base}.csv")


def cmd_id(args):
    link, ecu, eid = connect(args)
    link.close()


# ---------------------------------------------------------------- main
def main(argv=None):
    p = argparse.ArgumentParser(description="SH7058 RAM usage mapper over SSM K-line (Tactrix OpenPort 2.0)")
    sub = p.add_subparsers(dest="cmd", required=True)

    def link_opts(q):
        q.add_argument("--port", default="/dev/ttyACM0", help="Tactrix port (R36S: usually /dev/ttyACM1)")
        q.add_argument("--block", type=int, default=64, help="bytes per SSM read (1-128, default 64)")
        q.add_argument("--out", default="ramsniff", help="output folder (default ./ramsniff)")
        q.add_argument("--verbose", action="store_true")
        q.add_argument("--method", choices=("a0", "a8"), default="a0",
                       help="a0 block reads (default) or a8 address-list reads")

    def range_opts(q, full=True):
        q.add_argument("--range", action="append", type=lambda t: parse_range(t, "--any-address" in sys.argv),
                       help="START-END, repeatable")
        q.add_argument("--any-address", action="store_true", help="allow addresses outside RAM (reads only)")
        if full:
            q.add_argument("--full", action="store_true", help="whole RAM 0xFFFF0000-0xFFFFBFFF")
        q.add_argument("--candidates", action="store_true", help="the RAM-map free-block candidates")

    q = sub.add_parser("id"); link_opts(q); q.set_defaults(fn=cmd_id)

    q = sub.add_parser("sweep"); link_opts(q); range_opts(q)
    q.add_argument("--passes", type=int, default=0)
    q.add_argument("--secs", type=float, default=0)
    q.add_argument("--label", default="sweep", help="condition name, e.g. keyon, idle, drive")
    q.set_defaults(fn=cmd_sweep)

    q = sub.add_parser("report"); q.add_argument("sessions", nargs="+")
    q.add_argument("--out", default="ramsniff")
    q.add_argument("--min-run", type=int, default=64, help="smallest candidate block to list (default 64)")
    q.set_defaults(fn=cmd_report)

    q = sub.add_parser("fill"); link_opts(q); range_opts(q, full=False)
    q.add_argument("--pattern", default="A5", help="hex byte(s), repeated (default A5)")
    q.add_argument("--wblock", type=int, default=32, help="bytes per SSM write (default 32)")
    q.add_argument("--force", action="store_true", help="skip the non-candidate note")
    q.add_argument("--yes", action="store_true", help="don't ask")
    q.set_defaults(fn=cmd_fill)

    q = sub.add_parser("restore", help="write a fill backup .bin back"); link_opts(q)
    q.add_argument("file"); q.add_argument("--at", type=parse_addr, required=True)
    q.add_argument("--wblock", type=int, default=32); q.add_argument("--yes", action="store_true")
    q.set_defaults(fn=cmd_restore)

    q = sub.add_parser("check"); link_opts(q); range_opts(q, full=False)
    q.add_argument("--pattern", default="A5")
    q.add_argument("--passes", type=int, default=0)
    q.add_argument("--secs", type=float, default=0)
    q.add_argument("--label", default="check")
    q.add_argument("--min-run", type=int, default=16)
    q.set_defaults(fn=cmd_check)

    args = p.parse_args(argv)
    if hasattr(args, "block") and not 1 <= args.block <= 128:
        p.error("--block must be 1-128")
    if args.cmd in ("sweep", "check") and not (args.passes or args.secs):
        args.passes = 3 if args.cmd == "sweep" else 0  # noqa
        if args.cmd == "check":
            args.passes = 5
    args.fn(args)


if __name__ == "__main__":
    main()
