/**
 * 展示层：把设备 JSON 收成首页可直接绑定的字段。不补充设备没有上报的能力。
 */

function connText(status) {
  if (status === 'connected') return '已连接'
  if (status === 'reconnecting') return '重连中'
  if (status === 'connecting') return '连接中'
  return '已断开'
}

function formatFixed(value, digits) {
  if (typeof value !== 'number' || !isFinite(value)) return '--'
  const scale = Math.pow(10, digits)
  return (Math.round(value * scale) / scale).toFixed(digits)
}

function formatInt(value) {
  if (typeof value !== 'number' || !isFinite(value)) return '--'
  return String(Math.round(value))
}

function formatMetric(value, metric) {
  if (typeof value !== 'number' || !isFinite(value)) return '--'
  if (metric === 'fan_level') return String(Math.round(value))
  return formatFixed(value, 1)
}

function pad(n) {
  return n < 10 ? '0' + n : String(n)
}

function formatTime(ts) {
  if (!ts) return '--'
  const date = new Date(ts)
  if (isNaN(date.getTime())) return '--'
  return pad(date.getHours()) + ':' + pad(date.getMinutes()) + ':' + pad(date.getSeconds())
}

function formatAgo(ts, now) {
  if (!ts) return ''
  const sec = Math.max(0, Math.floor(((now || Date.now()) - ts) / 1000))
  if (sec < 5) return '刚刚'
  if (sec < 60) return sec + ' 秒前'
  const min = Math.floor(sec / 60)
  if (min < 60) return min + ' 分钟前'
  return Math.floor(min / 60) + ' 小时前'
}

function formatUptime(ms) {
  if (typeof ms !== 'number' || !isFinite(ms) || ms < 0) return ''
  let sec = Math.floor(ms / 1000)
  const day = Math.floor(sec / 86400)
  sec = sec % 86400
  const hour = Math.floor(sec / 3600)
  sec = sec % 3600
  const min = Math.floor(sec / 60)
  sec = sec % 60
  if (day > 0) return day + '天' + hour + '小时'
  if (hour > 0) return hour + '小时' + min + '分'
  if (min > 0) return min + '分' + sec + '秒'
  return sec + '秒'
}

function shortId(id) {
  if (!id) return '尚未收到'
  if (id.length <= 22) return id
  return id.slice(0, 10) + '…' + id.slice(-4)
}

