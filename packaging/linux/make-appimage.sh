#!/usr/bin/env bash
# Linux AppImage：自带 Qt 运行时，下载 chmod +x 就能跑。
#
# 用 linuxdeploy + linuxdeploy-plugin-qt。工具不在 PATH 里就现下到临时目录
# （CI 上走的就是这条），不想联网就先把两个 AppImage 放进 PATH。

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${1:-${repo_root}/build/release}"
output_dir="${2:-${repo_root}/dist}"

# shellcheck source=packaging/common.sh
. "${repo_root}/packaging/common.sh"

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
# 运行期从 <可执行文件>/../share/onvifsim/i18n 找译文，正好落在这儿。
onvifsim_install_translations "${build_dir}" "${appdir}/usr/share/onvifsim/i18n"

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

# 分两步：先把 AppDir 填好，换掉 AppRun，再打包。合成一步的话 linuxdeploy 会在
# 输出前重新生成 AppRun，把下面那个包装脚本盖掉。
"${linuxdeploy}" \
    --appdir "${appdir}" \
    --executable "${binary}" \
    --desktop-file "${repo_root}/packaging/linux/onvifsim.desktop" \
    --icon-file "${repo_root}/assets/logo/onvifsim.svg" \
    --plugin qt

# linuxdeploy 默认把 AppRun 做成指向 usr/bin/onvifsim 的**裸符号链接**，Qt 插件
# 全靠它在 usr/bin 里放的那份 qt.conf 来定位。可经 AppImage 运行时启动时，
# 传给程序的 argv[0] 是 .AppImage 文件自己的路径（不是挂载点里的真实路径），
# Qt 据此算出的「程序所在目录」指向了用户下载 AppImage 的那个目录 ——
# qt.conf 找不到，插件搜索路径是空的，于是任何平台插件都加载不了：
#
#   qt.qpa.plugin: Could not find the Qt platform plugin "offscreen" in ""
#
# **图形模式和 --headless 一起死**，而 `--version` 因为走不到 QGuiApplication
# 仍然正常打印版本号。v0.1.0 就是这么发出去的。
#
# 解法是自己写一个 AppRun 包装脚本，用运行时导出的 $APPDIR 明确指出插件在哪儿。
# 直接跑 AppDir（没有运行时、$APPDIR 为空）时退回按脚本自身位置推算。
rm -f "${appdir}/AppRun"
cat > "${appdir}/AppRun" <<'APPRUN'
#!/bin/sh
# 由 packaging/linux/make-appimage.sh 生成，不要手改。
HERE="${APPDIR:-$(dirname "$(readlink -f "$0")")}"
export QT_PLUGIN_PATH="${HERE}/usr/plugins${QT_PLUGIN_PATH:+:${QT_PLUGIN_PATH}}"
export LD_LIBRARY_PATH="${HERE}/usr/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
export XDG_DATA_DIRS="${HERE}/usr/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
exec "${HERE}/usr/bin/onvifsim" "$@"
APPRUN
chmod +x "${appdir}/AppRun"

"${linuxdeploy}" --appdir "${appdir}" --output appimage

# linuxdeploy 把产物丢在当前目录，挪到 dist/ 并统一命名。
produced="$(ls -1 onvifsim*.AppImage 2>/dev/null | head -n 1 || true)"
if [ -z "${produced}" ]; then
    echo "linuxdeploy 没产出 AppImage" >&2
    exit 1
fi
target="${output_dir}/onvifsim-${version}-x86_64.AppImage"
mv "${produced}" "${target}"
chmod +x "${target}"

# 跑一遍打好的 AppImage 本身 —— 经运行时启动，不是解包后跑里面的二进制。
# 这个区别就是全部：解包跑、挂载后跑 AppRun、手工设 QT_PLUGIN_PATH 跑，
# 三种方式在 v0.1.0 那个坏包上都是好的，只有用户实际用的那种方式是坏的。
onvifsim_smoke_test "${target}" "AppImage"

echo "打好了：${target}"
