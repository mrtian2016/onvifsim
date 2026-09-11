#!/usr/bin/env bash
# Linux .deb：装进 /usr，依赖发行版自带的 Qt6。
#
# **必须用发行版的 Qt 构建**，不能拿 conda 的那份：
# Depends 是用 dpkg-shlibdeps 从二进制实际链接的 .so 反推的，conda Qt 6.11
# 编出来的程序在只有 Qt 6.4 的系统上根本起不来，而 dpkg 拦不住这种错配 ——
# 用户装完双击没反应，还查不出原因。
#
#   sudo apt install qt6-base-dev qt6-tools-dev qt6-l10n-tools
#   cmake -S . -B build/deb -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF
#   cmake --build build/deb
#   packaging/linux/make-deb.sh build/deb dist
#
# 想要一份不挑发行版、下载即跑的，用 make-appimage.sh。

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${1:-${repo_root}/build/deb}"
output_dir="${2:-${repo_root}/dist}"

# shellcheck source=packaging/common.sh
. "${repo_root}/packaging/common.sh"

for tool in dpkg-deb dpkg-shlibdeps fakeroot; do
    command -v "${tool}" >/dev/null 2>&1 || {
        echo "缺少 ${tool}：sudo apt install dpkg-dev fakeroot" >&2
        exit 1
    }
done

binary="${build_dir}/bin/onvifsim"
[ -x "${binary}" ] || binary="${build_dir}/onvifsim"
if [ ! -x "${binary}" ]; then
    echo "找不到可执行文件，先构建：cmake -S . -B ${build_dir} && cmake --build ${build_dir}" >&2
    exit 1
fi

# conda 的 Qt 会把 rpath 指到环境目录里，装到别人机器上必然找不到库。
# 与其打出一个装完不能用的包，不如在这里就拦下来。
onvifsim_reject_conda_build "${binary}" "deb"

version_full="$("${binary}" --version 2>/dev/null | awk '{print $2}')"
[ -n "${version_full}" ] || { echo "取不到版本号" >&2; exit 1; }

# Debian 版本号里 '-' 是 upstream 和 revision 的分隔符，git describe 的
# "0.1.0-3-gabc1234" 直接拿来用会被解析成 upstream=0.1.0-3 revision=gabc1234。
# 把上游部分里的 '-' 换成 '+'，再统一加 -1 的打包修订号。
upstream="${version_full//-/+}"
deb_version="${upstream}-1"
arch="$(dpkg --print-architecture)"
pkg_name="onvifsim_${deb_version}_${arch}"

stage="$(mktemp -d)"
trap 'rm -rf "${stage}"' EXIT
root="${stage}/root"

install -Dm755 "${binary}" "${root}/usr/bin/onvifsim"
# 剥符号表：Release 构建里它对用户毫无用处，却占了将近一半体积，
# 而且 lintian 会直接判 E: unstripped-binary-or-object。剥的是暂存目录里的
# 副本，不动构建产物 —— 崩溃时还得靠原件配 gdb。
strip --strip-unneeded "${root}/usr/bin/onvifsim"

