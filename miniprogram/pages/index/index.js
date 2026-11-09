const store = require('../../utils/store')
const config = require('../../utils/config')
const format = require('../../utils/format')
const mqtt = require('../../utils/mqtt')
const tab = require('../../utils/tab')
const motion = require('../../utils/motion')

function fanRunning(vm) {
  if (!vm.hasData) return false
  if (vm.fanCustom) return vm.dutyWidth > 0
  return vm.fanLabel !== '关闭' && vm.fanLabel !== '未知'
}

Page({
  data: Object.assign({ entered: false, fresh: false, fanRunning: false }, format.buildMonitor(store.getState(), config.get(), Date.now())),
  onShow() {
    this.release()
    const self = this
    this.unsub = store.subscribe(function () {
      self.sync()
    })
    this.sync()
    this.tick = setInterval(function () {
      self.sync()
    }, 1000)
    tab.select(this, 0)
    motion.enter(this)
  },
  onHide() {
    this.release()
    this.setData({ entered: false })
  },
  onUnload() {
    this.release()
    if (this.refreshTimer) {
      clearTimeout(this.refreshTimer)
      this.refreshTimer = null
    }
    if (this.freshTimer) {
      clearTimeout(this.freshTimer)
      this.freshTimer = null
    }
  },
  release() {
    if (this.unsub) {
      this.unsub()
      this.unsub = null
    }
    if (this.tick) {
      clearInterval(this.tick)
      this.tick = null
    }
  },
  sync() {
    const vm = format.buildMonitor(store.getState(), config.get(), Date.now())
    vm.fanRunning = fanRunning(vm)
    const changed = this.seen && (
      vm.tempText !== this.data.tempText ||
      vm.humText !== this.data.humText ||
      vm.fanLabel !== this.data.fanLabel ||
      vm.sleepLabel !== this.data.sleepLabel ||
      vm.presenceText !== this.data.presenceText
    )
    this.seen = true
    if (changed) vm.fresh = true
    this.setData(vm)
    if (changed) {
      const self = this
      if (this.freshTimer) clearTimeout(this.freshTimer)
      this.freshTimer = setTimeout(function () {
        self.setData({ fresh: false })
      }, 520)
    }
  },
  onPullDownRefresh() {
    mqtt.reconnect()
    const self = this
    this.refreshTimer = setTimeout(function () {
      wx.stopPullDownRefresh()
      self.refreshTimer = null
    }, 800)
  }
})
