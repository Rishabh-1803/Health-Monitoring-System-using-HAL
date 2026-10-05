#!/usr/bin/env python3
"""soak_test.py — integration + soak test for the ESP32 dashboard.

Stdlib only (socket + http.client + base64/hashlib/json/time): no pip
installs, no websocket-client. The WebSocket client below is a minimal
RFC 6455 implementation (~80 lines) — enough for one-way text frames
and pings, which is all the dashboard uses.

Usage:
    python tools/soak_test.py --host 192.168.1.42
    python tools/soak_test.py --host 192.168.1.42 --minutes 10 --csv soak.csv
    python tools/soak_test.py --host 192.168.1.42 --no-drill

Checks performed:
    1. /api/status is reachable and parses as JSON
    2. WebSocket /ws connects and pushes ~1 Hz status
    3. every status: link up, telemetry age < 5 s, finite values
    4. threshold drill: lower temp threshold -> alarm raises -> restore
       -> alarm clears -> alarm-reset POST acked
    5. /api/history returns monotonic timestamps and sane values
    6. (optional) --watchdog drill hint printed for manual testing

Exit code 0 = all passed, 1 = failures (see the report).
"""

import argparse
import base64
import hashlib
import http.client
import json
import os
import socket
import struct
import sys
import time

# ------------------------------------------------------------------ #
#  REST helper                                                        #
# ------------------------------------------------------------------ #


class Rest:
    def __init__(self, host, timeout=5.0):
        self.host = host
        self.timeout = timeout

    def get(self, path):
        conn = http.client.HTTPConnection(self.host, timeout=self.timeout)
        try:
            conn.request("GET", path)
            r = conn.getresponse()
            body = r.read()
            return r.status, body
        finally:
            conn.close()

    def get_json(self, path):
        status, body = self.get(path)
        if status != 200:
            raise IOError("GET %s -> HTTP %d" % (path, status))
        return json.loads(body.decode("utf-8", "replace"))

    def post_json(self, path, obj):
        conn = http.client.HTTPConnection(self.host, timeout=self.timeout)
        try:
            payload = json.dumps(obj)
            conn.request("POST", path, body=payload,
                         headers={"Content-Type": "application/json"})
            r = conn.getresponse()
            body = r.read()
            try:
                return r.status, json.loads(body.decode("utf-8", "replace"))
            except ValueError:
                return r.status, {"ok": False, "err": "bad json reply"}
        finally:
            conn.close()


# ------------------------------------------------------------------ #
#  Minimal RFC 6455 client (text frames + ping)                       #
# ------------------------------------------------------------------ #


