# 场景文件

一份 JSON 描述整套模拟环境：全局配置 + 全部相机 + 各自的故障注入开关。
GUI 的「保存 / 加载场景」、REST 的 `/api/scenario`、命令行的 `--scenario`
走的都是这一套。

```bash
onvifsim --headless --scenario assets/scenarios/tplink-full-house.json
onvifsim --list-scenarios              # 列出内置的八个
```

内置场景是编译进可执行文件的（Qt 资源），源文件在 `assets/scenarios/`。

---

## 格式

```json
{
  "formatVersion": 1,
  "name": "my-lab",
  "description": "一句话说明这个场景是干什么的",

  "config": {
    "networkMode": "ports",
    "bindAddress": "0.0.0.0",
    "httpBasePort": 8000,
    "rtspBasePort": 8554,
    "discoveryEnabled": true,
    "discoveryInterface": "eth0",
    "controlApiEnabled": true,
    "controlApiAddress": "127.0.0.1",
    "controlApiPort": 9000
  },

  "globalQuirks": {
    "transport.global_delay": { "enabled": true, "params": { "ms": 200 } }
  },

  "cameras": [
    {
      "id": "cam1",
      "displayName": "大门",
      "persona": "hikvision",
      "network": { "httpPort": 8000, "rtspPort": 8554 },
      "quirks": {
        "ptz.factory_300_presets": true
      }
    }
  ]
}
```

### 解析是「尽力而为」的

**未知字段与非法值只记进 warnings，不中断解析。**
这样旧场景文件在新版本上照样能用，新写的字段在老版本上也只是被忽略。

- `formatVersion` 比程序认得的新 → 记一条告警，继续解析；
- 未知的 quirk key → 告警，跳过这一条；
- 参数超出范围或不在枚举里 → 告警，用默认值。

告警会出现在日志里，通过 REST 加载时也会出现在响应的 `warnings` 数组里。

### `config`

| 字段 | 默认 | 说明 |
|---|---|---|
| `networkMode` | `"ports"` | `ports` 同 IP 多端口 / `ip_alias` 每台一个 IP / `external` 地址由外部安排（Docker macvlan） |
| `bindAddress` | `"0.0.0.0"` | 监听地址 |
| `httpBasePort` | `8000` | HTTP 起始端口，相机依次递增 |
| `rtspBasePort` | `8554` | RTSP 起始端口，相机依次递增 |
| `discoveryEnabled` | `true` | 开不开 WS-Discovery（UDP 3702，全部相机共用一个 socket） |
| `discoveryInterface` | 空 | 指定网卡；空 = 全部网卡 |
| `controlApiEnabled` | `true` | 开不开 REST 控制面 |
| `controlApiAddress` | `"127.0.0.1"` | 控制面监听地址。**容器里要改成 `0.0.0.0`** |
| `controlApiPort` | `9000` | 控制面端口 |

命令行会覆盖其中一部分：`--control-port`、`--control-token`、`--log-file` 直接覆盖，
`--no-discovery` 只能关不能开（`discoveryEnabled && !--no-discovery`）。

`--http-port` / `--rtsp-port` / `--bind` **显式给出时会覆盖整个场景**：端口按场景里
相机的顺序从给定值开始递增重排，绑定地址整体替换。场景文件里的端口是作者写死的，
撞上本机已有服务时总得有个不改文件的出路：

```bash
# 场景里写的是 8000/8554，本机这两个端口被别的服务占了
onvifsim --headless --scenario assets/scenarios/eight-mixed-brands.json \
         --http-port 18000 --rtsp-port 18554 --bind 127.0.0.1
```

没给这几个选项时，端口与绑定地址完全听场景文件的。

### `globalQuirks`

全局默认的故障注入开关。相机自己的 `quirks` 优先级更高。
格式和相机的 `quirks` 一样。

### `cameras[]`

每一项就是一份 `CameraModel`（和 `GET /api/cameras/{id}` 返回的结构完全一致），
外加一个 `quirks`。**只写和默认值不同的部分就行** —— 解析时先按 `persona`
铺一份默认值，再让 JSON 里显式写出来的字段覆盖它。

