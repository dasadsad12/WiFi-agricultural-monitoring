(function (window) {
  'use strict';

  var HISTORY_KEY = 'lora-agri-v4-telemetry-history';
  var PROFILE_KEY = 'lora-agri-v4-node-profiles';
  var EXTRA_NODE_KEY = 'lora-agri-v4-extra-nodes';
  var PRESET_KEY = 'lora-agri-v4-threshold-presets';
  var PRESET_DELETED_KEY = 'lora-agri-v4-deleted-threshold-presets';
  var DEFAULT_PROFILES = {
    1: { id: 1, name: '监测设备', location: '一号生态园', mapX: 145, mapY: 154, virtual: false }
  };
  var DEFAULT_PRESETS = [
    { id: 'default', name: '默认阈值方案', builtin: true, note: '通用参考初始值', water: 80, co2: 1200, soil: 30, temp: 35, humidity: 80, light: 0 },
    { id: 'peanut', name: '花生种植方案', builtin: true, note: '参考初始值，需按现场校准', water: 70, co2: 1500, soil: 35, temp: 32, humidity: 75, light: 0 },
    { id: 'rice', name: '水稻育苗方案', builtin: true, note: '参考初始值，需按现场校准', water: 85, co2: 1500, soil: 45, temp: 30, humidity: 85, light: 0 },
    { id: 'tomato', name: '番茄种植方案', builtin: true, note: '参考初始值，需按现场校准', water: 75, co2: 1200, soil: 40, temp: 30, humidity: 75, light: 0 },
    { id: 'pepper', name: '辣椒种植方案', builtin: true, note: '参考初始值，需按现场校准', water: 75, co2: 1300, soil: 35, temp: 32, humidity: 70, light: 0 },
    { id: 'cucumber', name: '黄瓜种植方案', builtin: true, note: '参考初始值，需按现场校准', water: 80, co2: 1200, soil: 40, temp: 30, humidity: 80, light: 0 }
  ];

  function read(key, fallback) {
    try {
      var value = window.localStorage.getItem(key);
      return value ? JSON.parse(value) : fallback;
    } catch (error) { return fallback; }
  }
  function write(key, value) {
    try { window.localStorage.setItem(key, JSON.stringify(value)); } catch (error) {}
  }
  function clone(value) {
    try { return JSON.parse(JSON.stringify(value)); } catch (error) { return value; }
  }
  function profiles() {
    var saved = read(PROFILE_KEY, {});
    var result = {
      1: Object.assign({}, DEFAULT_PROFILES[1], saved[1] || {}),
      2: Object.assign({}, DEFAULT_PROFILES[2], saved[2] || {})
    };
    read(EXTRA_NODE_KEY, []).forEach(function (node) {
      if (!node || node.id === undefined) return;
      result[String(node.id)] = Object.assign({ virtual: true }, node);
    });
    return result;
  }
  function saveProfile(slave, patch) {
    var saved = read(PROFILE_KEY, {});
    saved[String(slave)] = Object.assign({}, saved[String(slave)] || {}, patch || {});
    write(PROFILE_KEY, saved);
    return profiles()[String(slave)];
  }
  function nodes() {
    return Object.keys(profiles()).map(function (key) { return profiles()[key]; }).sort(function (a, b) {
      return Number(a.id) - Number(b.id);
    });
  }
  function saveNode(node) {
    var list = read(EXTRA_NODE_KEY, []).filter(function (item) { return String(item.id) !== String(node.id); });
    list.push(Object.assign({ virtual: true }, node));
    write(EXTRA_NODE_KEY, list);
    return profiles()[String(node.id)] || node;
  }
  function removeNode(id) {
    write(EXTRA_NODE_KEY, read(EXTRA_NODE_KEY, []).filter(function (item) { return String(item.id) !== String(id); }));
  }
  function history() {
    var value = read(HISTORY_KEY, []);
    return Array.isArray(value) ? value : [];
  }
  function appendHistory(rows) {
    var current = history();
    (rows || []).forEach(function (row) {
      if (!row || row.slave === undefined || row.receivedAt === undefined) return;
      var exists = current.some(function (item) {
        return String(item.slave) === String(row.slave) &&
          String(item.receivedAt) === String(row.receivedAt);
      });
      if (!exists) current.push(clone(row));
    });
    current.sort(function (a, b) { return Number(a.receivedAt) - Number(b.receivedAt); });
    current = current.slice(-Number(MineConfig.historyLimit || 600));
    write(HISTORY_KEY, current);
    return current;
  }
  function clearHistory() {
    try { window.localStorage.removeItem(HISTORY_KEY); } catch (error) {}
  }
  function presets() {
    var custom = read(PRESET_KEY, []);
    var deleted = read(PRESET_DELETED_KEY, []);
    if (!Array.isArray(deleted)) deleted = [];
    var result = DEFAULT_PRESETS.filter(function (preset) {
      return deleted.indexOf(String(preset.id)) < 0;
    }).map(clone);
    (Array.isArray(custom) ? custom : []).forEach(function (item) {
      if (!item || !item.id) return;
      if (deleted.indexOf(String(item.id)) >= 0) return;
      var index = result.findIndex(function (preset) { return String(preset.id) === String(item.id); });
      if (index >= 0) result[index] = Object.assign({}, result[index], item, { builtin: result[index].builtin });
      else result.push(Object.assign({ builtin: false }, item));
    });
    return result;
  }
  function savePreset(preset) {
    var list = read(PRESET_KEY, []);
    var item = Object.assign({ builtin: false }, preset || {});
    var index = list.findIndex(function (current) { return String(current.id) === String(item.id); });
    if (index >= 0) list[index] = item; else list.push(item);
    write(PRESET_KEY, list);
    write(PRESET_DELETED_KEY, read(PRESET_DELETED_KEY, []).filter(function (id) {
      return String(id) !== String(item.id);
    }));
    return presets().filter(function (current) { return String(current.id) === String(item.id); })[0] || item;
  }
  function removePreset(id) {
    write(PRESET_KEY, read(PRESET_KEY, []).filter(function (item) { return String(item.id) !== String(id); }));
    var deleted = read(PRESET_DELETED_KEY, []);
    if (deleted.indexOf(String(id)) < 0) deleted.push(String(id));
    write(PRESET_DELETED_KEY, deleted);
  }

  window.MineStore = {
    profiles: profiles,
    saveProfile: saveProfile,
    nodes: nodes,
    saveNode: saveNode,
    removeNode: removeNode,
    history: history,
    appendHistory: appendHistory,
    clearHistory: clearHistory,
    presets: presets,
    savePreset: savePreset,
    removePreset: removePreset,
    clone: clone
  };
}(window));
