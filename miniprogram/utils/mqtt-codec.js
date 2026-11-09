/**
 * MQTT 3.1.1 报文编解码。
 * 只实现本项目用到的类型：CONNECT / CONNACK / PUBLISH / PUBACK / SUBSCRIBE / SUBACK / PING / DISCONNECT。
 * 不依赖 mqtt.js，微信开发者工具导入后即可运行。
 */

function utf8Encode(str) {
  const input = String(str)
  const out = []
  for (let i = 0; i < input.length; i++) {
    let code = input.charCodeAt(i)
    if (code >= 0xd800 && code <= 0xdbff && i + 1 < input.length) {
      const next = input.charCodeAt(i + 1)
      if (next >= 0xdc00 && next <= 0xdfff) {
        code = 0x10000 + ((code - 0xd800) << 10) + (next - 0xdc00)
        i++
      }
    }
    if (code < 0x80) {
      out.push(code)
    } else if (code < 0x800) {
      out.push(0xc0 | (code >> 6), 0x80 | (code & 0x3f))
    } else if (code < 0x10000) {
      out.push(0xe0 | (code >> 12), 0x80 | ((code >> 6) & 0x3f), 0x80 | (code & 0x3f))
    } else {
      out.push(
        0xf0 | (code >> 18),
        0x80 | ((code >> 12) & 0x3f),
        0x80 | ((code >> 6) & 0x3f),
        0x80 | (code & 0x3f)
      )
    }
  }
  return new Uint8Array(out)
}

function utf8Decode(bytes) {
  let out = ''
  for (let i = 0; i < bytes.length; i++) {
    const b1 = bytes[i]
    if (b1 < 0x80) {
      out += String.fromCharCode(b1)
    } else if ((b1 & 0xe0) === 0xc0 && i + 1 < bytes.length) {
      const b2 = bytes[++i] & 0x3f
      out += String.fromCharCode(((b1 & 0x1f) << 6) | b2)
    } else if ((b1 & 0xf0) === 0xe0 && i + 2 < bytes.length) {
      const b2 = bytes[++i] & 0x3f
      const b3 = bytes[++i] & 0x3f
      out += String.fromCharCode(((b1 & 0x0f) << 12) | (b2 << 6) | b3)
    } else if ((b1 & 0xf8) === 0xf0 && i + 3 < bytes.length) {
      const b2 = bytes[++i] & 0x3f
      const b3 = bytes[++i] & 0x3f
      const b4 = bytes[++i] & 0x3f
      let code = ((b1 & 0x07) << 18) | (b2 << 12) | (b3 << 6) | b4
      code -= 0x10000
      out += String.fromCharCode(0xd800 + (code >> 10), 0xdc00 + (code & 0x3ff))
    }
  }
  return out
}

function concat(parts) {
  let len = 0
  for (let i = 0; i < parts.length; i++) len += parts[i].length
  const out = new Uint8Array(len)
  let offset = 0
  for (let i = 0; i < parts.length; i++) {
    out.set(parts[i], offset)
    offset += parts[i].length
  }
  return out
}

function encodeRemaining(len) {
  const bytes = []
  do {
    let digit = len % 128
    len = Math.floor(len / 128)
    if (len > 0) digit = digit | 0x80
    bytes.push(digit)
  } while (len > 0)
  return new Uint8Array(bytes)
}

function strField(str) {
  const bytes = utf8Encode(str)
  if (bytes.length > 65535) throw new Error('MQTT 字符串过长')
  const out = new Uint8Array(2 + bytes.length)
  out[0] = (bytes.length >> 8) & 0xff
  out[1] = bytes.length & 0xff
  out.set(bytes, 2)
  return out
}

function u16(value) {
  return new Uint8Array([(value >> 8) & 0xff, value & 0xff])
}

function buildPacket(typeFlags, body) {
  const rl = encodeRemaining(body.length)
  const out = new Uint8Array(1 + rl.length + body.length)
  out[0] = typeFlags
  out.set(rl, 1)
  out.set(body, 1 + rl.length)
  return out
}

function encodeConnect(clientId, keepAlive) {
  const body = concat([
    strField('MQTT'),
    new Uint8Array([0x04]),
    new Uint8Array([0x02]),
    u16(keepAlive),
    strField(clientId)
  ])
  return buildPacket(0x10, body)
}