| 字段 | 说明 |
|---|---|
| `id` | 进程内唯一 id，如 `"cam1"`。REST 路径里用的就是它 |
| `displayName` | 界面上显示的名字，也会进 WS-Discovery 的 `onvif://…/name/` scope |
| `persona` | 品牌预设 key：`generic` / `hikvision` / `dahua` / `reolink` / `vigi` / `tplink` / `axis` / `uniview` |
| `identity` | 厂商 / 型号 / 固件版本 / 序列号 / 硬件 id / scopes / EPR |
| `network` | `bindAddress` / `httpPort` / `rtspPort` / `advertisedHost` / `advertisedHttpPort` / `advertisedRtspPort` / `macAddress` |
| `users` | 用户表：`username` / `password` / `level`（`Administrator` / `Operator` / `User` / `Anonymous`） |
| `profiles` | 媒体 profile 数组，见下 |
| `ptzNode` | PTZ 节点声明：支持哪些运动、`maxPresets` |
| `capabilities` | 装哪些服务：`ptz` / `imaging` / `events` / `analytics` / `deviceIo` / `media2` / `backchannel` / `relayOutputs` / `digitalInputs` / `whiteLight` |
| `enabled` | `false` 表示这台相机停在离线状态 |
| `quirks` | 这台相机的故障注入开关 |

**默认用户**：`admin` / `admin123`（Administrator）、`operator` / `operator123`、
`viewer` / `viewer123`。

#### `profiles[]`

默认三档：主码流 1080p、子码流 720p、第三码流 360p。

| 字段 | 说明 |
|---|---|
| `token` | profile token，如 `"Profile_1"` |
| `name` | **主 / 子码流全靠这个字串判**（含 main/primary/high 还是 sub/secondary/low），见 quirk B1 |
| `videoEncoder` | `encoding` / `width` / `height` / `frameRate` / `bitrateKbps` / `govLength` / `h264Profile` |
| `hasAudio` / `audioEncoder` | 音频编码：`G711` / `G726` / `AAC`，码率、采样率 |
| `hasPtz` / `ptzConfigToken` | 这条 profile 挂不挂 PTZConfiguration（quirk C1 / C2 在这上面做文章） |
| `hasAudioOutput` / `hasAudioDecoder` | 对讲相关的配置有没有 |
| `streamPath` | RTSP 路径，空则按 persona 生成 |
| `mediaAsset` | 用哪档内嵌样片：`"360p"` / `"720p"` / `"1080p"`，或一个外部文件的绝对路径 |

#### `quirks`

两种写法：

```json
"quirks": {
  "events.bad_xml": true,
  "connect.xaddr_odd_port": { "enabled": true, "params": { "port": 2020, "path": "/onvif/service" } }
}
```

全部 key、参数与出处编号见 [`quirks.md`](quirks.md)（从代码里的表生成，别手改），
也可以跑 `onvifsim --list-quirks` 或打 `GET /api/quirks`。

---

## 内置的八个场景

