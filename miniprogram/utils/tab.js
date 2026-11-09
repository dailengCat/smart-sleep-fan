function select(page, index) {
  if (typeof page.getTabBar !== 'function') return
  const bar = page.getTabBar()
  if (bar && bar.setData) bar.setData({ selected: index })
}

module.exports = {
  select: select
}
