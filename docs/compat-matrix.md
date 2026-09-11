# 互操作手测矩阵

自动化测试验的是「协议对不对」，这份矩阵验的是「**真客户端能不能用**」。
两者不能互相替代 —— e2e 里 onvif-zeep 解析得了的响应，
ONVIF Device Manager 可能照样不认。

> **这是一份模板。** 结果栏留空，测完自己填。
> 每次发版前至少把 P0 的三家（ODM / VLC / Home Assistant）过一遍。

## 怎么填

| 记号 | 含义 |
|---|---|
| ✅ | 通过 |
| ⚠️ | 能用但有毛病，**必须在备注里写清楚** |
| ❌ | 不通 |
| — | 这一项不适用 |
| 空 | 还没测 |

测试环境请一并记下来，不然结果没法复现：

```
日期：
onvifsim 版本：            （onvifsim --version）
场景：                     （如 assets/scenarios/single-camera.json）
网络模式：                 端口 / 独立 IP / Docker macvlan
宿主：                     OS 与版本
客户端版本：
```

---

## 优先级

| 级别 | 客户端 | 为什么 |
|---|---|---|
| **P0** | ONVIF Device Manager、VLC / ffplay、Home Assistant | 最常用；不通就是硬伤 |
| **P1** | Frigate、go2rtc、Synology Surveillance Station | 自建监控的主流选择 |
| **P2** | iSpy / Agent DVR、Blue Iris、tinyCam | 覆盖 Windows 与移动端 |

---

## 1. ONVIF Device Manager（Windows）

ONVIF 的「标准答案」型工具，最能暴露协议细节问题。

| # | 测什么 | 期望 | 结果 | 备注 |
|---|---|---|---|---|
| 1.1 | 设备列表里自动出现 | 发现后一台相机只出现一次，名字取自 `onvif://…/name/` scope | | |
| 1.2 | 输入口令后连上 | Identification 页显示厂商 / 型号 / 固件 / 序列号 / 硬件 id | | |
| 1.3 | Live Video 出画面 | 主码流有画面，分辨率与声明一致 | | |
| 1.4 | 切换到子码流 | 画面切到 720p | | |
| 1.5 | 有声音 | 音频轨能出声 | | |
| 1.6 | 快照 | Snapshot 按钮出图，且两次内容不同 | | |
| 1.7 | PTZ 摇杆 | 八向都能动，松手停 | | |
| 1.8 | PTZ 位置反馈 | 移动时坐标随时间变化 | | |
| 1.9 | 预置位 | 列表能读、能新建、能跳转 | | |
| 1.10 | 出厂 300 预置位（C6） | 列表能加载完不卡死 | | |
| 1.11 | Events 页收到运动事件 | REST 触发后界面上有反应 | | |
| 1.12 | 对讲 | 按住说话后 `/api/cameras/{id}/talkback` 的 `totalBytes` 与 `markers` 都非零 | | |
| 1.13 | Imaging 页 | 亮度 / 对比度 / IrCutFilter 能读能改 | | |
| 1.14 | 8 路混品牌场景 | 八台全部出现且能逐台连上 | | |
| 1.15 | tplink-full-house 场景 | 能连上、能取流；PTZ 在 Spaces 为空时仍可用 | | |

## 2. VLC / ffplay

只验 RTSP 与 RTP 这一层，不碰 ONVIF。

| # | 测什么 | 期望 | 结果 | 备注 |
|---|---|---|---|---|
| 2.1 | `rtsp://user:pass@host:port/profile1`（TCP） | 出画面 | | |
| 2.2 | 同上，UDP 传输 | 出画面 | | |
| 2.3 | 音频 | 有声音，无爆音 | | |
| 2.4 | 画面稳定性 | 跑 10 分钟不花屏、不卡死 | | |
| 2.5 | 帧率 | `ffprobe` 读出来接近声明值 | | |
| 2.6 | 三档码流 | 三条 URL 都能放，分辨率各不相同 | | |
| 2.7 | Digest 鉴权 | URL 里带凭据能连上 | | |
| 2.8 | 口令错 | 明确报鉴权失败，不是卡住 | | |
| 2.9 | bad-network 场景 | 丢包 12% 下仍能出画面（可能有块状损坏） | | |
| 2.10 | 周期 TEARDOWN | VLC 能自动重连 | | |

