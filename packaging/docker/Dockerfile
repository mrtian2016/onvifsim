# onvifsim 的 headless 镜像：没有 GUI，只有协议栈 + REST 控制面。
#
# 两段式构建。构建段用 Debian 12 自带的 Qt 6.4，运行段只装运行期库 ——
# 这也顺带验证了「源码必须能用发行版 Qt 编过」这条硬性约束。
#
# 构建：
#   docker build -f packaging/docker/Dockerfile -t onvifsim .
# 跑（端口模式，一台相机）：
#   docker run --rm -p 8000:8000 -p 8554:8554 -p 9000:9000 onvifsim
# 要让局域网里的客户端发现得到，得用 macvlan —— 见 compose.macvlan.yml。

# ---------- 构建段 ----------
FROM debian:12-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        git \
        qt6-base-dev \
        qt6-base-dev-tools \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# BUILD_GUI=OFF：容器里没有 Widgets 也没有显示器。
# 分层约束保证了 headless 版本编得出来 —— GUI 永远碰不到协议细节。
RUN cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_GUI=OFF \
        -DBUILD_TESTS=OFF \
    && cmake --build build --parallel

# ---------- 运行段 ----------
FROM debian:12-slim

# libqt6gui6 是必须的：快照是 QImage + QPainter 画出来的，
# 无界面模式跑的是 QGuiApplication + offscreen 平台插件。
# qt6-qpa-plugins 提供 offscreen 插件本身。
RUN apt-get update && apt-get install -y --no-install-recommends \
        libqt6core6 \
        libqt6network6 \
        libqt6gui6 \
        qt6-qpa-plugins \
        fonts-dejavu-core \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/bin/onvifsim /usr/local/bin/onvifsim
COPY --from=build /src/assets/scenarios /usr/share/onvifsim/scenarios

# 没有显示器也要能画快照。
ENV QT_QPA_PLATFORM=offscreen

# 相机的 HTTP / RTSP（端口模式下从这两个基准端口开始递增）与控制面。
EXPOSE 8000-8015/tcp 8554-8569/tcp 9000/tcp 3702/udp

# 控制面默认只听 127.0.0.1（它能改整台设备的行为，不该随手暴露），
# 容器里必须显式放开才能从宿主访问 —— 所以 CMD 里带上 --control-bind。
# 这里默认跑单相机场景，用 --scenario 换成别的。
ENTRYPOINT ["/usr/local/bin/onvifsim", "--headless"]
CMD ["--scenario", "/usr/share/onvifsim/scenarios/single-camera.json", \
     "--bind", "0.0.0.0", \
     "--control-bind", "0.0.0.0"]