function encodeSubscribe(packetId, topic, qos) {
  const body = concat([
    u16(packetId),
    strField(topic),
    new Uint8Array([qos & 0x03])
  ])
  return buildPacket(0x82, body)
}

function encodePublish(topic, payloadBytes, qos, packetId) {
  const parts = [strField(topic)]
  let flags = 0x30
  if (qos === 1) {
    flags = 0x32
    parts.push(u16(packetId || 1))
  }
  parts.push(payloadBytes)
  return buildPacket(flags, concat(parts))
}

function encodePuback(packetId) {
  return buildPacket(0x40, u16(packetId))
}

function encodePingreq() {
  return new Uint8Array([0xc0, 0x00])
}

function encodeDisconnect() {
  return new Uint8Array([0xe0, 0x00])
}

function readString(body, offset) {
  if (offset + 2 > body.length) throw new Error('MQTT 字符串越界')
  const len = (body[offset] << 8) | body[offset + 1]
  const start = offset + 2
  const end = start + len
  if (end > body.length) throw new Error('MQTT 字符串越界')
  return {
    value: utf8Decode(body.subarray(start, end)),
    offset: end
  }
}

function decodeOne(header, body) {
  const type = header >> 4
  const flags = header & 0x0f
  if (type === 2) {
    return {
      type: 'connack',
      sessionPresent: body.length > 0 && (body[0] & 0x01) === 1,
      returnCode: body.length > 1 ? body[1] : -1
    }
  }
  if (type === 3) {
    const qos = (flags >> 1) & 0x03
    const topic = readString(body, 0)
    let offset = topic.offset
    let packetId = 0
    if (qos > 0) {
      if (offset + 2 > body.length) throw new Error('PUBLISH 缺少报文标识')
      packetId = (body[offset] << 8) | body[offset + 1]
      offset += 2
    }
    return {
      type: 'publish',
      qos: qos,
      dup: (flags & 0x08) !== 0,
      retain: (flags & 0x01) !== 0,
      topic: topic.value,
      packetId: packetId,
      payload: body.subarray(offset)
    }
  }
  if (type === 4) {
    const packetId = body.length >= 2 ? ((body[0] << 8) | body[1]) : 0
    return { type: 'puback', packetId: packetId }
  }
  if (type === 9) {
    const packetId = body.length >= 2 ? ((body[0] << 8) | body[1]) : 0
    const codes = []
    for (let i = 2; i < body.length; i++) codes.push(body[i])
    return { type: 'suback', packetId: packetId, codes: codes }
  }
  if (type === 13) return { type: 'pingresp' }
  if (type === 14) return { type: 'disconnect' }
  return { type: 'unknown', packetType: type, flags: flags }
}

function decodePackets(buffer) {
  const bytes = buffer instanceof Uint8Array ? buffer : new Uint8Array(buffer)
  const packets = []
  let offset = 0
  while (offset < bytes.length) {
    if (bytes.length - offset < 2) break
    const header = bytes[offset]
    let multiplier = 1
    let remaining = 0
    let pos = offset + 1
    let used = 0
    let digit = 0
    for (;;) {
      if (pos >= bytes.length) return { packets: packets, consumed: offset }
      digit = bytes[pos++]
      remaining += (digit & 127) * multiplier
      multiplier *= 128
      used++
      if (used > 4) throw new Error('MQTT remaining length 非法')
      if ((digit & 128) === 0) break
    }
    if (remaining > 262144) throw new Error('MQTT 报文过大')
    if (bytes.length - pos < remaining) return { packets: packets, consumed: offset }
    packets.push(decodeOne(header, bytes.subarray(pos, pos + remaining)))
    offset = pos + remaining
  }
  return { packets: packets, consumed: offset }
}

module.exports = {
  utf8Encode: utf8Encode,
  utf8Decode: utf8Decode,
  concat: concat,
  encodeConnect: encodeConnect,
  encodeSubscribe: encodeSubscribe,
  encodePublish: encodePublish,
  encodePuback: encodePuback,
  encodePingreq: encodePingreq,
  encodeDisconnect: encodeDisconnect,
  decodePackets: decodePackets
}
