# 控制面 REST API

> 本文照着 `src/control/ControlApi.cpp` 的**实际实现**写。
> 实现与 `docs/plan.md` §4.15 的表格有出入的地方，一律以实现为准，文末列了差异。

模拟器内置一个小 REST 控制面，用来在**不重启进程**的前提下改配置：
加相机、开关故障注入、触发事件、看会话与对讲统计。
GUI、e2e 测试、CI 脚本走的都是这一套。

> **监听地址**：控制面默认只听 `127.0.0.1` —— 它能改整台设备的行为、触发事件、
> 甚至把相机打到离线，不该随手暴露到网上。要从别的机器（或容器宿主）访问，
> 用 `--control-bind 0.0.0.0` 显式放开，并配合 `--control-token` 加个令牌。

## 基本约定

| 项 | 值 |
|---|---|
| 默认地址 | `http://127.0.0.1:9000` |
| 监听地址 | `SimulatorConfig::controlApiAddress`，默认 `127.0.0.1`（**只听本机**） |
| 端口 | `--control-port`，默认 `9000` |
| 令牌 | `--control-token`，默认空（不校验） |
| 编码 | 请求与响应都是 `application/json; charset=utf-8` |
| CORS | 全开（`Access-Control-Allow-Origin: *`），`OPTIONS` 预检回 `204` |
| 连接 | 每个响应都带 `Connection: close`，不复用 |

容器里跑的时候记得把 `controlApiAddress` 设成 `0.0.0.0`，
否则默认只听 `127.0.0.1`，宿主连不上。

### 令牌

设了 `--control-token` 之后，两种带法都认：

```bash
curl -H "Authorization: Bearer secret" http://127.0.0.1:9000/api/cameras
curl "http://127.0.0.1:9000/api/cameras?token=secret"
```

查询参数那种是给 SSE 用的 —— 浏览器的 `EventSource` 加不了请求头。

### 状态码

| 码 | 含义 |
|---|---|
| `200` | 成功 |
| `201` | 相机创建成功（只有 `POST /api/cameras`） |
| `204` | `OPTIONS` 预检 |
| `400` | 请求体不是合法 JSON 对象，或参数值非法 |
| `401` | 令牌不对 |
| `404` | 没有这个端点，或没有这台相机 |
| `405` | 这个路径不支持这个方法 |
| `409` | 创建相机 / 启动相机 / 加 IP 别名失败 |

错误响应统一是 `{"error": "中文说明"}`。

---

## 端点一览

| 方法 | 路径 | 用途 |
|---|---|---|
| GET | `/api/cameras` | 相机列表（含状态、XAddr、流地址、quirks） |
| POST | `/api/cameras` | 从品牌预设新建一台 |
| GET | `/api/cameras/{id}` | 单台的完整模型 |
| PATCH | `/api/cameras/{id}` | 改 quirks / 预设 / 上下线 / 模型字段 |
| DELETE | `/api/cameras/{id}` | 删掉一台 |
| POST | `/api/cameras/{id}/events` | 触发事件 |
| GET | `/api/cameras/{id}/ptz` | PTZ 位置、移动状态、预置位 |
| POST | `/api/cameras/{id}/ptz` | PTZ 控制 |
| GET | `/api/cameras/{id}/sessions` | RTSP 会话、订阅、HTTP 客户端数 |
| GET | `/api/cameras/{id}/talkback` | 对讲接收统计 |
| POST | `/api/cameras/{id}/offline` | 模拟离线 N 秒 |
| GET | `/api/scenario` | 导出当前场景 |
| POST | `/api/scenario` | 加载场景（给路径或给整份 JSON） |
| GET | `/api/network` | 网卡列表、网络模式、已占的 IP 别名 |
| POST | `/api/network` | 分配 / 回收 IP 别名 |
| GET | `/api/quirks` | 全部故障注入开关的元数据 |
| GET | `/api/presets` | 全部品牌预设 |
| GET | `/api/log` | 日志流（SSE） |
| GET | `/metrics` | Prometheus 文本 |
| GET | `/openapi.json` | 端点清单（从实际路由生成） |

---

## 相机

### `GET /api/cameras`

```bash
curl -s http://127.0.0.1:9000/api/cameras | jq
```

返回一个数组。每项是相机模型（`CameraModel::toJson()`）再加四个运行期字段：

