#!/usr/bin/env bash
#
# 生成内嵌到二进制里的 H.264 / AAC 样片。
#
# 这是整个仓库里唯一允许出现 ffmpeg 的地方：样片是开发期一次性生成、提交进 git 的，
# 运行期 onvifsim 只从 qrc 里读裸流，绝不调用外部编码器（见 CLAUDE.md「硬性约束」）。
#
# 用法：
#   ./gen-media.sh            已存在的文件跳过（幂等，可以放心重复跑）
#   ./gen-media.sh --force    全部重新生成
#   FFMPEG=/path/to/ffmpeg ./gen-media.sh
#
# 产物（全部落在 assets/media/，由 src/media/assets.qrc 嵌入）：
#   h264-360p.h264     640x360   15fps 10s  Baseline  GOP 1s  testsrc2 + 时间码
#   h264-720p.h264     1280x720  同上
#   h264-1080p.h264    1920x1080 同上
#   h264-black.h264    640x360   全黑，测客户端的黑屏检测
#   h264-freeze.h264   640x360   单帧重复 10s，测冻结检测
#   h264-noise.h264    640x360   随机噪点，测码率/画质告警
#   aac-16k.aac        16 kHz 单声道 AAC-LC ADTS 10s，供 RFC 3640 打包循环用
#
# 全部产物合计需控制在 6 MB 以内（都要进 git 并链进可执行文件），
# 超了就把下面的 *_BITRATE 调低。

set -euo pipefail

FFMPEG="${FFMPEG:-ffmpeg}"
FORCE=0

usage() {
    sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
    exit 0
}

for arg in "$@"; do
    case "$arg" in
        --force|-f) FORCE=1 ;;
        --help|-h)  usage ;;
        *) echo "未知参数：$arg（试试 --help）" >&2; exit 2 ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

if ! command -v "$FFMPEG" >/dev/null 2>&1; then
    echo "找不到 ffmpeg（设 FFMPEG=... 指定路径）" >&2
    exit 1
fi

# ---- 参数 ---------------------------------------------------------------
# 15 fps 而不是 25：帧数少 40%，体积也少 40%，而 ONVIF 客户端的探流逻辑对帧率不敏感。
FPS=15
DURATION=10
# GOP 正好 1 秒：客户端 seek / 重连时最多等 1 秒出画，也方便 quirk 里做「只在关键帧前塞 SPS/PPS」。
GOP=$FPS

BITRATE_360P=300k
BITRATE_720P=700k
BITRATE_1080P=1400k
BITRATE_BLACK=100k
BITRATE_FREEZE=100k
BITRATE_NOISE=400k
BITRATE_AAC=32k

# drawtext 需要一个真实字体文件。挑等宽字体是为了时间码不抖。
FONT=""
for candidate in \
    /usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf \
    /usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf \
    /usr/share/fonts/TTF/DejaVuSansMono.ttf \
    /Library/Fonts/Menlo.ttc \
    /System/Library/Fonts/Menlo.ttc
do
    [ -f "$candidate" ] && FONT="$candidate" && break
done
if [ -z "$FONT" ] && command -v fc-match >/dev/null 2>&1; then
    FONT="$(fc-match -f '%{file}' monospace 2>/dev/null || true)"
fi
if [ -z "$FONT" ] || [ ! -f "$FONT" ]; then
    echo "警告：找不到可用字体，样片将不带时间码叠加" >&2
fi

# ---- 工具函数 -----------------------------------------------------------

# x264 统一参数：Baseline（无 B 帧、无 CABAC，最保守，什么破客户端都能解），
# 固定 GOP 且关掉场景切换检测，保证每 GOP 一个 IDR、循环点可预测。
x264_opts() {
    local bitrate="$1"
    printf '%s\0' \
        -c:v libx264 -profile:v baseline -pix_fmt yuv420p \
        -x264-params "keyint=${GOP}:min-keyint=${GOP}:scenecut=0:bframes=0:ref=1" \
        -b:v "$bitrate" -maxrate "$bitrate" -bufsize "$bitrate" \
        -r "$FPS" -an -f h264
}

# 叠加时间码 + 档位标签。时间码精确到帧，肉眼就能看出画面有没有卡住。
overlay_filter() {
    local label="$1" fontsize="$2"
    [ -z "$FONT" ] && { printf '%s' "null"; return; }
    printf '%s' "drawtext=fontfile='${FONT}':timecode='00\\:00\\:00\\:00':r=${FPS}:fontsize=${fontsize}:fontcolor=white:box=1:boxcolor=black@0.6:boxborderw=8:x=(w-text_w)/2:y=h-text_h-${fontsize}/2,drawtext=fontfile='${FONT}':text='${label}':fontsize=${fontsize}:fontcolor=yellow:box=1:boxcolor=black@0.6:boxborderw=8:x=w-text_w-${fontsize}/2:y=${fontsize}/2"
}

