(function (window) {
  'use strict';
  window.MineConfig = Object.freeze({
    apiBase: 'http://server.example.com:18127',
    deviceId: 'WIFI_AGRI_001',
    adminToken: 'REPLACE_WITH_WIFI_AGRI_ADMIN_TOKEN',
    pollMs: 3000,
    commandTimeoutMs: 15000,
    historyLimit: 1000
  });
}(window));