install -d "${root}/usr/share/onvifsim/scenarios"
install -m644 "${repo_root}"/assets/scenarios/*.json "${root}/usr/share/onvifsim/scenarios/"

onvifsim_install_translations "${build_dir}" "${root}/usr/share/onvifsim/i18n"

install -Dm644 "${repo_root}/packaging/linux/onvifsim.desktop" \
               "${root}/usr/share/applications/onvifsim.desktop"

install -d "${root}/usr/share/man/man1"
gzip -9nc "${repo_root}/packaging/linux/onvifsim.1" \
    > "${root}/usr/share/man/man1/onvifsim.1.gz"
chmod 644 "${root}/usr/share/man/man1/onvifsim.1.gz"

for size in 16 24 32 48 64 128 256; do
    install -Dm644 "${repo_root}/assets/logo/png/onvifsim-${size}.png" \
                   "${root}/usr/share/icons/hicolor/${size}x${size}/apps/onvifsim.png"
done
install -Dm644 "${repo_root}/assets/logo/onvifsim.svg" \
               "${root}/usr/share/icons/hicolor/scalable/apps/onvifsim.svg"

install -Dm644 "${repo_root}/README.md" "${root}/usr/share/doc/onvifsim/README.md"
[ -f "${repo_root}/README.zh-CN.md" ] && install -m644 "${repo_root}/README.zh-CN.md" \
    "${root}/usr/share/doc/onvifsim/README.zh-CN.md"

# Debian 要求每个包都有 copyright；直接把 MIT 正文放进去。
{
    echo "Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/"
    echo "Upstream-Name: onvifsim"
    echo "Source: https://github.com/mrtian2016/onvifsim"
    echo
    echo "Files: *"
    echo "License: MIT"
    sed 's/^$/./; s/^/ /' "${repo_root}/LICENSE"
} > "${root}/usr/share/doc/onvifsim/copyright"

printf 'onvifsim (%s) unstable; urgency=low\n\n  * 见 GitHub Releases。\n\n -- onvifsim <noreply@github.com>  %s\n' \
    "${deb_version}" "$(date -R)" \
    | gzip -9n > "${root}/usr/share/doc/onvifsim/changelog.Debian.gz"

# 重定向和 gzip 生成的文件跟着 umask 走（常见是 0664），Debian 要求 0644。
chmod 644 "${root}/usr/share/doc/onvifsim/copyright" \
          "${root}/usr/share/doc/onvifsim/changelog.Debian.gz"

# 依赖从二进制实际链接的库反推，不手写 —— 手写的清单在下一个发行版就过期了。
# dpkg-shlibdeps 认死了要有 debian/control，给它造一个最小的。
shlibs_dir="${stage}/shlibs"
mkdir -p "${shlibs_dir}/debian"
cat > "${shlibs_dir}/debian/control" <<EOF
Source: onvifsim

Package: onvifsim
Architecture: any
Description: placeholder
EOF
depends="$(cd "${shlibs_dir}" && dpkg-shlibdeps -O --ignore-missing-info \
           "${root}/usr/bin/onvifsim" 2>/dev/null | sed 's/^shlibs:Depends=//')"
[ -n "${depends}" ] || { echo "dpkg-shlibdeps 没算出依赖" >&2; exit 1; }
# 平台插件（xcb/wayland）不在 libqt6gui6 里，是单独一个包。少了它程序会
# 报 "could not load the Qt platform plugin" 然后退出 —— shlibdeps 看不出来。
depends="${depends}, qt6-qpa-plugins"

installed_size="$(du -ks "${root}" | cut -f1)"

install -d "${root}/DEBIAN"
cat > "${root}/DEBIAN/control" <<EOF
Package: onvifsim
Version: ${deb_version}
Architecture: ${arch}
Maintainer: onvifsim <noreply@github.com>
Installed-Size: ${installed_size}
Depends: ${depends}
Recommends: qt6-translations-l10n
Section: net
Priority: optional
Homepage: https://github.com/mrtian2016/onvifsim
Description: ONVIF camera simulator with fault injection
 Simulates ONVIF IP cameras for testing NVRs, VMS software, home automation
 integrations and mobile clients: WS-Discovery, authentication, RTSP streaming
 with embedded H.264 clips, snapshots, PTZ, imaging, PullPoint events and
 two-way audio.
 .
 Every camera can behave like a well-mannered device or reproduce real-world
 firmware quirks on demand - 90 fault-injection switches, each traced to
 observed device behaviour.
 .
 Ships a Qt Widgets GUI plus a headless mode with a REST control API and
 scenario files for CI.
EOF

# md5sums 让 dpkg --verify 和 debsums 能查出被改过的文件。
(cd "${root}" && find usr -type f -print0 | sort -z \
    | xargs -0 md5sum > DEBIAN/md5sums)

# 打包之前先跑一遍暂存目录里那份（剥过符号表、按 /usr 布局摆好的那个）。
# CI 里还会再装一遍真包，但这道在本地就能挡住。
onvifsim_smoke_test "${root}/usr/bin/onvifsim" "deb"

mkdir -p "${output_dir}"
deb="${output_dir}/${pkg_name}.deb"
fakeroot dpkg-deb --build --root-owner-group "${root}" "${deb}" >/dev/null
echo "打好了：${deb}"

if command -v lintian >/dev/null 2>&1; then
    echo "-- lintian（只提示，不阻断）"
    lintian --no-tag-display-limit "${deb}" 2>&1 | sed 's/^/   /' || true
fi
