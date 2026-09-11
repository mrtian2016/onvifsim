"""ffprobe 探流。

参照客户端不信 ``VideoEncoderConfiguration`` 里写的分辨率 / 编码 / 帧率，
一律靠探流拿真值 —— 所以 e2e 也这么验，才对得上真实行为。
ffprobe 只在测试里用；模拟器运行期一行 ffmpeg 都不碰。
"""

import json
import os
import re
import shutil
import subprocess


def ffprobe_path():
    return os.environ.get("FFPROBE") or shutil.which("ffprobe") or "/usr/bin/ffprobe"


def ffprobe_available():
    path = ffprobe_path()
    return bool(path) and os.path.exists(path)


def probe(url, timeout=25.0, transport="tcp", analyze_seconds=5):
    """探一条 RTSP 流，返回 ffprobe 的 JSON（含 streams / format）。"""
    command = [
        ffprobe_path(), "-hide_banner", "-v", "error",
        "-rtsp_transport", transport,
        "-analyzeduration", str(int(analyze_seconds * 1_000_000)),
        "-probesize", "5000000",
        "-show_streams", "-show_format",
        "-of", "json", url,
    ]
    completed = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               timeout=timeout, check=False)
    if completed.returncode != 0:
        raise RuntimeError("ffprobe 失败（%d）：%s\n命令：%s"
                          % (completed.returncode,
                             completed.stderr.decode("utf-8", "replace")[:800],
                             " ".join(command)))
    return json.loads(completed.stdout.decode("utf-8", "replace") or "{}")


def stream_of(result, kind):
    for stream in result.get("streams", []):
        if stream.get("codec_type") == kind:
            return stream
    return None


def video_info(result):
    stream = stream_of(result, "video")
    if stream is None:
        return None
    return {
        "codec": stream.get("codec_name"),
        "width": stream.get("width"),
        "height": stream.get("height"),
        "fps": parse_rate(stream.get("avg_frame_rate") or stream.get("r_frame_rate")),
        # 直播 RTSP 流上这两个字段来源不同，别混着用：
        # ``avg_frame_rate`` 是码流自己**声明**的帧率（H.264 SPS 的 VUI timing，
        # 内嵌样片烧死在 15），不管相机实际按什么节奏发都不会变；
        # ``r_frame_rate`` 是 ffprobe 按包到达的时间戳**量**出来的实际帧率。
        # 要验「实发帧率变了」只能看后者。
        "fps_measured": parse_rate(stream.get("r_frame_rate")),
    }


def audio_info(result):
    stream = stream_of(result, "audio")
    if stream is None:
        return None
    return {
        "codec": stream.get("codec_name"),
        "sample_rate": int(stream.get("sample_rate") or 0),
        "channels": stream.get("channels"),
    }


def parse_rate(text):
    """把 ffprobe 的 "15/1" 这类分数转成浮点。"""
    if not text:
        return None
    if "/" in text:
        numerator, denominator = text.split("/", 1)
        try:
            denominator = float(denominator)
            if denominator == 0:
                return None
            return float(numerator) / denominator
        except ValueError:
            return None
    try:
        return float(text)
    except ValueError:
        return None


def inject_credentials(url, user, password):
    """把 user:pass 塞进 RTSP URL —— 参照客户端也是丢弃原有 userinfo 再重注入。"""
    if "://" not in url:
        return url
    scheme, rest = url.split("://", 1)
    if "@" in rest.split("/", 1)[0]:
        rest = rest.split("@", 1)[1]
    return "%s://%s:%s@%s" % (scheme, user, password, rest)


# ---- 画面内容探测（用 ffmpeg 的滤镜，不是 ffprobe）--------------------
#
# 判黑屏 / 冻结必须真的解码取像素，ffprobe 只报容器与流的元信息，给不出。
# ffmpeg 自带 blackdetect 与 freezedetect 两个滤镜正是为此设计的，
# 结果走 stderr 的 lavfi 日志行。这仍然只发生在测试里 ——
# 模拟器运行期一行 ffmpeg 都不碰。

def ffmpeg_path():
    return os.environ.get("FFMPEG") or shutil.which("ffmpeg") or "/usr/bin/ffmpeg"


def ffmpeg_available():
    return shutil.which(ffmpeg_path()) is not None or os.path.exists(ffmpeg_path())


def _run_filter(url, vfilter, seconds, timeout, transport="tcp"):
    """拉一段流跑一个视频滤镜，返回 stderr 全文。"""
    command = [ffmpeg_path(), "-hide_banner", "-rtsp_transport", transport,
               "-i", url, "-t", str(seconds), "-vf", vfilter, "-f", "null", "-"]
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return ""
    return result.stderr or ""


def detect_black(url, seconds=3, min_duration=0.3, timeout=40.0):
    """返回检测到的黑屏总时长（秒）。完全没有黑屏时返回 0。"""
    text = _run_filter(url, "blackdetect=d=%s:pix_th=0.10" % min_duration, seconds, timeout)
    return sum(float(m.group(1)) for m in re.finditer(r"black_duration:([0-9.]+)", text))


def detect_freeze(url, seconds=4, min_duration=1.0, timeout=40.0):
    """画面是否被判定为冻结。"""
    text = _run_filter(url, "freezedetect=n=0.003:d=%s" % min_duration, seconds, timeout)
    return "freeze_start" in text
