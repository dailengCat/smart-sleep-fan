/**
 * 全局状态：连接、最新一帧设备状态、本地趋势缓存。
 * 各页面 subscribe 后自己 setData。过滤在这里做，避免把别人的公共 Broker 报文画进首页。
 */

const config = require('./config')
const storage = require('./storage')

const HISTORY_KEY = 'sleepfan_history_v1'
const HISTORY_MAX = 60
const HISTORY_TTL = 24 * 60 * 60 * 1000

const state = {
  connStatus: 'connecting',
  connDetail: '',
  status: null,
  lastUpdate: 0,
  history: [],
  ignoredCount: 0,
  lastIgnoredId: '',
  receivedCount: 0
}

const listeners = []

function finiteOrNull(value) {
  return typeof value === 'number' && isFinite(value) ? value : null
}

function num(value) {
  if (typeof value === 'number' && isFinite(value)) return value
  if (typeof value === 'string' && value.trim() !== '') {
    const parsed = Number(value)
    if (isFinite(parsed)) return parsed
  }
  return null
}

function boolOrNull(value) {
  if (typeof value === 'boolean') return value
  if (value === 1 || value === '1' || value === 'true') return true
  if (value === 0 || value === '0' || value === 'false') return false
  return null
}

function looksLikeStatus(raw) {
  const keys = ['temperature', 'humidity', 'fan_level', 'presence', 'mq135_raw', 'sleep_state', 'temp_hum_ok', 'radar_online']
  for (let i = 0; i < keys.length; i++) {
    if (raw[keys[i]] != null) return true
  }
  return false
}

function normalize(raw) {
  return {
    client_id: raw.client_id == null ? '' : String(raw.client_id),
    temperature: num(raw.temperature),
    humidity: num(raw.humidity),
    temp_hum_ok: boolOrNull(raw.temp_hum_ok),
    presence: boolOrNull(raw.presence),
    distance_cm: num(raw.distance_cm),
    radar_online: boolOrNull(raw.radar_online),
    sleeping: boolOrNull(raw.sleeping),
    sleep_state: raw.sleep_state == null ? '' : String(raw.sleep_state),
    fan_level: num(raw.fan_level),
    duty: num(raw.duty),
    auto_mode: boolOrNull(raw.auto_mode),
    decision_level: num(raw.decision_level),
    decision_reason: raw.decision_reason == null ? '' : String(raw.decision_reason),
    mq135_raw: num(raw.mq135_raw),
    uptime_ms: num(raw.uptime_ms)
  }
}

function sanitizeHistory(list) {
  if (!Array.isArray(list)) return []
  const cutoff = Date.now() - HISTORY_TTL
  const out = []
  for (let i = 0; i < list.length; i++) {
    const point = list[i]
    if (!point || typeof point.ts !== 'number' || point.ts < cutoff) continue
    out.push({
      ts: point.ts,
      temperature: finiteOrNull(point.temperature),
      humidity: finiteOrNull(point.humidity),
      fan_level: finiteOrNull(point.fan_level),
      duty: finiteOrNull(point.duty)
    })
  }
  if (out.length > HISTORY_MAX) return out.slice(out.length - HISTORY_MAX)
  return out
}

function loadHistory() {
  const raw = storage.read(HISTORY_KEY)
  if (Array.isArray(raw)) return sanitizeHistory(raw)
  if (typeof raw === 'string' && raw) {
    try {
      return sanitizeHistory(JSON.parse(raw))
    } catch (err) {
      return []
    }
  }
  return []
}

state.history = loadHistory()

function emit() {
  const snapshot = listeners.slice()
  for (let i = 0; i < snapshot.length; i++) {
    try {
      snapshot[i](state)
    } catch (err) {
      console.error('[sleepfan] 订阅回调失败', err)
    }
  }
}

function subscribe(fn) {
  listeners.push(fn)
  return function () {
    const index = listeners.indexOf(fn)
    if (index >= 0) listeners.splice(index, 1)
  }
}

function setConn(status, detail) {
  const nextDetail = detail || ''
  if (state.connStatus === status && state.connDetail === nextDetail) return
  state.connStatus = status
  state.connDetail = nextDetail
  emit()
}

function pushHistory(status) {
  state.history.push({
    ts: Date.now(),
    temperature: status.temperature,
    humidity: status.humidity,
    fan_level: status.fan_level == null || status.fan_level < 0 ? null : status.fan_level,
    duty: status.duty
  })
  if (state.history.length > HISTORY_MAX) {
    state.history.splice(0, state.history.length - HISTORY_MAX)
  }
  storage.write(HISTORY_KEY, state.history)
}

function ingestStatus(raw) {
  if (!raw || typeof raw !== 'object' || Array.isArray(raw)) return { ok: false, reason: 'shape' }
  if (!looksLikeStatus(raw)) return { ok: false, reason: 'shape' }
  const cfg = config.get()
  const id = raw.client_id == null ? '' : String(raw.client_id)
  const filter = cfg.clientIdFilter ? String(cfg.clientIdFilter).trim() : ''
  if (filter && id.indexOf(filter) === -1) {
    state.ignoredCount += 1
    state.lastIgnoredId = id
    console.log('[sleepfan] filtered', id || '(空 client_id)', 'filter=' + filter)
    emit()
    return { ok: false, reason: 'filtered' }
  }
  const status = normalize(raw)
  if (status.client_id) config.rememberDevice(status.client_id)
  state.status = status
  state.lastUpdate = Date.now()
  state.receivedCount += 1
  pushHistory(status)
  console.log(
    '[sleepfan] status',
    status.client_id,
    'T=' + status.temperature,
    'H=' + status.humidity,
    'fan=' + status.fan_level,
    'auto=' + status.auto_mode
  )
  emit()
  return { ok: true, status: status }
}

function clearStatus() {
  state.status = null
  state.lastUpdate = 0
  state.ignoredCount = 0
  state.lastIgnoredId = ''
  state.receivedCount = 0
  emit()
}

function clearHistory() {
  state.history = []
  storage.remove(HISTORY_KEY)
  emit()
}

function getState() {
  return state
}

function getHistory() {
  return state.history
}

module.exports = {
  HISTORY_MAX: HISTORY_MAX,
  subscribe: subscribe,
  setConn: setConn,
  ingestStatus: ingestStatus,
  clearStatus: clearStatus,
  clearHistory: clearHistory,
  getState: getState,
  getHistory: getHistory,
  normalize: normalize
}
