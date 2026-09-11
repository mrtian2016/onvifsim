#!/usr/bin/env bash
# Linux AppImage：自带 Qt 运行时，下载 chmod +x 就能跑。
#
# 用 linuxdeploy + linuxdeploy-plugin-qt。工具不在 PATH 里就现下到临时目录
# （CI 上走的就是这条），不想联网就先把两个 AppImage 放进 PATH。

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${1:-${repo_root}/build/release}"
output_dir="${2:-${repo_root}/dist}"

binary="${build_dir}/bin/onvifsim"
[ -x "${binary}" ] || binary="${build_dir}/onvifsim"
if [ ! -x "${binary}" ]; then
    echo "找不到可执行文件：${build_dir}" >&2
    exit 1
fi

tools_dir="${LINUXDEPLOY_DIR:-${repo_root}/build/.tools}"
mkdir -p "${tools_dir}" "${output_dir}"

fetch_tool() {
    local name="$1" url="$2" target="${tools_dir}/$1"
    if command -v "${name}" >/dev/null 2>&1; then
        command -v "${name}"
        return 0
    fi
    if [ ! -x "${target}" ]; then
        echo "下载 ${name} ..." >&2
        curl -fsSL -o "${target}" "${url}"
        chmod +x "${target}"
    fi
    printf '%s\n' "${target}"
}

base="https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous"
qt_base="https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous"
linuxdeploy="$(fetch_tool linuxdeploy-x86_64.AppImage "${base}/linuxdeploy-x86_64.AppImage")"
linuxdeploy_qt="$(fetch_tool linuxdeploy-plugin-qt-x86_64.AppImage \
                  "${qt_base}/linuxdeploy-plugin-qt-x86_64.AppImage")"
export PATH="${tools_dir}:${PATH}"

appdir="${build_dir}/AppDir"
rm -rf "${appdir}"
mkdir -p "${appdir}/usr/share/onvifsim"
cp -r "${repo_root}/assets/scenarios" "${appdir}/usr/share/onvifsim/scenarios"

version="$("${binary}" --version | awk '{print $2}')"
export VERSION="${version}"
# 容器 / 没有 FUSE 的机器上要靠这个才能跑 AppImage 工具本身。
export APPIMAGE_EXTRACT_AND_RUN="${APPIMAGE_EXTRACT_AND_RUN:-1}"
# Qt 插件从这里找：conda 环境或系统 Qt 都行。
if [ -n "${CONDA_PREFIX:-}" ]; then
    export QMAKE="${QMAKE:-${CONDA_PREFIX}/bin/qmake6}"
fi

# linuxdeploy-plugin-qt 默认只收 xcb 一个平台插件。但 --headless 会把
# QT_QPA_PLATFORM 设成 offscreen（快照要 QGuiApplication，见 src/main.cpp），
# 少了这个插件，无界面模式在任何机器上都是
# "Could not find the Qt platform plugin offscreen" 然后退出 ——
# 而 AppImage 本来就是最可能被丢进无头服务器 / CI 的那一份产物。
export EXTRA_PLATFORM_PLUGINS="libqoffscreen.so;libqminimal.so"

"${linuxdeploy}" \
    --appdir "${appdir}" \
    --executable "${binary}" \
    --desktop-file "${repo_root}/packaging/linux/onvifsim.desktop" \
    --icon-file "${repo_root}/assets/logo/onvifsim.svg" \
    --plugin qt \
    --output appimage

# linuxdeploy 把产物丢在当前目录，挪到 dist/ 并统一命名。
produced="$(ls -1 onvifsim*.AppImage 2>/dev/null | head -n 1 || true)"
if [ -z "${produced}" ]; then
    echo "linuxdeploy 没产出 AppImage" >&2
    exit 1
fi
target="${output_dir}/onvifsim-${version}-x86_64.AppImage"
mv "${produced}" "${target}"
chmod +x "${target}"
echo "打好了：${target}"
