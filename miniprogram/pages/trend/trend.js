const store = require('../../utils/store')
const chart = require('../../utils/chart')
const format = require('../../utils/format')
const tab = require('../../utils/tab')
const motion = require('../../utils/motion')

function pixelRatio() {
  try {
    if (wx.getWindowInfo) {
      const info = wx.getWindowInfo()
      if (info && info.pixelRatio) return info.pixelRatio
    }
  } catch (err) {}
  try {
    if (wx.getSystemInfoSync) {
      const info = wx.getSystemInfoSync()
      if (info && info.pixelRatio) return info.pixelRatio
    }
  } catch (err) {}
  return 2
}

Page({
  data: {
    metric: 'temperature',
    unit: '°C',
    count: 0,
    validCount: 0,
    latest: '--',
    minText: '--',
    maxText: '--',
    spanText: '尚无采样',
    note: '曲线只存在本机，最多 60 个点。未设置过滤时，多台设备会画在一起。',
    metricIndex: 0,
    entered: false,
    swap: false
  },
  onLoad() {
    this._series = []
  },
  onShow() {
    this.release()
    const self = this
    this.unsub = store.subscribe(function () {
      self.refresh()
    })
    this.refresh()
    tab.select(this, 2)
    motion.enter(this)
    if (wx.onWindowResize) {
      this._onResize = function () { self.draw() }
      wx.onWindowResize(this._onResize)
    }
  },
  onReady() {
    this._ready = true
    this.draw()
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
    if (this._onResize && wx.offWindowResize) {
      wx.offWindowResize(this._onResize)
      this._onResize = null
    }
  },
  onMetric(e) {
    const metric = e.currentTarget.dataset.metric
    if (!metric || !chart.METRICS[metric]) return
    const order = { temperature: 0, humidity: 1, fan_level: 2 }
    const self = this
    this.setData({ swap: false })
    setTimeout(function () {
      self.setData({
        metric: metric,
        unit: chart.METRICS[metric].unit,
        metricIndex: order[metric] || 0,
        swap: true
      })
      self.refresh()
    }, 20)
  },
  refresh() {
    const history = store.getHistory()
    const metric = this.data.metric
    const series = []
    for (let i = 0; i < history.length; i++) {
      series.push({ ts: history[i].ts, value: history[i][metric] })
    }
    const stats = chart.statsOf(series)
    let note = '曲线只存在本机，最多 60 个点。未设置过滤时，多台设备会画在一起。'
    if (metric === 'fan_level' && history.length > 0 && stats.count === 0) {
      note = '这些采样的档位是自定义（-1）。占空比不会被换算成档位，所以曲线留空。'
    } else if (series.length && (series[series.length - 1].value == null)) {
      note = '最近一点没有这个指标的数值，线段会在这里断开。'
    }
    this._series = series
    this.setData({
      count: history.length,
      validCount: stats.count,
      latest: format.formatMetric(stats.last, metric),
      minText: format.formatMetric(stats.min, metric),
      maxText: format.formatMetric(stats.max, metric),
      unit: chart.METRICS[metric].unit,
      spanText: history.length
        ? format.formatTime(history[0].ts) + ' – ' + format.formatTime(history[history.length - 1].ts)
        : '尚无采样',
      note: note
    })
    const self = this
    wx.nextTick(function () { self.draw() })
  },
  draw() {
    if (!this._ready) {
      this._pending = true
      return
    }
    if (this._drawing) {
      this._pending = true
      return
    }
    this._drawing = true
    const self = this
    const series = this._series || []
    const metric = this.data.metric
    this.createSelectorQuery()
      .select('#trendCanvas')
      .fields({ node: true, size: true })
      .exec(function (res) {
        self._drawing = false
        const node = res && res[0]
        if (!node || !node.node || !node.width || !node.height) {
          if (!self._sizeRetry) {
            self._sizeRetry = true
            setTimeout(function () { self.draw() }, 60)
          }
          if (self._pending) {
            self._pending = false
            self.draw()
          }
          return
        }
        self._sizeRetry = false
        const dpr = Math.min(pixelRatio() || 2, 3)
        const canvas = node.node
        const ctx = canvas.getContext('2d')
        canvas.width = node.width * dpr
        canvas.height = node.height * dpr
        ctx.setTransform(dpr, 0, 0, dpr, 0, 0)
        chart.drawChart(ctx, node.width, node.height, series, metric)
        if (self._pending) {
          self._pending = false
          self.draw()
        }
      })
  },
  onClear() {
    const self = this
    wx.showModal({
      title: '清空本地曲线',
      content: '只删除本机缓存的采样，不会影响设备和 Broker。',
      confirmText: '清空',
      success: function (res) {
        if (!res.confirm) return
        store.clearHistory()
        self.refresh()
      }
    })
  }
})
