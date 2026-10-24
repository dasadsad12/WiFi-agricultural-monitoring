(function () {
  "use strict";

  /*
   * Agriculture release configuration.
   * This token is intentionally embedded for the single-user release package.
   * Anyone who can inspect this web bundle can reuse the management token.
   */
  var AGRI_FIXED_CONFIG = {
    base: "http://server.example.com:18127",
    did: "WIFI_AGRI_001",
    token: "REPLACE_WITH_WIFI_AGRI_ADMIN_TOKEN"
  };

  var state = {
    base: "",
    did: AGRI_FIXED_CONFIG.did,
    token: AGRI_FIXED_CONFIG.token,
    snapshot: null,
    history: [],
    source: null,
    pollTimer: 0,
    renderTimer: 0,
    serverStatus: "idle",
    serverLastSuccessAt: 0,
    serverLastError: "",
    chartSignature: {},
    notified: {},
    commandBusy: {},
    commandText: {},
    optimisticRelay: {},
    thresholdDrafts: {},
    thresholdSaving: {},
    log: [],
    notificationPermission: "unknown",
    notificationRequesting: false,
    notificationQueue: [],
    notificationLastError: "",
    plusReady: false,
    view: "overview",
    selectedSlave: 1
  };

  var $ = function (id) { return document.getElementById(id); };
  var colors = { water: "#2d9b93", soil: "#477ec8", co2: "#c48727" };
  var NOTIFICATION_CHANNEL_ID = "agri-environment-alerts";

  function refreshIcons() {
    if (window.lucide && typeof window.lucide.createIcons === "function") {
      window.lucide.createIcons();
    }
  }

  function number(value, digits) {
    var parsed = Number(value);
    return isFinite(parsed) ? parsed.toFixed(digits || 0) : "--";
  }

  function escapeHtml(text) {
    return String(text === undefined || text === null ? "" : text)
      .replace(/&/g, "&amp;").replace(/</g, "&lt;")
      .replace(/>/g, "&gt;").replace(/"/g, "&quot;");
  }

  function log(message) {
    var now = new Date().toLocaleTimeString("zh-CN", { hour12: false });
    state.log.unshift(now + "  " + message);
    state.log = state.log.slice(0, 80);
    $("eventLog").textContent = state.log.join("\n");
  }

  function api(path, extra) {
    var query = [];
    var values = extra || {};
    var key;
    values.did = state.did;
    values.token = state.token;
    for (key in values) {
      if (Object.prototype.hasOwnProperty.call(values, key)) {
        query.push(encodeURIComponent(key) + "=" + encodeURIComponent(values[key]));
      }
    }
    return state.base.replace(/\/$/, "") + path + "?" + query.join("&");
  }

  function hasAlarm(t) {
    return !!(t && (t.waterAlarm || t.co2Alarm || t.soilAlarm ||
      t.tempAlarm || t.humidityAlarm));
  }

  function alarmReasons(t) {
    var reasons = [];
    if (!t) return reasons;
    if (t.waterAlarm) reasons.push("水位");
    if (t.co2Alarm) reasons.push("CO₂");
    if (t.soilAlarm) reasons.push("土壤湿度");
    if (t.tempAlarm) reasons.push("温度");
    if (t.humidityAlarm) reasons.push("环境湿度");
    return reasons;
  }

  function hasFault(t) {
    return !!(t && (!t.waterValid || !t.soilValid || !t.co2Valid ||
      !t.sht30Valid));
  }

  function statusOf(item) {
    if (!item || !item.online || !item.telemetry) return "offline";
    if (hasAlarm(item.telemetry)) return "danger";
    if (hasFault(item.telemetry)) return "warning";
    return "normal";
  }

  function valueText(t, valid, value, unit, digits) {
    if (!t || !valid) return "--";
    return number(value, digits) + (unit ? " " + unit : "");
  }

  function regionName(slave) {
    return slave === 1 ? "一号生态园" : "监测区域";
  }

  function slaveItem(slave) {
    var rows = state.snapshot && Array.isArray(state.snapshot.slaves) ?
      state.snapshot.slaves : [];
    return rows.filter(function (item) {
      return Number(item.slave) === Number(slave);
    })[0] || { slave: slave, online: false, telemetry: null };
  }

  function relayDisplayState(slave, t) {
    var now = Date.now();
    var optimistic = state.optimisticRelay[slave];
    var result = {
      pump: !!(t && t.pumpState),
      light: !!(t && t.lightState),
      fan: !!(t && t.fanState)
    };
    if (optimistic && optimistic.until > now) {
      if (optimistic.pump !== undefined) result.pump = !!optimistic.pump;
      if (optimistic.light !== undefined) result.light = !!optimistic.light;
      if (optimistic.fan !== undefined) result.fan = !!optimistic.fan;
    }
    return result;
  }

  function setOptimisticRelay(slave, op) {
    var current = state.optimisticRelay[slave] || {};
    current.until = Date.now() + 8000;
    if (op === "pump_on") current.pump = true;
    else if (op === "pump_off") current.pump = false;
    else if (op === "light_on") current.light = true;
    else if (op === "light_off") current.light = false;
    else if (op === "fan_on") current.fan = true;
    else if (op === "fan_off") current.fan = false;
    state.optimisticRelay[slave] = current;
  }

  function clearOptimisticRelay(slave, op) {
    var current = state.optimisticRelay[slave];
    if (!current) return;
    if (op.indexOf("pump_") === 0) delete current.pump;
    else if (op.indexOf("light_") === 0) delete current.light;
    else if (op.indexOf("fan_") === 0) delete current.fan;
    if (current.pump === undefined && current.light === undefined &&
        current.fan === undefined) delete state.optimisticRelay[slave];
  }

  function thresholdDraft(slave) {
    return state.thresholdDrafts[slave] || null;
  }

  function thresholdValue(slave, key, fallback) {
    var draft = thresholdDraft(slave);
    if (draft && draft[key] !== undefined) return draft[key];
    return fallback;
  }

  function allMetricHtml(t) {
    t = t || {};
    return '<div class="full-metrics">' +
      metricCard("水位", valueText(t, t.waterValid, t.waterPct, "%", 0), !!t.waterAlarm) +
      metricCard("土壤湿度", valueText(t, t.soilValid, t.soilPct, "%", 0), !!t.soilAlarm) +
      metricCard("二氧化碳", valueText(t, t.co2Valid, t.co2Ppm, "ppm", 0), !!t.co2Alarm) +
      metricCard("环境温度", valueText(t, t.sht30Valid, Number(t.tempX10) / 10, "℃", 1), !!t.tempAlarm) +
      metricCard("环境湿度", valueText(t, t.sht30Valid, Number(t.humidityX10) / 10, "%", 1), !!t.humidityAlarm) +
      metricCard("光照", valueText(t, t.luxAvailable, t.lux, "lux", 0), false) +
      '</div>';
  }

  function scheduleRender() {
    if (state.renderTimer) return;
    state.renderTimer = window.requestAnimationFrame ?
      window.requestAnimationFrame(function () {
        state.renderTimer = 0;
        render();
      }) : window.setTimeout(function () {
        state.renderTimer = 0;
        render();
      }, 16);
  }

  function render() {
    renderConnections();
    renderNodes();
    renderRegions();
    renderAlarmCenter();
    renderControls();
    renderReport();
    renderNavigation();
    drawMiniChart(1);
    drawMiniChart(2);
    drawDetailCharts();
    $("serverClock").textContent = state.snapshot ?
      new Date(state.snapshot.serverTime).toLocaleTimeString("zh-CN", { hour12: false }) : "--";
    refreshIcons();
  }

  function renderNavigation() {
    Array.prototype.forEach.call(document.querySelectorAll("[data-view]"), function (view) {
      view.classList.toggle("active", view.getAttribute("data-view") === state.view);
    });
    Array.prototype.forEach.call(document.querySelectorAll("[data-nav]"), function (button) {
      button.classList.toggle("active", button.getAttribute("data-nav") === state.view);
    });
    var count = [1].filter(function (slave) {
      var item = slaveItem(slave);
      return item.online && hasAlarm(item.telemetry);
    }).length;
    var badge = $("alarmBadge");
    if (badge) {
      badge.hidden = count === 0;
      badge.textContent = String(count);
    }
  }

  function renderConnections() {
    var serverText = {
      idle: "未连接",
      connecting: "连接中",
      online: "已连接",
      retrying: "重试中",
      error: "连接异常"
    }[state.serverStatus] || "未连接";
    var serverDotClass = state.serverStatus === "online" ? "online" :
      state.serverStatus === "error" ? "error" :
        state.serverStatus === "retrying" ? "warning" : "";
    var serverDot = $("serverStatusDot");
    var serverLabel = $("serverStatusText");
    if (serverDot) serverDot.className = "dot " + serverDotClass;
    if (serverLabel) serverLabel.textContent = serverText;

    var rows = state.snapshot && Array.isArray(state.snapshot.slaves) ?
      state.snapshot.slaves : null;
    var stale = !state.serverLastSuccessAt ||
      Date.now() - state.serverLastSuccessAt > 15000;
    var hardwareText = "等待数据";
    var hardwareDotClass = "";
    if (rows && !stale) {
      var total = rows.length;
      var online = rows.filter(function (item) { return !!item.online; }).length;
      if (!total) {
        hardwareText = "无设备数据";
        hardwareDotClass = "error";
      } else if (online === total) {
        hardwareText = online + "/" + total + " 在线";
        hardwareDotClass = "online";
      } else if (online > 0) {
        hardwareText = online + "/" + total + " 在线";
        hardwareDotClass = "warning";
      } else {
        hardwareText = "硬件离线";
        hardwareDotClass = "error";
      }
    } else if (rows) {
      hardwareText = "状态未知";
      hardwareDotClass = "warning";
    }
    var hardwareDot = $("hardwareStatusDot");
    var hardwareLabel = $("hardwareStatusText");
    if (hardwareDot) hardwareDot.className = "dot " + hardwareDotClass;
    if (hardwareLabel) hardwareLabel.textContent = hardwareText;
  }

  function renderNodes() {
    var grid = $("nodeGrid");
    var rows = state.snapshot && state.snapshot.slaves ?
      state.snapshot.slaves.filter(function (item) { return Number(item.slave) === 1; }) : [{ slave: 1, online: false }];
    var onlineCount = 0;
    grid.innerHTML = rows.map(function (item) {
      var t = item.telemetry || {};
      var status = statusOf(item);
      var allAlarms = alarmReasons(t);
      var alarm = allAlarms.filter(function (reason) {
        return reason === "水位" || reason === "CO₂";
      });
      var relay = relayDisplayState(item.slave, t);
      if (allAlarms.length && !alarm.length) alarm = ["其他报警"];
      if (item.online) onlineCount++;
      return '<article class="node-card ' + status + '" data-open-slave="' + item.slave + '">' +
        '<div class="node-head"><div><span class="eyebrow">' + escapeHtml(regionName(item.slave)) +
        '</span><strong>监测节点</strong></div><span class="state ' + status +
        '">' + (status === "offline" ? "离线" : status === "danger" ? "报警" :
          status === "warning" ? "数据预警" : "在线") + '</span></div>' +
        '<div class="metric-grid">' +
        metricCard("水位", valueText(t, t.waterValid, t.waterPct, "%", 0),
          !!t.waterAlarm) +
        metricCard("CO₂", valueText(t, t.co2Valid, t.co2Ppm, "ppm", 0),
          !!t.co2Alarm) + '</div>' +
        '<div class="mini-trend"><canvas id="miniChart' + item.slave +
        '" height="58"></canvas></div>' +
        '<div class="node-foot"><span>水泵 <b>' + (relay.pump ? "开启" : "关闭") +
        '</b> · 补光灯 <b>' + (relay.light ? "开启" : "关闭") +
        '</b> · 风扇 <b>' + (relay.fan ? "开启" : "关闭") + '</b></span></div>' +
        '<div class="node-foot"><span>' + (alarm.length ? "超标：" +
          escapeHtml(alarm.join("、")) : (item.online ? "各项指标在正常范围" : "等待设备上报")) +
        '</span><span>' + (item.ageMs === null || item.ageMs === undefined ?
          "无数据" : (item.ageMs / 1000).toFixed(1) + " 秒前") + '</span></div>' +
        '<div class="node-foot"><b>点击查看全部参数和执行器状态</b></div>' +
        '</article>';
    }).join("");
    $("summaryText").textContent = onlineCount + " / " + rows.length + " 个节点在线";
  }

  function renderRegions() {
    var root = $("regionMap");
    if (!root) return;
    var nodes = [1].map(function (slave) {
      var item = slaveItem(slave);
      var status = statusOf(item);
      var point = { x: 150, y: 140, name: "一号生态园" };
      return '<g class="map-node ' + status + '" data-open-slave="' + slave + '">' +
        '<circle class="outer" cx="' + point.x + '" cy="' + point.y + '" r="18"></circle>' +
        '<circle class="core" cx="' + point.x + '" cy="' + point.y + '" r="7"></circle>' +
        '<text x="' + (point.x + 25) + '" y="' + (point.y + 5) + '">' + point.name + '</text></g>';
    }).join("");
    var cards = [1].map(function (slave) {
      var item = slaveItem(slave);
      var status = statusOf(item);
      var t = item.telemetry || {};
      return '<article class="region-node ' + status + '">' +
        '<div class="node-head"><div><span class="eyebrow">监测节点</span>' +
        '<h3>' + escapeHtml(regionName(slave)) + '</h3></div><span class="state ' +
        status + '">' + (item.online ? (hasAlarm(t) ? "报警" : "在线") : "离线") +
        '</span></div>' + allMetricHtml(t) +
        '<div class="node-foot"><span>水泵 <b>' + (t.pumpState ? "开启" : "关闭") +
        '</b> · 补光灯 <b>' + (t.lightState ? "开启" : "关闭") +
        '</b> · 风扇 <b>' + (t.fanState ? "开启" : "关闭") +
        '</b></span></div><div class="detail-actions">' +
        '<button class="button" data-open-slave="' + slave + '">查看节点详情</button></div></article>';
    }).join("");
    root.innerHTML = '<div class="agri-map"><svg viewBox="0 0 620 410" role="img" aria-label="农业监测区域图">' +
      '<path d="M42 205 H578 M150 205 V140 H500 V285 H445 M300 140 V55 M300 285 V365" fill="none" stroke="#b6c9c0" stroke-width="28" stroke-linecap="round" stroke-linejoin="round"></path>' +
      '<path d="M42 205 H578 M150 205 V140 H500 V285 H445 M300 140 V55 M300 285 V365" fill="none" stroke="#f9fbfa" stroke-width="17" stroke-linecap="round" stroke-linejoin="round"></path>' +
      '<text x="48" y="187" fill="#71817d" font-size="12">农业主路</text>' +
      '<text x="170" y="120" fill="#71817d" font-size="12">一号生态园</text>' + nodes +
      '</svg></div><div class="map-legend"><span><i></i>正常</span><span><i class="warning"></i>预警</span><span><i class="danger"></i>高危报警</span><span><i class="offline"></i>离线故障</span></div>' +
      '<div class="map-note">点击地图节点或下方卡片查看完整参数与控制状态。</div><div class="region-cards">' + cards + '</div>';
  }

  function renderAlarmCenter() {
    var root = $("alarmCenter");
    if (!root) return;
    var current = [1].map(slaveItem).filter(function (item) {
      return item.online && hasAlarm(item.telemetry);
    });
    var history = state.history.filter(hasAlarm).slice(-20).reverse();
    var html = current.map(function (item) {
      return '<article class="alarm-item active"><div><strong>' +
        escapeHtml(regionName(item.slave)) + '正在报警</strong><span>' +
        escapeHtml(alarmReasons(item.telemetry).join("、")) +
        '超过设定阈值</span></div><time>实时</time></article>';
    });
    history.forEach(function (row) {
      html.push('<article class="alarm-item"><div><strong>监测节点 ' + row.slave +
        '历史报警</strong><span>' + escapeHtml(alarmReasons(row).join("、")) +
        '</span></div><time>' + new Date(Number(row.receivedAt)).toLocaleString("zh-CN", {
          hour12: false
        }) + '</time></article>');
    });
    root.innerHTML = html.length ? html.join("") :
      '<div class="empty">当前没有报警，历史记录中也没有异常样本</div>';
  }

  function openDetail(slave) {
    state.selectedSlave = Number(slave);
    var item = slaveItem(slave);
    var t = item.telemetry || {};
    var reasons = alarmReasons(t);
    var relay = relayDisplayState(slave, t);
    $("detailContent").innerHTML =
      '<div class="detail-title"><span class="eyebrow">监测节点 ' + slave +
      '</span><h2>' + escapeHtml(regionName(slave)) + '节点详情</h2><p class="muted">' +
      (item.online ? "硬件在线，数据来自实时上报" : "硬件离线，以下为最后一次上报") +
      '</p></div>' + allMetricHtml(t) +
      '<div class="node-foot"><span>执行器</span><b>水泵 ' +
      (relay.pump ? "开启" : "关闭") + " / 补光灯 " +
      (relay.light ? "开启" : "关闭") + " / 风扇 " +
      (relay.fan ? "开启" : "关闭") + '</b></div>' +
      '<div class="node-foot"><span>报警状态</span><b>' +
      (reasons.length ? escapeHtml(reasons.join("、")) : "正常") + '</b></div>' +
      '<div class="node-foot"><span>阈值</span><b>水位 ' +
      number(t.waterThreshold, 0) + "% / 土壤 " + number(t.soilThreshold, 0) +
      "% / 二氧化碳 " + number(t.co2Threshold, 0) + " ppm / 温度 " +
      number(Number(t.tempThresholdX10) / 10, 1) + "℃ / 湿度 " +
      number(Number(t.humidityThresholdX10) / 10, 1) + "%</b></div>" +
      '<section class="detail-chart-group"><div class="panel-head"><div>' +
      '<span class="eyebrow">历史曲线</span><h3>关键数据趋势</h3></div>' +
      '<span class="muted">缺测数据自动断开，不做假连线</span></div>' +
      '<div class="detail-charts">' +
      '<article class="chart-card"><div class="chart-title">水位 / CO₂</div><canvas id="detailChartWaterCo2" height="210"></canvas></article>' +
      '<article class="chart-card"><div class="chart-title">土壤湿度 / 环境温湿度</div><canvas id="detailChartEnv" height="210"></canvas></article>' +
      '<article class="chart-card wide"><div class="chart-title">光照强度</div><canvas id="detailChartLight" height="180"></canvas></article>' +
      '</div></section>';
    $("detailModal").hidden = false;
    state.chartSignature.detail = "";
    drawDetailCharts();
  }

  function metricCard(label, value, danger) {
    return '<div class="metric' + (danger ? " bad" : "") + '"><span>' +
      label + '</span><b>' + escapeHtml(value) + '</b></div>';
  }

  function historyFor(slave) {
    return state.history.filter(function (row) {
      return Number(row.slave) === Number(slave);
    }).sort(function (a, b) { return Number(a.receivedAt) - Number(b.receivedAt); });
  }

  function renderControls() {
    var root = $("controlGrid");
    if (document.activeElement &&
        document.activeElement.hasAttribute("data-threshold")) return;
    var rows = state.snapshot && state.snapshot.slaves ?
      state.snapshot.slaves.filter(function (item) { return Number(item.slave) === 1; }) : [{ slave: 1 }];
    root.innerHTML = rows.map(function (item) {
      var t = item.telemetry || {};
      var slave = item.slave;
      var busy = !!state.commandBusy[slave];
      var relay = relayDisplayState(slave, t);
      var busyText = state.commandText[slave] || "命令确认中";
      return '<article class="control-card"><div class="control-head"><strong>监测节点 ' +
        slave + ' · ' + escapeHtml(regionName(slave)) + '</strong><span class="muted">' +
        (busy ? escapeHtml(busyText) : "独立控制") + '</span></div>' +
        '<div class="relay-row">' +
        relayButton(slave, "pump_on", "启动水泵", relay.pump, busy) +
        relayButton(slave, "pump_off", "关闭水泵", !relay.pump, busy) +
        relayButton(slave, "light_on", "启动补光灯", relay.light, busy) +
        relayButton(slave, "light_off", "关闭补光灯", !relay.light, busy) +
        relayButton(slave, "fan_on", "启动风扇", relay.fan, busy) +
        relayButton(slave, "fan_off", "关闭风扇", !relay.fan, busy) +
        '</div><div class="threshold-grid">' +
        thresholdInput(slave, "water", "水位 %", thresholdValue(slave, "water", t.waterThreshold === undefined ? 80 : t.waterThreshold)) +
        thresholdInput(slave, "co2", "CO₂ ppm", thresholdValue(slave, "co2", t.co2Threshold === undefined ? 1200 : t.co2Threshold)) +
        thresholdInput(slave, "soil", "土壤湿度 %", thresholdValue(slave, "soil", t.soilThreshold === undefined ? 30 : t.soilThreshold)) +
        thresholdInput(slave, "temp", "温度 °C", thresholdValue(slave, "temp", t.tempThresholdX10 === undefined ? 35 : Number(t.tempThresholdX10) / 10)) +
        thresholdInput(slave, "humidity", "湿度 %", thresholdValue(slave, "humidity", t.humidityThresholdX10 === undefined ? 80 : Number(t.humidityThresholdX10) / 10)) +
        thresholdSelect(slave, "light", "暗光策略", thresholdValue(slave, "light", t.lightAlarmMode === undefined ? 0 : t.lightAlarmMode)) +
        '</div><div class="threshold-actions"><button class="button" data-action="save" data-slave="' +
        slave + '"' + (busy ? " disabled" : "") + '>保存阈值</button><button class="button" data-action="alarm" data-slave="' +
        slave + '"' + (busy ? " disabled" : "") + '>声光测试</button><button class="button" data-action="relay" data-slave="' +
        slave + '"' + (busy ? " disabled" : "") + '>继电器测试</button></div></article>';
    }).join("");
    Array.prototype.forEach.call(root.querySelectorAll("[data-action]"), function (button) {
      button.addEventListener("click", controlClick);
    });
    Array.prototype.forEach.call(root.querySelectorAll("[data-threshold]"), function (input) {
      input.addEventListener("input", thresholdChanged);
      input.addEventListener("change", thresholdChanged);
    });
  }

  function relayButton(slave, op, label, active, busy) {
    return '<button class="button' + (active ? " active" : "") + '" data-op="' +
      op + '" data-slave="' + slave + '"' + (busy ? " disabled" : "") + '>' +
      label + '</button>';
  }

  function thresholdInput(slave, key, label, value) {
    return '<label>' + label + '<input data-threshold="' + key + '" data-slave="' +
      slave + '" value="' + escapeHtml(value === undefined || value === null ? "" : String(value)) + '"></label>';
  }

  function thresholdSelect(slave, key, label, value) {
    return '<label>' + label + '<select data-threshold="' + key + '" data-slave="' +
      slave + '"><option value="0"' + (Number(value) === 0 ? " selected" : "") +
      '>低于暗光</option><option value="1"' + (Number(value) === 1 ? " selected" : "") +
      '>低于低照度</option><option value="2"' + (Number(value) === 2 ? " selected" : "") +
      '>高照度</option></select></label>';
  }

  function thresholdChanged(event) {
    var input = event.currentTarget;
    var slave = Number(input.getAttribute("data-slave"));
    var key = input.getAttribute("data-threshold");
    if (!state.thresholdDrafts[slave]) state.thresholdDrafts[slave] = {};
    state.thresholdDrafts[slave][key] = input.value;
  }

  function controlClick(event) {
    var button = event.currentTarget;
    var slave = Number(button.getAttribute("data-slave"));
    var action = button.getAttribute("data-action");
    var op = button.getAttribute("data-op");
    if (op) sendCommand(slave, op, null);
    else if (action === "alarm") sendCommand(slave, "alarm_test", 1);
    else if (action === "relay") sendCommand(slave, "relay_test", 1);
    else if (action === "save") saveThresholds(slave);
  }

  function saveThresholds(slave) {
    var fields = {};
    var item = slaveItem(slave);
    var t = item.telemetry || {};
    Array.prototype.forEach.call(document.querySelectorAll(
      '[data-slave="' + slave + '"][data-threshold]'), function (input) {
      fields[input.getAttribute("data-threshold")] = input.value;
    });
    var specs = [
      ["water", "set_water_threshold", Math.round(Number(fields.water)), Number(t.waterThreshold), 0, 100, "水位阈值"],
      ["co2", "set_co2_threshold", Math.round(Number(fields.co2)), Number(t.co2Threshold), 0, 50000, "CO₂阈值"],
      ["soil", "set_soil_threshold", Math.round(Number(fields.soil)), Number(t.soilThreshold), 0, 100, "土壤湿度阈值"],
      ["temp", "set_temperature_threshold", Math.round(Number(fields.temp) * 10), Number(t.tempThresholdX10), -400, 1250, "温度阈值"],
      ["humidity", "set_humidity_threshold", Math.round(Number(fields.humidity) * 10), Number(t.humidityThresholdX10), 0, 1000, "湿度阈值"],
      ["light", "set_light_alarm_mode", Math.round(Number(fields.light)), Number(t.lightAlarmMode), 0, 2, "光照策略"]
    ];
    var invalid = specs.filter(function (item) {
      return !isFinite(item[2]) || item[2] < item[4] || item[2] > item[5];
    });
    if (invalid.length) {
      log("从机 " + slave + " 阈值输入无效：" + invalid.map(function (item) {
        return item[6];
      }).join("、"));
      return;
    }
    var values = specs.filter(function (item) {
      return !isFinite(item[3]) || item[2] !== item[3];
    }).map(function (item) {
      return [item[1], item[2], item[6]];
    });
    if (!values.length) {
      delete state.thresholdDrafts[slave];
      log("从机 " + slave + " 阈值未变化，无需保存");
      renderControls();
      return;
    }
    state.thresholdSaving[slave] = true;
    var chain = Promise.resolve();
    values.forEach(function (item, index) {
      chain = chain.then(function () {
        state.commandText[slave] = "保存" + item[2] + " " + (index + 1) + "/" + values.length;
        renderControls();
        return sendCommand(slave, item[0], item[1]);
      }).then(function (ok) {
        if (!ok) throw new Error(item[2] + "未确认");
      });
    });
    chain.then(function () {
      delete state.thresholdDrafts[slave];
      delete state.thresholdSaving[slave];
      log("从机 " + slave + " 阈值已全部确认");
      loadSnapshot(false).then(scheduleRender);
    }).catch(function (error) {
      delete state.thresholdSaving[slave];
      log("从机 " + slave + " 阈值保存中断：" + error.message);
      renderControls();
    });
  }

  function renderReport() {
    var rows = state.history.filter(function (row) {
      return row.waterValid || row.soilValid || row.co2Valid || row.sht30Valid;
    });
    var values = function (key, validKey) {
      return rows.filter(function (r) { return !validKey || r[validKey]; })
        .map(function (r) { return Number(r[key]); }).filter(isFinite);
    };
    var specs = [
      ["水位均值", values("waterPct", "waterValid"), "%"],
      ["土壤湿度均值", values("soilPct", "soilValid"), "%"],
      ["CO₂均值", values("co2Ppm", "co2Valid"), "ppm"],
      ["环境温度均值", values("tempX10", "sht30Valid").map(function (v) { return v / 10; }), "℃"],
      ["环境湿度均值", values("humidityX10", "sht30Valid").map(function (v) { return v / 10; }), "%"],
      ["光照均值", values("lux", "luxAvailable"), "lux"],
      ["最高水位", values("waterPct", "waterValid"), "%", "max"],
      ["最高 CO₂", values("co2Ppm", "co2Valid"), "ppm", "max"],
      ["报警样本", rows.filter(hasAlarm).length, "条"]
    ];
    $("reportGrid").innerHTML = specs.map(function (spec) {
      var list = Array.isArray(spec[1]) ? spec[1] : [];
      var value = Array.isArray(spec[1]) ? (list.length ?
        (spec[3] === "max" ? Math.max.apply(Math, list) :
          list.reduce(function (a, b) { return a + b; }, 0) / list.length) : "--") : spec[1];
      return '<div class="stat"><span>' + spec[0] + '</span><b>' +
        (value === "--" ? "--" : number(value, spec[2] === "ppm" ? 0 : 1)) +
        ' <small>' + spec[2] + '</small></b></div>';
    }).join("");
    drawReportCharts(values("waterPct", "waterValid"), values("soilPct", "soilValid"),
      values("co2Ppm", "co2Valid"), rows.filter(hasAlarm).length, rows.length);
  }

  function canvasSize(canvas) {
    var rect = canvas.getBoundingClientRect();
    var width = Math.max(260, rect.width || 320);
    var height = Number(canvas.getAttribute("height")) || 200;
    var dpr = window.devicePixelRatio || 1;
    var w = Math.floor(width * dpr);
    var h = Math.floor(height * dpr);
    if (canvas.width !== w || canvas.height !== h) {
      canvas.width = w; canvas.height = h;
    }
    return { ctx: canvas.getContext("2d"), width: width, height: height, dpr: dpr };
  }

  function drawChart(slave) {
    var canvas = $("chart" + slave);
    if (!canvas) return;
    var rows = historyFor(slave).slice(-80);
    var signature = rows.map(function (r) {
      return [r.receivedAt, r.waterPct, r.co2Ppm, r.waterValid,
        r.co2Valid].join(":");
    }).join("|");
    if (state.chartSignature[slave] === signature && canvas.width) return;
    state.chartSignature[slave] = signature;
    var size = canvasSize(canvas);
    var ctx = size.ctx;
    var dpr = size.dpr;
    var width = size.width;
    var height = size.height;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, width, height);
    ctx.fillStyle = "#fcfefd"; ctx.fillRect(0, 0, width, height);
    var left = 38, right = 43, top = 22, bottom = 25;
    var plotW = Math.max(20, width - left - right);
    var plotH = Math.max(20, height - top - bottom);
    ctx.strokeStyle = "#dfeae5"; ctx.lineWidth = 1;
    for (var i = 0; i <= 4; i++) {
      var y = top + plotH * i / 4;
      ctx.beginPath(); ctx.moveTo(left, y); ctx.lineTo(width - right, y); ctx.stroke();
      ctx.fillStyle = "#81928b"; ctx.font = "10px system-ui";
      ctx.textAlign = "right"; ctx.fillText(String(100 - i * 25), left - 6, y + 3);
    }
    ctx.textAlign = "left"; ctx.fillText("%", 7, top + 3);
    ctx.textAlign = "right"; ctx.fillText("ppm", width - 4, top + 3);
    if (rows.length < 2) {
      ctx.fillStyle = "#81928b"; ctx.textAlign = "center";
      ctx.fillText("等待有效数据", width / 2, height / 2);
      return;
    }
    function drawSeries(key, validKey, color, max, rightAxis) {
      var points = [];
      rows.forEach(function (row, index) {
        if (!row[validKey] || !isFinite(Number(row[key]))) {
          points.push(null); return;
        }
        var value = Number(row[key]);
        var ratio = Math.max(0, Math.min(1, value / max));
        points.push({ x: left + plotW * index / (rows.length - 1),
          y: top + plotH * (1 - ratio) });
      });
      ctx.strokeStyle = color; ctx.lineWidth = 2;
      var drawing = false;
      points.forEach(function (point) {
        if (!point) { drawing = false; return; }
        if (!drawing) { ctx.beginPath(); ctx.moveTo(point.x, point.y); drawing = true; }
        else ctx.lineTo(point.x, point.y);
        if (point === points[points.length - 1] || !points[points.indexOf(point) + 1]) {
          ctx.stroke(); drawing = false;
        }
      });
      ctx.fillStyle = color; ctx.font = "10px system-ui";
      ctx.textAlign = "left";
      ctx.fillText(rightAxis ? "CO₂" : "水位",
        left + 5 + (rightAxis ? 40 : 0), 13);
    }
    drawSeries("waterPct", "waterValid", colors.water, 100, false);
    drawSeries("co2Ppm", "co2Valid", colors.co2, 5000, true);
    ctx.fillStyle = "#81928b"; ctx.font = "10px system-ui";
    ctx.textAlign = "left"; ctx.fillText(rows[0] ? new Date(rows[0].receivedAt).toLocaleTimeString("zh-CN", { hour12: false }) : "", left, height - 7);
    ctx.textAlign = "right"; ctx.fillText(rows[rows.length - 1] ? new Date(rows[rows.length - 1].receivedAt).toLocaleTimeString("zh-CN", { hour12: false }) : "", width - right, height - 7);
  }

  function drawMiniChart(slave) {
    var canvas = $("miniChart" + slave);
    if (!canvas) return;
    var rows = historyFor(slave).slice(-36);
    var signature = rows.map(function (r) {
      return [r.receivedAt, r.waterPct, r.co2Ppm, r.waterValid, r.co2Valid].join(":");
    }).join("|");
    var key = "mini" + slave;
    if (state.chartSignature[key] === signature && canvas.width) return;
    state.chartSignature[key] = signature;
    var size = canvasSize(canvas);
    var ctx = size.ctx;
    var dpr = size.dpr;
    var width = size.width;
    var height = size.height;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, width, height);
    ctx.fillStyle = "#f8fbf9";
    ctx.fillRect(0, 0, width, height);
    if (rows.length < 2) {
      ctx.fillStyle = "#8a9993";
      ctx.font = "10px system-ui";
      ctx.textAlign = "center";
      ctx.fillText("等待趋势数据", width / 2, height / 2 + 3);
      return;
    }
    function spark(keyName, validKey, max, color, offset) {
      var left = 8;
      var right = 8;
      var top = 9 + offset;
      var plotH = 36;
      var plotW = Math.max(10, width - left - right);
      var drawing = false;
      ctx.strokeStyle = color;
      ctx.lineWidth = 1.8;
      rows.forEach(function (row, index) {
        if (!row[validKey] || !isFinite(Number(row[keyName]))) {
          if (drawing) { ctx.stroke(); drawing = false; }
          return;
        }
        var ratio = Math.max(0, Math.min(1, Number(row[keyName]) / max));
        var x = left + plotW * index / Math.max(1, rows.length - 1);
        var y = top + plotH * (1 - ratio);
        if (!drawing) { ctx.beginPath(); ctx.moveTo(x, y); drawing = true; }
        else ctx.lineTo(x, y);
      });
      if (drawing) ctx.stroke();
    }
    ctx.strokeStyle = "#e4ece8";
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(8, height - 10);
    ctx.lineTo(width - 8, height - 10);
    ctx.stroke();
    spark("waterPct", "waterValid", 100, colors.water, 0);
    spark("co2Ppm", "co2Valid", 5000, colors.co2, 3);
    ctx.font = "10px system-ui";
    ctx.fillStyle = colors.water;
    ctx.textAlign = "left";
    ctx.fillText("水位", 8, 10);
    ctx.fillStyle = colors.co2;
    ctx.fillText("CO₂", 42, 10);
  }

  function drawMetricChart(canvasId, slave, specs, signatureKey) {
    var canvas = $(canvasId);
    if (!canvas) return;
    var rows = historyFor(slave).slice(-100);
    var signature = rows.map(function (r) {
      return [r.receivedAt].concat(specs.map(function (spec) {
        return r[spec.key] + ":" + r[spec.valid];
      })).join("|");
    }).join("/");
    if (state.chartSignature[signatureKey] === signature && canvas.width) return;
    state.chartSignature[signatureKey] = signature;
    var size = canvasSize(canvas);
    var ctx = size.ctx;
    var dpr = size.dpr;
    var width = size.width;
    var height = size.height;
    var left = 38, right = 14, top = 28, bottom = 26;
    var plotW = Math.max(20, width - left - right);
    var plotH = Math.max(20, height - top - bottom);
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, width, height);
    ctx.fillStyle = "#fcfefd";
    ctx.fillRect(0, 0, width, height);
    ctx.strokeStyle = "#dfeae5";
    ctx.lineWidth = 1;
    for (var i = 0; i <= 4; i++) {
      var y = top + plotH * i / 4;
      ctx.beginPath();
      ctx.moveTo(left, y);
      ctx.lineTo(width - right, y);
      ctx.stroke();
    }
    if (rows.length < 2) {
      ctx.fillStyle = "#81928b";
      ctx.font = "11px system-ui";
      ctx.textAlign = "center";
      ctx.fillText("等待有效历史数据", width / 2, height / 2);
      return;
    }
    specs.forEach(function (spec, specIndex) {
      var drawing = false;
      ctx.strokeStyle = spec.color;
      ctx.lineWidth = 2;
      rows.forEach(function (row, index) {
        var valid = spec.valid ? row[spec.valid] : true;
        var raw = spec.transform ? spec.transform(row[spec.key]) : Number(row[spec.key]);
        if (!valid || !isFinite(raw)) {
          if (drawing) { ctx.stroke(); drawing = false; }
          return;
        }
        var ratio = Math.max(0, Math.min(1, raw / spec.max));
        var x = left + plotW * index / Math.max(1, rows.length - 1);
        var y = top + plotH * (1 - ratio);
        if (!drawing) { ctx.beginPath(); ctx.moveTo(x, y); drawing = true; }
        else ctx.lineTo(x, y);
      });
      if (drawing) ctx.stroke();
      ctx.fillStyle = spec.color;
      ctx.font = "10px system-ui";
      ctx.textAlign = "left";
      ctx.fillText(spec.label, left + specIndex * 58, 14);
    });
    ctx.fillStyle = "#81928b";
    ctx.font = "10px system-ui";
    ctx.textAlign = "left";
    ctx.fillText(rows[0] ? new Date(rows[0].receivedAt).toLocaleTimeString("zh-CN", { hour12: false }) : "", left, height - 7);
    ctx.textAlign = "right";
    ctx.fillText(rows[rows.length - 1] ? new Date(rows[rows.length - 1].receivedAt).toLocaleTimeString("zh-CN", { hour12: false }) : "", width - right, height - 7);
  }

  function drawDetailCharts() {
    var modal = $("detailModal");
    if (!modal || modal.hidden) return;
    var slave = state.selectedSlave || 1;
    drawMetricChart("detailChartWaterCo2", slave, [
      { key: "waterPct", valid: "waterValid", label: "水位%", color: colors.water, max: 100 },
      { key: "co2Ppm", valid: "co2Valid", label: "CO₂", color: colors.co2, max: 5000 }
    ], "detail-water-co2-" + slave);
    drawMetricChart("detailChartEnv", slave, [
      { key: "soilPct", valid: "soilValid", label: "土壤%", color: colors.soil, max: 100 },
      { key: "tempX10", valid: "sht30Valid", label: "温度", color: "#c85f47", max: 60, transform: function (v) { return Number(v) / 10; } },
      { key: "humidityX10", valid: "sht30Valid", label: "湿度%", color: "#6f65c8", max: 100, transform: function (v) { return Number(v) / 10; } }
    ], "detail-env-" + slave);
    drawMetricChart("detailChartLight", slave, [
      { key: "lux", valid: "luxAvailable", label: "光照lux", color: "#d09b22", max: 5000 }
    ], "detail-light-" + slave);
  }

  function drawReportCharts(water, soil, co2, alarms, total) {
    var bar = canvasSize($("barChart"));
    var ctx = bar.ctx; var dpr = bar.dpr; var width = bar.width; var height = bar.height;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0); ctx.clearRect(0, 0, width, height);
    ctx.fillStyle = "#fcfefd"; ctx.fillRect(0, 0, width, height);
    var values = [water.length ? water.reduce(function (a, b) { return a + b; }, 0) / water.length : 0,
      soil.length ? soil.reduce(function (a, b) { return a + b; }, 0) / soil.length : 0,
      co2.length ? co2.reduce(function (a, b) { return a + b; }, 0) / co2.length / 50 : 0];
    var labels = ["水位 %", "土壤 %", "CO₂/50"];
    var max = Math.max(100, Math.max.apply(Math, values));
    values.forEach(function (value, index) {
      var x = 45 + index * (width - 70) / 3;
      var barH = (height - 50) * value / max;
      ctx.fillStyle = [colors.water, colors.soil, colors.co2][index];
      ctx.fillRect(x, height - 28 - barH, 34, barH);
      ctx.fillStyle = "#71827b"; ctx.font = "10px system-ui"; ctx.textAlign = "center";
      ctx.fillText(labels[index], x + 17, height - 10);
      ctx.fillText(number(index === 2 ? value * 50 : value, 0), x + 17, height - 34 - barH);
    });
    var pie = canvasSize($("pieChart")); ctx = pie.ctx; dpr = pie.dpr; width = pie.width; height = pie.height;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0); ctx.clearRect(0, 0, width, height);
    ctx.fillStyle = "#fcfefd"; ctx.fillRect(0, 0, width, height);
    var centerX = width * .35, centerY = height * .5, radius = Math.min(width, height) * .28;
    var safe = Math.max(0, total - alarms); var sum = Math.max(1, safe + alarms);
    var angle = -Math.PI / 2;
    [[safe, colors.green || "#4e9a75", "正常"], [alarms, "#c84d4d", "报警"]].forEach(function (part) {
      var next = angle + Math.PI * 2 * part[0] / sum;
      ctx.beginPath(); ctx.moveTo(centerX, centerY); ctx.arc(centerX, centerY, radius, angle, next); ctx.closePath();
      ctx.fillStyle = part[1]; ctx.fill(); angle = next;
    });
    ctx.fillStyle = "#71827b"; ctx.font = "11px system-ui"; ctx.textAlign = "left";
    ctx.fillText("正常样本 " + safe, width * .65, height * .4);
    ctx.fillText("报警样本 " + alarms, width * .65, height * .58);
  }

  function notifyText(t, slave) {
    var reasons = alarmReasons(t);
    return "从机 " + slave + " · " + reasons.join("、") + "超标，请及时处理";
  }

  function notificationError(error) {
    return String(error && error.message ? error.message : error || "")
      .replace(/\s+/g, " ").trim().slice(0, 160);
  }

  function hasDeniedPermission(result) {
    if (!result) return false;
    return ["denied", "deniedPresent", "deniedAlways"].some(function (key) {
      var value = result[key];
      return Array.isArray(value) ? value.length > 0 : !!value;
    });
  }

  function createNotificationChannel() {
    if (!window.plus || !plus.android ||
        typeof plus.android.runtimeMainActivity !== "function") return true;
    try {
      var Build = plus.android.importClass("android.os.Build");
      if (Build.VERSION.SDK_INT < 26) return true;
      var Context = plus.android.importClass("android.content.Context");
      var activity = plus.android.runtimeMainActivity();
      var manager = activity.getSystemService(Context.NOTIFICATION_SERVICE);
      var channel = plus.android.newObject(
        "android.app.NotificationChannel",
        NOTIFICATION_CHANNEL_ID,
        "农业环境报警",
        4
      );
      channel.setDescription("水位、CO₂、土壤湿度和环境异常提醒");
      channel.enableVibration(true);
      channel.setShowBadge(true);
      manager.createNotificationChannel(channel);
      return true;
    } catch (error) {
      state.notificationLastError = notificationError(error);
      return false;
    }
  }

  function nativeNotificationsEnabled() {
    if (!window.plus || !plus.android ||
        typeof plus.android.runtimeMainActivity !== "function") return true;
    try {
      var Build = plus.android.importClass("android.os.Build");
      var Context = plus.android.importClass("android.content.Context");
      var activity = plus.android.runtimeMainActivity();
      var manager = activity.getSystemService(Context.NOTIFICATION_SERVICE);
      if (typeof manager.areNotificationsEnabled === "function" &&
          !manager.areNotificationsEnabled()) return false;
      if (Build.VERSION.SDK_INT >= 26 &&
          typeof manager.getNotificationChannel === "function") {
        var channel = manager.getNotificationChannel(NOTIFICATION_CHANNEL_ID);
        if (channel && typeof channel.getImportance === "function" &&
            channel.getImportance() === 0) return false;
      }
      if (Build.VERSION.SDK_INT >= 33) {
        var PackageManager = plus.android.importClass(
          "android.content.pm.PackageManager"
        );
        var permission = null;
        if (typeof activity.checkSelfPermission === "function") {
          permission = activity.checkSelfPermission(
            "android.permission.POST_NOTIFICATIONS"
          );
        }
        if ((permission === null || permission === undefined) &&
            typeof plus.android.invoke === "function") {
          permission = plus.android.invoke(
            activity,
            "checkSelfPermission",
            "android.permission.POST_NOTIFICATIONS"
          );
        }
        if (Number(permission) !== Number(PackageManager.PERMISSION_GRANTED)) {
          return false;
        }
      }
      return true;
    } catch (error) {
      state.notificationLastError = notificationError(error);
      return true;
    }
  }

  function emitNativeAndroidNotification(title, body) {
    if (!window.plus || !plus.android ||
        typeof plus.android.runtimeMainActivity !== "function") return false;
    try {
      var Build = plus.android.importClass("android.os.Build");
      var Context = plus.android.importClass("android.content.Context");
      var activity = plus.android.runtimeMainActivity();
      var manager = activity.getSystemService(Context.NOTIFICATION_SERVICE);
      if (!nativeNotificationsEnabled()) return false;
      createNotificationChannel();
      var builder = Build.VERSION.SDK_INT >= 26 ?
        plus.android.newObject(
          "android.app.Notification$Builder",
          activity,
          NOTIFICATION_CHANNEL_ID
        ) :
        plus.android.newObject("android.app.Notification$Builder", activity);
      builder.setContentTitle(title);
      builder.setContentText(body);
      builder.setSmallIcon(activity.getApplicationInfo().icon);
      builder.setAutoCancel(true);
      builder.setDefaults(7);
      manager.notify(700, builder.build());
      state.notificationLastError = "";
      return true;
    } catch (error) {
      state.notificationLastError = notificationError(error);
      return false;
    }
  }

  function emitPlusPushNotification(title, body, tag) {
    if (!window.plus || !plus.push ||
        typeof plus.push.createMessage !== "function") return false;
    try {
      createNotificationChannel();
      if (!nativeNotificationsEnabled()) {
        state.notificationLastError = "系统通知权限或农业报警渠道未开启";
        return false;
      }
      if (typeof plus.push.setAutoNotification === "function") {
        plus.push.setAutoNotification(true);
      }
      plus.push.createMessage(
        body,
        JSON.stringify({
          type: "agriculture_alarm",
          tag: tag,
          at: new Date().toISOString()
        }),
        {
          title: title,
          cover: true,
          sound: "system",
          delay: 0,
          channelId: NOTIFICATION_CHANNEL_ID
        }
      );
      state.notificationLastError = "";
      return true;
    } catch (error) {
      state.notificationLastError = notificationError(error);
      return false;
    }
  }

  function emitSystemNotification(title, body, tag) {
    if (emitPlusPushNotification(title, body, tag)) return true;
    if (emitNativeAndroidNotification(title, body)) return true;
    try {
      if (window.Notification && Notification.permission === "granted") {
        new Notification(title, { body: body, tag: tag, renotify: true });
        state.notificationLastError = "";
        return true;
      }
    } catch (error) {
      state.notificationLastError = notificationError(error);
    }
    return false;
  }

  function flushNotificationQueue() {
    if (state.notificationPermission !== "granted") return;
    var queue = state.notificationQueue.slice();
    state.notificationQueue = [];
    queue.forEach(function (item) {
      emitSystemNotification(item.title, item.body, item.tag);
    });
  }

  function syncNotificationPermission() {
    if (!window.plus || !plus.android ||
        typeof plus.android.runtimeMainActivity !== "function") {
      if (window.Notification) {
        state.notificationPermission =
          Notification.permission === "granted" ? "granted" :
          Notification.permission === "denied" ? "denied" : "unknown";
      }
      return state.notificationPermission;
    }
    try {
      var Build = plus.android.importClass("android.os.Build");
      var Context = plus.android.importClass("android.content.Context");
      var activity = plus.android.runtimeMainActivity();
      var manager = activity.getSystemService(Context.NOTIFICATION_SERVICE);
      if (typeof manager.areNotificationsEnabled === "function" &&
          !manager.areNotificationsEnabled()) {
        state.notificationPermission = "denied";
        return state.notificationPermission;
      }
      if (Build.VERSION.SDK_INT >= 33) {
        var PackageManager = plus.android.importClass(
          "android.content.pm.PackageManager"
        );
        var permission = null;
        if (typeof activity.checkSelfPermission === "function") {
          permission = activity.checkSelfPermission(
            "android.permission.POST_NOTIFICATIONS"
          );
        }
        if ((permission === null || permission === undefined) &&
            typeof plus.android.invoke === "function") {
          permission = plus.android.invoke(
            activity,
            "checkSelfPermission",
            "android.permission.POST_NOTIFICATIONS"
          );
        }
        state.notificationPermission =
          Number(permission) === Number(PackageManager.PERMISSION_GRANTED) ?
          "granted" : "denied";
      } else {
        state.notificationPermission = "granted";
      }
    } catch (error) {
      state.notificationLastError = notificationError(error);
    }
    return state.notificationPermission;
  }

  function requestNotificationPermission(force) {
    if (state.notificationRequesting) return;
    if (window.plus && !state.plusReady) return;
    if (!force) syncNotificationPermission();
    if (!force && state.notificationPermission === "granted") {
      flushNotificationQueue();
      $("notifyState").textContent = "手机通知权限已允许";
      return;
    }
    if (window.plus && plus.android &&
        typeof plus.android.requestPermissions === "function") {
      state.notificationRequesting = true;
      try {
        var Build = plus.android.importClass("android.os.Build");
        if (Build.VERSION.SDK_INT < 33) {
          state.notificationPermission = "granted";
          state.notificationRequesting = false;
          flushNotificationQueue();
          $("notifyState").textContent = "手机通知权限已允许";
          return;
        }
        plus.android.requestPermissions(
          ["android.permission.POST_NOTIFICATIONS"],
          function (result) {
            state.notificationPermission =
              hasDeniedPermission(result) ? "denied" : "granted";
            state.notificationRequesting = false;
            if (state.notificationPermission === "granted") {
              createNotificationChannel();
              flushNotificationQueue();
              $("notifyState").textContent = "手机通知权限已允许";
            } else {
              $("notifyState").textContent =
                "手机通知权限未允许，请在系统设置中开启";
            }
          },
          function (error) {
            state.notificationPermission = "denied";
            state.notificationRequesting = false;
            state.notificationLastError = notificationError(error);
            $("notifyState").textContent =
              "通知权限申请失败，请在系统设置中开启";
          }
        );
      } catch (error) {
        state.notificationRequesting = false;
        state.notificationPermission = "denied";
        state.notificationLastError = notificationError(error);
        $("notifyState").textContent = "通知权限申请失败";
      }
      return;
    }
    if (window.plus && plus.push &&
        typeof plus.push.createMessage === "function") {
      state.notificationPermission = "granted";
      flushNotificationQueue();
      $("notifyState").textContent = "手机通知权限已允许";
      return;
    }
    if (!window.Notification ||
        typeof Notification.requestPermission !== "function") {
      $("notifyState").textContent =
        "当前环境没有系统通知能力；原生 APP 需要勾选 Push 模块";
      return;
    }
    state.notificationRequesting = true;
    Notification.requestPermission().then(function (permission) {
      state.notificationPermission =
        permission === "granted" ? "granted" : "denied";
      state.notificationRequesting = false;
      flushNotificationQueue();
      $("notifyState").textContent =
        state.notificationPermission === "granted" ?
        "浏览器通知权限已允许" : "浏览器通知权限未允许";
    }).catch(function (error) {
      state.notificationRequesting = false;
      state.notificationPermission = "denied";
      state.notificationLastError = notificationError(error);
      $("notifyState").textContent = "通知权限申请失败";
    });
  }

  function pushNotification(title, body, tag) {
    var item = { title: title, body: body, tag: tag };
    if (window.plus && !state.plusReady) {
      if (state.notificationQueue.length < 8) {
        state.notificationQueue.push(item);
      }
      return false;
    }
    if (window.plus) {
      syncNotificationPermission();
      if (state.notificationPermission !== "granted") {
        if (state.notificationPermission !== "denied" &&
            state.notificationQueue.length < 8) {
          state.notificationQueue.push(item);
          requestNotificationPermission(false);
        }
        return false;
      }
      return emitSystemNotification(title, body, tag);
    }
    if (window.Notification && Notification.permission === "granted") {
      return emitSystemNotification(title, body, tag);
    }
    if (state.notificationPermission === "unknown" &&
        state.notificationQueue.length < 8) {
      state.notificationQueue.push(item);
      requestNotificationPermission(false);
    }
    return false;
  }

  function notifyAlarm(item) {
    var t = item && item.telemetry;
    if (!item || !t || !item.online || !hasAlarm(t)) return;
    var key = String(item.slave) + ":" + String(t.seq) + ":" + alarmReasons(t).join("|");
    if (state.notified[key]) return;
    state.notified[key] = true;
    var body = notifyText(t, item.slave);
    if (pushNotification("智慧农业报警", body, "agri-alarm-" + item.slave)) {
      $("notifyState").textContent = "最近一次报警已发送到手机通知栏";
    } else {
      $("notifyState").textContent =
        "检测到报警：" + body + "；正在等待手机通知权限";
    }
  }

  function loadSnapshot(withHistory) {
    return fetch(api("/lora/api/snapshot"), { cache: "no-store" }).then(function (response) {
      if (!response.ok) throw new Error("snapshot HTTP " + response.status);
      return response.json();
    }).then(function (snapshot) {
      state.serverStatus = "online";
      state.serverLastSuccessAt = Date.now();
      state.serverLastError = "";
      state.snapshot = snapshot;
      var slaves = Array.isArray(snapshot.slaves) ? snapshot.slaves : [];
      slaves.forEach(notifyAlarm);
      if (withHistory !== false) {
        return fetch(api("/lora/api/history"), { cache: "no-store" }).then(function (response) {
          return response.ok ? response.json() : { history: [] };
        }).catch(function () {
          return { history: [] };
        }).then(function (history) {
          state.history = Array.isArray(history.history) ? history.history.slice(-600) : [];
          scheduleRender();
        });
      }
      scheduleRender();
    });
  }

  function openEvents() {
    if (state.source) state.source.close();
    state.source = new EventSource(api("/lora/events"));
    state.source.onopen = function () {
      state.serverStatus = "online";
      state.serverLastSuccessAt = Date.now();
      state.serverLastError = "";
      scheduleRender();
    };
    state.source.addEventListener("meter", function (event) {
      try {
        state.snapshot = JSON.parse(event.data);
        state.serverStatus = "online";
        state.serverLastSuccessAt = Date.now();
        state.serverLastError = "";
        var slaves = Array.isArray(state.snapshot.slaves) ?
          state.snapshot.slaves : [];
        slaves.forEach(function (item) {
          notifyAlarm(item);
          if (item.telemetry) {
            var last = state.history[state.history.length - 1];
            if (!last || last.receivedAt !== item.telemetry.receivedAt ||
                last.slave !== item.telemetry.slave) state.history.push(item.telemetry);
          }
        });
        state.history = state.history.slice(-600);
        scheduleRender();
      } catch (error) { log("实时数据解析失败"); }
    });
    state.source.onerror = function () {
      state.serverStatus = "retrying";
      state.serverLastError = "实时数据通道正在重试";
      scheduleRender();
    };
  }

  function startPolling() {
    if (state.pollTimer) window.clearInterval(state.pollTimer);
    state.pollTimer = window.setInterval(function () {
      if (state.base && state.token) {
        loadSnapshot(false).catch(function (error) {
          state.serverStatus = "error";
          state.serverLastError = error && error.message ? error.message : String(error);
          scheduleRender();
        });
      }
    }, 5000);
  }

  function connect() {
    state.base = AGRI_FIXED_CONFIG.base;
    state.did = AGRI_FIXED_CONFIG.did;
    state.token = AGRI_FIXED_CONFIG.token;
    $("serverUrl").value = state.base;
    $("deviceId").value = state.did;
    $("adminToken").value = state.token;
    if (!state.base || !state.did || !state.token) {
      log("请填写服务器地址、设备编号和管理令牌");
      return;
    }
    state.serverStatus = "connecting";
    state.serverLastError = "";
    scheduleRender();
    loadSnapshot(true).then(function () {
      openEvents(); startPolling(); requestNotificationPermission();
      log("已连接农业分区 " + state.base + " / " + state.did);
    }).catch(function (error) {
      state.serverStatus = "error";
      state.serverLastError = error && error.message ? error.message : String(error);
      scheduleRender();
      log("连接失败：" + error.message);
      startPolling();
    });
  }

  function sendCommand(slave, op, value) {
    if (!state.base || !state.token) { log("请先连接服务器"); return Promise.resolve(false); }
    if (state.commandBusy[slave]) return Promise.resolve(false);
    state.commandBusy[slave] = true;
    state.commandText[slave] = state.commandText[slave] || "命令确认中";
    if (/^(pump|light|fan)_(on|off)$/.test(op)) setOptimisticRelay(slave, op);
    renderControls();
    var query = { slave: slave, op: op };
    if (value !== null && value !== undefined) query.value = value;
    return fetch(api("/lora/api/command", query), { cache: "no-store" })
      .then(function (response) { return response.json().then(function (body) {
        if (!response.ok || !body.ok) throw new Error(body.message || "命令入队失败");
        return waitForCommand(body.command);
      }); })
      .then(function () {
        log("从机 " + slave + " 已确认：" + op);
        return loadSnapshot(false).then(function () { return true; });
      })
      .catch(function (error) {
        clearOptimisticRelay(slave, op);
        log("从机 " + slave + " 命令失败：" + error.message);
        return false;
      })
      .then(function (result) {
        state.commandBusy[slave] = false;
        delete state.commandText[slave];
        renderControls();
        return result;
      });
  }

  function waitForCommand(command) {
    var deadline = Date.now() + 15000;
    function check() {
      if (Date.now() >= deadline) return Promise.reject(new Error("硬件确认超时"));
      return loadSnapshot(false).then(function () {
        var ack = state.snapshot && state.snapshot.ack;
        if (ack && String(ack.id) === String(command.id) &&
            Number(ack.slave) === Number(command.slave)) {
          if (ack.result !== "ok") throw new Error(ack.result);
          return ack;
        }
        return new Promise(function (resolve) { window.setTimeout(resolve, 450); }).then(check);
      });
    }
    return check();
  }

  function allAlarmTest() {
    var rows = state.snapshot && state.snapshot.slaves ? state.snapshot.slaves : [];
    var online = rows.filter(function (row) { return row.online; });
    if (!online.length) { log("没有在线节点"); return; }
    var chain = Promise.resolve();
    online.forEach(function (row) {
      chain = chain.then(function () { return sendCommand(row.slave, "alarm_test", 1); });
    });
    chain.then(function () { log("所有在线节点声光测试完成"); });
  }

  $("connectButton").addEventListener("click", connect);
  $("notifyButton").addEventListener("click", requestNotificationPermission);
  $("allAlarmTest").addEventListener("click", allAlarmTest);
  $("clearLog").addEventListener("click", function () { state.log = []; $("eventLog").textContent = ""; });
  $("refreshTopButton").addEventListener("click", function () {
    connect();
  });
  $("connectionTopButton").addEventListener("click", function () {
    state.view = "settings";
    scheduleRender();
  });
  document.addEventListener("click", function (event) {
    var nav = event.target.closest("[data-nav]");
    var open = event.target.closest("[data-open-slave]");
    if (nav) {
      state.view = nav.getAttribute("data-nav");
      scheduleRender();
      return;
    }
    if (open) {
      openDetail(Number(open.getAttribute("data-open-slave")));
      return;
    }
  });
  $("closeDetail").addEventListener("click", function () {
    $("detailModal").hidden = true;
  });
  $("detailModal").addEventListener("click", function (event) {
    if (event.target === $("detailModal")) $("detailModal").hidden = true;
  });
  $("serverUrl").value = AGRI_FIXED_CONFIG.base;
  $("deviceId").value = AGRI_FIXED_CONFIG.did;
  $("adminToken").value = AGRI_FIXED_CONFIG.token;
  $("adminToken").readOnly = true;
  $("serverUrl").readOnly = true;
  $("deviceId").readOnly = true;
  window.addEventListener("resize", function () { state.chartSignature = {}; scheduleRender(); });
  document.addEventListener("plusready", function () {
    state.plusReady = true;
    try {
      if (plus.push && typeof plus.push.setAutoNotification === "function") {
        plus.push.setAutoNotification(true);
      }
      if (plus.push && typeof plus.push.addEventListener === "function") {
        plus.push.addEventListener("click", function () {
          $("notifyState").textContent = "已打开农业报警通知";
        });
      }
    } catch (error) {
      state.notificationLastError = notificationError(error);
    }
    createNotificationChannel();
    window.setTimeout(function () {
      requestNotificationPermission(true);
    }, 600);
  }, false);
  window.addEventListener("resume", function () {
    if (window.plus) {
      syncNotificationPermission();
      requestNotificationPermission(false);
    }
  }, false);
  render();
  window.setTimeout(connect, 80);
}());
