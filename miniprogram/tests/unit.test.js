/**
 * 不启动微信开发者工具的本地检查：报文编解码、过滤、首页文案、MQTT 状态机。
 * 运行：node tests/unit.test.js
 */

const mem = {}
global.wx = {
  getStorageSync: function (key) { return Object.prototype.hasOwnProperty.call(mem, key) ? mem[key] : '' },
  setStorageSync: function (key, value) { mem[key] = value },
  removeStorageSync: function (key) { delete mem[key] }
}

const codec = require('../utils/mqtt-codec')
const format = require('../utils/format')
const chart = require('../utils/chart')
const config = require('../utils/config')
const store = require('../utils/store')
const mqtt = require('../utils/mqtt')

let passed = 0
let failed = 0

function eq(actual, expected, msg) {
  if (actual !== expected) {
    failed += 1
    console.error('FAIL:', msg, '\n  expected:', expected, '\n  actual:  ', actual)
  } else {
    passed += 1
  }
}

function ok(cond, msg) {
  if (!cond) {
    failed += 1
    console.error('FAIL:', msg)
  } else {
    passed += 1
  }
}

function hex(bytes) {
  let out = ''
  for (let i = 0; i < bytes.length; i++) {
    const h = bytes[i].toString(16)
    out += (h.length < 2 ? '0' : '') + h
  }
  return out
}

function toAb(bytes) {
  const buffer = new ArrayBuffer(bytes.byteLength)
  new Uint8Array(buffer).set(bytes)
  return buffer
}

function delay(ms) {
  return new Promise(function (resolve) { setTimeout(resolve, ms) })
}

function waitFor(fn, ms) {
  const start = Date.now()
  return new Promise(function (resolve, reject) {
    const tick = function () {
      let good = false
      try {
        good = !!fn()
      } catch (err) {
        reject(err)
        return
      }
      if (good) {
        resolve()
        return
      }
      if (Date.now() - start > ms) {
        reject(new Error('timeout'))
        return
      }
      setTimeout(tick, 10)
    }
    tick()
  })
}

function createHarness() {
  const sockets = []
  function connectSocket() {
    const handlers = {}
    const sent = []
    const api = {
      onOpen: function (fn) { handlers.open = fn },
      onMessage: function (fn) { handlers.message = fn },
      onError: function (fn) { handlers.error = fn },
      onClose: function (fn) { handlers.close = fn },
      send: function (opts) {
        const bytes = new Uint8Array(opts.data)
        sent.push(bytes)
        const type = bytes[0] >> 4
        if (type === 1) {
          setTimeout(function () {
            if (handlers.message) handlers.message({ data: toAb(new Uint8Array([0x20, 0x02, 0x00, 0x00])) })
          }, 0)
        } else if (type === 8) {
          setTimeout(function () {
            const ack = new Uint8Array([0x90, 0x03, bytes[2], bytes[3], 0x00])
            if (handlers.message) handlers.message({ data: toAb(ack) })
          }, 0)
        } else if (type === 12) {
          setTimeout(function () {
            if (handlers.message) handlers.message({ data: toAb(new Uint8Array([0xd0, 0x00])) })
          }, 0)
        }
      },
      close: function (opts) {
        if (handlers.close) handlers.close()
        if (opts && opts.success) opts.success()
      }
    }
    const sock = {
      sent: sent,
      fireOpen: function () { if (handlers.open) handlers.open() },
      fireClose: function () { if (handlers.close) handlers.close() },
      push: function (bytes) { if (handlers.message) handlers.message({ data: toAb(bytes) }) }
    }
    sockets.push(sock)
    return api
  }
  return { connectSocket: connectSocket, sockets: sockets }
}

function testCodec() {
  eq(hex(codec.encodeConnect('abc', 60)), '100f00044d5154540402003c0003616263', 'CONNECT 固定字节')
  eq(hex(codec.encodeSubscribe(1, 't', 0)), '8206000100017400', 'SUBSCRIBE')
  eq(hex(codec.encodePublish('t', codec.utf8Encode('hi'), 0, 0)), '30050001746869', 'PUBLISH QoS0')
  eq(hex(codec.encodePingreq()), 'c000', 'PINGREQ')
  eq(hex(codec.encodeDisconnect()), 'e000', 'DISCONNECT')
  eq(hex(codec.encodePuback(7)), '40020007', 'PUBACK')

  const text = '{"decision_reason":"偏热 26.5℃"}'
  eq(codec.utf8Decode(codec.utf8Encode(text)), text, 'UTF-8 往返')

  const payload = codec.utf8Encode(text)
  const packet = codec.encodePublish('sleepfan/status', payload, 0, 0)
  let rx = new Uint8Array(0)
  const found = []
  for (let i = 0; i < packet.length; i++) {
    const merged = codec.concat([rx, packet.subarray(i, i + 1)])
    const decoded = codec.decodePackets(merged)
    for (let j = 0; j < decoded.packets.length; j++) found.push(decoded.packets[j])
    rx = merged.slice(decoded.consumed)
  }
  eq(found.length, 1, '拆包后只得到一帧')
  eq(found[0].type, 'publish', '拆包类型')
  eq(found[0].topic, 'sleepfan/status', '拆包主题')
  eq(codec.utf8Decode(found[0].payload), text, '拆包正文')

  const big = codec.encodePublish('a', new Uint8Array(180), 0, 0)
  const both = codec.concat([big, new Uint8Array([0xd0, 0x00])])
  const multi = codec.decodePackets(both)
  eq(multi.packets.length, 2, '一帧里的两个报文')
  eq(multi.packets[1].type, 'pingresp', '第二个是 PINGRESP')
  eq(multi.consumed, both.length, '全部消费')

  const partial = codec.decodePackets(packet.subarray(0, 4))
  eq(partial.packets.length, 0, '半包不产出报文')
  eq(partial.consumed, 0, '半包不前进')

  const connack = codec.decodePackets(new Uint8Array([0x20, 0x02, 0x00, 0x00]))
  eq(connack.packets[0].returnCode, 0, 'CONNACK 成功码')
}