```json
[
  {
    "id": "cam1",
    "displayName": "标准 ONVIF Generic 1",
    "persona": "generic",
    "identity": {
      "manufacturer": "ONVIFSim",
      "model": "Virtual Camera",
      "firmwareVersion": "1.0.0",
      "serialNumber": "SIM0001",
      "hardwareId": "ONVIFSIM-HW",
      "endpointReference": "urn:uuid:8f3c...",
      "hostname": "onvifsim-1",
      "location": "lab",
      "scopes": ["onvif://www.onvif.org/type/video_encoder", "..."]
    },
    "network": { "bindAddress": "0.0.0.0", "httpPort": 8000, "rtspPort": 8554 },
    "users": [{ "username": "admin", "password": "admin123", "level": "Administrator" }],
    "profiles": [ { "token": "Profile_1", "name": "MainStream", "videoEncoder": { "...": "..." } } ],
    "ptzNode": { "nodeToken": "PTZNode_1", "maxPresets": 300, "...": "..." },
    "capabilities": { "ptz": true, "events": true, "media2": true, "backchannel": true },
    "enabled": true,

    "status": {
      "running": true, "offline": false,
      "rtspSessions": 0, "subscriptions": 0, "httpClients": 1,
      "lastEventTopic": ""
    },
    "xaddr": "http://192.168.1.50:8000/onvif/device_service",
    "streamUris": ["rtsp://192.168.1.50:8554/profile1", "..."],
    "quirks": {}
  }
]
```

> **`xaddr` 与 `streamUris` 是「对外宣称」的地址**，会被 quirk 改脏
> （A5 把端口改成 `:2020`、B2 塞进 `user:pass@`、占位 IP 之类）。
> 要**真连得上**的地址，用 `network.httpPort` / `network.rtspPort` 自己拼。
> e2e 就是这么干的，也和参照客户端「只接管 host:port、保留 path」的做法一致。

### `POST /api/cameras`

从品牌预设新建一台。端口按当前网络模式自动分配。

```bash
curl -s -X POST http://127.0.0.1:9000/api/cameras \
  -H 'Content-Type: application/json' \
  -d '{"preset": "hikvision"}' | jq
```

| 字段 | 类型 | 说明 |
|---|---|---|
| `preset` | string | 预设 key，默认 `"generic"`。见 `GET /api/presets` |
| `quirks` | object | 可选，建好就打开这些 quirk |

成功返回 **`201`** + 相机模型（外加 `xaddr`）。
预设不存在或端口占用返回 `409`。

### `GET /api/cameras/{id}`

单台的完整模型，外加 `quirks` 与 `xaddr`（没有 `status` / `streamUris` ——
那两个只在列表里）。相机不存在返回 `404`。

### `PATCH /api/cameras/{id}`

改配置。四类字段可以混在一个请求里：

| 字段 | 语义 |
|---|---|
| `quirks` | **合并**：只覆盖提到的 quirk，没提到的保持不变 |
| `preset` | 换品牌预设（重填身份三件套与路径风格） |
| `enabled` | `true` 启动、`false` 停止这台相机 |
| `identity` / `network` / `profiles` | 整体覆盖对应的子对象 |

```bash
# 打开两条 quirk（其余的原样保留）
curl -s -X PATCH http://127.0.0.1:9000/api/cameras/cam1 \
  -H 'Content-Type: application/json' \
  -d '{"quirks": {
        "connect.xaddr_odd_port": {"enabled": true, "params": {"port": 2020}},
        "events.bad_xml": true
      }}' | jq .quirks

# 关掉一条 —— 要显式写 false，"不提" 等于 "不动"
curl -s -X PATCH http://127.0.0.1:9000/api/cameras/cam1 \
  -H 'Content-Type: application/json' \
  -d '{"quirks": {"events.bad_xml": false}}'

# 让相机下线（端口全关）
curl -s -X PATCH http://127.0.0.1:9000/api/cameras/cam1 \
  -H 'Content-Type: application/json' -d '{"enabled": false}'
```

quirk 的值有两种写法：

- `true` / `false` —— 只开关，参数用默认值；
- `{"enabled": true, "params": {...}}` —— 带参数。

未知的 key、越界的参数值不会让整个请求失败，而是进响应的 `warnings` 数组：

```json
{ "id": "cam1", "quirks": {}, "warnings": ["未知的 quirk：no.such.key"] }
```

### `DELETE /api/cameras/{id}`

```bash
curl -s -X DELETE http://127.0.0.1:9000/api/cameras/cam2
# {"ok":true}
```

