#!/usr/bin/env bash
# macOS：macdeployqt 收 Qt 运行时 → ad-hoc 签名 → 打 dmg。
#
# ad-hoc 签名（codesign -s -）不需要 Apple 开发者账号。
# 在 arm64 上这一步是**必须的** —— 未签名的二进制根本起不来。
# 代价是别的机器上首次打开要右键「打开」，或者
#   xattr -dr com.apple.quarantine /Applications/onvifsim.app
#
# 部署逻辑统一走 cmake/Deploy.cmake（脚本模式），三平台只有一份。

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${1:-${repo_root}/build/release}"
output_dir="${2:-${repo_root}/dist}"

bundle="${build_dir}/bin/onvifsim.app"
[ -d "${bundle}" ] || bundle="${build_dir}/onvifsim.app"
if [ ! -d "${bundle}" ]; then
    echo "找不到 onvifsim.app。先构建：cmake --preset release && cmake --build --preset release" >&2
    echo "（找过 ${build_dir}/bin/onvifsim.app 与 ${build_dir}/onvifsim.app）" >&2
    exit 1
fi

binary="${bundle}/Contents/MacOS/onvifsim"
version="$("${binary}" --version | awk '{print $2}')"
mkdir -p "${output_dir}"
dmg="${output_dir}/onvifsim-${version}-macos-$(uname -m).dmg"

# 场景文件放进 bundle 的 Resources，双击版也能直接加载。
mkdir -p "${bundle}/Contents/Resources/scenarios"
cp "${repo_root}"/assets/scenarios/*.json "${bundle}/Contents/Resources/scenarios/"

# 本仓库的 Info.plist 模板里有版本占位符，替换后覆盖 CMake 生成的那份。
plist_template="${repo_root}/packaging/macos/Info.plist"
if [ -f "${plist_template}" ]; then
    sed -e "s/@ONVIFSIM_VERSION_SHORT@/${version%%-*}/" \
        -e "s/@ONVIFSIM_VERSION_FULL@/${version}/" \
        "${plist_template}" > "${bundle}/Contents/Info.plist"
fi

cmake -DONVIFSIM_DEPLOY_BUNDLE="${bundle}" \
      -DONVIFSIM_DEPLOY_DMG="${dmg}" \
      -P "${repo_root}/cmake/Deploy.cmake"

echo "打好了：${dmg}"
echo
echo "第一次打开会被 Gatekeeper 拦（ad-hoc 签名不是 Apple 公证过的）："
echo "  右键 → 打开，或者 xattr -dr com.apple.quarantine /Applications/onvifsim.app"
