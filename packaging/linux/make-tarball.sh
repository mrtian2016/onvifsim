#!/usr/bin/env bash
# Linux 便携 tar.gz：可执行文件 + 场景 + 桌面文件 + 图标 + 许可证。
#
# 这一份不带 Qt 运行时，靠系统的 Qt6（Ubuntu 22.04 / Debian 12 自带的 6.2 就够）。
# 要一份自带 Qt 的、下载即跑的，用 make-appimage.sh。

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${1:-${repo_root}/build/release}"
output_dir="${2:-${repo_root}/dist}"

binary="${build_dir}/bin/onvifsim"
[ -x "${binary}" ] || binary="${build_dir}/onvifsim"
if [ ! -x "${binary}" ]; then
    echo "找不到可执行文件，先构建：cmake --preset release && cmake --build --preset release" >&2
    echo "（找过 ${build_dir}/bin/onvifsim 与 ${build_dir}/onvifsim）" >&2
    exit 1
fi

version="$("${binary}" --version | awk '{print $2}')"
name="onvifsim-${version}-linux-x86_64"
stage="$(mktemp -d)"
trap 'rm -rf "${stage}"' EXIT

mkdir -p "${stage}/${name}/bin" "${stage}/${name}/share"
cp "${binary}" "${stage}/${name}/bin/"
cp -r "${repo_root}/assets/scenarios" "${stage}/${name}/share/scenarios"
cp "${repo_root}/packaging/linux/onvifsim.desktop" "${stage}/${name}/share/"
cp "${repo_root}/assets/logo/onvifsim.svg" "${stage}/${name}/share/"

# hicolor 目录树：把 share/icons 整个拷进 ~/.local/share 就能让桌面环境认出图标。
# 只给 svg 不给位图的话，某些 DE（尤其是老一点的 XFCE）会退回一个通用图标。
for size in 16 24 32 48 64 128 256; do
    icon_dir="${stage}/${name}/share/icons/hicolor/${size}x${size}/apps"
    mkdir -p "${icon_dir}"
    cp "${repo_root}/assets/logo/png/onvifsim-${size}.png" "${icon_dir}/onvifsim.png"
done
mkdir -p "${stage}/${name}/share/icons/hicolor/scalable/apps"
cp "${repo_root}/assets/logo/onvifsim.svg" \
   "${stage}/${name}/share/icons/hicolor/scalable/apps/onvifsim.svg"
cp "${repo_root}/LICENSE" "${stage}/${name}/"
cp "${repo_root}/README.md" "${stage}/${name}/"
[ -f "${repo_root}/README.zh-CN.md" ] && cp "${repo_root}/README.zh-CN.md" "${stage}/${name}/"
[ -f "${repo_root}/CHANGELOG.md" ] && cp "${repo_root}/CHANGELOG.md" "${stage}/${name}/"

cat > "${stage}/${name}/README-FIRST.txt" <<'TXT'
onvifsim —— ONVIF 摄像头模拟器（Linux 便携版）

这一份**不带 Qt 运行时**，需要系统里有 Qt 6.2 或更高：
  Ubuntu/Debian:  sudo apt install qt6-base
  Fedora:         sudo dnf install qt6-qtbase

跑起来：
  ./bin/onvifsim                                          # 图形界面
  ./bin/onvifsim --headless --scenario share/scenarios/single-camera.json

想要下载即跑、不装任何东西的，用 AppImage 那一份。
TXT

mkdir -p "${output_dir}"
tar -czf "${output_dir}/${name}.tar.gz" -C "${stage}" "${name}"
echo "打好了：${output_dir}/${name}.tar.gz"