---

## 事件

### `POST /api/cameras/{id}/events`

触发一次事件，已订阅的 PullPoint 客户端就能拉到。

```bash
# 按语义种类触发，持续 5 秒（属性型事件到点会补一条 state=false）
curl -s -X POST http://127.0.0.1:9000/api/cameras/cam1/events \
  -H 'Content-Type: application/json' \
  -d '{"kind": "Motion", "duration": 5}'

# 直接给完整 topic 串
curl -s -X POST http://127.0.0.1:9000/api/cameras/cam1/events \
  -H 'Content-Type: application/json' \
  -d '{"topic": "tns1:RuleEngine/CellMotionDetector/Motion", "state": true, "duration": 3}'
```

| 字段 | 类型 | 说明 |
|---|---|---|
| `topic` | string | 完整 topic 串。给了就优先用它 |
| `kind` | string | 语义种类，默认 `"Motion"`。不认识返回 `400` |
| `state` | bool | 属性型事件的取值，默认 `true`（只在给了 `topic` 时用） |
| `duration` | int | 持续秒数，`0` 表示瞬时事件 |

`kind` 的取值：`Motion`、`MotionAlarm`、`LineCrossing`、`FieldIntrusion`、`Tamper`、
`SceneChange`、`AudioDetected`、`ImageTooDark`、`DigitalInput`、`RelayOutput`、
`ProcessorUsage`、`PeopleDetect`、`VehicleDetect`、`AnimalDetect`、`FaceDetect`。

实际吐出去的 topic 串由 quirk `events.topic_style` 决定
（`onvif` / `tplink` / `reolink` / `axis` 四套命名）。

---

## PTZ

### `GET /api/cameras/{id}/ptz`

```bash
curl -s http://127.0.0.1:9000/api/cameras/cam1/ptz | jq
```

```json
{
  "pan": 0.35, "tilt": -0.1, "zoom": 0.0,
  "panTiltStatus": "MOVING",
  "zoomStatus": "IDLE",
  "presets": [{ "token": "1", "name": "大门" }]
}
```

`panTiltStatus` / `zoomStatus` 是 `IDLE` / `MOVING` / `UNKNOWN`。

### `POST /api/cameras/{id}/ptz`

```bash
# 连续移动
curl -s -X POST http://127.0.0.1:9000/api/cameras/cam1/ptz \
  -H 'Content-Type: application/json' \
  -d '{"action": "continuous", "pan": 0.5, "tilt": 0.2, "timeout": 3}'

# 停
curl -s -X POST http://127.0.0.1:9000/api/cameras/cam1/ptz \
  -H 'Content-Type: application/json' -d '{"action": "stop"}'

# 一键生成海康那种出厂 300 预置位（C6）
curl -s -X POST http://127.0.0.1:9000/api/cameras/cam1/ptz \
  -H 'Content-Type: application/json' \
  -d '{"action": "generate_factory_presets", "count": 300}'
```

| `action` | 用到的字段 |
|---|---|
| `continuous` | `pan` / `tilt` / `zoom`（速度，-1..1）、`timeout`（秒） |
| `absolute` | `pan` / `tilt` / `zoom`（目标位置） |
| `relative` | `pan` / `tilt` / `zoom`（相对位移） |
| `stop` | 无 |
| `goto_preset` | `token` |
| `set_preset` | `name` |
| `generate_factory_presets` | `count`，默认 300 |

`pan` / `tilt` / `zoom` 哪个都可以省 —— 省掉的分量不参与本次运动
（`hasPanTilt` / `hasZoom` 按字段在不在决定）。未知的 `action` 返回 `400`。

---

## 会话与对讲

### `GET /api/cameras/{id}/sessions`

```bash
curl -s http://127.0.0.1:9000/api/cameras/cam1/sessions | jq
```

```json
{
  "rtsp": [
    { "id": "12345678", "peer": "192.168.1.10:51234", "profile": "Profile_1",
      "playing": true, "backchannel": true, "startedAt": "2026-09-10T08:12:00Z" }
  ],
  "subscriptions": [
    { "id": "sub-1", "address": "http://192.168.1.50:8000/onvif/subscription/sub-1",
      "queued": 0, "pulled": 12, "dropped": 0,
      "createdAt": "2026-09-10T08:11:40Z",
      "terminationTime": "2026-09-10T08:12:40Z",
      "peer": "192.168.1.10:51230" }
  ],
  "httpClients": 2
}
```

