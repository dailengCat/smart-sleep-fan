/**
 * 页面进入：先落一帧，再淡入并上移。微信 Tab 切换没有原生转场，用这一下补上。
 */

function enter(page) {
  if (!page || !page.setData) return
  page.setData({ entered: false })
  setTimeout(function () {
    page.setData({ entered: true })
  }, 30)
}

module.exports = {
  enter: enter
}
