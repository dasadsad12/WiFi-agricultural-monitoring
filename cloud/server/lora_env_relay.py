#!/usr/bin/env python3
"""Dedicated relay for the LORA智慧农业 project.

This partition intentionally uses a different protocol and data directory from
the completed V3/V4 projects.  It accepts only the agriculture telemetry
contract and never interprets legacy flame/MQ-4 fields.
"""

import argparse
import hmac
import json
import math
import os
import re
import threading
import time
from collections import defaultdict, deque
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
from socketserver import ThreadingMixIn
from urllib.parse import parse_qs, urlparse


PARTITION = "wifi-env-agri"
SUPPORTED_PROTOCOLS = frozenset(("4",))
DEFAULT_PROTOCOL_VERSION = "4"
DEVICE_ID_RE = re.compile(r"^[A-Za-z0-9_-]{1,40}$")
FIELD_RE = re.compile(r"^[A-Za-z][A-Za-z0-9]*$")
MAX_DATA_BYTES = 768
HISTORY_SIZE = 300
MAX_COMMANDS = 8
COMMAND_TTL_MS = 30000
SSE_HEARTBEAT_SECONDS = 2.0
DEFAULT_ONLINE_TIMEOUT_MS = 5000


def epoch_ms():
    return int(time.time() * 1000)


def parse_fields(payload):
    if not payload or len(payload) > MAX_DATA_BYTES:
        raise ValueError("invalid data length")
    fields = {}
    for item in payload.split(";"):
        if "=" not in item:
            raise ValueError("invalid field")
        key, value = item.split("=", 1)
        if not FIELD_RE.fullmatch(key) or not value or len(value) > 80:
            raise ValueError("invalid field")
        fields[key] = value
    if fields.get("v") not in SUPPORTED_PROTOCOLS:
        raise ValueError("unsupported protocol")
    return fields


def bounded_int(fields, key, low, high):
    try:
        value = int(fields[key], 10)
    except (KeyError, ValueError):
        raise ValueError("invalid {}".format(key))
    if value < low or value > high:
        raise ValueError("{} out of range".format(key))
    return value


def bounded_float(fields, key, low, high):
    try:
        value = float(fields[key])
    except (KeyError, ValueError):
        raise ValueError("invalid {}".format(key))
    if not math.isfinite(value) or value < low or value > high:
        raise ValueError("{} out of range".format(key))
    return round(value, 3)


def sensor_int(fields, key, low, high, valid_key, invalid_value):
    """Parse a sensor value without rejecting a documented invalid sentinel.

    Older firmware revisions used -32768/65535 for an unavailable SHT30
    sample.  The validity bit is the source of truth, so accept that pair
    only when the corresponding sensor is marked invalid and normalize it to
    zero for the JSON/API contract.  This keeps one bad optional sensor from
    dropping the complete telemetry packet with HTTP 400.
    """
    try:
        value = int(fields[key], 10)
    except (KeyError, ValueError):
        raise ValueError("invalid {}".format(key))
    if value == invalid_value and fields.get(valid_key) == "0":
        return 0
    if value < low or value > high:
        raise ValueError("{} out of range".format(key))
    return value