订阅泄漏（A9）、槽位上限（D3）、订阅换端口（D1）都靠这个端点观察。

### `GET /api/cameras/{id}/talkback`

```bash
curl -s http://127.0.0.1:9000/api/cameras/cam1/talkback | jq
```

```json
{
  "sessions": [
    { "session": "12345678", "packets": 250, "bytes": 43000, "dropped": 0,
      "markers": 3, "markerMissing": 0,
      "level": 0.42, "peakLevel": 0.81, "codec": "PCMU" }
  ],
  "totalBytes": 43000
}
```

只列出**带 backchannel 且已建好接收器**的会话；没有对讲时是空数组 + `totalBytes: 0`。

- `markers` —— 收到的 talkspurt 首包数（marker=1）；
- `markerMissing` —— 首包 marker=0 的次数，对应 quirk E13。

### `POST /api/cameras/{id}/offline?seconds=N`

把相机的全部监听端口关掉 N 秒再恢复，用来测客户端的重连。

```bash
curl -s -X POST 'http://127.0.0.1:9000/api/cameras/cam1/offline?seconds=15'
# {"ok":true}
```

`seconds` 缺省或 `<= 0` 时按 **10 秒**处理。注意参数在**查询串**里，不是请求体。

---

## 场景

### `GET /api/scenario`

把当前整套环境导出成场景 JSON（格式见 [`scenarios.md`](scenarios.md)），
可以直接存成文件下次 `--scenario` 加载。

```bash
curl -s http://127.0.0.1:9000/api/scenario > my-lab.json
```

### `POST /api/scenario`

两种用法：

```bash
# 从磁盘加载
curl -s -X POST http://127.0.0.1:9000/api/scenario \
  -H 'Content-Type: application/json' \
  -d '{"path": "assets/scenarios/tplink-full-house.json"}'

# 直接把整份场景 POST 进来
curl -s -X POST http://127.0.0.1:9000/api/scenario \
  -H 'Content-Type: application/json' \
  --data-binary @my-lab.json
```

请求体里有 `path` 就走磁盘加载；否则把整个请求体当成场景内容。
**两种都会先停掉并清空现有相机**再按新场景重建。

```json
{ "ok": true, "cameras": 8, "warnings": ["未知的 quirk：old.key"] }
```

解析里的未知字段与非法值进 `warnings`，不会中断加载 —— 旧场景文件在新版本上仍然能用。

---

## 网络

### `GET /api/network`

```json
{
  "interfaces": [
    { "name": "eth0", "displayName": "eth0", "mac": "aa:bb:cc:dd:ee:ff",
      "up": true, "multicast": true, "addresses": ["192.168.1.50", "fe80::1"] }
  ],
  "mode": "ports",
  "needsElevation": true,
  "elevationHint": "添加 IP 别名需要 root 权限……",
  "ownedAliases": []
}
```

`mode` 是 `ports` / `ip_alias` / `external` 三选一。

### `POST /api/network`

```bash
# 在 eth0 上加 8 个连续别名，让每台相机拥有独立 IP
curl -s -X POST http://127.0.0.1:9000/api/network \
  -H 'Content-Type: application/json' \
  -d '{"action": "add_range", "interface": "eth0",
       "start": "192.168.1.201", "count": 8, "netmask": "255.255.255.0"}'

# 回收本进程加过的全部别名
curl -s -X POST http://127.0.0.1:9000/api/network \
  -H 'Content-Type: application/json' -d '{"action": "remove_all"}'
```

| `action` | 字段 |
|---|---|
| `add_range` | `interface`、`start`、`count`（默认 1）、`netmask`（默认 `255.255.255.0`） |
| `remove_all` | 无 |

提权失败返回 **`409`**，并把手动命令一起给出来 —— **不会静默降级**到端口模式：

```json
{ "ok": false, "error": "……", "manualCommands": ["sudo ip addr add 192.168.1.201/24 dev eth0"] }
```

`start` 不是合法 IP 返回 `400`，未知 `action` 也是 `400`。

---

## 元数据

### `GET /api/quirks`

全部故障注入开关的元数据。GUI 的复选框、`docs/quirks.md`、
e2e 的覆盖率守卫都从这里取 —— **`src/core/Quirks.cpp` 里那张表是唯一真源**。

```bash
curl -s http://127.0.0.1:9000/api/quirks | jq '.[] | select(.sourceId == "D1")'
```

