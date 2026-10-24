(function (window, document) {
  'use strict';

  var APP_TITLE_KEY = 'lora-agri-app-title';
  var DEFAULT_APP_TITLE = 'WiFi智慧农业监测';
  var appTitle = DEFAULT_APP_TITLE;
  try {
    var storedAppTitle = String(localStorage.getItem(APP_TITLE_KEY) || '').trim();
    if (storedAppTitle) appTitle = storedAppTitle.slice(0, 40);
  } catch (e) {}

  var state = {
    view: 'overview',
    filter: 'all',
    selected: 1,
    metric: 'co2',
    snapshot: null,
    history: MineStore.history(),
    profiles: MineStore.profiles(),
    presets: MineStore.presets(),
    selectedPreset: {},
    thresholdDrafts: {},
    connected: false,
    lastUpdate: 0,
    loading: false,
    source: null,
    pollTimer: null,
    jobs: {},
    commandBusy: {},
    actuatorOverrides: {},
    manualModalSlave: null,
    notificationPermission: 'unknown',
    lastAlarmStates: {},
    notificationLastKeys: {},
    modalConfirm: null,
    plusReady: false
  };

  var METRICS = {
    water: { label: '水位', unit: '%', color: '#1b8b64', digits: 0, max: 100, value: function (t) { return t && t.waterValid ? Number(t.waterPct) : null; } },
    co2: { label: '二氧化碳', unit: 'ppm', color: '#c48727', digits: 0, max: 3000, value: function (t) { return t && t.co2Valid ? Number(t.co2Ppm) : null; } },
    soil: { label: '土壤湿度', unit: '%', color: '#477ec8', digits: 0, max: 100, value: function (t) { return t && t.soilValid ? Number(t.soilPct) : null; } },
    temp: { label: '环境温度', unit: '℃', color: '#c85f47', digits: 1, max: 60, value: function (t) { return t && t.sht30Valid ? Number(t.tempX10) / 10 : null; } },
    humidity: { label: '环境湿度', unit: '%', color: '#6f65c8', digits: 1, max: 100, value: function (t) { return t && t.sht30Valid ? Number(t.humidityX10) / 10 : null; } },
    lux: { label: '光照', unit: 'lux', color: '#d09b22', digits: 0, max: 5000, value: function (t) { return t && t.luxAvailable ? Number(t.lux) : null; } }
  };

  var ICONS = {
    overview: 'layout-dashboard', tunnel: 'map', alerts: 'bell', reports: 'bar-chart-3', settings: 'settings-2',
    refresh: 'refresh-cw', wifi: 'wifi', back: 'arrow-left', arrow: 'arrow-right', pin: 'map-pin',
    signal: 'radio-tower', warning: 'triangle-alert', check: 'check-circle-2', offline: 'wifi-off',
    command: 'send', clock: 'clock-3', edit: 'pencil', trash: 'trash-2', pump: 'droplets',
    fan: 'fan', lamp: 'lamp-ceiling', chart: 'chart-line', sprout: 'sprout', locate: 'crosshair',
    route: 'git-branch', bell: 'bell-ring', plus: 'plus', close: 'x', save: 'save', alerts: 'bell-ring'
  };

  function $(id) { return document.getElementById(id); }
  function currentAppTitle() { return appTitle; }
  function syncAppTitle() {
    document.title = currentAppTitle();
    document.querySelectorAll('[data-app-title]').forEach(function (node) {
      node.textContent = currentAppTitle();
    });
  }
  function saveAppTitle(value) {
    var next = String(value === undefined || value === null ? '' : value).trim().replace(/\s+/g, ' ');
    if (!next) {
      toast('项目名称不能为空', 'warning');
      return false;
    }
    appTitle = next.slice(0, 40);
    try { localStorage.setItem(APP_TITLE_KEY, appTitle); } catch (e) {}
    syncAppTitle();
    return true;
  }
  function editAppTitle() {
    openModal('修改项目名称', '<div class="field"><label>项目名称</label><input id="app-title-input" maxlength="40" autocomplete="off" value="' + escapeHtml(currentAppTitle()) + '"></div>', '保存', function () {
      if (!saveAppTitle($('app-title-input') && $('app-title-input').value)) return;
      closeModal();
      render();
      toast('项目名称已更新');
    });
    window.setTimeout(function () {
      var input = $('app-title-input');
      if (input) { input.focus(); input.select(); }
    }, 0);
  }
  function icon(name) { return '<i data-lucide="' + (ICONS[name] || name) + '"></i>'; }
  function icons() {
    if (window.lucide && typeof window.lucide.createIcons === 'function') window.lucide.createIcons();
  }
  function escapeHtml(value) {
    return String(value === undefined || value === null ? '' : value).replace(/[&<>"']/g, function (c) {
      return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c];
    });
  }
  function number(value, digits) {
    return value === null || value === undefined || !isFinite(Number(value)) ? '--' : Number(value).toFixed(digits || 0);
  }
  function timeText(ms) { return ms ? new Date(ms).toLocaleTimeString('zh-CN', { hour12: false }) : '--:--:--'; }
  function ageText(ms) {
    if (ms === null || ms === undefined) return '暂无有效上报';
    if (ms < 1000) return '刚刚更新';
    return (Number(ms) / 1000).toFixed(1) + ' 秒前更新';
  }
  function realSlaves() { return [1]; }
  function allNodes() {
    state.profiles = MineStore.profiles();
    return realSlaves().map(function (id) { return profile(id); });
  }
  function profile(id) {
    state.profiles = MineStore.profiles();
    return state.profiles[String(id)] || { id: id, name: '展示节点 ' + id, location: '未设置区域', mapX: 120, mapY: 160, virtual: true };
  }
  function isVirtual(id) { return !!profile(id).virtual; }
  function slaveItem(slave) {
    var rows = state.snapshot && Array.isArray(state.snapshot.slaves) ? state.snapshot.slaves : [];
    return rows.filter(function (row) { return Number(row.slave) === Number(slave); })[0] ||
      { slave: slave, online: false, ageMs: null, telemetry: null };
  }
  function telemetryFor(slave) {
    var item = slaveItem(slave);
    return item.telemetry || null;
  }
  function historyFor(slave) {
    return state.history.filter(function (row) { return Number(row.slave) === Number(slave); })
      .sort(function (a, b) { return Number(a.receivedAt) - Number(b.receivedAt); });
  }
  function alarmReasons(t) {
    var reasons = [];
    if (!t) return reasons;
    if (t.waterAlarm) reasons.push('水位');
    if (t.co2Alarm) reasons.push('二氧化碳');
    if (t.soilAlarm) reasons.push('土壤湿度');
    if (t.tempAlarm) reasons.push('环境温度');
    if (t.humidityAlarm) reasons.push('环境湿度');
    if (t.lightAlarm) reasons.push('光照');
    return reasons;
  }
  function hasSensorAlarm(t) { return alarmReasons(t).length > 0; }
  // alarmEnabled controls the physical siren/buzzer, not whether a sensor
  // condition is abnormal or whether the APP should report that condition.
  function hasAlarm(t) { return hasSensorAlarm(t); }
  function hasFault(t) { return !!(t && (!t.waterValid || !t.soilValid || !t.co2Valid || !t.sht30Valid)); }
  function statusOf(item) {
    if (!item || !item.online || !item.telemetry) return 'offline';
    if (hasAlarm(item.telemetry)) return 'danger';
    if (hasFault(item.telemetry)) return 'warning';
    return 'normal';
  }
  function statusText(value) { return { normal: '正常', warning: '数据预警', danger: '高危报警', offline: '离线故障' }[value] || '未知'; }
  function metricAlarm(t, key) {
    if (!t) return false;
    return (key === 'water' && t.waterAlarm) || (key === 'co2' && t.co2Alarm) ||
      (key === 'soil' && t.soilAlarm) || (key === 'temp' && t.tempAlarm) ||
      (key === 'humidity' && t.humidityAlarm) || (key === 'lux' && t.lightAlarm);
  }
  function metricValue(t, key) { return METRICS[key].value(t); }
  function displayMetric(t, key) {
    var metric = METRICS[key]; var value = metricValue(t, key);
    if (value === null || value === undefined) return '<b class="unavailable">--</b><small>' + metric.unit + '</small>';
    return '<b class="metric-value' + (metricAlarm(t, key) ? ' bad' : '') + '">' + number(value, metric.digits) + '</b><small>' + metric.unit + '</small>';
  }
  function detailMetric(label, value, unit, bad, key) {
    return '<div class="detail-metric' + (bad ? ' metric-danger' : '') + '"' + (key ? ' data-detail-metric="' + key + '"' : '') + '><span>' + escapeHtml(label) + '</span><strong class="' + (bad ? 'bad' : '') + '">' + escapeHtml(value) + '</strong><small>' + escapeHtml(unit) + '</small></div>';
  }
  function stats() {
    var online = 0; var offline = 0; var faults = 0; var danger = 0;
    realSlaves().forEach(function (slave) {
      var item = slaveItem(slave); var status = statusOf(item);
      if (status === 'offline') offline += 1; else online += 1;
      if (status === 'danger') danger += 1;
      if (item.online && hasFault(item.telemetry)) faults += 1;
    });
    return { online: online, offline: offline, faults: faults, danger: danger };
  }
  function connectionStrip() {
    var s = stats();
    return '<div class="connection-strip"><i class="connection-dot ' + (state.connected ? 'online' : 'error') + '"></i><strong class="connection-text">' +
      (state.connected ? '云服务器：已连接' : '云服务器：连接中') + '</strong><span class="last-update">硬件设备：' +
      s.online + '/1 在线 · ' + (state.lastUpdate ? '最近同步 ' + timeText(state.lastUpdate) : '等待首次同步') + '</span></div>';
  }
  function renderStats() {
    var s = stats();
    return '<div class="stat-grid"><article class="stat-card"><div class="stat-label">在线节点' + icon('wifi') + '</div><strong>' + s.online + '</strong><small>实时链路</small></article>' +
      '<article class="stat-card dark"><div class="stat-label">离线故障' + icon('offline') + '</div><strong>' + s.offline + '</strong><small>等待上报</small></article>' +
      '<article class="stat-card danger"><div class="stat-label">当前报警' + icon('alerts') + '</div><strong>' + s.danger + '</strong><small>阈值异常节点</small></article>' +
      '<article class="stat-card warning"><div class="stat-label">传感器故障' + icon('warning') + '</div><strong>' + s.faults + '</strong><small>有效位异常</small></article></div>';
  }
  function miniMetric(key, slave, t, chart) {
    return '<div class="metric has-mini' + (metricAlarm(t, key) ? ' metric-danger' : '') + '"><span>' + METRICS[key].label + '</span>' +
      displayMetric(t, key) + (chart && !isVirtual(slave) ? '<canvas class="mini-chart" data-mini-chart="' + key + '" data-slave="' + slave + '" aria-label="' + METRICS[key].label + '趋势"></canvas>' : '') + '</div>';
  }
  function renderNodeCard(slave) {
    var p = profile(slave); var item = isVirtual(slave) ? { slave: slave, online: false, ageMs: null, telemetry: null } : slaveItem(slave);
    var t = item.telemetry || null; var status = isVirtual(slave) ? 'offline' : statusOf(item);
    return '<article class="node-card status-' + status + '"><div class="node-head"><div class="node-title">' +
      '<button type="button" data-open-slave="' + slave + '">' + escapeHtml(p.name) + '</button><div class="node-subtitle">' + icon('pin') + '<span>' + escapeHtml(p.location) + '</span></div></div>' +
      '<span class="status-pill ' + status + '">' + (isVirtual(slave) ? '展示节点' : statusText(status)) + '</span></div><div class="node-age">' + (isVirtual(slave) ? '暂无硬件通信' : ageText(item.ageMs)) + '</div>' +
      '<div class="metric-grid primary-metrics">' + ['water', 'co2'].map(function (key) { return miniMetric(key, slave, t, false); }).join('') + '</div><div class="node-trend-grid"><div class="trend-title">关键指标趋势</div>' + ['co2', 'soil', 'temp', 'humidity'].map(function (key) { return miniMetric(key, slave, t, true); }).join('') + '</div>' +
      '<div class="node-note">' + (isVirtual(slave) ? '可先保存节点档案，接入硬件后再启用控制。' : '首页曲线显示 CO₂、土壤湿度、环境温度和环境湿度。') + '</div>' +
      '<div class="node-footer"><button class="node-open" type="button" data-open-slave="' + slave + '">' + (isVirtual(slave) ? '查看节点档案' : '查看节点详情') + ' ' + icon('arrow') + '</button></div></article>';
  }
  function filteredNodes() {
    return allNodes();
  }
  function renderOverview() {
    var cards = filteredNodes().map(function (node) { return renderNodeCard(node.id); }).join('');
    if (!cards) cards = '<div class="empty-state">' + icon('check') + '<strong>暂无设备</strong><p>等待监测节点接入</p></div>';
    return connectionStrip() + '<div class="page-heading"><div><span class="eyebrow">农业环境监测</span><h1>设备总览</h1></div></div>' +
      '<div class="toolbar single-node-toolbar"><span class="muted">' + (state.lastUpdate ? '同步 ' + timeText(state.lastUpdate) : '等待同步') +
      '</span><button class="button" data-action="refresh">' + icon('refresh') + '刷新数据</button></div><div class="node-grid">' + cards + '</div>';
  }

  function thresholdValues(slave, t) {
    var draft = state.thresholdDrafts[String(slave)] || {};
    var preset = currentPreset(slave);
    return {
      water: draft.water !== undefined ? draft.water : (t && t.waterThreshold !== undefined ? t.waterThreshold : preset.water),
      co2: draft.co2 !== undefined ? draft.co2 : (t && t.co2Threshold !== undefined ? t.co2Threshold : preset.co2),
      soil: draft.soil !== undefined ? draft.soil : (t && t.soilThreshold !== undefined ? t.soilThreshold : preset.soil),
      temp: draft.temp !== undefined ? draft.temp : (t && t.tempThresholdX10 !== undefined ? Number(t.tempThresholdX10) / 10 : preset.temp),
      humidity: draft.humidity !== undefined ? draft.humidity : (t && t.humidityThresholdX10 !== undefined ? Number(t.humidityThresholdX10) / 10 : preset.humidity),
      light: draft.light !== undefined ? draft.light : (t && t.lightAlarmMode !== undefined ? t.lightAlarmMode : preset.light)
    };
  }
  function thresholdFormIsActive() {
    var active = document.activeElement;
    return !!(active && active.id && /^threshold-/.test(active.id));
  }
  function syncThresholdForm(slave) {
    var values = thresholdValues(slave, telemetryFor(slave));
    ['water', 'co2', 'soil', 'temp', 'humidity', 'light'].forEach(function (key) {
      var node = $('threshold-' + key);
      if (node && document.activeElement !== node) node.value = values[key];
    });
  }
  function currentPreset(slave) {
    return state.selectedPreset[String(slave)] || state.presets[0] || {
      id: 'temporary-default', name: '临时默认阈值', water: 80, co2: 1200, soil: 30, temp: 35, humidity: 80, light: 0
    };
  }
  function presetOptions(slave) {
    var current = currentPreset(slave);
    return state.presets.map(function (preset) {
      return '<option value="' + escapeHtml(preset.id) + '"' + (String(current.id) === String(preset.id) ? ' selected' : '') + '>' + escapeHtml(preset.name) + '</option>';
    }).join('');
  }
  function actuatorCommandInfo(op) {
    var map = {
      pump_on: { key: 'pumpState', value: 1 },
      pump_off: { key: 'pumpState', value: 0 },
      light_on: { key: 'lightState', value: 1 },
      light_off: { key: 'lightState', value: 0 },
      fan_on: { key: 'fanState', value: 1 },
      fan_off: { key: 'fanState', value: 0 },
      set_alarm_enabled: { key: 'alarmEnabled', value: null }
    };
    return map[op] || null;
  }
  function effectiveActuatorState(slave, telemetry, key) {
    var overrides = state.actuatorOverrides[String(slave)] || {};
    var override = overrides[key];
    if (override && override.expiresAt && Date.now() >= override.expiresAt) {
      delete overrides[key];
      if (!Object.keys(overrides).length) delete state.actuatorOverrides[String(slave)];
      override = null;
    }
    if (override) return Number(override.value) === 1;
    return !!(telemetry && Number(telemetry[key]) === 1);
  }
  function setActuatorOverride(slave, op, value) {
    var info = actuatorCommandInfo(op);
    if (!info) return;
    var key = String(slave);
    if (!state.actuatorOverrides[key]) state.actuatorOverrides[key] = {};
    state.actuatorOverrides[key][info.key] = {
      value: info.value === null ? (Number(value) === 1 ? 1 : 0) : info.value,
      expiresAt: Date.now() + Number(MineConfig.commandTimeoutMs || 30000) + 5000
    };
  }
  function clearActuatorOverride(slave, op) {
    var info = actuatorCommandInfo(op); if (!info) return;
    var overrides = state.actuatorOverrides[String(slave)];
    if (!overrides) return;
    delete overrides[info.key];
    if (!Object.keys(overrides).length) delete state.actuatorOverrides[String(slave)];
  }
  function reconcileActuatorOverrides(snapshot) {
    var rows = snapshot && Array.isArray(snapshot.slaves) ? snapshot.slaves : [];
    rows.forEach(function (item) {
      var overrides = state.actuatorOverrides[String(item.slave)];
      var telemetry = item && item.telemetry;
      if (!overrides) return;
      Object.keys(overrides).forEach(function (key) {
        var override = overrides[key];
        var expired = override && override.expiresAt && Date.now() >= override.expiresAt;
        var reported = telemetry && telemetry[key] !== undefined && telemetry[key] !== null;
        if (expired || (reported && Number(telemetry[key]) === Number(override.value))) delete overrides[key];
      });
      if (!Object.keys(overrides).length) delete state.actuatorOverrides[String(item.slave)];
    });
  }
  function refreshManualModal(slave) {
    if (state.manualModalSlave === null || Number(state.manualModalSlave) !== Number(slave)) return;
    var modal = $('modal-root'); var body = modal && modal.querySelector('.modal-body');
    if (!body) return;
    body.innerHTML = renderManualControl(Number(slave));
    icons();
  }
  function renderManualControl(slave) {
    var t = telemetryFor(slave) || {};
    var pumpState = effectiveActuatorState(slave, t, 'pumpState');
    var lightState = effectiveActuatorState(slave, t, 'lightState');
    var fanState = effectiveActuatorState(slave, t, 'fanState');
    var alarmEnabled = effectiveActuatorState(slave, t, 'alarmEnabled');
    var disabled = state.connected ? '' : ' disabled';
    function toggle(opOn, opOff, value, label, iconName) {
      var on = !!value;
      return '<button class="button control-toggle ' + (on ? 'active' : '') + '" data-command="' + (on ? opOff : opOn) + '" data-slave="' + slave + '" data-value="' + (on ? 0 : 1) + '"' + disabled + '>' +
        icon(iconName) + (on ? '关闭' : '启动') + label + '</button>';
    }
    return '<div class="manual-control-grid">' + toggle('pump_on', 'pump_off', pumpState, '水泵', 'pump') +
      toggle('light_on', 'light_off', lightState, '补光灯', 'lamp') + toggle('fan_on', 'fan_off', fanState, '风扇', 'fan') +
      '<button class="button ' + (alarmEnabled ? 'danger' : 'primary') + '" data-command="set_alarm_enabled" data-slave="' + slave + '" data-value="' + (alarmEnabled ? 0 : 1) + '"' + disabled + '>' + icon('bell') + (alarmEnabled ? '关闭声光报警' : '开启声光报警') + '</button>' +
      '<button class="button warning" data-command="relay_test" data-slave="' + slave + '" data-value="1"' + disabled + '>' + icon('command') + '继电器测试</button>' +
      '<button class="button warning" data-command="alarm_test" data-slave="' + slave + '" data-value="1"' + disabled + '>' + icon('warning') + '声光测试</button></div>';
  }
  function renderDetail() {
    var slave = state.selected; var p = profile(slave);
    if (isVirtual(slave)) return connectionStrip() + '<button class="detail-back" data-nav="overview">' + icon('back') + '返回总览</button><section class="detail-hero status-offline"><div><span class="eyebrow">展示设备</span><h1>' + escapeHtml(p.name) + '</h1><p>' + escapeHtml(p.location) + '</p></div><span class="status-pill offline">暂无硬件通信</span></section><section class="panel"><div class="empty-state">' + icon('route') + '<strong>这是展示设备档案</strong><p>接入实际 WiFi 设备后再启用实时数据和控制。</p></div></section>';
    var item = slaveItem(slave); var t = item.telemetry || null; var status = statusOf(item); var disabled = item.online ? '' : ' disabled'; var values = thresholdValues(slave, t);
    var cards = detailMetric('水位', t && t.waterValid ? number(t.waterPct, 0) : '--', '%', t && t.waterAlarm, 'water') +
      detailMetric('二氧化碳', t && t.co2Valid ? number(t.co2Ppm, 0) : '--', 'ppm', t && t.co2Alarm, 'co2') +
      detailMetric('土壤湿度', t && t.soilValid ? number(t.soilPct, 0) : '--', '%', t && t.soilAlarm, 'soil') +
      detailMetric('环境温度', t && t.sht30Valid ? number(Number(t.tempX10) / 10, 1) : '--', '℃', t && t.tempAlarm, 'temp') +
      detailMetric('环境湿度', t && t.sht30Valid ? number(Number(t.humidityX10) / 10, 1) : '--', '%', t && t.humidityAlarm, 'humidity') +
      detailMetric('光照', t && t.luxAvailable ? number(t.lux, 0) : '--', 'lux', t && t.lightAlarm, 'lux');
    var control = '<section class="panel threshold-panel"><div class="panel-head"><div><h2>当前阈值方案</h2></div></div>' +
      '<div class="field"><select id="threshold-preset" aria-label="当前阈值方案"' + disabled + '>' + presetOptions(slave) + '</select></div>' +
      '<div class="field-grid threshold-grid"><div class="field"><label>水位阈值 %</label><input id="threshold-water" type="number" min="0" max="100" value="' + values.water + '"' + disabled + '></div><div class="field"><label>CO₂阈值 ppm</label><input id="threshold-co2" type="number" min="0" max="50000" value="' + values.co2 + '"' + disabled + '></div><div class="field"><label>土壤湿度阈值 %</label><input id="threshold-soil" type="number" min="0" max="100" value="' + values.soil + '"' + disabled + '></div><div class="field"><label>环境温度阈值 ℃</label><input id="threshold-temp" type="number" step="0.1" value="' + values.temp + '"' + disabled + '></div><div class="field"><label>环境湿度阈值 %</label><input id="threshold-humidity" type="number" step="0.1" value="' + values.humidity + '"' + disabled + '></div><div class="field"><label>光照报警策略</label><select id="threshold-light"' + disabled + '><option value="0"' + (Number(values.light) === 0 ? ' selected' : '') + '>暗光报警</option><option value="1"' + (Number(values.light) === 1 ? ' selected' : '') + '>低光照报警</option><option value="2"' + (Number(values.light) === 2 ? ' selected' : '') + '>高光照报警</option></select></div></div>' +
      '<div class="button-row"><button class="button primary" data-action="save-thresholds" data-slave="' + slave + '"' + disabled + '>' + icon('save') + '保存阈值</button></div></section>';
    var pumpState = effectiveActuatorState(slave, t, 'pumpState');
    var lightState = effectiveActuatorState(slave, t, 'lightState');
    var fanState = effectiveActuatorState(slave, t, 'fanState');
    var manual = '<section class="panel"><div class="panel-head"><div><h2>手动控制</h2></div><span class="eyebrow">执行器</span></div><div class="button-row"><button class="button primary" data-action="manual-control" data-slave="' + slave + '"' + disabled + '>' + icon('command') + '打开手动控制</button></div><div class="info-list actuator-summary"><div class="info-row"><span>水泵</span><strong>' + (pumpState ? '运行中' : '已关闭') + '</strong></div><div class="info-row"><span>补光灯</span><strong>' + (lightState ? '已开启' : '已关闭') + '</strong></div><div class="info-row"><span>风扇</span><strong>' + (fanState ? '运行中' : '已关闭') + '</strong></div></div></section>';
    return connectionStrip() + '<button class="detail-back" data-nav="overview">' + icon('back') + '返回总览</button><section class="detail-hero status-' + status + '"><div><span class="eyebrow">从机 ' + slave + '</span><h1>' + escapeHtml(p.name) + '</h1><p>' + escapeHtml(p.location) + ' · ' + ageText(item.ageMs) + '</p></div><span class="status-pill ' + status + '">' + statusText(status) + '</span></section><div class="detail-metrics">' + cards + '</div>' +
      chartPanel(slave) + '<div class="two-column">' + control + manual + '</div>';
  }
  function chartPanel(slave) {
    return '<section class="panel chart-panel"><div class="panel-head"><div><h2>实时趋势</h2></div><span class="chart-live-dot">实时</span></div><div class="chart-tabs">' +
      Object.keys(METRICS).map(function (key) { return '<button class="' + (state.metric === key ? 'active' : '') + '" data-metric="' + key + '">' + METRICS[key].label + '</button>'; }).join('') +
      '</div><div class="chart-wrap"><canvas id="trend-chart" aria-label="数据趋势"></canvas></div></section>';
  }

  function renderTunnel() {
    var nodes = allNodes().filter(function (n) { return !n.virtual; }).map(function (node) { var item = slaveItem(node.id); var status = statusOf(item); var p = profile(node.id); return '<g class="map-node ' + status + '" data-open-slave="' + node.id + '"><circle class="outer" cx="' + p.mapX + '" cy="' + p.mapY + '" r="18"></circle><circle class="core" cx="' + p.mapX + '" cy="' + p.mapY + '" r="7"></circle><text x="' + (p.mapX + 25) + '" y="' + (p.mapY + 5) + '">' + escapeHtml(p.name) + '</text></g>'; }).join('');
    return connectionStrip() + '<div class="page-heading"><div><span class="eyebrow">农业区域分布</span><h1>农业区域</h1><p>节点状态、区域名称与实时链路一目了然。</p></div><button class="button" data-nav="settings">' + icon('edit') + '维护区域</button></div><section class="panel"><div class="tunnel-map"><svg viewBox="0 0 620 410" role="img" aria-label="农业监测区域图"><path d="M42 205 H578 M145 205 V96 H520 V292 H445 M285 96 V40 M285 292 V370" fill="none" stroke="#b6c9c0" stroke-width="28" stroke-linecap="round"></path><path d="M42 205 H578 M145 205 V96 H520 V292 H445 M285 96 V40 M285 292 V370" fill="none" stroke="#f9fbfa" stroke-width="17" stroke-linecap="round"></path><text x="48" y="187" fill="#71817d" font-size="12">农业主路</text><text x="162" y="84" fill="#71817d" font-size="12">' + escapeHtml(profile(1).location) + '</text><text x="445" y="280" fill="#71817d" font-size="12">' + escapeHtml(profile(2).location) + '</text>' + nodes + '</svg></div><div class="map-legend"><span><i></i>正常</span><span><i class="warning"></i>数据预警</span><span><i class="danger"></i>高危报警</span><span><i class="offline"></i>离线故障</span></div><div class="map-note">点击节点进入详情页，查看历史曲线和控制功能。</div></section>';
  }
  function renderAlerts() {
    var active = realSlaves().filter(function (slave) { var item = slaveItem(slave); return item.online && hasAlarm(item.telemetry); });
    var history = state.history.filter(function (row) { return alarmReasons(row).length; }).sort(function (a, b) { return Number(b.receivedAt) - Number(a.receivedAt); }).slice(0, 5);
    var activeHtml = active.length ? active.map(function (slave) { var t = telemetryFor(slave); return '<div class="alert-item"><span class="alert-icon">' + icon('warning') + '</span><div><strong>' + escapeHtml(profile(slave).name) + ' · 当前报警</strong><small>' + escapeHtml(alarmReasons(t).join('、')) + '超过设定阈值</small></div><button class="button danger" data-command="set_alarm_enabled" data-value="0" data-slave="' + slave + '">关闭声光报警</button></div>'; }).join('') : '<div class="empty-state">' + icon('check') + '<strong>当前没有活动报警</strong><p>在线节点数据均在设定范围内</p></div>';
    return connectionStrip() + '<div class="page-heading"><div><span class="eyebrow">报警处置</span><h1>报警中心</h1><p>这里只显示最近 5 条历史报警。</p></div></div><section class="alert-summary"><div><h2>当前活动报警 ' + active.length + '</h2><p>状态来自设备真实遥测上报</p></div></section><div class="section-title"><h2>待处置</h2><small>在线节点</small></div><div class="alert-list">' + activeHtml + '</div><div class="section-title"><h2>最近历史报警</h2><small>最多 5 条</small></div>' + (history.length ? '<div class="alert-list">' + history.map(function (row) { return '<div class="alert-item"><span class="alert-icon">' + icon('warning') + '</span><div><strong>' + escapeHtml(profile(row.slave).name) + ' · ' + escapeHtml(alarmReasons(row).join('、')) + '异常</strong><small>水位 ' + number(row.waterPct, 0) + '% · CO₂ ' + number(row.co2Ppm, 0) + ' ppm</small></div><time>' + timeText(row.receivedAt) + '</time></div>'; }).join('') + '</div>' : '<div class="empty-state">' + icon('clock') + '<strong>暂无历史报警</strong><p>等待设备上报</p></div>');
  }
  function renderReports() {
    var rows = state.history; var alarms = rows.filter(function (row) { return alarmReasons(row).length; }).length;
    function avg(key, validKey, scale, digits) { var values = rows.filter(function (row) { return !validKey || row[validKey]; }).map(function (row) { return Number(row[key]) / (scale || 1); }).filter(isFinite); return values.length ? number(values.reduce(function (a, b) { return a + b; }, 0) / values.length, digits) : '--'; }
    var nodeRows = allNodes().map(function (node) {
      var item = node.virtual ? { online: false, telemetry: null } : slaveItem(node.id);
      var status = node.virtual ? 'offline' : statusOf(item);
      var latest = item.telemetry || {};
      return '<article class="setting-card" data-open-slave="' + node.id + '"><span class="setting-icon">' + icon(node.virtual ? 'route' : 'pin') + '</span><div><strong>' + escapeHtml(node.name) + '</strong><small>农业区域：' + escapeHtml(node.location) + ' · ' + (node.virtual ? '展示节点' : statusText(status)) + '</small></div><span class="muted">' + (latest.receivedAt ? '最近 ' + timeText(latest.receivedAt) : '暂无实时数据') + '</span></article>';
    }).join('');
    return connectionStrip() + '<div class="page-heading"><div><span class="eyebrow">数据统计</span><h1>数据报表</h1><p>真实遥测历史、报警统计与当前节点状态。</p></div></div><div class="report-grid"><article class="report-card"><h3>监测样本</h3><strong>' + rows.length + '</strong><small>服务器历史 + 本机缓存</small></article><article class="report-card status-danger"><h3>报警样本</h3><strong>' + alarms + '</strong><small>真实报警字段统计</small></article><article class="report-card"><h3>硬件状态</h3><strong>' + (stats().online ? '在线' : '离线') + '</strong><small>当前快照状态</small></article></div><section class="panel report-metrics-panel"><div class="panel-head"><div><h2>环境指标统计</h2><p>来自当前历史样本</p></div><span class="eyebrow">指标</span></div><div class="metric-stat-grid"><article class="metric-stat"><span>水位均值</span><strong>' + avg('waterPct', 'waterValid', 1, 0) + ' %</strong></article><article class="metric-stat"><span>土壤湿度均值</span><strong>' + avg('soilPct', 'soilValid', 1, 0) + ' %</strong></article><article class="metric-stat"><span>CO₂均值</span><strong>' + avg('co2Ppm', 'co2Valid', 1, 0) + ' ppm</strong></article><article class="metric-stat"><span>环境温度均值</span><strong>' + avg('tempX10', 'sht30Valid', 10, 1) + ' ℃</strong></article><article class="metric-stat"><span>环境湿度均值</span><strong>' + avg('humidityX10', 'sht30Valid', 10, 1) + ' %</strong></article><article class="metric-stat"><span>光照均值</span><strong>' + avg('lux', 'luxAvailable', 1, 0) + ' lux</strong></article></div></section><section class="panel"><div class="panel-head"><div><h2>节点区域</h2><p>区域名称修改后会同步显示在报表和其他页面。</p></div></div><div class="setting-list">' + nodeRows + '</div></section>';
  }

  function renderSettings() {
    state.presets = MineStore.presets();
    var nodeCards = allNodes().map(function (node) {
      return '<article class="setting-card"><span class="setting-icon">' + icon(node.virtual ? 'route' : 'pin') + '</span><div><strong>' + escapeHtml(node.name) + '</strong><small>农业区域：' + escapeHtml(node.location) + (node.virtual ? ' · 展示节点' : '') + '</small></div><button class="button" data-action="edit-profile" data-slave="' + node.id + '">' + icon('edit') + '编辑</button>' + (node.virtual ? '<button class="icon-button" data-action="delete-node" data-node-id="' + node.id + '" title="删除展示节点">' + icon('trash') + '</button>' : '') + '</article>';
    }).join('');
    var presets = state.presets.map(function (preset) {
      return '<article class="preset-card"><div><strong>' + escapeHtml(preset.name) + '</strong><small>' + escapeHtml(preset.note || (preset.builtin ? '系统参考初始值' : '用户自定义方案')) + '</small></div><span>水位 ' + preset.water + '% · CO₂ ' + preset.co2 + ' ppm · 土壤 ' + preset.soil + '%</span><div class="button-row"><span class="preset-badge">' + (preset.builtin ? '内置方案' : '自定义方案') + '</span><button class="button" data-action="edit-preset" data-preset-id="' + escapeHtml(preset.id) + '">' + icon('edit') + '编辑</button><button class="button danger" data-action="delete-preset" data-preset-id="' + escapeHtml(preset.id) + '">' + icon('trash') + '删除</button></div></article>';
    }).join('');
    return connectionStrip() + '<div class="page-heading"><div><span class="eyebrow">系统配置</span><h1>设置</h1><p>维护节点档案与农业阈值方案。</p></div></div><section class="panel"><div class="panel-head"><div><h2>检测区域</h2><p>农业区域名称修改后会同步到总览、报警和报表。</p></div></div><div class="setting-list">' + nodeCards + '</div></section><section class="panel"><div class="panel-head"><div><h2>阈值方案</h2><p>一次显示 3 个方案，可上下滑动浏览；内置方案也可编辑或删除。以下作物数值只是参考初始值。</p></div><button class="button primary" data-action="add-preset">' + icon('plus') + '添加预设阈值</button></div><div class="preset-scroll" tabindex="0" aria-label="阈值方案列表">' + presets + '</div></section>';
  }

  function render() {
    var root = $('view-root'); if (!root) return;
    if (state.view === 'detail') root.innerHTML = renderDetail();
    else if (state.view === 'tunnel') root.innerHTML = renderTunnel();
    else if (state.view === 'alerts') root.innerHTML = renderAlerts();
    else if (state.view === 'reports') root.innerHTML = renderReports();
    else if (state.view === 'settings') root.innerHTML = renderSettings();
    else root.innerHTML = renderOverview();
    updateNav(); icons();
    if (state.view === 'overview') window.setTimeout(drawMiniCharts, 0);
    if (state.view === 'detail') window.setTimeout(drawChart, 0);
  }
  function updateNav() {
    document.querySelectorAll('[data-nav]').forEach(function (button) { button.classList.toggle('active', button.getAttribute('data-nav') === state.view); });
    var count = realSlaves().filter(function (slave) { var item = slaveItem(slave); return item.online && hasAlarm(item.telemetry); }).length;
    var badge = $('alert-count'); if (badge) { badge.textContent = String(count); badge.classList.toggle('is-hidden', !count); }
  }

  function canvasSize(canvas, height) {
    var rect = canvas.getBoundingClientRect(); var dpr = Math.min(window.devicePixelRatio || 1, 2); var width = Math.max(160, Math.floor(rect.width || 260));
    canvas.width = width * dpr; canvas.height = height * dpr; return { ctx: canvas.getContext('2d'), width: width, height: height, dpr: dpr };
  }
  function drawMiniCharts() {
    document.querySelectorAll('[data-mini-chart]').forEach(function (canvas) {
      var metric = METRICS[canvas.getAttribute('data-mini-chart')]; var slave = Number(canvas.getAttribute('data-slave')); if (!metric) return;
      var rows = historyFor(slave).slice(-24).map(function (row) { return metric.value(row); }).filter(function (value) { return value !== null && isFinite(Number(value)); }).map(Number);
      var s = canvasSize(canvas, 34); var ctx = s.ctx; ctx.setTransform(s.dpr, 0, 0, s.dpr, 0, 0); ctx.clearRect(0, 0, s.width, s.height);
      ctx.strokeStyle = metric.color; ctx.lineWidth = 2.5; ctx.lineCap = 'round'; ctx.lineJoin = 'round'; ctx.beginPath();
      if (rows.length < 2) { ctx.moveTo(2, s.height / 2); ctx.lineTo(s.width - 2, s.height / 2); ctx.stroke(); return; }
      var min = Math.min.apply(null, rows); var max = Math.max.apply(null, rows); var span = Math.max(1, max - min); min -= span * .1; max += span * .1;
      rows.forEach(function (value, index) { var x = 2 + (s.width - 4) * index / (rows.length - 1); var y = s.height - 4 - (value - min) / (max - min) * (s.height - 8); if (index === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y); }); ctx.stroke();
    });
  }
  function drawChart() {
    var canvas = $('trend-chart'); if (!canvas) return;
    var rows = historyFor(state.selected).slice(-60); var metric = METRICS[state.metric] || METRICS.co2; var s = canvasSize(canvas, 270); var ctx = s.ctx;
    ctx.setTransform(s.dpr, 0, 0, s.dpr, 0, 0); ctx.clearRect(0, 0, s.width, s.height); ctx.fillStyle = '#fbfcfb'; ctx.fillRect(0, 0, s.width, s.height);
    var left = 48; var right = 22; var top = 28; var bottom = 30; var plotW = s.width - left - right; var plotH = s.height - top - bottom;
    ctx.strokeStyle = '#dfeae5'; ctx.lineWidth = 1; ctx.font = '10px Microsoft YaHei, Arial';
    for (var i = 0; i <= 4; i += 1) { var y = top + plotH * i / 4; ctx.beginPath(); ctx.moveTo(left, y); ctx.lineTo(s.width - right, y); ctx.stroke(); ctx.fillStyle = '#81928b'; ctx.textAlign = 'right'; ctx.fillText(number(metric.max * (1 - i / 4), metric.digits), left - 6, y + 3); }
    ctx.fillStyle = metric.color; ctx.textAlign = 'left'; ctx.fillText(metric.label + '（' + metric.unit + '）', left, 15);
    if (rows.length < 2) { ctx.fillStyle = '#81928b'; ctx.textAlign = 'center'; ctx.fillText('等待至少两个有效样本', s.width / 2, s.height / 2); return; }
    ctx.strokeStyle = metric.color; ctx.lineWidth = 3; ctx.beginPath(); var drawing = false;
    rows.forEach(function (row, index) { var value = metric.value(row); if (value === null || !isFinite(Number(value))) { drawing = false; return; } var x = left + plotW * index / (rows.length - 1); var y = top + plotH * (1 - Math.max(0, Math.min(1, value / metric.max))); if (!drawing) { ctx.moveTo(x, y); drawing = true; } else ctx.lineTo(x, y); }); ctx.stroke();
    ctx.fillStyle = '#81928b'; ctx.textAlign = 'left'; ctx.fillText(timeText(rows[0].receivedAt), left, s.height - 8); ctx.textAlign = 'right'; ctx.fillText(timeText(rows[rows.length - 1].receivedAt), s.width - right, s.height - 8);
  }

  function openModal(title, body, confirmText, onConfirm, hideConfirm) {
    var root = $('modal-root'); if (!root) return;
    state.modalConfirm = onConfirm || null;
    root.innerHTML = '<div class="modal-backdrop" data-action="close-modal"><section class="modal" role="dialog" aria-modal="true"><header class="modal-head"><h2>' + escapeHtml(title) + '</h2><button class="icon-button" data-action="close-modal" aria-label="关闭">' + icon('close') + '</button></header><div class="modal-body">' + body + '</div><footer class="modal-foot"><button class="button" data-action="close-modal">取消</button>' + (hideConfirm ? '' : '<button class="button primary" data-action="modal-confirm">' + escapeHtml(confirmText || '确认') + '</button>') + '</footer></section></div>';
    icons();
  }
  function closeModal() { state.modalConfirm = null; state.manualModalSlave = null; if ($('modal-root')) $('modal-root').textContent = ''; }
  function editProfile(id) {
    var p = profile(id);
    openModal('编辑节点信息', '<div class="field"><label>节点名称</label><input id="profile-name" maxlength="24" value="' + escapeHtml(p.name) + '"></div><div class="field"><label>农业区域名称</label><input id="profile-location" maxlength="40" value="' + escapeHtml(p.location) + '"></div>', '保存', function () {
      var name = $('profile-name').value.trim(); var location = $('profile-location').value.trim(); if (!name || !location) { toast('名称不能为空', 'warning'); return; }
      MineStore.saveProfile(id, { name: name, location: location }); closeModal(); render(); toast('节点和农业区域名称已保存');
    });
  }
  function presetForm(id) {
    var preset = state.presets.filter(function (item) { return String(item.id) === String(id); })[0] || { id: 'preset-' + Date.now(), name: '', note: '用户自定义方案', water: 80, co2: 1200, soil: 30, temp: 35, humidity: 80, light: 0 };
    var body = '<div class="field"><label>方案名称</label><input id="preset-name" maxlength="30" value="' + escapeHtml(preset.name) + '"></div><div class="field"><label>说明</label><input id="preset-note" maxlength="60" value="' + escapeHtml(preset.note || '') + '"></div><div class="field-grid"><div class="field"><label>水位 %</label><input id="preset-water" type="number" value="' + preset.water + '"></div><div class="field"><label>CO₂ ppm</label><input id="preset-co2" type="number" value="' + preset.co2 + '"></div><div class="field"><label>土壤湿度 %</label><input id="preset-soil" type="number" value="' + preset.soil + '"></div><div class="field"><label>环境温度 ℃</label><input id="preset-temp" type="number" step="0.1" value="' + preset.temp + '"></div><div class="field"><label>环境湿度 %</label><input id="preset-humidity" type="number" step="0.1" value="' + preset.humidity + '"></div><div class="field"><label>光照策略</label><select id="preset-light"><option value="0"' + (Number(preset.light) === 0 ? ' selected' : '') + '>暗光报警</option><option value="1"' + (Number(preset.light) === 1 ? ' selected' : '') + '>低光照报警</option><option value="2"' + (Number(preset.light) === 2 ? ' selected' : '') + '>高光照报警</option></select></div></div>';
    openModal(id ? '编辑阈值方案' : '添加预设阈值', body, '保存方案', function () {
      var name = $('preset-name').value.trim(); if (!name) { toast('请填写方案名称', 'warning'); return; }
      var item = { id: preset.id, name: name, note: $('preset-note').value.trim() || '用户自定义方案', water: Number($('preset-water').value), co2: Number($('preset-co2').value), soil: Number($('preset-soil').value), temp: Number($('preset-temp').value), humidity: Number($('preset-humidity').value), light: Number($('preset-light').value) };
      if ([item.water, item.co2, item.soil, item.temp, item.humidity, item.light].some(function (v) { return !isFinite(v); })) { toast('方案参数必须是数字', 'warning'); return; }
      MineStore.savePreset(item); state.presets = MineStore.presets(); closeModal(); render(); toast('阈值方案已保存');
    });
  }
  function sendCommand(slave, op, value, options) {
    options = options || {};
    var strict = options.strict !== false;
    if (isVirtual(slave)) return Promise.reject(new Error('展示节点暂无硬件通信'));
    if (!state.connected) { toast('当前没有连接云服务器', 'warning'); return Promise.reject(new Error('未连接')); }
    if (!slaveItem(slave).online) {
      if (strict) { toast('从机 ' + slave + ' 当前离线', 'warning'); return Promise.reject(new Error('节点离线')); }
      toast('从机状态正在同步，已尝试发送命令', 'warning');
    }
    if (strict && state.commandBusy[slave]) { toast('该节点还有命令等待确认', 'warning'); return Promise.reject(new Error('命令进行中')); }
    setActuatorOverride(slave, op, value);
    if (strict) { state.commandBusy[slave] = true; render(); }
    else { updateDetailLive(); }
    refreshManualModal(slave);
    var commandId = null;
    var commandPromise = MineApi.command(slave, op, value).then(function (body) {
      var command = body.command; commandId = command.id; state.jobs[String(command.id)] = { id: command.id, slave: Number(slave), op: op, value: value, status: 'pending', createdAt: Date.now() };
      if (!strict) {
        refreshManualModal(slave);
        waitCommand(command, 8000).then(function () {
          if (state.jobs[String(command.id)]) state.jobs[String(command.id)].status = 'ok';
          return refresh();
        }).then(function () {
          refreshManualModal(slave);
        }).catch(function (error) {
          if (state.jobs[String(command.id)]) state.jobs[String(command.id)].status = error.message === '硬件确认超时' ? 'sent' : 'failed';
          if (error.message !== '硬件确认超时') { clearActuatorOverride(slave, op); toast('命令失败：' + error.message, 'error'); }
          refresh().then(function () { refreshManualModal(slave); }).catch(function () { refreshManualModal(slave); });
        });
        return body;
      }
      render();
      return waitCommand(command);
    });
    if (!strict) {
      return commandPromise.then(function (body) {
        toast('从机 ' + slave + ' 命令已发送', 'success');
        return body;
      }).catch(function (error) {
        clearActuatorOverride(slave, op);
        refreshManualModal(slave);
        if (['未连接', '节点离线', '命令进行中'].indexOf(error.message) < 0) toast('命令发送失败：' + error.message, 'error');
        throw error;
      });
    }
    return commandPromise.then(function () {
      state.commandBusy[slave] = false;
      toast('从机 ' + slave + ' 已确认：' + op, 'success');
      return refresh().then(function () { refreshManualModal(slave); });
    }).catch(function (error) {
      clearActuatorOverride(slave, op);
      state.commandBusy[slave] = false;
      if (commandId !== null && state.jobs[String(commandId)]) state.jobs[String(commandId)].status = 'failed';
      render();
      refreshManualModal(slave);
      if (['未连接', '节点离线', '命令进行中'].indexOf(error.message) < 0) toast('命令失败：' + error.message, 'error');
      throw error;
    });
  }
  function waitCommand(command, timeoutMs) {
    var end = Date.now() + (timeoutMs || MineConfig.commandTimeoutMs);
    function step() {
      if (Date.now() >= end) return Promise.reject(new Error('硬件确认超时'));
      return MineApi.snapshot().then(function (snapshot) {
        state.snapshot = snapshot; var ack = snapshot.ack;
        if (ack && String(ack.id) === String(command.id) && Number(ack.slave) === Number(command.slave)) {
          if (state.jobs[String(command.id)]) state.jobs[String(command.id)].status = ack.result;
          if (ack.result !== 'ok') return Promise.reject(new Error('硬件返回 ' + ack.result));
          return ack;
        }
        return new Promise(function (resolve) { window.setTimeout(resolve, 500); }).then(step);
      });
    }
    return step();
  }
  function saveThresholds(slave) {
    var fields = ['water', 'co2', 'soil', 'temp', 'humidity', 'light']; var values = {};
    fields.forEach(function (key) { var node = $('threshold-' + key); values[key] = node ? Number(node.value) : NaN; });
    if (!isFinite(values.water) || values.water < 0 || values.water > 100 || !isFinite(values.co2) || values.co2 < 0 || !isFinite(values.soil) || values.soil < 0 || values.soil > 100 || !isFinite(values.temp) || !isFinite(values.humidity) || values.humidity < 0 || values.humidity > 100 || !isFinite(values.light) || values.light < 0 || values.light > 2) { toast('阈值范围无效，请检查输入', 'warning'); return; }
    state.thresholdDrafts[String(slave)] = values;
    var commands = [['set_water_threshold', Math.round(values.water)], ['set_co2_threshold', Math.round(values.co2)], ['set_soil_threshold', Math.round(values.soil)], ['set_temperature_threshold', Math.round(values.temp * 10)], ['set_humidity_threshold', Math.round(values.humidity * 10)], ['set_light_alarm_mode', Math.round(values.light)]];
    var chain = Promise.resolve(); commands.forEach(function (command) { chain = chain.then(function () { return sendCommand(slave, command[0], command[1], { strict: true }); }); });
    chain.then(function () { toast('从机 ' + slave + ' 的阈值已逐项确认', 'success'); }).catch(function () {});
  }
  function applyPreset(slave, id) {
    var preset = state.presets.filter(function (item) { return String(item.id) === String(id); })[0]; if (!preset) return;
    state.selectedPreset[String(slave)] = preset;
    state.thresholdDrafts[String(slave)] = { water: preset.water, co2: preset.co2, soil: preset.soil, temp: preset.temp, humidity: preset.humidity, light: preset.light };
    syncThresholdForm(slave);
    toast('已填入“' + preset.name + '”，保存阈值后才会下发硬件');
  }

  function notificationPermissionText() {
    if (window.plus && plus.android) return state.notificationPermission === 'granted' ? '已允许' : '未申请或未允许';
    if (window.Notification) return window.Notification.permission === 'granted' ? '已允许' : (window.Notification.permission === 'denied' ? '未允许' : '未申请');
    return 'APP 内可用';
  }
  function notificationActionText() { return state.notificationPermission === 'denied' ? '打开通知设置' : '申请通知权限'; }
  function notificationCapabilityText() { return window.plus && plus.push ? 'APP 系统通知可用' : (window.Notification ? '浏览器通知能力' : '打包 APP 后可用'); }
  function setupPlusNotifications() {
    if (!window.plus || state.plusReady) return;
    state.plusReady = true;
    try {
      if (plus.push && typeof plus.push.setAutoNotification === 'function') plus.push.setAutoNotification(true);
      if (plus.push && typeof plus.push.addEventListener === 'function') {
        plus.push.addEventListener('click', function (event) {
          var payload = event && event.payload;
          if (payload && typeof payload === 'string') {
            try { payload = JSON.parse(payload); } catch (error) {}
          }
          if (payload && payload.slave) state.selected = Number(payload.slave);
          state.view = 'alerts';
          render();
        });
      }
    } catch (error) {}
    render();
  }
  function openNotificationSettings() {
    if (!window.plus || !plus.android || typeof plus.android.runtimeMainActivity !== 'function') {
      toast('请在手机系统设置中开启本 APP 的通知权限', 'warning');
      return;
    }
    try {
      var activity = plus.android.runtimeMainActivity();
      var Intent = plus.android.importClass('android.content.Intent');
      var intent = new Intent('android.settings.APP_NOTIFICATION_SETTINGS');
      intent.putExtra('android.provider.extra.APP_PACKAGE', activity.getPackageName());
      activity.startActivity(intent);
    } catch (error) {
      toast('无法自动打开通知设置，请手动进入系统设置', 'warning');
    }
  }
  function requestNotificationPermission() {
    if (window.plus && plus.android && typeof plus.android.requestPermissions === 'function') { try { plus.android.requestPermissions(['android.permission.POST_NOTIFICATIONS'], function () { state.notificationPermission = 'granted'; render(); }, function () { state.notificationPermission = 'denied'; render(); }); return; } catch (e) {} }
    if (!window.Notification) return;
    window.Notification.requestPermission().then(function (permission) { state.notificationPermission = permission; render(); });
  }
  function sendSystemNotification(item) {
    try { if (window.plus && plus.push && plus.push.createMessage) { plus.push.createMessage(item.body, JSON.stringify({ type: 'agri-alarm', slave: item.slave }), { title: item.title, cover: true }); return true; } } catch (e) {}
    try { if (window.Notification && window.Notification.permission === 'granted') { new window.Notification(item.title, { body: item.body, tag: item.tag }); return true; } } catch (e2) {}
    return false;
  }
  function alarmNotification(row) {
    var reasons = alarmReasons(row); var values = []; if (row.waterAlarm) values.push('水位 ' + number(row.waterPct, 0) + '%'); if (row.co2Alarm) values.push('CO₂ ' + number(row.co2Ppm, 0) + ' ppm'); if (row.soilAlarm) values.push('土壤湿度 ' + number(row.soilPct, 0) + '%'); if (row.tempAlarm) values.push('环境温度 ' + number(row.tempX10 / 10, 1) + '℃'); if (row.humidityAlarm) values.push('环境湿度 ' + number(row.humidityX10 / 10, 1) + '%'); return { title: currentAppTitle(), body: profile(row.slave).name + ' · ' + (reasons.join('、') || '环境参数') + '异常' + (values.length ? '：' + values.join('，') : ''), tag: 'agri-alarm-' + row.slave, slave: row.slave };
  }
  function notifyAlarm(row) {
    var key = String(row.slave) + ':' + String(row.receivedAt); if (state.notificationLastKeys[key]) return; state.notificationLastKeys[key] = true; var item = alarmNotification(row); toast(item.body, 'error'); sendSystemNotification(item);
  }
  function updateDetailMetric(key, value, bad) {
    var card = document.querySelector('[data-detail-metric="' + key + '"]');
    if (!card) return;
    var strong = card.querySelector('strong');
    if (strong) { strong.textContent = value; strong.classList.toggle('bad', !!bad); }
    card.classList.toggle('metric-danger', !!bad);
  }
  function updateDetailLive() {
    var root = $('view-root'); if (!root || state.view !== 'detail' || isVirtual(state.selected)) { render(); return; }
    var item = slaveItem(state.selected); var t = item.telemetry || null; var status = statusOf(item); var p = profile(state.selected);
    var strip = root.querySelector('.connection-strip'); if (strip) strip.outerHTML = connectionStrip();
    var hero = root.querySelector('.detail-hero');
    if (hero) {
      hero.className = 'detail-hero status-' + status;
      var info = hero.querySelector('p'); if (info) info.textContent = p.location + ' · ' + ageText(item.ageMs);
      var pill = hero.querySelector('.status-pill'); if (pill) { pill.className = 'status-pill ' + status; pill.textContent = statusText(status); }
    }
    updateDetailMetric('water', t && t.waterValid ? number(t.waterPct, 0) : '--', t && t.waterAlarm);
    updateDetailMetric('co2', t && t.co2Valid ? number(t.co2Ppm, 0) : '--', t && t.co2Alarm);
    updateDetailMetric('soil', t && t.soilValid ? number(t.soilPct, 0) : '--', t && t.soilAlarm);
    updateDetailMetric('temp', t && t.sht30Valid ? number(Number(t.tempX10) / 10, 1) : '--', t && t.tempAlarm);
    updateDetailMetric('humidity', t && t.sht30Valid ? number(Number(t.humidityX10) / 10, 1) : '--', t && t.humidityAlarm);
    updateDetailMetric('lux', t && t.luxAvailable ? number(t.lux, 0) : '--', t && t.lightAlarm);
    var actuatorValues = [effectiveActuatorState(state.selected, t, 'pumpState') ? '运行中' : '已关闭', effectiveActuatorState(state.selected, t, 'lightState') ? '已开启' : '已关闭', effectiveActuatorState(state.selected, t, 'fanState') ? '运行中' : '已关闭'];
    document.querySelectorAll('.actuator-summary .info-row strong').forEach(function (node, index) { if (actuatorValues[index]) node.textContent = actuatorValues[index]; });
    updateNav();
    drawChart();
  }
  function updateSettingsLive() {
    var root = $('view-root'); if (!root || state.view !== 'settings') { render(); return; }
    var strip = root.querySelector('.connection-strip'); if (strip) strip.outerHTML = connectionStrip();
    var rows = root.querySelectorAll('.info-list .info-row');
    rows.forEach(function (row) {
      var label = row.querySelector('span');
      var value = row.querySelector('strong');
      if (!label || !value) return;
      if (label.textContent === '云服务器') value.textContent = state.connected ? '已连接' : '连接中';
      if (label.textContent === '通知状态') value.textContent = notificationPermissionText();
      if (label.textContent === '通知能力') value.textContent = notificationCapabilityText();
    });
    updateNav();
  }
  function applySnapshot(snapshot) {
    state.snapshot = snapshot || null; state.connected = !!(snapshot && snapshot.ok !== false); state.lastUpdate = Date.now();
    reconcileActuatorOverrides(snapshot);
    var rows = snapshot && Array.isArray(snapshot.slaves) ? snapshot.slaves.filter(function (item) { return item && item.online && item.telemetry; }).map(function (item) { return item.telemetry; }) : [];
    state.history = MineStore.appendHistory(rows);
    rows.forEach(function (row) {
      // Phone notifications are independent of the device buzzer/alarm-enable switch.
      // The alarm flags already represent validated sensor threshold violations.
      var active = hasSensorAlarm(row); var key = String(row.slave);
      if (active && !state.lastAlarmStates[key]) notifyAlarm(row);
      state.lastAlarmStates[key] = active;
    });
    realSlaves().forEach(function (slave) { if (!rows.some(function (row) { return Number(row.slave) === slave; })) state.lastAlarmStates[String(slave)] = false; });
    if (state.view === 'detail') { updateDetailLive(); return; }
    if (state.view === 'settings') { updateSettingsLive(); return; }
    if (!thresholdFormIsActive()) render();
    else updateNav();
  }
  function refresh() {
    if (state.loading) return Promise.resolve();
    state.loading = true;
    return Promise.all([MineApi.snapshot(), MineApi.history()]).then(function (result) { if (result[1] && Array.isArray(result[1].history)) state.history = MineStore.appendHistory(result[1].history); applySnapshot(result[0]); }).catch(function (error) { state.connected = false; render(); if (state.lastUpdate) toast(error.message, 'error'); }).finally(function () { state.loading = false; });
  }
  function connect() {
    Promise.all([MineApi.health(), MineApi.snapshot(), MineApi.history()]).then(function (result) { if (result[2] && Array.isArray(result[2].history)) state.history = MineStore.appendHistory(result[2].history); applySnapshot(result[1]); openEvents(); startPolling(); }).catch(function (error) { state.connected = false; render(); toast(error.message || '无法连接云服务器', 'error'); startPolling(); });
  }
  function openEvents() {
    if (!window.EventSource) return; if (state.source) state.source.close();
    try { state.source = new EventSource(MineApi.eventsUrl()); state.source.addEventListener('meter', function (event) { try { applySnapshot(JSON.parse(event.data)); } catch (e) {} }); state.source.onerror = function () {}; } catch (e) {}
  }
  function startPolling() { if (state.pollTimer) window.clearInterval(state.pollTimer); state.pollTimer = window.setInterval(function () { if (!document.hidden) refresh(); }, MineConfig.pollMs); }
  function toast(message, type) { var root = $('toast-root'); if (!root) return; var item = document.createElement('div'); item.className = 'toast ' + (type || ''); item.textContent = message; root.appendChild(item); window.setTimeout(function () { if (item.parentNode) item.parentNode.removeChild(item); }, 3500); }

  document.addEventListener('click', function (event) {
    var nav = event.target.closest('[data-nav]'); if (nav) { state.view = nav.getAttribute('data-nav'); closeModal(); render(); return; }
    var open = event.target.closest('[data-open-slave]'); if (open) { state.selected = Number(open.getAttribute('data-open-slave')); state.view = 'detail'; render(); return; }
    var filter = event.target.closest('[data-filter]'); if (filter) { state.filter = filter.getAttribute('data-filter'); render(); return; }
    var metric = event.target.closest('[data-metric]'); if (metric) { state.metric = metric.getAttribute('data-metric'); render(); return; }
    var command = event.target.closest('[data-command]'); if (command) { var op = command.getAttribute('data-command'); var raw = command.getAttribute('data-value'); var value = raw === null ? undefined : Number(raw); var commandSlave = Number(command.getAttribute('data-slave')); sendCommand(commandSlave, op, value).then(function () { refreshManualModal(commandSlave); }).catch(function () {}); return; }
    var action = event.target.closest('[data-action]'); if (!action) return;
    var name = action.getAttribute('data-action');
    if (name === 'refresh') refresh();
    else if (name === 'edit-app-title') editAppTitle();
    else if (name === 'connection') toast(state.connected ? '云服务器已连接，正在同步硬件状态' : '云服务器未连接，正在重试', state.connected ? '' : 'warning');
    else if (name === 'close-modal') { if (event.target === action || action.matches('button')) closeModal(); }
    else if (name === 'modal-confirm') { if (state.modalConfirm) state.modalConfirm(); }
    else if (name === 'edit-profile') editProfile(Number(action.getAttribute('data-slave')));
    else if (name === 'delete-node') { render(); }
    else if (name === 'add-preset') presetForm();
    else if (name === 'edit-preset') presetForm(action.getAttribute('data-preset-id'));
    else if (name === 'delete-preset') { var pid = action.getAttribute('data-preset-id'); if (window.confirm('确定删除这个阈值方案吗？删除后可重新添加同名方案。')) { MineStore.removePreset(pid); Object.keys(state.selectedPreset).forEach(function (slave) { if (String(state.selectedPreset[slave].id) === String(pid)) delete state.selectedPreset[slave]; }); state.presets = MineStore.presets(); render(); toast('阈值方案已删除'); } }
    else if (name === 'manual-control') { state.manualModalSlave = Number(action.getAttribute('data-slave')); openModal('手动控制', renderManualControl(state.manualModalSlave), '关闭', closeModal, true); }
    else if (name === 'save-thresholds') saveThresholds(Number(action.getAttribute('data-slave')));
    else if (name === 'notification-permission') { if (state.notificationPermission === 'denied') openNotificationSettings(); else requestNotificationPermission(); }
    else if (name === 'notification-test') { sendSystemNotification({ title: currentAppTitle(), body: currentAppTitle() + '通知功能正常。', tag: 'agri-test' }); toast('已发送通知测试'); }
    else if (name === 'clear-history') { MineStore.clearHistory(); state.history = []; render(); toast('本机历史已清空'); }
  });
  document.addEventListener('change', function (event) {
    var preset = event.target.closest('#threshold-preset'); if (preset) { applyPreset(state.selected, preset.value); return; }
  });
  document.addEventListener('plusready', setupPlusNotifications, false);
  window.addEventListener('plusready', setupPlusNotifications, false);
  if (window.plus) setupPlusNotifications();
  window.addEventListener('resize', function () { if (state.view === 'overview') drawMiniCharts(); if (state.view === 'detail') drawChart(); });

  syncAppTitle();
  render();
  connect();
}(window, document));
