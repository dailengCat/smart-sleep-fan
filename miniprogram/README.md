# 睡眠风扇监控小程序

原生微信小程序，用来看 ESP32 睡眠风扇上报的温湿度、毫米波雷达、睡眠状态和风扇档位。界面按床头夜灯来做：暖黑底、月光色字、大号室温和很慢的呼吸光晕，不是通用物联网仪表盘。动效用 CSS 过渡，卡片和按钮按压缩放，档位与分段控件带回弹。底部仍是「监控 / 控制 / 趋势 / 设置」。

设备用 TCP MQTT 向公共 Broker `broker.emqx.io:1883` 的 `sleepfan/status` 发布 JSON，大约每 10 秒一帧。小程序不能走 TCP，这里用 WebSocket MQTT：`wss://broker.emqx.io:8084/mqtt`。

工程自带 MQTT 3.1.1 编解码（`utils/mqtt-codec.js`、`utils/mqtt.js`），**不用 npm，也不用微信开发者工具的「构建 npm」**。用微信开发者工具导入本目录即可。

## 导入

1. 安装 [微信开发者工具](https://developers.weixin.qq.com/miniprogram/dev/devtools/download.html) 稳定版。不要用 HBuilderX 或 uni-app 打开。
2. 选择「导入项目」，目录选本仓库根目录（能看到 `project.config.json` 和 `app.json` 的那一层）。
3. AppID：
   - 直接用仓库里的 `touristappid` 可以游客模式打开。
   - 需要预览、真机调试时，在导入窗口选测试号，或把 `project.config.json` 的 `appid` 换成你自己的小程序 AppID。
4. 基础库使用 `3.5.5`（工具会按 `project.config.json` 的 `libVersion` 提示）。趋势页用 canvas 2d，不要降到过旧的基础库。

## 开发阶段连上 EMQX

公共 Broker 不在小程序默认合法域名里。仓库已经把 `project.config.json` 和 `project.private.config.json` 的 `urlCheck` 设为 `false`。

如果控制台仍出现 `url not in domain list`：

1. 打开右上角「详情 → 本地设置」。
2. 勾选「不校验合法域名、web-view（业务域名）、TLS 版本以及 HTTPS 证书」。
3. 重新编译。

正式发布前，到微信公众平台「开发 → 开发管理 → 开发设置 → 服务器域名 → socket 合法域名」添加：

```text
broker.emqx.io
```

只填主机名，不要写 `wss://`，不要写 `:8084`，不要写 `/mqtt`。正式版必须使用 `wss://`，`ws://` 只能在开发者工具里碰运气。

## 和设备联调

1. 用 [MQTTX](https://mqttx.app/) 新建连接：Host `broker.emqx.io`，端口 `1883`，MQTT 3.1.1，不要用户名密码。
2. 订阅 `sleepfan/status`，QoS 0。确认能看到类似下面的 JSON，再打开小程序。设备不上线时，小程序会停在「等待设备上报…」，这是正常的。
3. 编译小程序。监控页顶部应变成「已连接」，并显示简短的设备 `client_id`。大约 10 秒内温湿度、雷达、睡眠、档位、决策原因、MQ135 会刷新。
4. 调试器 Console 过滤 `[sleepfan]`。连上、订阅、每一帧状态都会打日志。
5. 公共 Broker 上可能有别人的报文。到「设置」填 `sleepfan-`，或点「只看最近设备」锁定完整 `client_id`。过滤是区分大小写的包含匹配。
6. 下拉监控页可以立即重连。

状态示例：

```json
{"client_id":"sleepfan-1234abcd","temperature":26.5,"humidity":40.0,"temp_hum_ok":true,"presence":true,"distance_cm":120,"radar_online":true,"sleeping":false,"sleep_state":"awake","fan_level":3,"duty":60,"auto_mode":true,"decision_level":3,"decision_reason":"mild 26.5C","mq135_raw":1500,"uptime_ms":123456}
```

| 字段 | 页面上怎么用 |
| --- | --- |
| `client_id` | 顶部设备名；设置里可按它过滤 |
| `temperature` / `humidity` | 大号温湿度。`temp_hum_ok` 为 false 时标「传感器异常」，原始数仍显示 |
| `presence` / `distance_cm` / `radar_online` | 有人/无人、距离、雷达是否在线。离线时不改写有人/无人 |
| `sleeping` / `sleep_state` | 睡眠卡片。`awake` 显示清醒，`light` / `deep` / `rem` 有对应中文，不认识的字符串原样显示 |
| `fan_level` | 0 关闭，1–5 档。`-1` 显示「自定义」，并用 `duty` 画占空比 |
| `auto_mode` | 自动开 / 自动关 |
| `decision_level` / `decision_reason` | 决策原因原样显示。决策档位和当前档位不同时才额外写一行 |
| `mq135_raw` | 原始值，并标明未校准 |
| `uptime_ms` | 换算成本地的运行时间 |

## 控制下行：固件还没接

控制页可以选 0–5 档、开关自动、摇头「开 / 关」和「点动」。点击后会向 `sleepfan/cmd` 发布 QoS 0 JSON，并 toast「已发送指令，设备侧待接入」。

当前 ESP32 固件**只发布** `sleepfan/status`，**没有订阅** `sleepfan/cmd`。因此这些按钮不会改档位、不会开关自动、也不会让风扇摇头。页面上的高亮只表示「这只小程序刚才发出过」，不是设备的真实状态。设备真实状态在控制页「设备刚刚上报」和监控页里。

未连上 Broker 时不会发报文，toast 是「未连接，指令未发出」。

预留报文：

```json
{"cmd":"set_fan_level","level":3}
{"cmd":"set_auto","auto":true}
{"cmd":"set_auto","auto":false}
{"cmd":"oscillate","action":"toggle"}
{"cmd":"oscillate","action":"step"}
```

要用 MQTTX 核对的话，另开一个订阅 `sleepfan/cmd`。能看到上面的 JSON，只说明 Broker 收到了，不说明风扇执行了。

## 四个页面

- **监控**：连接状态、设备 ID、温湿度、雷达、睡眠、风扇档位和决策原因、MQ135、最近更新时间。没收到帧时显示「等待设备上报…」。
- **控制**：档位、自动、摇头。只发布预留指令。
- **趋势**：本机环形保存最近 60 条温度、湿度、档位，用 canvas 2d 自绘。可切换指标。`fan_level` 为 -1 的点留空，不把占空比换算成档位。更换过滤或订阅主题会清空曲线。
- **设置**：Broker、订阅主题、下行主题、Client ID 过滤、小程序自己的 MQTT Client ID。Client ID 形如 `wxsf-` 加随机串，存在本地，避免和设备的 `sleepfan-` 撞车。

默认：

| 项 | 值 |
| --- | --- |
| Broker | `wss://broker.emqx.io:8084/mqtt` |
| 订阅 | `sleepfan/status` |
| 下行 | `sleepfan/cmd` |
| 过滤 | 空，收到该主题上所有状态帧 |

## 目录

```text
app.js / app.json / app.wxss
project.config.json
project.private.config.json
sitemap.json
custom-tab-bar/          底部四个 Tab
components/conn-bar/     连接状态条
pages/index              监控
pages/control            控制
pages/trend              趋势
pages/settings           设置
utils/config.js          配置与本地持久化
utils/store.js           最新状态和 60 点缓存
utils/mqtt.js            WebSocket MQTT 连接
utils/mqtt-codec.js      MQTT 3.1.1 编解码
utils/format.js          首页文案
utils/chart.js           canvas 曲线
```

本地存储键：`sleepfan_config_v1`、`sleepfan_last_device`、`sleepfan_history_v1`。清缓存后 Client ID 会重新生成，曲线会空。

## 可选自测

不需要跑这些也能导入小程序。有 Node 18+ 时可以检查编解码和公共 Broker：

```bash
node tests/unit.test.js
node tests/live-broker.js
```

`live-broker.js` 只往临时主题 `sleepfan-selftest/...` 自发自收，不会往 `sleepfan/status` 或 `sleepfan/cmd` 写数据。

## 常见问题

- **一直「连接中」或提示不在合法域名**：按上面的步骤关掉域名校验，确认地址是 `wss://broker.emqx.io:8084/mqtt`。
- **已连接，但一直等待上报**：先用 MQTTX 看 `sleepfan/status` 有没有 JSON。过滤太严也会把帧丢掉，页面会写「已忽略 N 条」。
- **首页数字在跳，但不是我的板子**：设置里填完整 `client_id`，或至少填 `sleepfan-`。
- **点了控制，风扇没动**：这是预期。固件还没订阅 `sleepfan/cmd`。
- **趋势是空的**：多留在小程序里等几帧。曲线存在手机本地，不在 Broker 上。
