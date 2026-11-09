const store = require('../../utils/store')
const format = require('../../utils/format')

Component({
  data: {
    connStatus: 'connecting',
    connText: '连接中',
    connDetail: ''
  },
  lifetimes: {
    attached() {
      this.sync()
      const self = this
      this.unsub = store.subscribe(function () {
        self.sync()
      })
    },
    detached() {
      if (this.unsub) this.unsub()
    }
  },
  methods: {
    sync() {
      const state = store.getState()
      this.setData({
        connStatus: state.connStatus,
        connText: format.connText(state.connStatus),
        connDetail: state.connDetail || ''
      })
    }
  }
})
