/**
 * 用 wx.connectSocket 跑 MQTT over WebSocket。
 * 公共 Broker：wss://broker.emqx.io:8084/mqtt ，子协议 mqtt。
 * publish() 只把报文交给 Broker，不代表固件已经执行。
 */

const codec = require('./mqtt-codec')
const config = require('./config')
const store = require('./store')

const CONNACK_TEXT = [
  '已接受',
  '不支持的协议版本',
  'Client ID 被拒绝',
  '服务不可用',
  '用户名或密码错误',
  '未授权'
]

function connackText(code) {
  if (code >= 0 && code < CONNACK_TEXT.length) return CONNACK_TEXT[code]
  return '连接被拒绝（' + code + '）'
}

function toArrayBuffer(bytes) {
  const buffer = new ArrayBuffer(bytes.byteLength)
  new Uint8Array(buffer).set(bytes)
  return buffer
}

function createMqtt(options) {
  const connectSocket = options.connectSocket
  const onStatus = options.onStatus
  const onConn = options.onConn
  const getConfig = options.getConfig
  const setTimer = options.setTimer || function (fn, ms) { return setTimeout(fn, ms) }
  const clearTimer = options.clearTimer || function (id) { clearTimeout(id) }
  const connectTimeout = options.connectTimeout || 10000
  const pingInterval = options.pingInterval || 30000
  const pongTimeout = options.pongTimeout || 10000
  const closeGrace = options.closeGrace == null ? 300 : options.closeGrace
  const initialBackoff = options.initialBackoff || 1000
  const maxBackoff = options.maxBackoff || 30000

  let socket = null
  let rx = new Uint8Array(0)
  let generation = 0
  let activeGen = 0
  let phase = 'idle'
  let connected = false
  let subscribed = false
  let manualClose = false
  let hasConnectedOnce = false
  let packetId = 0
  let backoff = initialBackoff
  let reconnectTimer = null
  let releaseTimer = null
  let connTimer = null
  let pingTimer = null
  let pongTimer = null
  let waitingPong = false

  function setConn(status, detail) {
    if (onConn) onConn(status, detail || '')
  }

  function clearKeepalive() {
    if (connTimer) {
      clearTimer(connTimer)
      connTimer = null
    }
    if (pingTimer) {
      clearTimer(pingTimer)
      pingTimer = null
    }
    if (pongTimer) {
      clearTimer(pongTimer)
      pongTimer = null
    }
    waitingPong = false
  }

  function clearRelease() {
    if (releaseTimer) {
      clearTimer(releaseTimer)
      releaseTimer = null
    }
  }

  function nextPacketId() {
    packetId = packetId >= 65535 ? 1 : packetId + 1
    return packetId
  }

  function send(bytes) {
    if (!socket || !bytes) return false
    try {
      socket.send({
        data: toArrayBuffer(bytes),
        fail: function (err) {
          console.error('[sleepfan] 发送失败', err)
        }
      })
      return true
    } catch (err) {
      console.error('[sleepfan] 发送异常', err)
      return false
    }
  }

  function safeClose(current) {
    if (!current || !current.close) return
    try {
      current.close({})
    } catch (err) {
      // 重复关闭时忽略
    }
  }

  function scheduleReconnect(reason) {
    if (manualClose) return
    if (reconnectTimer) return
    connected = false
    subscribed = false
    phase = 'idle'
    clearKeepalive()
    const wait = backoff
    backoff = Math.min(backoff * 2, maxBackoff)
    setConn('reconnecting', reason || '准备重连')
    console.log('[sleepfan] ' + wait + 'ms 后重连', reason || '')
    reconnectTimer = setTimer(function () {
      reconnectTimer = null
      if (manualClose) return
      arm()
    }, wait)
  }

  function startPing(gen) {
    if (pingTimer) clearTimer(pingTimer)
    pingTimer = setTimer(function tick() {
      if (gen !== activeGen || !connected) return
      sendPing(gen)
      pingTimer = setTimer(tick, pingInterval)
    }, pingInterval)
  }

  function sendPing(gen) {
    if (!connected || !socket || gen !== activeGen) return
    waitingPong = true
    send(codec.encodePingreq())
    if (pongTimer) clearTimer(pongTimer)
    pongTimer = setTimer(function () {
      if (!waitingPong || gen !== activeGen) return
      console.warn('[sleepfan] PINGRESP 超时')
      const current = socket
      scheduleReconnect('心跳超时')
      safeClose(current)
    }, pongTimeout)
  }

  function handlePacket(packet, gen) {
    if (gen !== activeGen) return
    const cfg = getConfig()
    if (packet.type === 'connack') {
      if (connTimer) {
        clearTimer(connTimer)
        connTimer = null
      }
      if (packet.returnCode !== 0) {
        const text = connackText(packet.returnCode)
        console.error('[sleepfan] CONNACK', packet.returnCode, text)
        const current = socket
        scheduleReconnect(text)
        safeClose(current)
        return
      }
      connected = true
      phase = 'connected'
      hasConnectedOnce = true
      backoff = initialBackoff
      setConn('connected', '')
      console.log('[sleepfan] connected', cfg.brokerUrl, 'as', cfg.mqttClientId)
      if (!cfg.statusTopic) {
        setConn('connected', '未配置订阅主题')
        return
      }
      send(codec.encodeSubscribe(nextPacketId(), cfg.statusTopic, 0))
      console.log('[sleepfan] subscribe', cfg.statusTopic)
      startPing(gen)
      return
    }
    if (packet.type === 'suback') {
      const code = packet.codes && packet.codes.length ? packet.codes[0] : 0
      if (code === 0x80) {
        subscribed = false
        setConn('connected', '订阅被拒绝')
        console.error('[sleepfan] SUBACK 失败', packet.codes)
      } else {
        subscribed = true
        console.log('[sleepfan] subscribed')
      }
      return
    }
    if (packet.type === 'pingresp') {
      waitingPong = false
      if (pongTimer) {
        clearTimer(pongTimer)
        pongTimer = null
      }
      return
    }
    if (packet.type === 'publish') {
      if (packet.qos === 1 && packet.packetId) send(codec.encodePuback(packet.packetId))
      if (packet.qos > 1) {
        console.warn('[sleepfan] 忽略 QoS 2 报文')
        return
      }
      let text = codec.utf8Decode(packet.payload)
      if (text && text.charCodeAt(0) === 0xfeff) text = text.slice(1)
      let obj = null
      try {
        obj = JSON.parse(text)
      } catch (err) {
        console.warn('[sleepfan] 非 JSON 报文', String(text).slice(0, 160))
        return
      }
      if (onStatus) onStatus(obj, packet.topic)
      return
    }
    if (packet.type === 'disconnect') {
      const current = socket
      scheduleReconnect('Broker 主动断开')
      safeClose(current)
    }
  }

  function ingest(data, gen) {
    let chunk = null
    if (data == null) return
    if (typeof data === 'string') chunk = codec.utf8Encode(data)
    else if (data instanceof ArrayBuffer) chunk = new Uint8Array(data)
    else if (typeof ArrayBuffer !== 'undefined' && ArrayBuffer.isView && ArrayBuffer.isView(data)) {
      chunk = new Uint8Array(data.buffer, data.byteOffset, data.byteLength)
    } else return

    const merged = codec.concat([rx, chunk])
    let result = null
    try {
      result = codec.decodePackets(merged)
    } catch (err) {
      console.error('[sleepfan] 解码失败', err)
      rx = new Uint8Array(0)
      return
    }
    rx = new Uint8Array(merged.subarray(result.consumed))
    if (rx.length > 65536) {
      rx = new Uint8Array(0)
      const current = socket
      scheduleReconnect('报文异常')
      safeClose(current)
      return
    }
    for (let i = 0; i < result.packets.length; i++) handlePacket(result.packets[i], gen)
  }

  function spawn(gen) {
    if (gen !== activeGen || manualClose || socket) return
    const cfg = getConfig()
    phase = 'connecting'
    let created = null
    try {
      created = connectSocket({
        url: cfg.brokerUrl,
        protocols: ['mqtt']
      })
    } catch (err) {
      phase = 'idle'
      scheduleReconnect((err && err.message) || '无法发起连接')
      return
    }
    if (!created || typeof created.onOpen !== 'function') {
      phase = 'idle'
      scheduleReconnect('当前基础库不支持 SocketTask')
      return
    }
    socket = created
    console.log('[sleepfan] socket connect', cfg.brokerUrl)
    connTimer = setTimer(function () {
      if (gen !== activeGen || connected) return
      console.warn('[sleepfan] 连接超时')
      const current = socket
      scheduleReconnect('连接超时')
      safeClose(current)
    }, connectTimeout)

    created.onOpen(function () {
      if (gen !== activeGen) return
      const fresh = getConfig()
      console.log('[sleepfan] socket open')
      send(codec.encodeConnect(fresh.mqttClientId, 60))
    })
    created.onMessage(function (res) {
      if (gen !== activeGen) return
      ingest(res && res.data, gen)
    })
    created.onError(function (err) {
      if (gen !== activeGen) return
      const msg = (err && (err.errMsg || err.message)) || '连接出错'
      console.error('[sleepfan] socket error', msg)
      const current = socket
      scheduleReconnect(msg)
      safeClose(current)
    })
    created.onClose(function () {
      if (socket === created) socket = null
      if (gen !== activeGen) return
      clearKeepalive()
      connected = false
      subscribed = false
      if (manualClose) {
        phase = 'idle'
        setConn('disconnected', '')
        return
      }
      scheduleReconnect('连接已关闭')
    })
  }

  function arm() {
    const gen = ++generation
    activeGen = gen
    if (reconnectTimer) {
      clearTimer(reconnectTimer)
      reconnectTimer = null
    }
    clearKeepalive()
    clearRelease()
    connected = false
    subscribed = false
    rx = new Uint8Array(0)
    setConn(hasConnectedOnce ? 'reconnecting' : 'connecting', '')

    if (!socket) {
      spawn(gen)
      return
    }

    phase = 'closing'
    const old = socket
    let moved = false
    const go = function () {
      if (moved) return
      moved = true
      clearRelease()
      if (manualClose || gen !== activeGen) return
      if (socket === old) socket = null
      phase = 'idle'
      spawn(activeGen)
    }
    try {
      old.close({ success: go, fail: go })
    } catch (err) {
      go()
    }
    releaseTimer = setTimer(go, closeGrace)
  }

  function start() {
    manualClose = false
    if (phase === 'idle' && !socket && !reconnectTimer) {
      arm()
      return
    }
    ensure()
  }

  function ensure() {
    if (manualClose) return
    if (connected) {
      sendPing(activeGen)
      return
    }
    if (phase === 'connecting' || phase === 'closing') return
    if (reconnectTimer) {
      clearTimer(reconnectTimer)
      reconnectTimer = null
    }
    backoff = initialBackoff
    arm()
  }

  function reconnect() {
    manualClose = false
    backoff = initialBackoff
    arm()
  }

  function disconnect() {
    manualClose = true
    activeGen = ++generation
    if (reconnectTimer) {
      clearTimer(reconnectTimer)
      reconnectTimer = null
    }
    clearKeepalive()
    clearRelease()
    connected = false
    subscribed = false
    phase = 'idle'
    const current = socket
    socket = null
    safeClose(current)
    setConn('disconnected', '')
  }

  function publish(topic, payload) {
    const json = typeof payload === 'string' ? payload : JSON.stringify(payload)
    if (!topic) return { ok: false, reason: '主题为空', topic: topic || '', payload: json }
    if (!connected || !socket) return { ok: false, reason: '未连接', topic: topic, payload: json }
    const packet = codec.encodePublish(topic, codec.utf8Encode(json), 0, 0)
    if (!send(packet)) return { ok: false, reason: '发送失败', topic: topic, payload: json }
    return { ok: true, reason: '', topic: topic, payload: json }
  }

  return {
    start: start,
    ensure: ensure,
    reconnect: reconnect,
    disconnect: disconnect,
    publish: publish,
    isConnected: function () { return connected },
    isSubscribed: function () { return subscribed }
  }
}

function defaultConnectSocket(opts) {
  return wx.connectSocket({
    url: opts.url,
    protocols: opts.protocols || ['mqtt'],
    tcpNoDelay: true,
    success: function () {},
    fail: function (err) {
      console.error('[sleepfan] connectSocket fail', err && (err.errMsg || err))
    }
  })
}

const client = createMqtt({
  connectSocket: defaultConnectSocket,
  getConfig: function () { return config.get() },
  onConn: function (status, detail) { store.setConn(status, detail) },
  onStatus: function (raw) { store.ingestStatus(raw) }
})

module.exports = {
  start: function () { client.start() },
  ensure: function () { client.ensure() },
  reconnect: function () { client.reconnect() },
  disconnect: function () { client.disconnect() },
  publish: function (topic, payload) { return client.publish(topic, payload) },
  isConnected: function () { return client.isConnected() },
  createMqtt: createMqtt
}