function brokerHost(url) {
  const stripped = String(url || '').replace(/^[a-z]+:\/\//i, '')
  const host = stripped.split('/')[0]
  return host || url || ''
}

function fanLabel(level) {
  if (typeof level !== 'number' || !isFinite(level)) return '未知'
  if (level < 0) return '自定义'
  if (level === 0) return '关闭'
  const rounded = Math.round(level * 10) / 10
  return rounded + ' 档'
}

function boolText(value, yes, no) {
  if (value === true) return yes
  if (value === false) return no
  return '未知'
}

function sleepInfo(state, sleeping) {
  const raw = state ? String(state) : ''
  const key = raw.toLowerCase()
  const map = {
    awake: '清醒',
    wake: '清醒',
    idle: '清醒',
    sleep: '睡眠',
    sleeping: '睡眠',
    asleep: '睡眠',
    light: '浅睡',
    light_sleep: '浅睡',
    deep: '深睡',
    deep_sleep: '深睡',
    rem: 'REM'
  }
  let label = '未知'
  let tone = 'unknown'
  if (map[key]) {
    label = map[key]
    tone = key === 'awake' || key === 'wake' || key === 'idle' ? 'awake' : 'asleep'
  } else if (raw) {
    label = raw
    tone = 'asleep'
  } else if (sleeping === true) {
    label = '睡眠'
    tone = 'asleep'
  } else if (sleeping === false) {
    label = '清醒'
    tone = 'awake'
  }
  let sleepingText = ''
  if (sleeping === true) sleepingText = '设备标记：睡眠中'
  if (sleeping === false) sleepingText = '设备标记：非睡眠'
  return {
    label: label,
    raw: raw && raw !== label ? raw : '',
    tone: tone,
    sleepingText: sleepingText
  }
}

function fanCells(level) {
  const cells = []
  for (let n = 0; n <= 5; n++) {
    cells.push({
      n: n,
      label: n === 0 ? '0' : String(n),
      lit: typeof level === 'number' && level > 0 && n > 0 && n <= level,
      active: typeof level === 'number' && level >= 0 && level === n
    })
  }
  return cells
}

function emptyCopy(connStatus, connDetail) {
  if (connStatus === 'connected') {
    return {
      title: '等待设备上报…',
      sub: '已连上 Broker，设备大约每 10 秒发布一帧'
    }
  }
  if (connStatus === 'reconnecting') {
    return {
      title: '正在重新连接…',
      sub: connDetail || '连上后在这里等待设备上报'
    }
  }
  if (connStatus === 'connecting') {
    return {
      title: '正在连接 Broker…',
      sub: connDetail || '连上后在这里等待设备上报'
    }
  }
  return {
    title: '尚未连上 Broker',
    sub: connDetail || '请确认开发者工具已关闭合法域名校验'
  }
}

function buildMonitor(state, cfg, now) {
  const current = now || Date.now()
  const config = cfg || {}
  const topic = config.statusTopic || 'sleepfan/status'
  const filter = config.clientIdFilter ? String(config.clientIdFilter).trim() : ''
  const empty = emptyCopy(state.connStatus, state.connDetail)
  const base = {
    connStatus: state.connStatus || 'disconnected',
    connText: connText(state.connStatus),
    connDetail: state.connDetail || '',
    deviceShort: '尚未收到',
    deviceId: '',
    hasData: false,
    sensorFault: false,
    tempText: '--',
    humText: '--',
    radarOnlineText: '未知',
    radarTone: 'unknown',
    presenceText: '未知',
    presenceTone: 'unknown',
    distanceText: '--',
    sleepLabel: '未知',
    sleepRaw: '',
    sleepTone: 'unknown',
    sleepingText: '',
    fanLabel: '未知',
    fanCustom: false,
    fanCells: fanCells(null),
    dutyText: '--',
    dutyWidth: 0,
    autoText: '自动未知',
    autoOn: false,
    reason: '暂无',
    decisionExtra: '',
    mqText: '--',
    updatedText: '',
    agoText: '',
    uptimeText: '',
    stale: false,
    emptyTitle: empty.title,
    emptySub: empty.sub,
    ignoredText: '',
    receivedText: state.receivedCount ? '已接收 ' + state.receivedCount + ' 帧' : '',
    topicText: topic,
    brokerText: brokerHost(config.brokerUrl),
    filterText: filter
  }
  if (state.ignoredCount > 0) {
    const who = state.lastIgnoredId ? '，最近 ' + state.lastIgnoredId : ''
    base.ignoredText = '已忽略 ' + state.ignoredCount + ' 条不含「' + filter + '」的报文' + who
  }
  const status = state.status
  if (!status) return base
  const sleep = sleepInfo(status.sleep_state, status.sleeping)
  let dutyWidth = 0
  if (typeof status.duty === 'number' && isFinite(status.duty)) {
    dutyWidth = Math.max(0, Math.min(100, status.duty))
  }
  let decisionExtra = ''
  if (typeof status.decision_level === 'number' && status.decision_level !== status.fan_level) {
    decisionExtra = '决策档位 ' + (Math.round(status.decision_level * 10) / 10)
  }
  let radarTone = 'unknown'
  if (status.radar_online === true) radarTone = 'online'
  if (status.radar_online === false) radarTone = 'offline'
  let presenceTone = 'unknown'
  if (status.presence === true) presenceTone = 'here'
  if (status.presence === false) presenceTone = 'away'
  base.deviceShort = shortId(status.client_id)
  base.deviceId = status.client_id || ''
  base.hasData = true
  base.sensorFault = status.temp_hum_ok === false
  base.tempText = formatFixed(status.temperature, 1)
  base.humText = formatFixed(status.humidity, 1)
  base.radarOnlineText = boolText(status.radar_online, '在线', '离线')
  base.radarTone = radarTone
  base.presenceText = boolText(status.presence, '有人', '无人')
  base.presenceTone = presenceTone
  base.distanceText = formatInt(status.distance_cm)
  base.sleepLabel = sleep.label
  base.sleepRaw = sleep.raw
  base.sleepTone = sleep.tone
  base.sleepingText = sleep.sleepingText
  base.fanLabel = fanLabel(status.fan_level)
  base.fanCustom = typeof status.fan_level === 'number' && status.fan_level < 0
  base.fanCells = fanCells(status.fan_level)
  base.dutyText = formatInt(status.duty)
  base.dutyWidth = dutyWidth
  base.autoText = boolText(status.auto_mode, '自动开', '自动关')
  base.autoOn = status.auto_mode === true
  base.reason = status.decision_reason || '暂无'
  base.decisionExtra = decisionExtra
  base.mqText = formatInt(status.mq135_raw)
  base.updatedText = formatTime(state.lastUpdate)
  base.agoText = formatAgo(state.lastUpdate, current)
  base.uptimeText = formatUptime(status.uptime_ms)
  base.stale = !!(state.lastUpdate && current - state.lastUpdate > 30000)
  return base
}

module.exports = {
  connText: connText,
  formatFixed: formatFixed,
  formatInt: formatInt,
  formatMetric: formatMetric,
  formatTime: formatTime,
  formatAgo: formatAgo,
  formatUptime: formatUptime,
  shortId: shortId,
  brokerHost: brokerHost,
  fanLabel: fanLabel,
  sleepInfo: sleepInfo,
  fanCells: fanCells,
  buildMonitor: buildMonitor
}
