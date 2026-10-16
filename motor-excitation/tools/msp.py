"""Minimal MSP v1 client for Betaflight bench work."""
import struct
import time
import serial

MSP_API_VERSION = 1
MSP_FC_VARIANT = 2
MSP_FC_VERSION = 3
MSP_BOARD_INFO = 4
MSP_STATUS = 101
MSP_ANALOG = 110
MSP_MOTOR_CONFIG = 131
MSP_MOTOR_TELEMETRY = 139
MSP_SET_MOTOR = 214
MSP_MOTOR = 104
MSP_DEBUG = 254
MSP2_MOTOR_EXCITATION_STATUS = 0x3013
MSP2_SET_MOTOR_EXCITATION = 0x3014
MSP2_BLACKBOX_STREAM = 0x3015

EXCITATION_WAVEFORMS = {"none": 0, "sine": 1, "white": 2, "chirp": 3, "square": 4, "record": 5}
EXCITATION_STATES = ["idle", "ramp", "run", "ended"]
EXCITATION_ENDS = ["-", "done", "stop", "timeout", "armed", "battery"]


def crc8_dvb_s2(data, crc=0):
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0xD5) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


class MspError(Exception):
    pass


class Msp:
    def __init__(self, port, baud=115200, timeout=0.2):
        self.ser = serial.Serial(port, baud, timeout=timeout)
        time.sleep(0.1)
        self.ser.reset_input_buffer()

    def close(self):
        self.ser.close()

    def send(self, cmd, payload=b""):
        n = len(payload)
        chk = n ^ cmd
        for b in payload:
            chk ^= b
        self.ser.write(b"$M<" + bytes([n, cmd]) + payload + bytes([chk]))

    def recv(self, want_cmd, deadline=0.5):
        t0 = time.time()
        buf = b""
        while time.time() - t0 < deadline:
            b = self.ser.read(1)
            if not b:
                continue
            buf += b
            if len(buf) < 3:
                if buf != b"$M<"[: len(buf)] and buf != b"$M>"[: len(buf)] and buf != b"$M!"[: len(buf)]:
                    buf = b""
                continue
            if buf[:3] not in (b"$M>", b"$M!"):
                buf = buf[1:]
                continue
            hdr = self.ser.read(2)
            if len(hdr) < 2:
                return None
            n, cmd = hdr
            payload = self.ser.read(n)
            chk = self.ser.read(1)
            if len(payload) < n or not chk:
                return None
            c = n ^ cmd
            for x in payload:
                c ^= x
            if c != chk[0]:
                buf = b""
                continue
            if cmd == want_cmd:
                return None if buf[:3] == b"$M!" else payload
            buf = b""
        return None

    # --- MSP v2 ----------------------------------------------------------
    def send2(self, cmd, payload=b""):
        body = struct.pack("<BHH", 0, cmd, len(payload)) + payload
        self.ser.write(b"$X<" + body + bytes([crc8_dvb_s2(body)]))

    def recv2(self, want_cmd, deadline=0.5):
        """Returns payload, or None on timeout; raises MspError on an error reply ($X!)."""
        t0 = time.time()
        win = b""
        while time.time() - t0 < deadline:
            b = self.ser.read(1)
            if not b:
                continue
            win = (win + b)[-3:]
            if win not in (b"$X>", b"$X!"):
                continue
            hdr = self.ser.read(5)
            if len(hdr) < 5:
                return None
            flag, cmd, n = struct.unpack("<BHH", hdr)
            payload = self.ser.read(n)
            crc = self.ser.read(1)
            if len(payload) < n or not crc or crc8_dvb_s2(hdr + payload) != crc[0]:
                win = b""
                continue
            if cmd == want_cmd:
                if win == b"$X!":
                    raise MspError(cmd)
                return payload
            win = b""
        return None

    def query2(self, cmd, payload=b"", deadline=0.5):
        self.ser.reset_input_buffer()
        self.send2(cmd, payload)
        return self.recv2(cmd, deadline)

    # --- bench motor excitation (USE_MOTOR_EXCITATION firmware) -----------
    def excite_start(self, session, mask, waveform, base, amp=0, f0=0.0, f1=0.0, duration=10.0,
                     ramp=2.0, maxcmd=1500, timeout=1.0, seed=1, independent=False, invert_mask=0):
        """Raises MspError when the FC rejects the parameters (armed, out of range)."""
        wf = EXCITATION_WAVEFORMS[waveform] if isinstance(waveform, str) else waveform
        p = struct.pack("<BHBBHHHHHHHHI", 1, session, mask, wf, int(base), int(amp),
                        int(round(f0 * 10)), int(round(f1 * 10)), int(round(duration * 10)),
                        int(ramp * 1000), int(maxcmd), int(timeout * 1000), seed)
        p += struct.pack("<BB", 0x01 if independent else 0, invert_mask)
        self.drain()
        if self.query2(MSP2_SET_MOTOR_EXCITATION, p) is None:
            raise MspError("no reply to excitation start")
        # acks carry no session id: confirm through the status that this session is the one running
        st = self.excite_status()
        if not st or st["session"] != session or st["state"] not in ("ramp", "run"):
            raise MspError(f"excitation start not confirmed: {st}")

    def excite_keepalive(self, session):
        self.send2(MSP2_SET_MOTOR_EXCITATION, struct.pack("<BH", 2, session))

    def excite_mark(self, session, marker):
        """Set the session marker (RECORD sessions); also acts as a keepalive."""
        self.send2(MSP2_SET_MOTOR_EXCITATION, struct.pack("<BHH", 3, session, marker))

    def excite_stop(self):
        for _ in range(3):   # retry until acknowledged (noisy / slow link)
            try:
                if self.query2(MSP2_SET_MOTOR_EXCITATION, b"\x00") is not None:
                    break
            except MspError:
                break
        self.drain()

    def drain(self, quiet=0.03):
        """Discard late replies (keepalive acks) until the line is quiet."""
        t_end = time.time() + 0.5
        self.ser.reset_input_buffer()
        while time.time() < t_end:
            time.sleep(quiet)
            if not self.ser.in_waiting:
                break
            self.ser.reset_input_buffer()

    def bb_stream_read(self, max_bytes=1024):   # replies must fit the 2 KiB VCP IN buffer or the FC blocks
        """One chunk of the MSP blackbox stream: (offset, dropped, data) or None. Does not flush the input,
        so keepalive acks in flight are skipped by recv2."""
        self.send2(MSP2_BLACKBOX_STREAM, struct.pack("<H", max_bytes))
        p = self.recv2(MSP2_BLACKBOX_STREAM, deadline=0.3)
        if not p or len(p) < 10:
            return None
        off, dropped, n = struct.unpack_from("<IIH", p, 0)
        return off, dropped, p[10:10 + n]

    def bb_stream_read_n(self, n=2, max_bytes=1024):
        """n requests in flight, replies come back in order. Returns a list of (offset, dropped, data)."""
        for _ in range(n):
            self.send2(MSP2_BLACKBOX_STREAM, struct.pack("<H", max_bytes))
        out = []
        for _ in range(n):
            p = self.recv2(MSP2_BLACKBOX_STREAM, deadline=0.3)
            if p and len(p) >= 10:
                off, dropped, k = struct.unpack_from("<IIH", p, 0)
                out.append((off, dropped, p[10:10 + k]))
        return out

    def excite_status(self):
        p = self.query2(MSP2_MOTOR_EXCITATION_STATUS)
        if not p or len(p) < 27:
            return None
        (ver, state, end, session, mask, wf, elapsed, freq, sample, rms, loops, clipped,
         rate) = struct.unpack_from("<BBBHBBIHhHIIH", p, 0)
        marker = struct.unpack_from("<H", p, 27)[0] if len(p) >= 29 else None
        return {"marker": marker, "state": EXCITATION_STATES[state] if state < 4 else state,
                "end": EXCITATION_ENDS[end] if end < len(EXCITATION_ENDS) else end,
                "session": session, "mask": mask, "waveform": wf, "elapsed_s": elapsed / 1000.0,
                "freq_hz": freq / 10.0, "sample": sample, "rms": rms / 10.0, "loops": loops,
                "clipped_pct": 100.0 * clipped / loops if loops else 0.0, "rate_hz": rate}

    def query(self, cmd, payload=b""):
        self.ser.reset_input_buffer()
        self.send(cmd, payload)
        return self.recv(cmd)

    # --- decoded helpers -------------------------------------------------
    def api_version(self):
        p = self.query(MSP_API_VERSION)
        return (p[1], p[2]) if p else None

    def fc_variant(self):
        p = self.query(MSP_FC_VARIANT)
        return p.decode(errors="replace") if p else None

    def fc_version(self):
        p = self.query(MSP_FC_VERSION)
        return tuple(p[:3]) if p else None

    def board_info(self):
        p = self.query(MSP_BOARD_INFO)
        if not p:
            return None
        ident = p[:4].decode(errors="replace")
        # 4: hw rev u16, 6: fc type u8, 7: capabilities u8, 8: target name len
        tn = p[8]
        target = p[9 : 9 + tn].decode(errors="replace")
        off = 9 + tn
        bn = p[off]
        board = p[off + 1 : off + 1 + bn].decode(errors="replace")
        return ident, target, board

    def motor_config(self):
        p = self.query(MSP_MOTOR_CONFIG)
        if not p:
            return None
        minthr, maxthr, mincmd = struct.unpack_from("<HHH", p, 0)
        d = {"maxthrottle": maxthr, "mincommand": mincmd}
        if len(p) >= 10:
            d["motor_count"] = p[6]
            d["poles"] = p[7]
            d["dshot_telemetry"] = p[8]
            d["esc_sensor"] = p[9]
        return d

    def analog(self):
        p = self.query(MSP_ANALOG)
        if not p:
            return None
        vbat8, mah, rssi, amps = struct.unpack_from("<BHHh", p, 0)
        vbat = struct.unpack_from("<H", p, 7)[0] / 100.0 if len(p) >= 9 else vbat8 / 10.0
        return {"vbat": vbat, "mah": mah, "amps": amps / 100.0}

    def motor_telemetry(self):
        p = self.query(MSP_MOTOR_TELEMETRY)
        if not p:
            return None
        n = p[0]
        out = []
        off = 1
        for _ in range(n):
            rpm, inv, temp, volt, cur, cons = struct.unpack_from("<IHBHHH", p, off)
            off += 13
            out.append({"rpm": rpm, "invalid_pct": inv / 100.0, "temp": temp,
                        "volt": volt / 100.0, "amps": cur / 100.0, "mah": cons})
        return out

    def motors(self):
        p = self.query(MSP_MOTOR)
        return list(struct.unpack("<8H", p[:16])) if p else None

    def set_motors(self, values, bench=None):
        """values: motor commands (1000..2000). bench: optional (amp, hz, mode, maxcmd) for the
        USE_BENCH_NOISE firmware; it goes into the four slots after the real motors."""
        vals = list(values) + [1000] * (4 - len(values))
        vals = vals[:4] + (list(bench) if bench else [0, 0, 0, 0])
        self.send(MSP_SET_MOTOR, struct.pack("<8H", *vals))
        # drain ack without blocking long
        self.recv(MSP_SET_MOTOR, deadline=0.05)

    def debug(self):
        p = self.query(MSP_DEBUG)
        if not p or len(p) < 8:
            return None
        n = min(len(p) // 2, 8)   # 4 int16 on old firmware, 8 on current
        return list(struct.unpack("<%dh" % n, p[: 2 * n]))