class WsClient:
    def __init__(self, host, port=80, timeout=5.0):
        self.sock = socket.create_connection((host, port), timeout)
        self.buf = b""
        self._handshake(host, port)

    def _handshake(self, host, port):
        key = base64.b64encode(os.urandom(16)).decode()
        req = (
            "GET /ws HTTP/1.1\r\n"
            "Host: %s:%d\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: %s\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n" % (host, port, key)
        )
        self.sock.sendall(req.encode())

        # Read until end of the HTTP headers.
        while b"\r\n\r\n" not in self.buf:
            chunk = self.sock.recv(1024)
            if not chunk:
                raise IOError("ws: closed during handshake")
            self.buf += chunk
        head, self.buf = self.buf.split(b"\r\n\r\n", 1)
        lines = head.decode("latin1").split("\r\n")
        if "101" not in lines[0]:
            raise IOError("ws: handshake refused: " + lines[0])

        expect = base64.b64encode(
            hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")
                         .encode()).digest()).decode()
        accept = [l for l in lines if l.lower().startswith("sec-websocket-accept")]
        if not accept or expect not in accept[0]:
            raise IOError("ws: bad accept key")

    def _read_exact(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise IOError("ws: closed")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def recv_text(self, timeout):
        """Wait up to timeout seconds; return a text payload or None."""
        self.sock.settimeout(timeout)
        try:
            while True:
                if len(self.buf) < 2:
                    chunk = self.sock.recv(4096)
                    if not chunk:
                        raise IOError("ws: closed")
                    self.buf += chunk
                    continue
                b1, b2 = self.buf[0], self.buf[1]
                opcode = b1 & 0x0F
                masked = b2 & 0x80
                length = b2 & 0x7F
                off = 2
                if length == 126:
                    if len(self.buf) < 4:
                        self.buf += self.sock.recv(4096)
                        continue
                    length = struct.unpack(">H", self.buf[2:4])[0]
                    off = 4
                elif length == 127:
                    if len(self.buf) < 10:
                        self.buf += self.sock.recv(4096)
                        continue
                    length = struct.unpack(">Q", self.buf[2:10])[0]
                    off = 10
                if masked:
                    length_known = False  # server frames are unmasked
                frame_len = off + length
                if len(self.buf) < frame_len:
                    self.buf += self.sock.recv(65536)
                    continue

                payload = self.buf[off:frame_len]
                self.buf = self.buf[frame_len:]

                if opcode == 0x1:          # text
                    return payload.decode("utf-8", "replace")
                if opcode == 0x9:          # ping -> pong
                    self._send_frame(0xA, payload)
                    continue
                if opcode == 0x8:          # close
                    raise IOError("ws: server sent close")
                # binary/continuation: ignore
        except socket.timeout:
            return None

    def _send_frame(self, opcode, payload=b""):
        mask = os.urandom(4)
        header = bytes([0x80 | opcode])
        n = len(payload)
        if n < 126:
            header += bytes([0x80 | n])
        elif n < 65536:
            header += bytes([0x80 | 126]) + struct.pack(">H", n)
        else:
            header += bytes([0x80 | 127]) + struct.pack(">Q", n)
        masked = bytes(c ^ mask[i % 4] for i, c in enumerate(payload))
        self.sock.sendall(header + mask + masked)

    def close(self):
        try:
            self._send_frame(0x8, b"")
        except OSError:
            pass
        self.sock.close()


# ------------------------------------------------------------------ #
#  Test driver                                                        #
# ------------------------------------------------------------------ #

class Report:
    def __init__(self):
        self.items = []      # (name, ok, detail)

    def check(self, name, ok, detail=""):
        self.items.append((name, bool(ok), detail))
        print("  %-6s %-46s %s" % ("PASS" if ok else "FAIL", name, detail))

    @property
    def failures(self):
        return [i for i in self.items if not i[1]]

    def summary(self):
        print("\n== soak summary ====================================")
        print("  %d checks, %d failures" % (len(self.items),
                                            len(self.failures)))
        for name, _, detail in self.failures:
            print("  FAILED: %s  %s" % (name, detail))
        return len(self.failures) == 0


def finite(x):
    return isinstance(x, (int, float)) and x == x and abs(x) != float("inf")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True, help="ESP32 IP address")
    ap.add_argument("--minutes", type=float, default=2.0,
                    help="soak duration (default 2)")
    ap.add_argument("--csv", default=None, help="write samples to CSV")
    ap.add_argument("--no-drill", action="store_true",
                    help="skip the threshold alarm drill")
    args = ap.parse_args()

    rep = Report()
    rest = Rest(args.host)

    # -- 1. REST reachable ------------------------------------------ #
    print("[1] REST reachability")
    try:
        st = rest.get_json("/api/status")
        rep.check("GET /api/status", st.get("type") == "status")
    except Exception as e:
        rep.check("GET /api/status", False, str(e))
        rep.summary()
        return 1

    # -- 2..3. WebSocket soak ---------------------------------------- #
    print("[2] WebSocket soak (%.1f min)" % args.minutes)
    deadline = time.time() + args.minutes * 60
    msgs = 0
    bad_status = []
    csv_file = open(args.csv, "w", newline="") if args.csv else None
    if csv_file:
        csv_file.write("t,link,age_ms,temp,cur,vib,alm,cpu,heap\n")

    ws = None
    try:
        ws = WsClient(args.host)
        rep.check("ws connect + handshake", True)
    except Exception as e:
        rep.check("ws connect + handshake", False, str(e))

    alm_seen = None
    if ws:
        while time.time() < deadline:
            msg = ws.recv_text(min(3.0, deadline - time.time()))
            if msg is None:
                continue
            try:
                st = json.loads(msg)
            except ValueError:
                bad_status.append("unparsable")
                continue
            if st.get("type") != "status":
                continue
            msgs += 1
            if csv_file:
                csv_file.write("%s,%s,%s,%s,%s,%s,%s,%s,%s\n" % (
                    st.get("ts"), st.get("link"), st.get("age"),
                    st.get("temp"), st.get("cur"), st.get("vib"),
                    st.get("alm"), st.get("cpu"), st.get("heap")))
            problems = []
            if not st.get("link"):
                problems.append("link down")
            if not isinstance(st.get("age"), int) or st["age"] > 5000:
                problems.append("stale telemetry (%s)" % st.get("age"))
            for k in ("temp", "cur", "vib"):
                if not finite(st.get(k)):
                    problems.append("non-finite %s" % k)
            if problems:
                bad_status.append("; ".join(problems))
            alm_seen = st.get("alm")
        if csv_file:
            csv_file.flush()

        rate = msgs / max(args.minutes * 60, 1e-9)
        rep.check("push rate >= 0.8 Hz (was %.2f Hz)" % rate, rate >= 0.8)
        rep.check("all statuses sane (%d bad)" % len(bad_status),
                  len(bad_status) == 0,
                  bad_status[0] if bad_status else "")

    # -- 4. threshold alarm drill ------------------------------------ #
    if not args.no_drill:
        print("[3] threshold alarm drill")
        try:
            original = rest.get_json("/api/status")["th"]["t"]
        except Exception:
            original = 60.0

        # 4a. lower the threshold below ambient -> alarm must raise
        low_thr = 5.0
        code, r = rest.post_json("/api/threshold", {"id": 0, "value": low_thr})
        rep.check("POST threshold(%.1f) ok" % low_thr, r.get("ok") is True,
                  str(r))

        raised = False
        t0 = time.time()
        while time.time() - t0 < 20:
            st = rest.get_json("/api/status")
            if st.get("alm", 0) & 0x01:
                raised = True
                break
            time.sleep(0.5)
        rep.check("overtemp alarm raised within 20 s", raised,
                  "alm=0x%02x" % (st.get("alm", 0) if 'st' in dir() else 0))

        # 4b. restore -> alarm clears (hysteresis window)
        code, r = rest.post_json("/api/threshold",
                                 {"id": 0, "value": original})
        rep.check("POST threshold(%s) restore ok" % original,
                  r.get("ok") is True, str(r))

        cleared = False
        t0 = time.time()
        while time.time() - t0 < 20:
            st = rest.get_json("/api/status")
            if not (st.get("alm", 0) & 0x01):
                cleared = True
                break
            time.sleep(0.5)
        rep.check("overtemp alarm cleared within 20 s", cleared)

        # 4c. explicit reset (operator ack)
        code, r = rest.post_json("/api/alarm/reset", {})
        rep.check("POST alarm/reset ok", r.get("ok") is True, str(r))

    # -- 5. history endpoint ----------------------------------------- #
    print("[4] history endpoint")
    try:
        h = rest.get_json("/api/history?win=300")
        pts = h.get("pts", [])
        rep.check("history has points", len(pts) > 0, "%d pts" % len(pts))
        ts = [p[0] for p in pts]
        rep.check("timestamps monotonic", ts == sorted(ts))
        if pts:
            vals_ok = all(finite(p[1]) and finite(p[2]) and finite(p[3])
                          for p in pts)
            rep.check("values finite", vals_ok)
            span = (ts[-1] - ts[0]) / 1000.0
            rep.check("span plausible", span <= 305 and span >= 0,
                      "%.0f s" % span)
    except Exception as e:
        rep.check("history endpoint", False, str(e))

    # -- 6. hint for the manual watchdog drill ----------------------- #
    print("[5] manual drill hints")
    print("  - watchdog: power off the STM32; within ~60 s the logs show")
    print("    'supervisor' escalation, within ~120 s the ESP32 reboots")
    print("  - reconnect STM32 -> 'STM32 link recovered' in /api/logs")
    code = rest.get("/api/logs")[0]
    rep.check("GET /api/logs", code == 200, "HTTP %d" % code)

    if ws:
        ws.close()
    if csv_file:
        csv_file.close()
        print("\nCSV samples written to %s" % args.csv)

    ok = rep.summary()
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
