/**
 * 本地存储封装。开发者工具和真机都有 wx；Node 自测时没有 wx，读写变为空操作。
 */

function read(key) {
  try {
    if (typeof wx === 'undefined' || !wx.getStorageSync) return ''
    const value = wx.getStorageSync(key)
    return value == null ? '' : value
  } catch (err) {
    return ''
  }
}

function write(key, value) {
  try {
    if (typeof wx === 'undefined' || !wx.setStorageSync) return false
    wx.setStorageSync(key, value)
    return true
  } catch (err) {
    console.warn('[sleepfan] 存储失败', key)
    return false
  }
}

function remove(key) {
  try {
    if (typeof wx === 'undefined' || !wx.removeStorageSync) return
    wx.removeStorageSync(key)
  } catch (err) {
    // 存储不可用时忽略，页面仍可使用内存数据
  }
}

module.exports = {
  read: read,
  write: write,
  remove: remove
}