**命令行参考**

```bash
ffplay -rtsp_transport tcp "rtsp://admin:admin123@192.168.1.50:8554/profile1"
ffprobe -v error -rtsp_transport tcp -show_streams \
        "rtsp://admin:admin123@192.168.1.50:8554/profile1"
```

## 3. Home Assistant（ONVIF 集成）

| # | 测什么 | 期望 | 结果 | 备注 |
|---|---|---|---|---|
| 3.1 | 集成能自动发现 | 「发现到的设备」里出现 | | |
| 3.2 | 添加流程走完 | 配置项建立成功 | | |
| 3.3 | 摄像头实体出画面 | 预览有画面 | | |
| 3.4 | 快照实体 | `camera.snapshot` 出图 | | |
| 3.5 | **运动传感器实体存在** | 有 `binary_sensor.*_motion` | | |
| 3.6 | 触发后传感器变 on | REST 触发 → 实体变 `on` | | |
| 3.7 | 事件结束后变 off | `duration` 到点后变 `off` | | |
| 3.8 | 关掉配对 false（D11） | 传感器会**永远卡在 on** ——这就是这条 quirk 的现象 | | |
| 3.9 | PTZ 服务 | `onvif.ptz` 能动 | | |
| 3.10 | 长时间稳定性 | 跑 24 小时不掉线；`/api/cameras/{id}/sessions` 的订阅数不无限增长 | | |

> HA 每次重连都会新建订阅。开着 `auth.subscription_never_expires`
> 跑一夜，订阅数会一直涨 —— 这正是真机上「跑几天就收不到事件」的成因。

## 4. Frigate

| # | 测什么 | 期望 | 结果 | 备注 |
|---|---|---|---|---|
| 4.1 | go2rtc 拉流成功 | 日志无重连风暴 | | |
| 4.2 | 检测正常跑 | 有画面帧送进检测 | | |
| 4.3 | 子码流做检测、主码流做录制 | 两条流同时拉不互相影响 | | |
| 4.4 | 录制文件可播放 | 时间戳连续，无跳变 | | |
| 4.5 | ONVIF PTZ（autotracking） | 能控制 | | |
| 4.6 | RTP 时间戳跳变（quirk） | Frigate 的表现（大概率录制切片异常） | | |

## 5. Synology Surveillance Station

| # | 测什么 | 期望 | 结果 | 备注 |
|---|---|---|---|---|
| 5.1 | 添加设备时能自动搜到 | 出现在列表里 | | |
| 5.2 | 选 ONVIF 通用型号能连上 | 出画面 | | |
| 5.3 | 双码流配置 | 主录制 + 子预览 | | |
| 5.4 | 录制与回放 | 能录能放 | | |
| 5.5 | 移动侦测（相机侧事件） | 能收到 | | |
| 5.6 | 对讲 | 有声音过去，`talkback` 统计非零 | | |
| 5.7 | 8 路场景 | 八台一起录不掉帧 | | |

## 6. iSpy / Agent DVR

| # | 测什么 | 期望 | 结果 | 备注 |
|---|---|---|---|---|
| 6.1 | ONVIF 扫描能发现 | 列表里出现 | | |
| 6.2 | 自动配置出流地址 | 不用手填 RTSP URL | | |
| 6.3 | 出画面 | | | |
| 6.4 | PTZ | 能动 | | |
| 6.5 | 快照 | 出图 | | |

## 7. Blue Iris（Windows）

| # | 测什么 | 期望 | 结果 | 备注 |
|---|---|---|---|---|
| 7.1 | ONVIF 自动配置 | Find/Inspect 能识别 | | |
| 7.2 | 主 / 子码流都识别出来 | | | |
| 7.3 | 出画面 | | | |
| 7.4 | PTZ 与预置位 | | | |
| 7.5 | ONVIF 事件触发录制 | | | |

## 8. tinyCam Monitor（Android）

| # | 测什么 | 期望 | 结果 | 备注 |
|---|---|---|---|---|
| 8.1 | 局域网扫描能发现 | | | |
| 8.2 | 出画面 | | | |
| 8.3 | 有声音 | | | |
| 8.4 | PTZ | | | |
| 8.5 | 对讲 | `talkback` 统计非零 | | |

## 9. go2rtc