def parse_telemetry(did, payload):
    fields = parse_fields(payload)
    protocol = fields["v"]
    record = {
        "did": did,
        "receivedAt": epoch_ms(),
        "hostSeq": bounded_int(fields, "hostSeq", 0, 4294967295),
        "slave": bounded_int(fields, "slave", 1, 2),
        "seq": bounded_int(fields, "seq", 0, 9999),
        "waterRaw": bounded_int(fields, "waterRaw", 0, 65535),
        "waterPct": bounded_int(fields, "waterPct", 0, 100),
        "waterValid": bounded_int(fields, "waterValid", 0, 1),
        "soilRaw": bounded_int(fields, "soilRaw", 0, 65535),
        "soilPct": bounded_int(fields, "soilPct", 0, 100),
        "soilValid": bounded_int(fields, "soilValid", 0, 1),
        "co2Ppm": bounded_int(fields, "co2Ppm", 0, 50000),
        "co2Valid": bounded_int(fields, "co2Valid", 0, 1),
        "lux": bounded_int(fields, "lux", 0, 65535),
        "sht30Valid": bounded_int(fields, "sht30Valid", 0, 1),
        "gpsValid": bounded_int(fields, "gpsValid", 0, 1),
        "latX10000": bounded_int(fields, "latX10000", -900000, 900000),
        "lonX10000": bounded_int(fields, "lonX10000", -1800000, 1800000),
        "alarmEnabled": bounded_int(fields, "alarmEnabled", 0, 1),
        "waterThreshold": bounded_int(fields, "waterThreshold", 0, 100),
        "co2Threshold": bounded_int(fields, "co2Threshold", 0, 50000),
        "soilThreshold": bounded_int(fields, "soilThreshold", 0, 100),
        "tempThresholdX10": bounded_int(fields, "tempThresholdX10", -400, 1250),
        "humidityThresholdX10": bounded_int(fields, "humidityThresholdX10", 0, 1000),
        "lightAlarmMode": bounded_int(fields, "lightAlarmMode", 0, 2),
        "waterAlarm": bounded_int(fields, "waterAlarm", 0, 1),
        "co2Alarm": bounded_int(fields, "co2Alarm", 0, 1),
        "soilAlarm": bounded_int(fields, "soilAlarm", 0, 1),
        "tempAlarm": bounded_int(fields, "tempAlarm", 0, 1),
        "humidityAlarm": bounded_int(fields, "humidityAlarm", 0, 1),
        "lightAlarm": bounded_int(fields, "lightAlarm", 0, 1),
        "pumpState": bounded_int(fields, "pumpState", 0, 1),
        "lightState": bounded_int(fields, "lightState", 0, 1),
        "fanState": bounded_int(fields, "fanState", 0, 1),
        "alarmTest": bounded_int(fields, "alarmTest", 0, 1),
    }
    # Keep these after sht30Valid has been parsed so the invalid sentinel rule
    # can be checked against the validity bit.
    record["tempX10"] = sensor_int(
        fields, "tempX10", -4000, 1250, "sht30Valid", -32768
    )
    record["humidityX10"] = sensor_int(
        fields, "humidityX10", 0, 1000, "sht30Valid", 65535
    )
    record["protocol"] = protocol
    if record["lux"] == 65535:
        record["luxAvailable"] = False
    else:
        record["luxAvailable"] = True
    record["raw"] = payload
    return record


