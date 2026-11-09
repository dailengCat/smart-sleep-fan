const config = require('../../utils/config')
const store = require('../../utils/store')
const mqtt = require('../../utils/mqtt')
const tab = require('../../utils/tab')
const motion = require('../../utils/motion')

function readForm() {
  const cfg = config.get()
  return {
    brokerUrl: cfg.brokerUrl,
    statusTopic: cfg.statusTopic,
    cmdTopic: cfg.cmdTopic,
    clientIdFilter: cfg.clientIdFilter,
    mqttClientId: cfg.mqttClientId
  }
}

Page({
  data: {
    form: readForm(),
    lastDeviceId: '',
    error: '',
    wsHint: false,
    savedHint: '',
    entered: false
  },
  onLoad() {
    this.loadForm()
  },
  onShow() {
    const cfg = config.get()
    this.setData({ lastDeviceId: cfg.lastDeviceId || '' })
    tab.select(this, 3)
    motion.enter(this)
  },
  loadForm() {
    const form = readForm()
    this.setData({
      form: form,
      lastDeviceId: config.get().lastDeviceId || '',
      wsHint: form.brokerUrl.indexOf('ws://') === 0,
      error: ''
    })
  },
  onInput(e) {
    const field = e.currentTarget.dataset.field
    const value = e.detail.value
    const patch = { error: '', savedHint: '' }
    patch['form.' + field] = value
    if (field === 'brokerUrl') patch.wsHint = String(value).indexOf('ws://') === 0
    this.setData(patch)
  },
  apply(patch, clearSeries) {
    const next = {
      brokerUrl: patch.brokerUrl,
      statusTopic: patch.statusTopic,
      cmdTopic: patch.cmdTopic,
      clientIdFilter: patch.clientIdFilter,
      mqttClientId: patch.mqttClientId
    }
    const error = config.validate(next)
    if (error) {
      this.setData({ error: error, savedHint: '' })
      return false
    }
    const prev = config.get()
    const filterChanged = (prev.clientIdFilter || '') !== String(next.clientIdFilter || '').trim()
    const topicChanged = prev.statusTopic !== String(next.statusTopic || '').trim()
    config.save(next)
    if (clearSeries || filterChanged || topicChanged) {
      store.clearStatus()
      store.clearHistory()
    }
    mqtt.reconnect()
    this.loadForm()
    return true
  },
  onSave() {
    const ok = this.apply(this.data.form, false)
    if (!ok) return
    this.setData({ savedHint: '已保存并重连' })
    wx.showToast({ title: '已保存并重连', icon: 'none' })
  },
  onReconnect() {
    mqtt.reconnect()
    wx.showToast({ title: '正在重连', icon: 'none' })
  },
  onReset() {
    const self = this
    wx.showModal({
      title: '恢复默认连接',
      content: 'Broker、两个 Topic 和过滤会恢复默认。本机 Client ID 保持不变。',
      success: function (res) {
        if (!res.confirm) return
        const current = config.get()
        const ok = self.apply({
          brokerUrl: config.DEFAULTS.brokerUrl,
          statusTopic: config.DEFAULTS.statusTopic,
          cmdTopic: config.DEFAULTS.cmdTopic,
          clientIdFilter: '',
          mqttClientId: current.mqttClientId
        }, true)
        if (!ok) return
        self.setData({ savedHint: '已恢复默认并重连' })
        wx.showToast({ title: '已恢复默认', icon: 'none' })
      }
    })
  },
  onRegen() {
    const self = this
    wx.showModal({
      title: '重新生成 Client ID',
      content: '只更换小程序自己的连接标识，不会改设备。生成后会立即重连。',
      confirmText: '生成',
      success: function (res) {
        if (!res.confirm) return
        const id = config.regenerateClientId()
        const form = self.data.form
        form.mqttClientId = id
        self.setData({ form: form })
        mqtt.reconnect()
        wx.showToast({ title: '已更换并重连', icon: 'none' })
      }
    })
  },
  useLastDevice() {
    const id = config.get().lastDeviceId
    if (!id) {
      wx.showToast({ title: '还没有收到设备', icon: 'none' })
      return
    }
    const form = this.data.form
    form.clientIdFilter = id
    this.setData({ form: form })
    const ok = this.apply(form, false)
    if (!ok) return
    wx.showToast({ title: '已只看这台设备', icon: 'none' })
  },
  usePrefix() {
    const form = this.data.form
    form.clientIdFilter = 'sleepfan-'
    this.setData({ form: form })
    const ok = this.apply(form, false)
    if (!ok) return
    wx.showToast({ title: '已过滤 sleepfan-', icon: 'none' })
  },
  onCopyId() {
    wx.setClipboardData({ data: this.data.form.mqttClientId || '' })
  }
})
