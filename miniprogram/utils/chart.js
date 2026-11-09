/**
 * 趋势页 canvas 2d 自绘。不引入图表库。
 * 横轴按采样时间，档位为 -1（自定义）的点留空，不把占空比换算成档位。
 */

const METRICS = {
  temperature: { color: '#e6d3b4', fill: 'rgba(230, 211, 180, 0.22)', fade: 'rgba(230, 211, 180, 0)', unit: '°C', name: '温度' },
  humidity: { color: '#c4b6d4', fill: 'rgba(196, 182, 212, 0.2)', fade: 'rgba(196, 182, 212, 0)', unit: '%', name: '湿度' },
  fan_level: { color: '#b7c4ae', fill: 'rgba(183, 196, 174, 0.2)', fade: 'rgba(183, 196, 174, 0)', unit: '档', name: '档位' }
}

function isNum(value) {
  return typeof value === 'number' && isFinite(value)
}

function computeScale(values, metric) {
  if (metric === 'humidity') return { min: 0, max: 100 }
  if (metric === 'fan_level') return { min: 0, max: 5 }
  let min = null
  let max = null
  for (let i = 0; i < values.length; i++) {
    if (!isNum(values[i])) continue
    if (min == null || values[i] < min) min = values[i]
    if (max == null || values[i] > max) max = values[i]
  }
  if (min == null) return { min: 20, max: 30 }
  if (min === max) {
    return { min: min - 1.5, max: max + 1.5 }
  }
  const pad = (max - min) * 0.18
  return { min: min - pad, max: max + pad }
}

function statsOf(points) {
  let min = null
  let max = null
  let last = null
  let count = 0
  for (let i = 0; i < points.length; i++) {
    if (!isNum(points[i].value)) continue
    count++
    if (min == null || points[i].value < min) min = points[i].value
    if (max == null || points[i].value > max) max = points[i].value
    last = points[i].value
  }
  return { min: min, max: max, last: last, count: count }
}

function formatTick(value, metric) {
  if (metric === 'fan_level' || metric === 'humidity') return String(Math.round(value))
  return (Math.round(value * 10) / 10).toFixed(1)
}

function yOf(value, scale, top, bottom) {
  const span = scale.max - scale.min || 1
  const ratio = (value - scale.min) / span
  let y = bottom - ratio * (bottom - top)
  if (y < top) y = top
  if (y > bottom) y = bottom
  return y
}

function xOf(index, points, padL, plotW) {
  if (points.length <= 1) return padL + plotW / 2
  const start = points[0].ts
  const end = points[points.length - 1].ts
  if (!start || !end || end === start) {
    return padL + (plotW * index) / (points.length - 1)
  }
  return padL + plotW * ((points[index].ts - start) / (end - start))
}

function pad2(n) {
  return n < 10 ? '0' + n : String(n)
}

function formatClock(ts) {
  const date = new Date(ts)
  if (isNaN(date.getTime())) return ''
  return pad2(date.getHours()) + ':' + pad2(date.getMinutes()) + ':' + pad2(date.getSeconds())
}

function drawChart(ctx, width, height, points, metric) {
  const meta = METRICS[metric] || METRICS.temperature
  const series = points || []
  ctx.clearRect(0, 0, width, height)
  const padL = 44
  const padR = 12
  const padT = 16
  const padB = 28
  const plotW = Math.max(1, width - padL - padR)
  const plotH = Math.max(1, height - padT - padB)
  const values = []
  for (let i = 0; i < series.length; i++) values.push(series[i].value)
  const scale = computeScale(values, metric)
  const rows = metric === 'fan_level' ? 5 : 4
  ctx.font = '11px sans-serif'
  ctx.textAlign = 'right'
  ctx.textBaseline = 'middle'
  ctx.lineWidth = 1
  for (let i = 0; i <= rows; i++) {
    const y = padT + (plotH * i) / rows
    ctx.strokeStyle = 'rgba(244, 240, 230, 0.08)'
    ctx.beginPath()
    ctx.moveTo(padL, y)
    ctx.lineTo(width - padR, y)
    ctx.stroke()
    const tick = scale.max - ((scale.max - scale.min) * i) / rows
    ctx.fillStyle = '#8d867a'
    ctx.fillText(formatTick(tick, metric), padL - 6, y)
  }

  const coords = []
  let valid = 0
  for (let i = 0; i < series.length; i++) {
    if (!isNum(series[i].value)) {
      coords.push(null)
      continue
    }
    valid++
    coords.push({
      x: xOf(i, series, padL, plotW),
      y: yOf(series[i].value, scale, padT, padT + plotH)
    })
  }

  if (!valid) {
    ctx.shadowBlur = 0
    ctx.fillStyle = '#8d867a'
    ctx.textAlign = 'center'
    ctx.textBaseline = 'middle'
    ctx.font = '13px sans-serif'
    ctx.fillText(series.length ? '这些采样没有可绘制的值' : '等待采样…', width / 2, padT + plotH / 2)
  } else {
    let seg = []
    const flush = function () {
      if (!seg.length) return
      if (seg.length > 1) {
        const grad = ctx.createLinearGradient(0, padT, 0, padT + plotH)
        grad.addColorStop(0, meta.fill)
        grad.addColorStop(1, meta.fade)
        ctx.beginPath()
        ctx.moveTo(seg[0].x, seg[0].y)
        for (let i = 1; i < seg.length; i++) ctx.lineTo(seg[i].x, seg[i].y)
        ctx.lineTo(seg[seg.length - 1].x, padT + plotH)
        ctx.lineTo(seg[0].x, padT + plotH)
        ctx.closePath()
        ctx.fillStyle = grad
        ctx.fill()
      }
      ctx.beginPath()
      ctx.moveTo(seg[0].x, seg[0].y)
      for (let i = 1; i < seg.length; i++) ctx.lineTo(seg[i].x, seg[i].y)
      ctx.strokeStyle = meta.color
      ctx.lineWidth = 1.75
      ctx.lineJoin = 'round'
      ctx.lineCap = 'round'
      ctx.stroke()
      seg = []
    }
    for (let i = 0; i < coords.length; i++) {
      if (!coords[i]) flush()
      else seg.push(coords[i])
    }
    flush()

    if (series.length <= 40) {
      ctx.fillStyle = meta.color
      for (let i = 0; i < coords.length; i++) {
        if (!coords[i]) continue
        ctx.beginPath()
        ctx.arc(coords[i].x, coords[i].y, 2.2, 0, Math.PI * 2)
        ctx.fill()
      }
    }
    let last = null
    for (let i = coords.length - 1; i >= 0; i--) {
      if (coords[i]) {
        last = coords[i]
        break
      }
    }
    if (last) {
      ctx.beginPath()
      ctx.arc(last.x, last.y, 5.5, 0, Math.PI * 2)
      ctx.fillStyle = '#1c1b18'
      ctx.fill()
      ctx.lineWidth = 2
      ctx.strokeStyle = meta.color
      ctx.stroke()
    }
  }

  ctx.shadowBlur = 0
  ctx.fillStyle = '#8d867a'
  ctx.font = '11px sans-serif'
  ctx.textBaseline = 'top'
  if (series.length) {
    ctx.textAlign = 'left'
    ctx.fillText(formatClock(series[0].ts), padL, padT + plotH + 8)
    ctx.textAlign = 'right'
    ctx.fillText(formatClock(series[series.length - 1].ts), width - padR, padT + plotH + 8)
  }
}

module.exports = {
  METRICS: METRICS,
  computeScale: computeScale,
  statsOf: statsOf,
  drawChart: drawChart
}