class RelayStore:
    def __init__(self, data_dir, online_timeout_ms=DEFAULT_ONLINE_TIMEOUT_MS):
        self._condition = threading.Condition(threading.RLock())
        self._latest = {}
        self._history = defaultdict(lambda: deque(maxlen=HISTORY_SIZE))
        # Keep independent FIFO queues for each slave. The device endpoint
        # still returns one command at a time, selecting the oldest head.
        self._commands = defaultdict(lambda: defaultdict(deque))
        self._acks = {}
        self._ack_history = {}
        # The slave sequence identifies a sensor sample, while hostSeq
        # identifies an upload attempt. A host may legitimately upload the
        # same sample more than once while the ESP8266 retries HTTP.
        self._last_host_seq = {}
        self._last_sample_seq = {}
        self._protocols = {}
        self._revision = 0
        self._next_command_id = max(1, int(time.time()))
        self.online_timeout_ms = online_timeout_ms
        self.data_dir = Path(data_dir)
        self.data_dir.mkdir(parents=True, exist_ok=True)
        self.telemetry_log = self.data_dir / "telemetry.jsonl"
        self._last_logged_at = {}
        self._load_latest_telemetry()

    def _load_latest_telemetry(self):
        """Restore the last valid device state after a service restart."""
        history_keys = defaultdict(set)
        try:
            with self.telemetry_log.open("r", encoding="utf-8") as stream:
                for line in stream:
                    try:
                        record = json.loads(line)
                        did = record.get("did")
                        slave = int(record.get("slave", 0))
                        if not DEVICE_ID_RE.fullmatch(did or "") or slave not in (1, 2):
                            continue
                        if not isinstance(record.get("raw"), str):
                            continue
                        self._latest[(did, slave)] = record
                        self._last_host_seq[(did, slave)] = int(record.get("hostSeq", 0))
                        self._last_sample_seq[(did, slave)] = int(record.get("seq", 0))
                        # The log is the durable source for the report page as
                        # well as for the latest cards.  Rebuild the bounded
                        # in-memory history here so a service restart does
                        # not make the charts and statistics appear empty.
                        history_key = (
                            slave,
                            int(record.get("hostSeq", 0)),
                            int(record.get("seq", 0)),
                        )
                        if history_key not in history_keys[did]:
                            self._history[did].append(record.copy())
                            history_keys[did].add(history_key)
                        protocol = str(record.get("protocol", ""))
                        if protocol not in SUPPORTED_PROTOCOLS:
                            raw_version = re.search(r"(?:^|;)v=([0-9]+)(?:;|$)", record["raw"])
                            protocol = raw_version.group(1) if raw_version else ""
                        if protocol in SUPPORTED_PROTOCOLS:
                            self._protocols[(did, slave)] = protocol
                        received_at = int(record.get("receivedAt", 0))
                        if received_at > 0:
                            self._last_logged_at[(did, slave)] = received_at
                    except (ValueError, TypeError, json.JSONDecodeError):
                        continue
        except OSError:
            return

    def _changed(self):
        self._revision += 1
        self._condition.notify_all()

    def _append_log(self, record):
        try:
            if self.telemetry_log.exists() and self.telemetry_log.stat().st_size > 5 * 1024 * 1024:
                rotated = self.data_dir / "telemetry.jsonl.1"
                if rotated.exists():
                    rotated.unlink()
                self.telemetry_log.replace(rotated)
            with self.telemetry_log.open("a", encoding="utf-8") as stream:
                stream.write(json.dumps(record, ensure_ascii=True,
                                        separators=(",", ":")) + "\n")
        except OSError:
            pass

    def set_telemetry(self, did, payload):
        record = parse_telemetry(did, payload)
        with self._condition:
            sequence_key = (did, record["slave"])
            self._protocols[sequence_key] = record["protocol"]
            host_seq = record["hostSeq"]
            last_host_seq = self._last_host_seq.get(sequence_key)
            if last_host_seq == host_seq:
                # A retransmission of the same upload is already applied.
                # Refresh its receive time so a temporary HTTP retry does not
                # make an otherwise live device appear offline.
                previous = self._latest.get((did, record["slave"]))
                if previous is not None and previous["raw"] == payload:
                    previous = previous.copy()
                    previous["receivedAt"] = record["receivedAt"]
                    self._latest[(did, record["slave"])] = previous
                    self._changed()
                    return previous.copy()
            self._last_host_seq[sequence_key] = host_seq
            self._latest[(did, record["slave"])] = record
            is_new_sample = self._last_sample_seq.get(sequence_key) != record["seq"]
            self._last_sample_seq[sequence_key] = record["seq"]
            if is_new_sample:
                self._history[did].append(record.copy())
            last_logged = self._last_logged_at.get((did, record["slave"]), 0)
            should_log = record["receivedAt"] - last_logged >= 10000
            if should_log:
                self._last_logged_at[(did, record["slave"])] = record["receivedAt"]
            self._changed()
        if should_log:
            self._append_log(record)
        return record

    @staticmethod
    def _command_value(op, value):
        if op in ("ping", "snapshot"):
            return None
        if op in ("alarm_test", "relay_test"):
            try:
                parsed = int(value or "1")
            except (TypeError, ValueError):
                raise ValueError("invalid alarm test value")
            if parsed != 1:
                raise ValueError("alarm test value must be 1")
            return "1"
        if op in ("pump_on", "light_on", "fan_on") and (
                value is None or value == ""):
            return "1"
        if op in ("pump_off", "light_off", "fan_off") and (
                value is None or value == ""):
            return "0"
        try:
            parsed = int(value or "", 10)
        except ValueError:
            raise ValueError("invalid command value")
        if op == "set_alarm_enabled" and parsed not in (0, 1):
            raise ValueError("alarm value out of range")
        if op == "set_water_threshold" and not 0 <= parsed <= 100:
            raise ValueError("water threshold out of range")
        if op == "set_co2_threshold" and not 0 <= parsed <= 50000:
            raise ValueError("CO2 threshold out of range")
        if op == "set_soil_threshold" and not 0 <= parsed <= 100:
            raise ValueError("soil threshold out of range")
        if op == "set_temperature_threshold" and not -400 <= parsed <= 1250:
            raise ValueError("temperature threshold out of range")
        if op == "set_humidity_threshold" and not 0 <= parsed <= 1000:
            raise ValueError("humidity threshold out of range")
        if op == "set_light_alarm_mode" and parsed not in (0, 1, 2):
            raise ValueError("light alarm mode out of range")
        if op in ("pump_on", "pump_off", "light_on", "light_off",
                  "fan_on", "fan_off") and parsed not in (0, 1):
            raise ValueError("relay value out of range")
        return str(parsed)

    def queue_command(self, did, slave, op, value, protocol=None):
        if slave not in (1, 2):
            raise ValueError("invalid slave")
        if op not in ("ping", "snapshot", "alarm_test", "relay_test",
                      "pump_on", "pump_off", "light_on", "light_off",
                      "fan_on", "fan_off", "set_alarm_enabled",
                      "set_water_threshold", "set_co2_threshold",
                      "set_soil_threshold", "set_temperature_threshold",
                      "set_humidity_threshold", "set_light_alarm_mode"):
            raise ValueError("unsupported command")
        normalized = self._command_value(op, value)
        with self._condition:
            protocol = protocol or self._protocols.get((did, slave), DEFAULT_PROTOCOL_VERSION)
            if protocol not in SUPPORTED_PROTOCOLS:
                raise ValueError("unsupported protocol")
            queue = self._commands[did][slave]
            if len(queue) >= MAX_COMMANDS:
                raise ValueError("command queue full")
            self._next_command_id += 1
            command_id = self._next_command_id
            parts = ["v={}".format(protocol), "id={}".format(command_id),
                     "slave={}".format(slave), "op={}".format(op)]
            if normalized is not None:
                parts.append("value={}".format(normalized))
            parts.append("ttl=30")
            command = {
                "id": command_id,
                "slave": slave,
                "op": op,
                "value": normalized,
                "protocol": protocol,
                "data": ";".join(parts),
                "createdAt": epoch_ms(),
            }
            queue.append(command)
            self._changed()
            return command.copy()

    def _expire_commands(self, did, slave=None):
        queues = (self._commands[did][slave],) if slave in (1, 2) else (
            self._commands[did][1], self._commands[did][2])
        changed = False
        now = epoch_ms()
        for queue in queues:
            while queue and now - queue[0]["createdAt"] > COMMAND_TTL_MS:
                expired = queue.popleft().copy()
                expired["result"] = "expired"
                expired["receivedAt"] = now
                expired["protocol"] = expired.get("protocol", DEFAULT_PROTOCOL_VERSION)
                self._acks[did] = expired
                self._ack_history[(did, expired["id"], expired["slave"])] = expired.copy()
                changed = True
        if changed:
            self._changed()

    def get_command(self, did):
        with self._condition:
            self._expire_commands(did)
            candidates = [self._commands[did][slave][0]
                          for slave in (1, 2)
                          if self._commands[did][slave]]
            if not candidates:
                return {"id": 0, "data": ""}
            return min(candidates, key=lambda item: (item["createdAt"], item["id"])).copy()

    def set_ack(self, did, payload):
        fields = parse_fields(payload)
        protocol = fields["v"]
        command_id = bounded_int(fields, "id", 1, 4294967295)
        slave = bounded_int(fields, "slave", 1, 2)
        result = fields.get("result", "")
        if result not in ("ok", "invalid", "unsupported", "failed"):
            raise ValueError("invalid result")
        ack = {
            "id": command_id,
            "slave": slave,
            "result": result,
            "receivedAt": epoch_ms(),
            "raw": payload,
        }
        with self._condition:
            self._expire_commands(did, slave)
            saved = self._ack_history.get((did, command_id, slave))
            if saved is not None:
                if saved.get("protocol", DEFAULT_PROTOCOL_VERSION) != protocol:
                    raise ValueError("ack protocol does not match command")
                return saved.copy()
            queue = self._commands[did][slave]
            if queue and queue[0].get("protocol", DEFAULT_PROTOCOL_VERSION) != protocol:
                raise ValueError("ack protocol does not match command")
            if not queue or queue[0]["id"] != command_id:
                raise ValueError("ack does not match pending command")
            command = queue.popleft()
            ack["op"] = command["op"]
            ack["value"] = command["value"]
            ack["protocol"] = protocol
            self._ack_history[(did, command_id, slave)] = ack.copy()
            self._acks[did] = ack
            self._changed()
        return ack.copy()

    def _slave_snapshot(self, did, slave):
        telemetry = self._latest.get((did, slave))
        age_ms = None if telemetry is None else max(0, epoch_ms() - telemetry["receivedAt"])
        online = age_ms is not None and age_ms <= self.online_timeout_ms
        return {
            "slave": slave,
            "online": online,
            "ageMs": age_ms,
            "telemetry": None if telemetry is None else telemetry.copy(),
        }

    def snapshot(self, did, include_history=False):
        with self._condition:
            self._expire_commands(did)
            # WiFi 版是单设备直连云端，保留字段名 slaves 仅为兼容前端协议，
            # 对外只返回实际接入的设备 1，不伪造第二个节点。
            slaves = [self._slave_snapshot(did, 1)]
            pending = []
            for slave in (1,):
                pending.extend(item.copy() for item in self._commands[did][slave])
            pending.sort(key=lambda item: (item["createdAt"], item["id"]))
            result = {
                "ok": True,
                "partition": PARTITION,
                "protocol": "/".join(sorted(SUPPORTED_PROTOCOLS)),
                "protocols": sorted(SUPPORTED_PROTOCOLS),
                "did": did,
                "online": any(item["online"] for item in slaves),
                "slaves": slaves,
                "pendingCommands": pending,
                "ack": None if did not in self._acks else self._acks[did].copy(),
                "serverTime": epoch_ms(),
                "revision": self._revision,
            }
            if include_history:
                result["history"] = list(self._history[did])
            return result

    def wait_for_change(self, did, revision, timeout):
        with self._condition:
            if self._revision <= revision:
                self._condition.wait(timeout)
            return self.snapshot(did)


