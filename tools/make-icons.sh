#!/usr/bin/env bash
# 从 assets/logo/*.svg 重新生成所有平台图标。改了标志之后手工跑一次，
# 生成物是入库的 —— 构建和打包都不依赖这个脚本。
#
#   tools/make-icons.sh
#
# 依赖：Qt6（编译 tools/svg2png）+ python3。都是这个仓库本来就要有的东西。
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

logo="assets/logo/onvifsim.svg"
logo_small="assets/logo/onvifsim-small.svg"
png_dir="assets/logo/png"

# 小于等于这个尺寸用简化版：发现环的虚线在 32px 以下会糊成一圈噪点。
simplify_below=32

sizes=(16 24 32 48 64 128 256 512 1024)

builder="${repo_root}/build/svg2png"
svg2png="${builder}/svg2png"
if [[ ! -x "${svg2png}" ]]; then
    echo "-- 构建 tools/svg2png"
    cmake -S tools/svg2png -B "${builder}" -G Ninja \
          -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_PREFIX_PATH="${CONDA_PREFIX:-/usr}" >/dev/null
    cmake --build "${builder}" >/dev/null
fi

mkdir -p "${png_dir}"
for size in "${sizes[@]}"; do
    src="${logo}"
    (( size <= simplify_below )) && src="${logo_small}"
    "${svg2png}" "${src}" "${png_dir}/onvifsim-${size}.png" "${size}"
done
echo "-- png: ${png_dir} (${sizes[*]})"

python3 tools/make-icons.py "${png_dir}" \
    --ico packaging/windows/onvifsim.ico \
    --icns packaging/macos/onvifsim.icns
