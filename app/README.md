# WiFi 智慧农业监测 APP

这是 WiFi 智慧农业单节点项目的 HBuilderX 前端源码，默认设备编号为 `WIFI_AGRI_001`，云端服务端口示例为 `18127`。

## 功能

- 水位、土壤湿度、CO2、光照和环境温湿度实时显示。
- 单节点在线状态、数据有效性和报警状态展示。
- 实时趋势、历史记录、统计报表和报警中心。
- 水泵、补光灯、风扇远程控制及硬件 ACK 确认。
- 水位、CO2、土壤湿度、温湿度和光照阈值方案。
- 声光报警开关和声光测试；关闭声光报警不影响自动继电器联动。
- 节点名称、农业区域名称和应用标题本地修改。

## 运行配置

编辑 `js/config.js`：

```js
apiBase: 'http://server.example.com:18127',
deviceId: 'WIFI_AGRI_001',
adminToken: 'REPLACE_WITH_WIFI_AGRI_ADMIN_TOKEN'
```

同时把 `index.html` CSP 中的 `server.example.com` 修改为实际服务器域名或 IP。仓库中的配置均为占位符，不包含真实管理令牌。

入口文件为 `index.html`，业务脚本位于 `js/`，样式位于 `css/`。`manifest.json` 使用独立应用 ID 和包名，不会覆盖其他项目。