class RelayServer(ThreadingMixIn, HTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, address, handler, store, device_token, admin_token,
                 device_id, web_root):
        super().__init__(address, handler)
        self.store = store
        self.device_token = device_token
        self.admin_token = admin_token
        self.device_id = device_id
        self.web_root = Path(web_root)


class RelayHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "LoraEnvRelay/1"

    def log_message(self, fmt, *args):
        message = fmt % args
        if '"GET /lora/device/telemetry?' in message and " 200 -" in message:
            return
        message = re.sub(r"(token=)[^&\s]+", r"\1[redacted]", message)
        print("{} - {}".format(self.address_string(), message), flush=True)

    def _query(self):
        parsed = urlparse(self.path)
        values = parse_qs(parsed.query, keep_blank_values=True)
        return parsed.path, {key: items[-1] for key, items in values.items()}

    def _json(self, status, payload):
        body = (json.dumps(payload, ensure_ascii=False,
                           separators=(",", ":")) + "\n").encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "keep-alive")
        self.send_header("Keep-Alive", "timeout=15, max=100")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)
        self.wfile.flush()

    def _error(self, status, message):
        self._json(status, {"ok": False, "partition": PARTITION,
                            "message": message})

    @staticmethod
    def _valid_did(query):
        did = query.get("did", "")
        if not DEVICE_ID_RE.fullmatch(did):
            raise ValueError("invalid device id")
        return did

    def _device_auth(self, query, did):
        return (bool(self.server.device_token) and
                hmac.compare_digest(query.get("token", ""), self.server.device_token) and
                hmac.compare_digest(did, self.server.device_id))

    def _admin_auth(self, query):
        return (bool(self.server.admin_token) and
                hmac.compare_digest(query.get("token", ""), self.server.admin_token))

    def _serve_asset(self, path):
        assets = {
            "/": ("index.html", "text/html; charset=utf-8"),
            "/index.html": ("index.html", "text/html; charset=utf-8"),
            "/app.js": ("app.js", "text/javascript; charset=utf-8"),
            "/styles.css": ("styles.css", "text/css; charset=utf-8"),
        }
        if path not in assets:
            return False
        name, content_type = assets[path]
        try:
            body = (self.server.web_root / name).read_bytes()
        except OSError:
            self._error(404, "web asset not found")
            return True
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "keep-alive")
        self.send_header("Keep-Alive", "timeout=15, max=100")
        self.end_headers()
        self.wfile.write(body)
        self.wfile.flush()
        return True

    def _events(self, did, query):
        try:
            revision = int(query.get("cursor", "0"), 10)
        except ValueError:
            revision = 0
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "keep-alive")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("X-Accel-Buffering", "no")
        self.end_headers()
        try:
            self.wfile.write(b"retry: 1500\n\n")
            self.wfile.flush()
            while True:
                snapshot = self.server.store.wait_for_change(
                    did, revision, SSE_HEARTBEAT_SECONDS)
                if snapshot["revision"] != revision:
                    revision = snapshot["revision"]
                    data = json.dumps(snapshot, ensure_ascii=False,
                                      separators=(",", ":"))
                    event = "id: {}\nevent: meter\ndata: {}\n\n".format(revision, data)
                    self.wfile.write(event.encode("utf-8"))
                else:
                    self.wfile.write(b": heartbeat\n\n")
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass
        self.close_connection = True

    def do_GET(self):
        path, query = self._query()
        try:
            if self._serve_asset(path):
                return
            if path == "/health":
                self._json(200, {"ok": True, "partition": PARTITION,
                                 "protocol": "/".join(sorted(SUPPORTED_PROTOCOLS)),
                                 "protocols": sorted(SUPPORTED_PROTOCOLS),
                                 "port": self.server.server_port})
                return
            if path.startswith("/lora/device/"):
                did = self._valid_did(query)
                if not self._device_auth(query, did):
                    self._error(401, "unauthorized device")
                    return
                if path == "/lora/device/telemetry":
                    record = self.server.store.set_telemetry(did, query.get("data", ""))
                    self._json(200, {"ok": True, "slave": record["slave"],
                                     "seq": record["seq"]})
                elif path == "/lora/device/command":
                    command = self.server.store.get_command(did)
                    self._json(200, {"ok": True, "id": command.get("id", 0),
                                     "data": command.get("data", "")})
                elif path == "/lora/device/ack":
                    ack = self.server.store.set_ack(did, query.get("data", ""))
                    self._json(200, {"ok": True, "id": ack["id"],
                                     "result": ack["result"]})
                else:
                    self._error(404, "unknown device endpoint")
                return
            if path.startswith("/lora/api/") or path == "/lora/events":
                if not self._admin_auth(query):
                    self._error(401, "unauthorized admin")
                    return
                did = self._valid_did(query)
                if path == "/lora/api/snapshot":
                    self._json(200, self.server.store.snapshot(did))
                elif path == "/lora/api/history":
                    self._json(200, self.server.store.snapshot(did, include_history=True))
                elif path == "/lora/api/command":
                    try:
                        slave = int(query.get("slave", "0"), 10)
                    except ValueError:
                        slave = 0
                    command = self.server.store.queue_command(
                        did, slave, query.get("op", ""), query.get("value"))
                    self._json(200, {"ok": True, "command": command})
                elif path == "/lora/events":
                    self._events(did, query)
                else:
                    self._error(404, "unknown admin endpoint")
                return
            self._error(404, "not found")
        except ValueError as exc:
            self._error(400, str(exc))
        except Exception as exc:
            self._error(500, "internal error: {}".format(type(exc).__name__))


