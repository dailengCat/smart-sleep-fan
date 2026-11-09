const config = require('./utils/config')
const mqtt = require('./utils/mqtt')

App({
  onLaunch() {
    try {
      config.load()
      mqtt.start()
    } catch (err) {
      console.error('[sleepfan] 启动失败', err)
    }
  },
  onShow() {
    mqtt.ensure()
  }
})