```json
{
  "key": "events.subscription_port_increment",
  "group": "events",
  "groupTitle": "D. 事件",
  "sourceId": "D1",
  "title": "订阅管理器开在独立端口且每次递增",
  "description": "真机 TL-IPC652P-A4 ……",
  "params": [
    { "name": "base_port", "default": 1024, "min": 1, "max": 65535,
      "description": "起始端口" }
  ]
}
```

`params` 只在这条 quirk 有参数时出现。参数里 `min`/`max`（数值型）与
`choices`（枚举型）也只在有的时候出现。

```bash
# 按分组统计
curl -s http://127.0.0.1:9000/api/quirks | jq -r '.[].group' | sort | uniq -c
```

### `GET /api/presets`

全部品牌预设：`generic`、`hikvision`、`dahua`、`reolink`、`vigi`、`tplink`、
`axis`、`uniview`。每项含身份三件套、RTSP / 快照路径风格、topic 命名风格、
私有 API 种类，以及这个预设默认打开哪些 quirk。

```bash
curl -s http://127.0.0.1:9000/api/presets | jq -r '.[] | "\(.key)\t\(.rtspMainPath)"'
```

### `GET /api/log`（SSE）

日志流。接上先回填最近 200 条历史，之后实时推。

```bash
curl -N http://127.0.0.1:9000/api/log
```

```
data: {"time":"2026-09-10T08:12:00.123Z","level":"INFO","category":"soap","camera":"cam1","peer":"192.168.1.10:51234","summary":"GetCapabilities","ok":true,"durationUs":1820}
```

| 字段 | 说明 |
|---|---|
| `time` | ISO 8601，毫秒精度，UTC |
| `level` | `DEBUG` / `INFO` / `WARN` / `ERROR` |
| `category` | `soap` / `rtsp` / `discovery` / `core` … |
| `camera` | 相机 id，进程级日志为空 |
| `peer` | 客户端地址 |
| `summary` | 一行摘要 |
| `ok` | 这次处理成不成功 |
| `quirk` | 可选：这条日志是哪条 quirk 造成的 |
| `durationUs` | 可选：处理耗时（微秒） |

回填的那 200 条只有 `time` / `level` / `category` / `camera` / `summary` 五个字段。

浏览器里用 `EventSource`（加不了请求头，令牌走查询参数）：

```js
const es = new EventSource("http://127.0.0.1:9000/api/log?token=secret");
es.onmessage = (e) => console.log(JSON.parse(e.data));
```

### `GET /metrics`

Prometheus 文本格式，压测时直接抓：

```
# HELP onvifsim_cameras 相机数量
# TYPE onvifsim_cameras gauge
onvifsim_cameras 8
onvifsim_rtsp_sessions{camera="cam1"} 2
onvifsim_subscriptions{camera="cam1"} 1
onvifsim_camera_online{camera="cam1"} 1
```

### `GET /openapi.json`

一份最小的 OpenAPI 3.0 文档，`paths` 从**实际路由**列出来，
所以它不会和实现漂移。当前只有路径清单，没有 schema。

---

## 和 plan.md §4.15 的差异

实现比计划里的表格多了几个东西，文档以实现为准：

| 差异 | 说明 |
|---|---|
| 多了 `GET /api/presets` | 计划的表里没有，但 GUI 的「添加相机」需要它 |
| 多了 `GET /api/cameras/{id}` | 计划只列了 `PATCH` / `DELETE` |
| 多了 `GET /api/cameras/{id}/ptz` | 计划只列了 `POST`；GET 用来读位置与预置位 |
| 多了 `GET /openapi.json` | 计划里写的是「附 OpenAPI JSON」，实现放在了根路径而不是 `/api/` 下 |
| `POST /api/network` 用 `action` 区分 | 计划只写了「分配 / 回收 IP 别名」，实现是 `add_range` / `remove_all` |
| `PATCH` 的 `quirks` 是合并语义 | 计划没说；实现走 `Quirks::merge()`，关一条要显式写 `false` |
| `offline` 的 `seconds` 在查询串里 | 计划的表里写的是 `?seconds=`，实现一致，这里只是强调不是请求体 |

## 自动化里怎么用

e2e 测试的封装在 `tests/e2e/helpers/control.py`，
`tests/e2e/test_control_api.py` 把每个端点都打了一遍 ——
实现改了而这份文档没跟上，那些用例会先红。