| # | 测什么 | 期望 | 结果 | 备注 |
|---|---|---|---|---|
| 9.1 | `rtsp://` 源能拉 | | | |
| 9.2 | `onvif://` 源能拉 | | | |
| 9.3 | 转 WebRTC 出画面 | | | |
| 9.4 | 转 HLS 出画面 | | | |
| 9.5 | 双向音频（WebRTC 对讲） | `talkback` 统计非零 | | |
| 9.6 | ONVIF 设备发现页 | 能列出相机 | | |

---

## 跨平台的坑（测之前先看这里）

测不通的时候，先排除下面这些环境问题，别急着怀疑协议。

### 多播 / 防火墙

| 平台 | 要注意什么 |
|---|---|
| **Linux** | `firewalld` / `ufw` 默认可能挡 UDP 3702。多网卡时确认 WS-Discovery 加入了正确的接口（`--iface`） |
| **Windows** | 首次运行会弹防火墙窗，**必须勾「专用网络」**。多网卡要逐接口 join 组播 |
| **macOS** | 首次运行弹「允许接受传入连接」。**macOS 14 起还要 `Info.plist` 里的 `NSLocalNetworkUsageDescription`，否则多播被静默拦掉** —— 系统不会给任何提示，表现就是「客户端搜不到」 |

`packaging/macos/Info.plist` 里已经声明了那个键，从源码直接跑
（不打 bundle）的时候不会生效，测发现请用打好包的 `.app`。

### 同机测试

客户端和模拟器在同一台机器上时，**XAddr 里必须给 LAN IP，不能给 `127.0.0.1`**。
程序会按 Probe 的来源接口选源地址；如果客户端跑在虚拟机 / WSL 里，
确认那边看到的网段和宿主一致。

### 标准端口

80 / 554 要管理员权限，所以默认用高端口（8000 / 8554）。
有些客户端会**默认往 80 / 554 上打**而不看 XAddrs 里的端口 ——
遇到这种就用独立 IP 模式或 Docker macvlan 让相机真的占住标准端口。

### Docker

macvlan 只在 **Linux 宿主**上到得了物理局域网。
macOS / Windows 的 Docker Desktop 跑在一层轻量虚拟机里，到不了。
另外 macvlan 有个通病：**宿主自己访问不到 macvlan 容器**，
要从宿主测得另建一个 macvlan 子接口做桥接。
详见 [`packaging/docker/README.md`](../packaging/docker/README.md)。

---

## 用 quirk 做回归

手测矩阵还有一个用法：**验证某条 quirk 在真客户端上确实会造成故障**。
这比「协议字段对不对」更有说服力 —— 它证明这条 quirk 复现的是真问题。

建议至少验这几条（都是真机上高发的）：

| quirk | 拿哪家验 | 预期现象 |
|---|---|---|
| `discovery.no_metadata_version`（A3） | ODM / HA | **设备直接搜不到**，抓包却看得见应答 |
| `connect.xaddr_odd_port`（A5） | ODM | 仍能连上（客户端只接管 host:port） |
| `media.profile_naming`（B1） | Frigate / HA | 主子码流判错，拿 1080p 去做检测 |
| `media.snapshot_empty_body`（B4） | HA | 快照实体给出空图或报错 |
| `ptz.spaces_empty`（C3） | ODM | PTZ 界面可能整个变灰，但 ContinuousMove 其实可用 |
| `events.subscription_slot_limit`（D3） | HA + ODM 同时连 | 后连的把先连的挤掉，先连的收不到事件了 |
| `events.state_not_paired`（D11） | HA | 运动传感器**永远卡在 on** |
| `rtsp.talkback_require_marker`（E13） | ODM / tinyCam | 第一句话丢掉，第二句开始才有声 |
| `rtsp.auth_strict_digest_params`（E9） | VLC | RTSP 鉴权失败 |
| `transport.global_delay` | 全部 | 各家的超时设置差异一目了然 |

对应的场景文件：`discovery-quirks`、`tplink-full-house`、`talkback`、
`event-storm`、`legacy-firmware`、`bad-network`。

---

## 历史记录

每轮测完把结果连同环境信息追加到这里，别覆盖上一轮 ——
「上个版本还好好的」这种判断只有对比才做得出来。

### （待填）第一轮

```
日期：
版本：
结论：
```
