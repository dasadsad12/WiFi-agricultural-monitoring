# WiFi 智慧农业监测系统

基于 STM32、ESP8266 和独立云端中继服务实现的单节点智慧农业监测与控制系统。系统支持水位、土壤湿度、CO2、环境温湿度和光照采集，并可远程控制水泵、补光灯、风扇及声光报警。

## 项目结构

- `firmware/`：STM32 Keil 工程、传感器采集、OLED、按键、继电器和 ESP8266 通信代码。
- `app/`：HBuilderX 移动端前端，包含实时监测、趋势、历史、阈值方案和远程控制。
- `cloud/`：Python 云端中继服务、静态网页、systemd 服务和自动测试。
- `docs/`：服务器部署说明。

## 主要功能

- 水位、土壤湿度、CO2、环境温湿度和光照监测。
- OLED 本地数据显示与参数设置。
- 水泵、补光灯、风扇自动联动和远程手动控制。
- 声光报警独立开关；关闭声光报警不会停止自动继电器联动。
- APP 与网页实时监测、趋势曲线、历史记录和阈值方案。
- 云端命令排队、设备轮询、执行结果 ACK 和设备在线判断。

## 配置说明

仓库不包含真实 WiFi、服务器地址或访问令牌。使用前需要分别修改：

1. `firmware/user/WifiRelay.c`
   - `WIFI_SSID`
   - `WIFI_PASSWORD`
   - `WIFI_SERVER_HOST`
   - `WIFI_DEVICE_TOKEN`
2. `app/js/config.js`
   - `apiBase`
   - `adminToken`
3. `cloud/web/app.js`
   - `base`
   - `token`
4. `app/index.html`
   - 将 CSP 中的 `server.example.com` 改为实际服务器域名或 IP。

设备令牌与管理令牌必须不同，并建议使用随机生成的长令牌。不要将填写真实令牌后的配置提交到公开仓库。

## 验证

云端测试：

```powershell
cd cloud/server
python -m unittest test_lora_env_relay.py
```

前端语法检查：

```powershell
node --check app/js/app.js
node --check app/js/api.js
node --check cloud/web/app.js
```

固件使用 Keil MDK-ARM V5 打开 `firmware/LoRa_Slave.uvproj` 编译。烧录后应依次验证传感器上报、阈值保存、继电器控制、声光报警开关和硬件 ACK。
