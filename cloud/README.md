# WiFi 智慧农业独立云端分区

这是农业项目的独立 relay 分区，和已经运行的 V3、V4 完全隔离：

| 项目 | 农业版 |
| --- | --- |
| 分区名 | `wifi-env-agri` |
| 监听端口 | `18127/TCP` |
| 设备编号 | `WIFI_AGRI_001` |
| 协议 | 农业专用 `v=4` |
| 数据目录 | `/var/lib/wifi-env-agri` |
| 程序目录 | `/opt/wifi-env-agri` |
| 配置目录 | `/etc/wifi-env-agri` |
| 服务名 | `wifi-env-agri-relay.service` |

农业版不解析火焰、可燃气体、矿井或森林字段。遥测字段包括水位、土壤湿度、CO₂、BH1750 光照、SHT30 环境温湿度、阈值、报警状态和三路继电器状态。

## 接口

设备端：

```text
GET /lora/device/telemetry?did=...&token=...&data=...
GET /lora/device/command?did=...&token=...
GET /lora/device/ack?did=...&token=...&data=...
```

管理端：

```text
GET /lora/api/snapshot
GET /lora/api/history
GET /lora/api/command?slave=1&op=pump_on
GET /lora/events
```

历史遥测按 10 秒粒度写入 `WIFI_AGRI_DATA_DIR/telemetry.jsonl`，relay 重启后会
自动恢复最近 300 条有效样本，前端曲线和报表不会因服务重启而清空。每日维护任务
只保留最近 5000 行，不会再直接删除全部历史。

支持的命令：

```text
alarm_test
relay_test
pump_on / pump_off
light_on / light_off
fan_on / fan_off
set_alarm_enabled
set_water_threshold
set_co2_threshold
set_soil_threshold
set_temperature_threshold
set_humidity_threshold
set_light_alarm_mode
ping / snapshot
```

## 首次部署（不会自动执行）

需要在服务器上单独建立用户、目录、令牌和端口规则。仓库只提供设备令牌占位示例：

```text
example-generated-device-token
```

管理令牌必须另行生成，不能与设备令牌相同。建议配置文件：

```ini
WIFI_AGRI_HOST=0.0.0.0
WIFI_AGRI_PORT=18127
WIFI_AGRI_DEVICE_ID=WIFI_AGRI_001
WIFI_AGRI_DEVICE_TOKEN=<设备令牌>
WIFI_AGRI_ADMIN_TOKEN=<新的管理令牌>
WIFI_AGRI_DATA_DIR=/var/lib/wifi-env-agri
WIFI_AGRI_WEB_ROOT=/opt/wifi-env-agri/web
WIFI_AGRI_ONLINE_TIMEOUT_MS=10000
```

安装服务前，必须同时放行服务器系统防火墙和云安全组的 `18127/tcp`。

## 本地验证

```powershell
python -m py_compile .\server\lora_env_relay.py
python .\server\lora_env_relay.py --host 127.0.0.1 --port 18127 `
  --device-id WIFI_AGRI_001 `
  --device-token device-token `
  --admin-token admin-token `
  --data-dir .\server\test-data-agri `
  --web-root .\web
```

随后访问 `/health`，应看到 `partition=wifi-env-agri`、`port=18127`、`protocol=4`。
部署完成后再确认设备上报、命令和 ACK 闭环。