function testViewModel() {
  config.load()
  config.save({
    brokerUrl: config.DEFAULTS.brokerUrl,
    statusTopic: 'sleepfan/status',
    cmdTopic: 'sleepfan/cmd',
    clientIdFilter: 'sleepfan-',
    mqttClientId: config.get().mqttClientId
  })
  ok(/^wxsf-/.test(config.get().mqttClientId), '小程序 Client ID 使用 wxsf-')
  eq(config.validate({ brokerUrl: 'broker.emqx.io', statusTopic: 'a', cmdTopic: 'b', mqttClientId: 'wxsf-1' }), 'Broker 需以 ws:// 或 wss:// 开头', '拒绝没有协议的 Broker')
  eq(config.validate({
    brokerUrl: 'wss://broker.emqx.io:8084/mqtt',
    statusTopic: 'sleepfan/status',
    cmdTopic: 'sleepfan/cmd',
    mqttClientId: 'sleepfan-phone'
  }), 'Client ID 不要用 sleepfan 开头，那是设备的', '拒绝占用设备 Client ID')

  store.clearStatus()
  store.clearHistory()
  store.setConn('connected', '')
  let vm = format.buildMonitor(store.getState(), config.get(), Date.now())
  eq(vm.emptyTitle, '等待设备上报…', '无数据空状态')
  eq(vm.hasData, false, '无数据标记')
  eq(vm.connText, '已连接', '连接文案')

  const ignored = store.ingestStatus({ client_id: 'other-device', temperature: 30, fan_level: 1 })
  eq(ignored.reason, 'filtered', '过滤其他设备')
  eq(store.getState().status, null, '被过滤的帧不进入首页')

  const sample = {
    client_id: 'sleepfan-1234abcd',
    temperature: 26.5,
    humidity: 40,
    temp_hum_ok: false,
    presence: true,
    distance_cm: 120,
    radar_online: false,
    sleeping: false,
    sleep_state: 'awake',
    fan_level: -1,
    duty: 60,
    auto_mode: true,
    decision_level: 3,
    decision_reason: 'mild 26.5C',
    mq135_raw: 1500,
    uptime_ms: 123456
  }
  const accepted = store.ingestStatus(sample)
  eq(accepted.ok, true, '匹配过滤的帧被接受')
  eq(store.getHistory().length, 1, '写入一条趋势')
  eq(store.getHistory()[0].fan_level, null, '自定义档位不画进档位曲线')
  eq(config.get().lastDeviceId, 'sleepfan-1234abcd', '记住最近设备')

  vm = format.buildMonitor(store.getState(), config.get(), Date.now())
  eq(vm.sensorFault, true, '传感器异常')
  eq(vm.tempText, '26.5', '温度保留一位')
  eq(vm.humText, '40.0', '湿度保留一位')
  eq(vm.presenceText, '有人', '雷达有人')
  eq(vm.radarOnlineText, '离线', '雷达离线仍展示原始字段')
  eq(vm.distanceText, '120', '距离')
  eq(vm.sleepLabel, '清醒', 'sleep_state')
  eq(vm.fanLabel, '自定义', 'fan_level -1')
  eq(vm.fanCustom, true, '自定义标记')
  eq(vm.dutyText, '60', '占空比')
  eq(vm.dutyWidth, 60, '占空比条宽度')
  eq(vm.reason, 'mild 26.5C', '决策原因原样展示')
  eq(vm.mqText, '1500', 'MQ135')
  eq(vm.uptimeText, '2分3秒', '运行时间')
  ok(vm.ignoredText.indexOf('other-device') >= 0, '提示被忽略的设备')

  store.ingestStatus({
    client_id: 'sleepfan-1234abcd',
    temperature: 27,
    humidity: 41,
    fan_level: 3,
    duty: 60,
    temp_hum_ok: true,
    presence: false,
    radar_online: true,
    sleeping: true,
    sleep_state: 'deep',
    auto_mode: false,
    decision_level: 3,
    decision_reason: 'steady',
    mq135_raw: 1400
  })
  vm = format.buildMonitor(store.getState(), config.get(), Date.now())
  eq(vm.fanLabel, '3 档', '3 档文案')
  eq(vm.sensorFault, false, '传感器恢复')
  eq(vm.sleepLabel, '深睡', '深睡映射')
  eq(vm.fanCells[0].active, false, '0 档不是当前档')
  eq(vm.fanCells[0].lit, false, '0 不点亮速度格')
  eq(vm.fanCells[3].active, true, '3 档高亮')
  eq(vm.fanCells[3].lit, true, '3 档点亮')
  eq(vm.fanCells[4].lit, false, '4 档不亮')
  eq(vm.decisionExtra, '', '决策档位和风扇档位相同则不重复')

  const zero = format.fanCells(0)
  eq(zero[0].active, true, '关闭时选中 0')
  eq(zero[1].lit, false, '关闭时没有速度格')

  store.getState().lastUpdate = Date.now() - 31000
  vm = format.buildMonitor(store.getState(), config.get(), Date.now())
  eq(vm.stale, true, '超过 30 秒标记为没有新报文')

  const scale = chart.computeScale([26, 27], 'temperature')
  ok(scale.min < 26 && scale.max > 27, '温度轴留白')
  eq(chart.computeScale([10], 'humidity').max, 100, '湿度轴固定 0-100')
  eq(chart.computeScale([null, null], 'fan_level').max, 5, '档位轴固定 0-5')
  const stats = chart.statsOf([{ value: null }, { value: 3 }, { value: 1 }])
  eq(stats.count, 2, '统计跳过空点')
  eq(stats.last, 1, '最新有效值')
  eq(stats.min, 1, '最小值')
}

