#!/usr/bin/env bash
# 四个打包脚本（deb / tar.gz / AppImage / dmg）共用的守卫。
#
# 这个文件的由来：首次发版打出来的六个产物里有三个是坏的 —— AppImage 任何模式都
# 起不来、tar.gz 链的是构建机上的 conda Qt、两者都漏了 .qm。而单测、e2e、CI 全绿，
# 因为**没有任何一处跑过打好的产物本身**。唯一完好的是 .deb，恰恰也是唯一在 CI 里
# 装了再跑一遍的那个。
#
# 所以这里的三条守卫都只做一件事：在产物发出去之前，拿产物本身验一遍。

# 拒绝 conda 构建。
#
# conda 的 Qt 会把 rpath 指到环境目录里，还会要求 Qt_6.11 这样的版本化符号。
# 打进「靠系统 Qt」的包（deb / tar.gz）里，装到别人机器上必然是
#   libQt6Core.so.6: version `Qt_6.11' not found
# 而 dpkg 和 tar 对这种错配毫无察觉。
onvifsim_reject_conda_build() {
    local binary="$1" kind="$2"
    if readelf -d "${binary}" 2>/dev/null | grep -qE "R(UN)?PATH.*(conda|miniforge|mamba|micromamba)"; then
        echo "这个二进制链接的是 conda 的 Qt（rpath 指向 conda 环境），不能拿来打 ${kind}。" >&2
        echo "用发行版的 Qt 重新构建：" >&2
        echo "  cmake -S . -B build/deb -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF" >&2
        return 1
    fi
}

# 把构建期生成的界面译文拷到包里。
#
# 漏了 .qm 不会报任何错 —— QTranslator 找不到译文就静默退回中文源字串，
# 表现是「切成英文之后界面还是中文」。首次发版时 AppImage / tar.gz / dmg 全中。
onvifsim_install_translations() {
    local build_dir="$1" dest="$2"
    if compgen -G "${build_dir}/bin/i18n/*.qm" >/dev/null; then
        mkdir -p "${dest}"
        cp "${build_dir}"/bin/i18n/*.qm "${dest}/"
        chmod 644 "${dest}"/*.qm
        return 0
    fi
    echo "警告：${build_dir}/bin/i18n/ 下没有 .qm，打出来的包切成英文仍会显示中文。" >&2
    echo "      构建时没找到 Qt LinguistTools（Debian/Ubuntu 上是 qt6-l10n-tools）。" >&2
    return 0
}

# 跑一遍打好的产物本身。
#
# 两个地方都很容易写成「跑了但什么也没验到」：
#
# 1. **只跑 `--version` 是白跑的。** 它走不到 QGuiApplication，缺平台插件照样正常
#    打印版本号 —— 首次发版时那个坏 AppImage 就是 `--version` 好好的、一进 --headless
#    直接 core dump。`--headless` 会构造 QGuiApplication 并加载 offscreen 平台插件，
#    和图形模式走同一条插件查找路径，所以必须跑它。
#
# 2. **必须去掉 `APPIMAGE_EXTRACT_AND_RUN`。** 这个变量（make-appimage.sh 为
#    linuxdeploy 那几个工具导出的）会让 AppImage 自解压再执行里面的二进制，
#    而 AppImage 的 Qt 插件问题恰恰只在「经运行时挂载启动」这条路上才暴露。
#    带着它跑，坏包也会一路绿灯 —— 这个坑当场踩过一次。
#
# 不用 `timeout`：macOS 不带它。改成后台起进程、等几秒、看还活着没有。
# 端口挑的是 48000 段，避开默认的 8000 / 8554 / 9000，免得和本机正在跑的实例撞上。
onvifsim_smoke_test() {
    local exe="$1"
    local label="${2:-$(basename "${exe}")}"
    local out log pid rc

    if ! out="$(env -u APPIMAGE_EXTRACT_AND_RUN "${exe}" --version 2>&1)"; then
        echo "冒烟失败：${label} --version 跑不起来" >&2
        printf '%s\n' "${out}" >&2
        return 1
    fi

    log="$(mktemp "${TMPDIR:-/tmp}/onvifsim-smoke.XXXXXX")"
    env -u APPIMAGE_EXTRACT_AND_RUN "${exe}" --headless --cameras 1 --no-discovery \
        --http-port 48000 --rtsp-port 48554 --control-port 49000 \
        > "${log}" 2>&1 &
    pid=$!
    sleep 5

    if kill -0 "${pid}" 2>/dev/null; then
        kill "${pid}" 2>/dev/null || true
        set +e; wait "${pid}" >/dev/null 2>&1; set -e
        rm -f "${log}"
        echo "冒烟通过：${label}"
        return 0
    fi

    # 已经退了 = 没起来。
    set +e; wait "${pid}" >/dev/null 2>&1; rc=$?; set -e
    echo "冒烟失败：${label} --headless 没能跑起来（退出码 ${rc}，它本该一直跑着）" >&2
    cat "${log}" >&2
    if grep -qE "Cannot mount AppImage|FUSE" "${log}" 2>/dev/null; then
        echo "" >&2
        echo "看着是本机没有 FUSE，不是产物坏了。装上再打：" >&2
        echo "  sudo apt install fuse libfuse2" >&2
        echo "不要改用 APPIMAGE_EXTRACT_AND_RUN 绕过 —— 那样等于没验。" >&2
    fi
    rm -f "${log}"
    return 1
}
