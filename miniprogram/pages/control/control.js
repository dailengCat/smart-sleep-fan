const store = require('../../utils/store')
const config = require('../../utils/config')
const format = require('../../utils/format')
const mqtt = require('../../utils/mqtt')
const tab = require('../../utils/tab')
const motion = require('../../utils/motion')

Page({
  data: {
    levels: [0, 1, 2, 3, 4, 5],
    sentLevel: null,
    sentAuto: '',
    cmdTopic: 'sleepfan/cmd',
    hasDevice: false,
    deviceLevel: '暂无',
    deviceAuto: '暂无',
    deviceReason: '暂无',
    lastPayload: '',
    logs: [],
    entered: false
  },
  onShow() {
    this.release()
    const self = this
    this.unsub = store.subscribe(function () {
      self.sync()
    })
    this.sync()
    tab.select(this, 1)
    motion.enter(this)
  },
  onHide() {
    this.release()
    this.setData({ entered: false })
  },
  onUnload() {
    this.release()
  },
  release() {
    if (this.unsub) {
      this.unsub()
      this.unsub = null
    }
  },
  sync() {
    const state = store.getState()
    const status = state.status
    const cfg = config.get()
    this.setData({
      cmdTopic: cfg.cmdTopic || 'sleepfan/cmd',
      hasDevice: !!status,
      deviceLevel: status ? format.fanLabel(status.fan_level) : '暂无',
      deviceAuto: status ? format.boolText(status.auto_mode, '自动开', '自动关') : '暂无',
      deviceReason: status && status.decision_reason ? status.decision_reason : '暂无'
    })
  },
  onLevel(e) {
    const level = Number(e.currentTarget.dataset.level)
    if (!isFinite(level)) return
    this.setData({ sentLevel: level })
    this.sendCmd({ cmd: 'set_fan_level', level: level })
  },
  onAuto(e) {
    const on = e.currentTarget.dataset.mode === 'on'
    this.setData({ sentAuto: on ? 'on' : 'off' })
    this.sendCmd({ cmd: 'set_auto', auto: on })
  },
  onOsc(e) {
    const action = e.currentTarget.dataset.action
    if (!action) return
    this.sendCmd({ cmd: 'oscillate', action: action })
  },
  sendCmd(obj) {
    const topic = config.get().cmdTopic || 'sleepfan/cmd'
    const result = mqtt.publish(topic, obj)
    if (result.ok) {
      console.log('[sleepfan] payload', result.payload)
    } else {
      console.warn('[sleepfan] payload not sent', result.payload, result.reason)
    }
    const line = {
      id: Date.now() + '-' + this.data.logs.length,
      time: format.formatTime(Date.now()),
      payload: result.payload,
      ok: result.ok,
      note: result.ok ? '已交给 Broker。固件未订阅，不会执行' : '未发送：' + result.reason
    }
    const logs = [line].concat(this.data.logs).slice(0, 8)
    this.setData({
      logs: logs,
      lastPayload: result.payload
    })
    wx.showToast({
      title: result.ok ? '已发送指令，设备侧待接入' : '未连接，指令未发出',
      icon: 'none'
    })
  }
})
