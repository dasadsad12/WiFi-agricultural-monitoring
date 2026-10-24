(function (window) {
  'use strict';

  function ApiError(message, status, network) {
    this.name = 'AgriApiError';
    this.message = message || '网络请求失败';
    this.status = status || 0;
    this.network = !!network;
  }
  ApiError.prototype = Object.create(Error.prototype);
  ApiError.prototype.constructor = ApiError;

  function query(values) {
    return Object.keys(values || {}).filter(function (key) {
      return values[key] !== undefined && values[key] !== null && values[key] !== '';
    }).map(function (key) {
      return encodeURIComponent(key) + '=' + encodeURIComponent(values[key]);
    }).join('&');
  }

  function url(path, values) {
    var params = Object.assign({ did: MineConfig.deviceId, token: MineConfig.adminToken }, values || {});
    return MineConfig.apiBase.replace(/\/$/, '') + path + '?' + query(params);
  }

  function get(path, values, timeout) {
    return new Promise(function (resolve, reject) {
      var xhr = new XMLHttpRequest();
      var settled = false;
      var timer = window.setTimeout(function () {
        if (settled) return;
        settled = true;
        xhr.abort();
        reject(new ApiError('请求超时，请检查网络连接', 0, true));
      }, timeout || 8000);
      function finish(fn, value) {
        if (settled) return;
        settled = true;
        window.clearTimeout(timer);
        fn(value);
      }
      try { xhr.open('GET', url(path, values), true); } catch (error) {
        finish(reject, new ApiError('无法建立网络连接', 0, true));
        return;
      }
      xhr.setRequestHeader('Accept', 'application/json');
      xhr.onreadystatechange = function () {
        if (xhr.readyState !== 4) return;
        var body = {};
        try { body = xhr.responseText ? JSON.parse(xhr.responseText) : {}; } catch (error) {}
        if (xhr.status >= 200 && xhr.status < 300 && body.ok !== false) {
          finish(resolve, body);
        } else {
          finish(reject, new ApiError(body.message || ('服务器返回 ' + xhr.status), xhr.status, xhr.status === 0));
        }
      };
      xhr.onerror = function () { finish(reject, new ApiError('无法连接监测服务', 0, true)); };
      xhr.ontimeout = function () { finish(reject, new ApiError('请求超时', 0, true)); };
      try { xhr.send(); } catch (error) { finish(reject, new ApiError('网络请求失败', 0, true)); }
    });
  }

  window.MineApi = {
    ApiError: ApiError,
    health: function () { return get('/health', {}, 6000); },
    snapshot: function () { return get('/lora/api/snapshot', {}, 8000); },
    history: function () { return get('/lora/api/history', {}, 10000); },
    command: function (slave, op, value) {
      var values = { slave: slave, op: op };
      if (value !== undefined && value !== null && value !== '') values.value = value;
      return get('/lora/api/command', values, 8000);
    },
    eventsUrl: function (cursor) {
      return url('/lora/events', cursor ? { cursor: cursor } : {});
    }
  };
}(window));