async function testClient() {
  const harness = createHarness()
  const seen = []
  const conn = []
  const client = mqtt.createMqtt({
    connectSocket: harness.connectSocket,
    connectTimeout: 8000,
    pingInterval: 60000,
    initialBackoff: 40,
    closeGrace: 20,
    getConfig: function () {
      return {
        brokerUrl: 'wss://broker.emqx.io:8084/mqtt',
        statusTopic: 'sleepfan/status',
        cmdTopic: 'sleepfan/cmd',
        clientIdFilter: '',
        mqttClientId: 'wxsf-unit'
      }
    },
    onConn: function (status, detail) { conn.push(status + ':' + detail) },
    onStatus: function (raw) { seen.push(raw) }
  })

  const early = client.publish('sleepfan/cmd', { cmd: 'set_fan_level', level: 1 })
  eq(early.ok, false, '未连接时不假装已发送')

  try {
    client.start()
    eq(harness.sockets.length, 1, '发起一次连接')
    harness.sockets[0].fireOpen()
    await waitFor(function () { return client.isSubscribed() }, 1000)
    eq(client.isConnected(), true, 'CONNACK 后已连接')
    const sent = harness.sockets[0].sent
    ok(sent[0][0] === 0x10, '第一帧是 CONNECT')
    ok(sent.some(function (bytes) { return bytes[0] === 0x82 }), '发送了 SUBSCRIBE')

    const bad = codec.encodePublish('sleepfan/status', codec.utf8Encode('not-json'), 0, 0)
    harness.sockets[0].push(bad)
    const qos1 = codec.encodePublish('sleepfan/status', codec.utf8Encode('{"temperature":26.5,"fan_level":3}'), 1, 9)
    harness.sockets[0].push(qos1)
    await delay(20)
    eq(seen.length, 1, '非法 JSON 被丢掉，合法状态留下')
    eq(seen[0].temperature, 26.5, '状态温度')
    ok(sent.some(function (bytes) { return bytes[0] === 0x40 && bytes[2] === 0 && bytes[3] === 9 }), 'QoS1 回了 PUBACK')

    const published = client.publish('sleepfan/cmd', { cmd: 'set_auto', auto: true })
    eq(published.ok, true, '连接后可以发布')
    eq(published.payload, '{"cmd":"set_auto","auto":true}', '自动模式报文')
    const level = client.publish('sleepfan/cmd', { cmd: 'set_fan_level', level: 3 })
    eq(level.payload, '{"cmd":"set_fan_level","level":3}', '档位报文')
    const osc = client.publish('sleepfan/cmd', { cmd: 'oscillate', action: 'toggle' })
    eq(osc.payload, '{"cmd":"oscillate","action":"toggle"}', '摇头切换报文')

    const before = harness.sockets.length
    harness.sockets[0].fireClose()
    await waitFor(function () { return harness.sockets.length > before }, 1000)
    eq(harness.sockets.length, 2, '断线后重连')
    ok(conn.some(function (item) { return item.indexOf('reconnecting') === 0 }), '状态变为重连中')
  } finally {
    client.disconnect()
  }
}

async function main() {
  testCodec()
  testViewModel()
  await testClient()
  if (failed) {
    console.error(failed + ' failed, ' + passed + ' passed')
    process.exit(1)
  }
  console.log(passed + ' passed')
}

main().catch(function (err) {
  console.error(err)
  process.exit(1)
})
