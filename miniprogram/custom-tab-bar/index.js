const store = require('../utils/store')

Component({
  data: {
    selected: 0,
    connStatus: 'connecting',
    list: [
      { pagePath: 'pages/index/index', text: '监控', icon: 'monitor' },
      { pagePath: 'pages/control/control', text: '控制', icon: 'control' },
      { pagePath: 'pages/trend/trend', text: '趋势', icon: 'trend' },
      { pagePath: 'pages/settings/settings', text: '设置', icon: 'settings' }
    ]
  },
  lifetimes: {
    attached() {
      this.syncSelected()
      this.syncConn()
      const self = this
      this.unsub = store.subscribe(function () {
        self.syncConn()
      })
    },
    detached() {
      if (this.unsub) this.unsub()
    }
  },
  pageLifetimes: {
    show() {
      this.syncSelected()
    }
  },
  methods: {
    syncSelected() {
      const pages = getCurrentPages()
      if (!pages.length) return
      const route = pages[pages.length - 1].route || ''
      const list = this.data.list
      for (let i = 0; i < list.length; i++) {
        if (list[i].pagePath === route && this.data.selected !== i) {
          this.setData({ selected: i })
          return
        }
      }
    },
    syncConn() {
      const status = store.getState().connStatus || 'disconnected'
      if (status !== this.data.connStatus) this.setData({ connStatus: status })
    },
    onSwitch(e) {
      const index = Number(e.currentTarget.dataset.index)
      const item = this.data.list[index]
      if (!item) return
      this.setData({ selected: index })
      wx.switchTab({ url: '/' + item.pagePath })
    }
  }
})
