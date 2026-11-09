/**
 * 连接配置。Broker、主题、过滤和小程序自己的 MQTT Client ID 都集中在这里，并写入本地存储。
 * 小程序 Client ID 使用 wxsf- 前缀，避免和设备的 sleepfan- 冲突。
 */

const storage = require('./storage')

const STORAGE_KEY = 'sleepfan_config_v1'
const LAST_DEVICE_KEY = 'sleepfan_last_device'

const DEFAULTS = {
  brokerUrl: 'wss://broker.emqx.io:8084/mqtt',
  statusTopic: 'sleepfan/status',
  cmdTopic: 'sleepfan/cmd',
  clientIdFilter: ''
}

let cache = null

function generateId() {
  const alphabet = '0123456789abcdef'
  let rand = ''
  for (let i = 0; i < 8; i++) {
    rand += alphabet.charAt(Math.floor(Math.random() * alphabet.length))
  }
  const tail = Date.now().toString(16).slice(-8)
  return 'wxsf-' + tail + rand
}

function load() {
  const stored = storage.read(STORAGE_KEY)
  const saved = stored && typeof stored === 'object' ? stored : {}
  let mqttClientId = saved.mqttClientId ? String(saved.mqttClientId) : ''
  if (!mqttClientId) mqttClientId = generateId()
  const lastDevice = storage.read(LAST_DEVICE_KEY)
  cache = {
    brokerUrl: saved.brokerUrl ? String(saved.brokerUrl) : DEFAULTS.brokerUrl,
    statusTopic: saved.statusTopic ? String(saved.statusTopic) : DEFAULTS.statusTopic,
    cmdTopic: saved.cmdTopic ? String(saved.cmdTopic) : DEFAULTS.cmdTopic,
    clientIdFilter: saved.clientIdFilter ? String(saved.clientIdFilter) : '',
    mqttClientId: mqttClientId,
    lastDeviceId: lastDevice ? String(lastDevice) : ''
  }
  persist()
  return get()
}

function persist() {
  storage.write(STORAGE_KEY, {
    brokerUrl: cache.brokerUrl,
    statusTopic: cache.statusTopic,
    cmdTopic: cache.cmdTopic,
    clientIdFilter: cache.clientIdFilter,
    mqttClientId: cache.mqttClientId
  })
}

function get() {
  if (!cache) load()
  return {
    brokerUrl: cache.brokerUrl,
    statusTopic: cache.statusTopic,
    cmdTopic: cache.cmdTopic,
    clientIdFilter: cache.clientIdFilter,
    mqttClientId: cache.mqttClientId,
    lastDeviceId: cache.lastDeviceId
  }
}

function save(patch) {
  if (!cache) load()
  const fields = ['brokerUrl', 'statusTopic', 'cmdTopic', 'clientIdFilter', 'mqttClientId']
  for (let i = 0; i < fields.length; i++) {
    const key = fields[i]
    if (patch[key] != null) cache[key] = String(patch[key]).trim()
  }
  persist()
  return get()
}

function validate(patch) {
  const broker = patch.brokerUrl ? String(patch.brokerUrl).trim() : ''
  if (!/^wss?:\/\/.+/i.test(broker)) return 'Broker 需以 ws:// 或 wss:// 开头'
  if (broker.length > 200) return 'Broker 地址过长'
  const topic = patch.statusTopic ? String(patch.statusTopic).trim() : ''
  if (!topic) return '订阅 Topic 不能为空'
  if (/\s/.test(topic)) return 'Topic 不能包含空格'
  const cmd = patch.cmdTopic ? String(patch.cmdTopic).trim() : ''
  if (!cmd) return '下行 Topic 不能为空'
  if (/\s/.test(cmd)) return '下行 Topic 不能包含空格'
  const filter = patch.clientIdFilter ? String(patch.clientIdFilter).trim() : ''
  if (filter.length > 128) return '过滤条件过长'
  const cid = patch.mqttClientId ? String(patch.mqttClientId).trim() : ''
  if (cid && !/^[0-9A-Za-z_-]{1,64}$/.test(cid)) return 'Client ID 仅限字母数字、下划线和短横线'
  if (cid && cid.indexOf('sleepfan') === 0) return 'Client ID 不要用 sleepfan 开头，那是设备的'
  return ''
}

function rememberDevice(id) {
  if (!id) return
  if (!cache) load()
  cache.lastDeviceId = String(id)
  storage.write(LAST_DEVICE_KEY, cache.lastDeviceId)
}

function regenerateClientId() {
  if (!cache) load()
  cache.mqttClientId = generateId()
  persist()
  return cache.mqttClientId
}

module.exports = {
  DEFAULTS: DEFAULTS,
  STORAGE_KEY: STORAGE_KEY,
  load: load,
  get: get,
  save: save,
  validate: validate,
  rememberDevice: rememberDevice,
  regenerateClientId: regenerateClientId
}