def create_server(host, port, device_token, admin_token, device_id,
                  data_dir, web_root, online_timeout_ms):
    if not device_token or not admin_token or device_token == admin_token:
        raise ValueError("distinct device and admin tokens are required")
    if not device_id or not DEVICE_ID_RE.fullmatch(device_id):
        raise ValueError("a valid device id is required")
    store = RelayStore(data_dir, online_timeout_ms)
    return RelayServer((host, port), RelayHandler, store, device_token,
                       admin_token, device_id, web_root)


def main():
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description="LORA agriculture relay")
    parser.add_argument("--host", default=os.getenv("WIFI_AGRI_HOST", "0.0.0.0"))
    parser.add_argument("--port", type=int,
                        default=int(os.getenv("WIFI_AGRI_PORT", "18127")))
    parser.add_argument("--device-token",
                        default=os.getenv("WIFI_AGRI_DEVICE_TOKEN", ""))
    parser.add_argument("--admin-token",
                        default=os.getenv("WIFI_AGRI_ADMIN_TOKEN", ""))
    parser.add_argument("--device-id",
                        default=os.getenv("WIFI_AGRI_DEVICE_ID", "WIFI_AGRI_001"))
    parser.add_argument("--data-dir", default=os.getenv(
        "WIFI_AGRI_DATA_DIR", str(root / "data")))
    parser.add_argument("--web-root", default=os.getenv(
        "WIFI_AGRI_WEB_ROOT", str(root / "web")))
    parser.add_argument("--online-timeout-ms", type=int, default=int(os.getenv(
        "WIFI_AGRI_ONLINE_TIMEOUT_MS", str(DEFAULT_ONLINE_TIMEOUT_MS))))
    args = parser.parse_args()
    server = create_server(args.host, args.port, args.device_token,
                           args.admin_token, args.device_id, args.data_dir,
                           args.web_root, args.online_timeout_ms)
    print("{} relay listening on {}:{}".format(
        PARTITION, args.host, server.server_port), flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