| 场景 | 相机数 | 用来干什么 |
|---|---|---|
| [`single-camera`](#single-camera) | 1 | 一台什么毛病都没有的标准相机，验客户端的基本流程 |
| [`eight-mixed-brands`](#eight-mixed-brands) | 8 | 混品牌，压 NVR 的多相机流程 |
| [`bad-network`](#bad-network) | 1 | 恶劣网络，测重连与超时 |
| [`talkback`](#talkback) | 3 | 对讲三连：海康单轨 / 大华双轨 / 能力谎标 |
| [`event-storm`](#event-storm) | 1 | 事件风暴 + 订阅槽位泄漏 |
| [`legacy-firmware`](#legacy-firmware) | 1 | 老固件，只剩 GetCapabilities 一条路 |
| [`tplink-full-house`](#tplink-full-house) | 1 | 怪癖最密集的一台，TP-Link 全家桶 |
| [`discovery-quirks`](#discovery-quirks) | 4 | 发现层的四种毛病各一台 |

### single-camera

一台标准 ONVIF 相机，一条 quirk 都不开。

**用来干什么**：任何客户端接入的第一步。先在这里跑通发现 → 建连 →
取流 → 快照 → PTZ → 事件，再去啃有毛病的场景。
e2e 里绝大多数用例的基线也是它 —— 「基线能过、开了 quirk 才不过」，
对比才有意义。

### eight-mixed-brands

八台不同品牌：海康、大华、Reolink、VIGI、TP-Link、Axis、宇视、Generic。
每家的服务路径风格、RTSP 路径、topic 命名、对讲布局都不一样。

**用来干什么**：压 NVR / 监控平台的多相机流程 ——
一次发现出八台、批量添加、并发取流、厂商识别是不是都命中了。
也用来看客户端的相机列表 UI 在混品牌下会不会串味。

### bad-network

一台相机，全套传输层故障：整机延迟 1.5 秒、慢发送、8% 概率随机掉线 15 秒、
RTP 丢包 12%、每 2 分钟主动断流、RTP 限速 512 kbps、WS-Discovery 延迟 2.2 秒。

**用来干什么**：测客户端的超时设置、重连退避、丢包容忍。
发现延迟 2.2 秒这条特别值得看 —— wsdiscovery 默认只等 3 秒，
再慢一点设备就直接「不存在」了。

### talkback

三台，分别复现对讲的三种真机形态：

- **cam1（海康）** 单轨 backchannel + 忙槽位（E7，TEARDOWN 后 4 秒内新
  DESCRIBE 回 401）+ 严格校验 talkspurt 首包 marker（E13）；
- **cam2（大华）** 双轨：麦克风 `recvonly` + 对讲 `sendonly`（E6）；
- **cam3** 能力谎标：声明 AAC 实发 PCMU（E1）、G.722 采样率错写成 8000（E2）、
  `GetAudioDecoderConfigurationOptions` 用另一种响应形态（E4）。

**用来干什么**：对讲是最容易在真机上翻车的功能。
这三台把「客户端以为的」和「相机实际给的」之间的三种错位都摆出来了。

### event-storm

一台相机：每秒 80 条事件、订阅永不过期（槽位泄漏）、订阅上限 4 条、
属性型事件只发 `true` 不补 `false`。

**用来干什么**：压客户端的事件队列与订阅管理。
参照客户端每次建连都建一条 PullPoint 订阅且**从不退订** ——
这个场景就是让那个毛病在几分钟内暴露出来。
「只发 true 不发 false」则会让运动传感器的状态永远卡在「有人」。

### legacy-firmware

老固件：没有 GetServices、没有 Media2、GetEventProperties 不实现、
GetSnapshotUri 不实现、GetPresets 回文本 Fault、SDP 不写 rtpmap、
没有 AudioOutput 配置、设备信息缺 HardwareId 与 SerialNumber。

**用来干什么**：验客户端的**回落路径**。
只剩 `GetCapabilities` 一条路能拿 XAddr 的时候，还连不连得上、取不取得到流。
按 `HardwareId` 选厂商适配器的客户端会在这里落空。

### tplink-full-house

本项目怪癖最密集的一台，全部来自 TL-IPC652P-A4 真机：

- XAddr 报 `:2020/onvif/service`，实际服务在另一个端口（A5）；
- `SupportedPTZSpaces` 为空，但八向 ContinuousMove 全可用（C3）；
- `GetEventProperties` 返回属性值不加引号的**非法 XML**（D5）；
- 订阅地址开在独立端口且每次递增（D1）；
- 预置位名百分号编码原样吐回（C7）；
- 出厂预填 300 个预置位（C6）；
- profile 命名不含主子语义（B1）。

**用来干什么**：客户端的「毕业考」。能在这台上跑通全流程，
基本就能接住市面上绝大多数廉价固件。

### discovery-quirks

四台相机，各带一种发现层毛病：

| 相机 | 毛病 | 客户端看到的 |
|---|---|---|
| cam1 | 缺 `MetadataVersion`（A3） | **设备直接消失** —— wsdiscovery 2.1.2 解析失败会丢整包 |
| cam2 | 不带 `XAddrs`（A2） | 搜得到但没地址，得补发 Resolve |
| cam3 | 坏 XAddr 排第一（A4） | 只取 `getXAddrs()[0]` 的客户端会连到 `192.168.99.99` |
| cam4 | 同一 Probe 回两次 + Scopes 不带 name | 去重之后「后到的那份胜出」；相机显示为无名 |

**用来干什么**：抓包看得见、客户端却搜不到 —— 这类问题最难排查。
这个场景把四种成因摆在一起，一眼能对上号。

---

## 自己写一个

最省事的办法是先跑起来，用 GUI 或 REST 调到满意，再导出：

```bash
# 起一个 8 路混品牌
onvifsim --headless --cameras 8 --preset hikvision &

# 调几条 quirk
curl -s -X PATCH http://127.0.0.1:9000/api/cameras/cam1 \
  -H 'Content-Type: application/json' \
  -d '{"quirks": {"ptz.spaces_empty": true}}'

# 导出成场景文件
curl -s http://127.0.0.1:9000/api/scenario > my-lab.json

# 下次直接用
onvifsim --headless --scenario my-lab.json
```

GUI 里是「文件 → 保存场景」。

### 端口怎么排

端口模式下八台相机会占 `httpBasePort..+7` 与 `rtspBasePort..+7`。
如果场景里给每台相机显式写了 `network.httpPort`，那就以显式的为准。

用 1024 以下的端口（80 / 554）需要管理员权限，
所以默认用高端口。要让客户端「像连真相机一样」用标准端口，
走 IP 别名模式或 Docker macvlan —— 见 [`architecture.md`](architecture.md)
的网络模式一节。

### 别把口令写进版本库

场景文件里的 `users[].password` 是明文。
仓库里的示例场景用的都是默认口令，自己的场景要往版本库里放之前先想清楚。
