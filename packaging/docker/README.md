# packaging/docker

headless 镜像与 macvlan 编排示例。

## 镜像

```bash
docker build -f packaging/docker/Dockerfile -t onvifsim .
docker run --rm -p 8000:8000 -p 8554:8554 -p 9000:9000 onvifsim
```

镜像里是 `BUILD_GUI=OFF` 的构建，用 Debian 12 自带的 Qt 6.4 编 ——
顺带验证「源码必须能用发行版 Qt 编过」这条硬性约束。
运行期设了 `QT_QPA_PLATFORM=offscreen`：快照是 QImage 画出来的，
无界面模式跑的是 `QGuiApplication`，没有显示器也要能画。

### 端口映射够用吗

不够。`-p` 映射能让客户端**手工填 IP** 连上，但：

- **WS-Discovery 发现不到** —— 多播 3702 穿不过 NAT，客户端搜不出设备；
- **多相机会撞端口** —— 端口模式下相机端口是递增的，映射要一条条写；
- **XAddrs 里报的是容器内地址**，客户端按它去连会连错地方。

要让「像真相机一样被发现」，用 macvlan。

## macvlan

`compose.macvlan.yml` 给每台相机一个独立的 IP 和 MAC，各自占标准的 80 / 554。

**只在 Linux 宿主上成立。** macOS / Windows 上的 Docker Desktop 跑在一层
轻量虚拟机里，macvlan 接口连的是那层 VM 的网络，**到不了物理局域网**。
那两个平台请直接在宿主上跑 onvifsim（端口模式，或用 IP 别名模式让每台相机
拥有自己的地址）。

另外 macvlan 有个通病：**宿主自己访问不到 macvlan 容器**（内核限制，
不是 Docker 的问题）。要从宿主测，得在宿主上另建一个 macvlan 子接口做桥接：

```bash
sudo ip link add macvlan-shim link eth0 type macvlan mode bridge
sudo ip addr add 192.168.1.250/32 dev macvlan-shim
sudo ip link set macvlan-shim up
sudo ip route add 192.168.1.200/29 dev macvlan-shim
```

用之前把 `parent`、`subnet`、`gateway`、`ip_range` 改成你自己的网络，
并确认那段 IP **不在 DHCP 池里**。
