/**
 * 用和真机相同的 MQTT 客户端连接公共 EMQX。
 * 只在临时主题上自发自收，不向 sleepfan/status 或 sleepfan/cmd 发布。
 * 运行：node tests/live-broker.js
 */

const mem = {}
global.wx = {
  getStorageSync: function (key) { return Object.prototype.hasOwnProperty.call(mem, key) ? mem[key] : '' },
  setStorageSync: function (key, value) { mem[key] = value },
  removeStorageSync: function (key) { delete mem[key] }
}

const store = require('../utils/store')
const config = require('../utils/config')
const mqtt = require('../utils/mqtt')

function delay(ms) {
  return new Promise(function (resolve) { setTimeout(resolve, ms) })
}

function waitFor(fn, ms, label) {
  const start = Date.now()
  return new Promise(function (resolve, reject) {
    const tick = function () {
      if (fn()) {
        resolve()
        return
      }
      if (Date.now() - start > ms) {
        reject(new Error(label || 'timeout'))
        return
      }
      setTimeout(tick, 50)
    }
    tick()
  })
}

function nodeConnectSocket(opts) {
  const ws = new WebSocket(opts.url, opts.protocols || ['mqtt'])
  ws.binaryType = 'arraybuffer'
  const handlers = {}
  ws.addEventListener('open', function () {
    if (handlers.open) handlers.open()
  })
  ws.addEventListener('message', function (ev) {
    if (handlers.message) handlers.message({ data: ev.data })
  })
  ws.addEventListener('error', function () {
    if (handlers.error) handlers.error({ errMsg: 'websocket error' })
  })
  ws.addEventListener('close', function () {
    if (handlers.close) handlers.close()
  })
  return {
    onOpen: function (fn) { handlers.open = fn },
    onMessage: function (fn) { handlers.message = fn },
    onError: function (fn) { handlers.error = fn },
    onClose: function (fn) { handlers.close = fn },
    send: function (opts) {
      try {
        ws.send(opts.data)
      } catch (err) {
        if (opts.fail) opts.fail(err)
      }
    },
    close: function (opts) {
      try { ws.close() } catch (err) {}
      if (opts && opts.success) opts.success()
    }
  }
}

async function main() {
  if (typeof WebSocket !== 'function') throw new Error('当前 Node 没有 WebSocket')
  const topic = 'sleepfan-selftest/' + Date.now().toString(16) + Math.random().toString(16).slice(2)
  config.load()
  config.save({
    brokerUrl: 'wss://broker.emqx.io:8084/mqtt',
    statusTopic: topic,
    cmdTopic: 'sleepfan/cmd',
    clientIdFilter: 'sleepfan-',
    mqttClientId: 'wxsf-live' + Date.now().toString(16)
  })
  store.clearStatus()
  store.clearHistory()

  const logs = []
  const client = mqtt.createMqtt({
    connectSocket: nodeConnectSocket,
    connectTimeout: 8000,
    pingInterval: 20000,
    initialBackoff: 500,
    getConfig: function () { return config.get() },
    onConn: function (status, detail) {
      logs.push(status + (detail ? ' ' + detail : ''))
      console.log('[live] conn', status, detail || '')
    },
    onStatus: function (raw) { store.ingestStatus(raw) }
  })

  const hard = setTimeout(function () {
    console.error('[live] 总超时', logs.join(' | '))
    client.disconnect()
    process.exit(1)
  }, 20000)

  try {
    client.start()
    await waitFor(function () { return client.isSubscribed() }, 8000, '没有订上 ' + topic + ' / ' + logs.join(' | '))
    console.log('[live] subscribed', topic)

    client.publish(topic, {
      client_id: 'other-device',
      temperature: 1,
      fan_level: 1
    })
    await delay(300)
    client.publish(topic, {
      client_id: 'sleepfan-test',
      temperature: 26.5,
      humidity: 40,
      temp_hum_ok: true,
      presence: true,
      distance_cm: 120,
      radar_online: true,
      sleeping: false,
      sleep_state: 'awake',
      fan_level: 3,
      duty: 60,
      auto_mode: true,
      decision_level: 3,
      decision_reason: 'mild 26.5C',
      mq135_raw: 1500,
      uptime_ms: 123456
    })

    await waitFor(function () {
      const status = store.getState().status
      return status && status.client_id === 'sleepfan-test'
    }, 8000, '没有收到自发自收的状态，日志：' + logs.join(' | '))

    const state = store.getState()
    if (state.status.temperature !== 26.5) throw new Error('温度不对 ' + state.status.temperature)
    if (state.status.fan_level !== 3) throw new Error('档位不对')
    if (!state.history.length) throw new Error('趋势没有记点')
    if (state.ignoredCount < 1) {
      console.log('[live] other-device 没有回到订阅端，过滤另由 unit test 覆盖')
    }
    console.log('[live] roundtrip ok, ignored', state.ignoredCount, 'history', state.history.length)
  } finally {
    clearTimeout(hard)
    client.disconnect()
    await delay(200)
  }
}

main().catch(function (err) {
  console.error(err)
  process.exit(1)
})
