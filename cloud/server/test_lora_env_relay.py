#!/usr/bin/env python3
"""Regression tests for the independent agriculture relay partition."""

import json
import tempfile
import threading
import unittest
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

import lora_env_relay as relay
import cleanup_history


PAYLOAD = (
    "v=4;hostSeq=1;slave=1;seq=8;waterRaw=3000;waterPct=80;waterValid=1;"
    "soilRaw=1800;soilPct=70;soilValid=1;co2Ppm=900;co2Valid=1;lux=80;"
    "tempX10=256;humidityX10=642;sht30Valid=1;gpsValid=0;latX10000=0;"
    "lonX10000=0;alarmEnabled=1;waterThreshold=80;co2Threshold=1200;"
    "soilThreshold=30;tempThresholdX10=350;humidityThresholdX10=800;"
    "lightAlarmMode=0;waterAlarm=0;co2Alarm=0;soilAlarm=0;tempAlarm=0;"
    "humidityAlarm=0;lightAlarm=0;pumpState=0;lightState=0;fanState=0;"
    "alarmTest=0"
)


class RelayTests(unittest.TestCase):
    def test_agriculture_protocol_and_bounds(self):
        record = relay.parse_telemetry("LORA_AGRI_001", PAYLOAD)
        self.assertEqual(record["protocol"], "4")
        self.assertEqual(record["waterPct"], 80)
        self.assertEqual(record["co2Ppm"], 900)
        for key, value in (("co2Ppm", "50001"), ("soilPct", "101")):
            with self.assertRaises(ValueError):
                fields = dict(item.split("=", 1) for item in PAYLOAD.split(";"))
                mutated = PAYLOAD.replace(key + "=" + fields[key],
                                          key + "=" + value)
                relay.parse_telemetry("LORA_AGRI_001", mutated)

    def test_invalid_sht30_sentinels_do_not_drop_packet(self):
        invalid = PAYLOAD.replace("tempX10=256", "tempX10=-32768")
        invalid = invalid.replace("humidityX10=642", "humidityX10=65535")
        invalid = invalid.replace("sht30Valid=1", "sht30Valid=0")
        record = relay.parse_telemetry("WIFI_AGRI_001", invalid)
        self.assertEqual(record["sht30Valid"], 0)
        self.assertEqual(record["tempX10"], 0)
        self.assertEqual(record["humidityX10"], 0)

    def test_store_accepts_retransmit_and_matches_ack(self):
        with tempfile.TemporaryDirectory() as data_dir:
            store = relay.RelayStore(data_dir)
            first = store.set_telemetry("LORA_AGRI_001", PAYLOAD)
            retransmit = store.set_telemetry("LORA_AGRI_001", PAYLOAD)
            self.assertEqual(retransmit["raw"], first["raw"])
            self.assertEqual(len(store.snapshot("LORA_AGRI_001", True)["history"]), 1)
            command = store.queue_command("LORA_AGRI_001", 1,
                                          "set_water_threshold", "75")
            ack = store.set_ack(
                "LORA_AGRI_001",
                "v=4;id={};slave=1;result=ok".format(command["id"]),
            )
            self.assertEqual(ack["op"], "set_water_threshold")
            self.assertEqual(store.get_command("LORA_AGRI_001")["id"], 0)

    def test_history_survives_store_restart(self):
        with tempfile.TemporaryDirectory() as data_dir:
            store = relay.RelayStore(data_dir)
            store.set_telemetry("LORA_AGRI_001", PAYLOAD)
            restored = relay.RelayStore(data_dir)
            snapshot = restored.snapshot("LORA_AGRI_001", True)
            self.assertEqual(len(snapshot["history"]), 1)
            self.assertEqual(snapshot["history"][0]["co2Ppm"], 900)

    def test_cleanup_keeps_recent_history(self):
        with tempfile.TemporaryDirectory() as data_dir:
            path = Path(data_dir) / "telemetry.jsonl"
            path.write_text("".join("{}\n".format(index) for index in range(7)),
                            encoding="utf-8")
            cleanup_history.cleanup(data_dir, max_lines=3)
            self.assertEqual(path.read_text(encoding="utf-8"),
                             "4\n5\n6\n")

    def test_commands_and_relay_values(self):
        with tempfile.TemporaryDirectory() as data_dir:
            store = relay.RelayStore(data_dir)
            store.set_telemetry("LORA_AGRI_001", PAYLOAD)
            for op in ("pump_on", "light_off", "fan_on", "alarm_test",
                       "relay_test", "set_co2_threshold"):
                command = store.queue_command(
                    "LORA_AGRI_001", 1, op,
                    "1200" if op == "set_co2_threshold" else None)
                self.assertTrue(command["data"].startswith("v=4;"))
                store.set_ack(
                    "LORA_AGRI_001",
                    "v=4;id={};slave=1;result=ok".format(command["id"]),
                )

    def test_slave_queues_are_isolated(self):
        with tempfile.TemporaryDirectory() as data_dir:
            store = relay.RelayStore(data_dir)
            slave1 = store.queue_command("LORA_AGRI_001", 1, "pump_on", None)
            slave2 = store.queue_command("LORA_AGRI_001", 2, "ping", None)
            pending = store.snapshot("LORA_AGRI_001")["pendingCommands"]
            # WiFi 版本对外只展示实际接入的设备 1；内部仍可验证
            # 队列确认逻辑不会被第二个历史队列污染。
            self.assertEqual([item["slave"] for item in pending], [1])
            with self.assertRaises(ValueError):
                store.set_ack(
                    "LORA_AGRI_001",
                    "v=4;id={};slave=2;result=ok".format(slave1["id"]),
                )
            store.set_ack(
                "LORA_AGRI_001",
                "v=4;id={};slave=1;result=ok".format(slave1["id"]),
            )
            self.assertEqual(store.get_command("LORA_AGRI_001")["id"], slave2["id"])

    def test_http_auth_and_command_loop(self):
        with tempfile.TemporaryDirectory() as data_dir:
            server = relay.create_server(
                "127.0.0.1", 0, "device-token", "admin-token",
                "LORA_AGRI_001", data_dir,
                Path(__file__).resolve().parent.parent / "web", 5000,
            )
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            base = "http://127.0.0.1:{}".format(server.server_port)

            def get(path, values):
                url = base + path + "?" + urllib.parse.urlencode(values)
                with urllib.request.urlopen(url, timeout=3) as response:
                    return response.status, json.loads(response.read().decode())

            self.assertEqual(get("/health", {})[1]["partition"], "wifi-env-agri")
            self.assertEqual(get("/health", {})[1]["port"], server.server_port)
            get("/lora/device/telemetry", {
                "did": "LORA_AGRI_001", "token": "device-token", "data": PAYLOAD,
            })
            command = get("/lora/api/command", {
                "did": "LORA_AGRI_001", "token": "admin-token",
                "slave": 2, "op": "light_on",
            })[1]["command"]
            polled = get("/lora/device/command", {
                "did": "LORA_AGRI_001", "token": "device-token",
            })[1]
            self.assertEqual(polled["id"], command["id"])
            with self.assertRaises(urllib.error.HTTPError) as context:
                get("/lora/api/snapshot", {
                    "did": "LORA_AGRI_001", "token": "bad",
                })
            self.assertEqual(context.exception.code, 401)
            server.shutdown()
            server.server_close()


if __name__ == "__main__":
    unittest.main()