# 已存在就跳过；这样反复跑脚本不会把 git 里的样片刷成新的二进制（否则每次 diff 都是全文件）。
need_gen() {
    local out="$1"
    if [ -f "$out" ] && [ "$FORCE" -eq 0 ]; then
        echo "  跳过（已存在）$(basename "$out")"
        return 1
    fi
    return 0
}

run_ffmpeg() {
    "$FFMPEG" -hide_banner -loglevel error -y "$@"
}

# ---- 三档正常样片 -------------------------------------------------------

gen_testsrc() {
    local out="$OUT_DIR/h264-$1.h264" w="$2" h="$3" bitrate="$4" fontsize="$5"
    need_gen "$out" || return 0
    echo "  生成 $(basename "$out") (${w}x${h} @${FPS}fps ${bitrate})"
    local args=()
    while IFS= read -r -d '' a; do args+=("$a"); done < <(x264_opts "$bitrate")
    run_ffmpeg -f lavfi -i "testsrc2=size=${w}x${h}:rate=${FPS}" \
        -t "$DURATION" -vf "$(overlay_filter "$1" "$fontsize")" \
        "${args[@]}" "$out"
}

# ---- 特殊样片 -----------------------------------------------------------

gen_black() {
    local out="$OUT_DIR/h264-black.h264"
    need_gen "$out" || return 0
    echo "  生成 $(basename "$out")"
    local args=()
    while IFS= read -r -d '' a; do args+=("$a"); done < <(x264_opts "$BITRATE_BLACK")
    run_ffmpeg -f lavfi -i "color=c=black:s=640x360:r=${FPS}" -t "$DURATION" "${args[@]}" "$out"
}

# 单帧重复：先抠出 testsrc2 的第 0 帧，再原地重复 10 秒。
# 注意不能直接用 color 源 —— 冻结检测要的是「有内容但不动」，纯色会被当成黑屏。
gen_freeze() {
    local out="$OUT_DIR/h264-freeze.h264"
    need_gen "$out" || return 0
    echo "  生成 $(basename "$out")"
    local tmp
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' RETURN
    run_ffmpeg -f lavfi -i "testsrc2=size=640x360:rate=${FPS}" -frames:v 1 \
        -vf "$(overlay_filter "FREEZE" 28)" "$tmp/frame.png"
    local args=()
    while IFS= read -r -d '' a; do args+=("$a"); done < <(x264_opts "$BITRATE_FREEZE")
    run_ffmpeg -loop 1 -framerate "$FPS" -i "$tmp/frame.png" -t "$DURATION" "${args[@]}" "$out"
}

# 噪点最难压，码率给得比正常样片还高才不至于糊成一团；分辨率保持 360p 压体积。
gen_noise() {
    local out="$OUT_DIR/h264-noise.h264"
    need_gen "$out" || return 0
    echo "  生成 $(basename "$out")"
    local args=()
    while IFS= read -r -d '' a; do args+=("$a"); done < <(x264_opts "$BITRATE_NOISE")
    run_ffmpeg -f lavfi -i "nullsrc=s=640x360:r=${FPS}" -t "$DURATION" \
        -vf "geq=lum_expr='random(1)*255':cb_expr=128:cr_expr=128" \
        "${args[@]}" "$out"
}

# ---- 音频 ---------------------------------------------------------------

# 16 kHz 单声道：AAC 的 AudioSpecificConfig 只有 2 字节（samplingFrequencyIndex=8），
# 而且和 G.722 的真实采样率一致，SDP 那边一套参数够用。
gen_aac() {
    local out="$OUT_DIR/aac-16k.aac"
    need_gen "$out" || return 0
    echo "  生成 $(basename "$out")"
    run_ffmpeg -f lavfi -i "sine=frequency=440:sample_rate=16000:duration=${DURATION}" \
        -af "volume=0.5" -c:a aac -profile:a aac_low -b:a "$BITRATE_AAC" -ac 1 -ar 16000 \
        -f adts "$out"
}

# ---- 主流程 -------------------------------------------------------------

echo "输出目录：$OUT_DIR"
gen_testsrc 360p   640  360 "$BITRATE_360P"  20
gen_testsrc 720p  1280  720 "$BITRATE_720P"  36
gen_testsrc 1080p 1920 1080 "$BITRATE_1080P" 54
gen_black
gen_freeze
gen_noise
gen_aac

echo
echo "产物："
ls -l "$OUT_DIR"/*.h264 "$OUT_DIR"/*.aac 2>/dev/null | awk '{printf "  %8d  %s\n", $5, $9}'
total=$(cat "$OUT_DIR"/*.h264 "$OUT_DIR"/*.aac 2>/dev/null | wc -c)
printf "  合计 %d 字节（%.2f MB，上限 6 MB）\n" "$total" "$(echo "$total" | awk '{print $1/1048576}')"
if [ "$total" -gt 6291456 ]; then
    echo "超出 6 MB 上限，请调低脚本里的 *_BITRATE 后重跑 --force" >&2
    exit 1
fi
