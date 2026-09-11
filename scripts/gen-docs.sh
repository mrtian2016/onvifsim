#!/usr/bin/env bash
# 从代码里的 Quirks 表生成 docs/quirks.md。
#
# ⚠️  docs/quirks.md 是**生成物，不要手改**。
#     唯一真源是 src/core/Quirks.cpp 里的那张表；`--list-quirks` 与这份文档同源。
#     改了 quirk 就跑一遍这个脚本，然后把结果一起提交。
#
# 用法：
#   scripts/gen-docs.sh                       # 自动在 build/ 下找可执行文件
#   ONVIFSIM_BINARY=/path/to/onvifsim scripts/gen-docs.sh
#   scripts/gen-docs.sh /path/to/onvifsim

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
output="${repo_root}/docs/quirks.md"

find_binary() {
    if [ $# -ge 1 ] && [ -n "${1:-}" ]; then
        printf '%s\n' "$1"
        return 0
    fi
    if [ -n "${ONVIFSIM_BINARY:-}" ]; then
        printf '%s\n' "${ONVIFSIM_BINARY}"
        return 0
    fi
    local candidate
    for candidate in \
        "${repo_root}"/build/*/bin/onvifsim \
        "${repo_root}"/build/*/onvifsim \
        "${repo_root}"/build/bin/onvifsim \
        "${repo_root}"/build/onvifsim
    do
        if [ -x "${candidate}" ]; then
            printf '%s\n' "${candidate}"
            return 0
        fi
    done
    return 1
}

if ! binary="$(find_binary "${1:-}")"; then
    cat >&2 <<'MSG'
找不到 onvifsim 可执行文件。先构建：
  cmake --preset conda-linux && cmake --build --preset conda-linux
或者显式指定：
  ONVIFSIM_BINARY=/path/to/onvifsim scripts/gen-docs.sh
MSG
    exit 1
fi

if [ ! -x "${binary}" ]; then
    echo "不是可执行文件：${binary}" >&2
    exit 1
fi

echo "用 ${binary} 生成 ${output}"
mkdir -p "$(dirname "${output}")"

# 先写到临时文件，成功了再落盘 —— 免得程序挂了留下一份半截文档。
tmp="$(mktemp)"
trap 'rm -f "${tmp}"' EXIT
"${binary}" --list-quirks --markdown > "${tmp}"

if [ ! -s "${tmp}" ]; then
    echo "--list-quirks --markdown 什么都没输出" >&2
    exit 1
fi

# mktemp 建出来的是 0600，直接 mv 会把这份权限带到文档上（别人 clone 之后看着正常，
# 本地却成了只有自己能读）。按当前 umask 补成正常的可读模式再落盘。
chmod "$(printf '%03o' $(( 0666 & ~0$(umask) )))" "${tmp}"
mv "${tmp}" "${output}"
trap - EXIT

count="$("${binary}" --list-quirks | tail -n 1)"
echo "完成：${output}（${count}）"
