"""故障注入的断言表。

**每一条 quirk 至少一条断言** —— 这是 CLAUDE.md 里定死的规矩：
新增一个 quirk 必须同时补齐 Quirks 表条目、REST 字段、GUI 复选框和一条 e2e 断言。

这里是那条 e2e 断言的落点。``CASES`` 是一张 (key → 检查函数) 的表，
``test_quirks.py`` 把它参数化跑，``test_quirk_coverage.py`` 拿它和
``GET /api/quirks`` 对账，谁漏了立刻现形。

每个检查函数自己负责打开 quirk（而不是由外面统一打开），
因为不少断言要先取一份「关着的时候」的基线再对比。
函数进来时相机的 quirk 一定是空的 —— ``env`` fixture 每个用例结束都会清。
"""

import contextlib
import re
import socket
import ssl
import time
import xml.etree.ElementTree as ET

import pytest
import requests
from requests.auth import HTTPBasicAuth, HTTPDigestAuth

from . import media, ptz as ptzlib, pullpoint, rtsp, soap, wsd
from .ports import free_port
from .soap import SoapFault

SETTLE = 0.25          # 改完 quirk 让服务端消化一下


class Case:
    """一条 quirk 的断言。

    ``key``   Quirks 表里的稳定键，同时也是用例 id 与覆盖率对账的依据。
    ``check`` 接收 ``CameraEnv``，自己负责开 quirk 并断言。
    ``marks`` 传给 ``pytest.param`` 的标记（discovery / slow / stream …）。
    """

    def __init__(self, key, check, marks=(), note=""):
        self.key = key
        self.check = check
        self.marks = tuple(marks)
        self.note = note or (check.__doc__ or "").strip().splitlines()[0]

    def __repr__(self):
        return "<Case %s>" % self.key


# 明确还没覆盖的 quirk：键 → 原因。test_quirk_coverage 会把它们算作「已知缺口」，
# 但要求原因非空，防止悄悄挂账。
# 90 条 quirk 全部有断言。真要挂账时在这里登记原因，
# test_quirk_coverage.py 会检查理由写得够不够具体。
UNCOVERED = {}


def check_video_black(env):
    """画面切成全黑。客户端的黑屏检测就是拿这个触发的。

    ffprobe 只报容器与流的元信息、给不出像素，但 ffmpeg 的 blackdetect 滤镜可以。
    先取基线（正常样片一段黑都没有），再开 quirk 看整段都被判黑。
    """
    if not media.ffmpeg_available():
        pytest.skip("没有 ffmpeg，判不了黑屏")

    url = env.stream_url(0)
    baseline = media.detect_black(url)
    assert baseline < 0.5, "基线码流本身就有 %.2f 秒黑屏，没法对比" % baseline

    enable(env, "rtsp.video_black")
    black = media.detect_black(url)
    assert black >= 1.0, "开了 video_black，却只检出 %.2f 秒黑屏" % black


def check_video_freeze(env):
    """画面冻住不动，但 RTP 还在发 —— 最阴的一种故障。

    客户端看流量正常、看解码器也正常，只有逐帧比对才发现画面没变。
    """
    if not media.ffmpeg_available():
        pytest.skip("没有 ffmpeg，判不了冻结")

    url = env.stream_url(0)
    assert not media.detect_freeze(url), "基线码流就被判成冻结了，没法对比"

    enable(env, "rtsp.video_freeze")
    assert media.detect_freeze(url), "开了 video_freeze，画面却还在动"


# ---- 通用小工具 --------------------------------------------------------

def enable(env, key, settle=SETTLE, **params):
    """只打开这一条 quirk（服务端是整体替换语义），然后等它生效。"""
    env.set_quirk(key, **params)
    time.sleep(settle)


def elapsed(function, *args, **kwargs):
    start = time.monotonic()
    result = function(*args, **kwargs)
    return time.monotonic() - start, result


def probe_replies(env, **kwargs):
    """裸 Probe，只留 EPR 对得上这台相机的应答。"""
    target = env.camera["identity"]["endpointReference"]
    return [reply for reply in wsd.raw_probe(**kwargs)
            if wsd.epr_of(reply["text"]) == target]


def device_info_raw(env, **kwargs):
    return soap.post(env.device_url, "<tds:GetDeviceInformation/>",
                     env.user, env.password, **kwargs)


def audio_decoder_options_raw(env, profile_token=None):
    token = profile_token or env.main_profile_token()
    body = ("<trt:GetAudioDecoderConfigurationOptions>"
            "<trt:ProfileToken>%s</trt:ProfileToken>"
            "</trt:GetAudioDecoderConfigurationOptions>" % token)
    return soap.post(env.media_url, body, env.user, env.password)


def audio_output_configs_raw(env, profile_token=None):
    token = profile_token or env.main_profile_token()
    body = ("<trt:GetCompatibleAudioOutputConfigurations>"
            "<trt:ProfileToken>%s</trt:ProfileToken>"
            "</trt:GetCompatibleAudioOutputConfigurations>" % token)
    return soap.post(env.media_url, body, env.user, env.password)


def decoder_options_for(root, encoding):
    """从 GetAudioDecoderConfigurationOptions 的响应里取某个编码的选项节点。

    节点名有三种写法，都得认：ONVIF schema 里的 ``G711DecOptions``、
    真机常见的裸 ``G711``（E4 的 a 档），以及形态 B 的 ``AudioDecoderOptions``
    + ``Encoding`` 子元素。节点叫什么名字是 E4 管的事，E2 / E3 / E5 只关心里头的值，
    所以这里按编码找而不是按元素名找。
    """
    nodes = [node for node in soap.iter_all(root)
             if soap.localname(node) in (encoding, encoding + "DecOptions")]
    for node in soap.findall(root, "AudioDecoderOptions"):
        if (soap.text(soap.find(node, "Encoding")) or "").upper() == encoding.upper():
            nodes.append(node)
    return nodes


def int_field_values(node, *names):
    """把 IntList（Items）/ IntRange（Min-Max）/ 裸数字这几种形态里的数字全捞出来。"""
    values = []
    for name in names:
        for field in soap.findall(node, name):
            for element in soap.iter_all(field):
                value = soap.text(element)
                if value and value.lstrip("-").isdigit():
                    values.append(int(value))
    return values


def decoder_option_values(text, encoding, *names):
    """(选项节点, 字段里的数字) —— E2 / E3 / E5 三条断言共用的取值路径。

    入参是 GetAudioDecoderConfigurationOptions 的响应原文，
    这样同一份响应既能做整体比对又能取值，不必重复发请求。
    """
    root = ET.fromstring(text)
    nodes = decoder_options_for(root, encoding)
    assert nodes, "响应里找不到 %s 的选项节点：%s" % (encoding, text[:400])
    return nodes[0], int_field_values(nodes[0], *names)


def profile_frame_rate_limit(env, index=0):
    """GetProfiles 里视频编码配置**声明**的帧率（tt:RateControl/FrameRateLimit）。"""
    profiles = env.soap_profiles()
    encoder = soap.find(profiles[index], "VideoEncoderConfiguration")
    limit = soap.find(encoder, "FrameRateLimit")
    text = soap.text(limit)
    assert text, "VideoEncoderConfiguration 里没有 FrameRateLimit"
    return float(text)


def profile_audio_encodings(env):
    """GetProfiles 里各 profile 声明的音频编码与码率。"""
    result = []
    for profile in env.soap_profiles():
        encoder = soap.find(profile, "AudioEncoderConfiguration")
        if encoder is None:
            continue
        result.append({
            "encoding": soap.text(soap.find(encoder, "Encoding")),
            "bitrate": soap.text(soap.find(encoder, "Bitrate")),
            "sampleRate": soap.text(soap.find(encoder, "SampleRate")),
        })
    return result


def describe_sdp(env, require=True, path_index=0):
    """开一条连接做 DESCRIBE，返回 (response, parsed_sdp)，连接随手关掉。"""
    with env.rtsp(path_index) as client:
        response = client.describe(require_backchannel=require)
        parsed = rtsp.parse_sdp(response.sdp) if response.status == 200 else None
        return response, parsed


def sdp_codecs(parsed, kind="audio"):
    """SDP 里某类媒体的 rtpmap 编码名集合。"""
    codecs = set()
    for entry in parsed["media"]:
        if not entry["m"].startswith(kind):
            continue
        for attr in entry["attrs"]:
            match = re.match(r"rtpmap:\d+\s+([\w-]+)", attr)
            if match:
                codecs.add(match.group(1).upper())
    return codecs


def first_video_track(parsed):
    for entry in parsed["media"]:
        if entry["m"].startswith("video"):
            return entry
    return None


def play_video(env, channel=0):
    """DESCRIBE → SETUP（interleaved）→ PLAY 视频轨，返回已在放的 client。"""
    client = env.rtsp()
    describe = client.describe()
    assert describe.status == 200, "DESCRIBE 失败：%s" % describe
    parsed = rtsp.parse_sdp(describe.sdp)
    track = first_video_track(parsed)
    assert track is not None, "SDP 里没有视频轨：\n%s" % describe.sdp
    control = rtsp.media_control(track) or rtsp.session_control(parsed)
    setup = client.setup(control, interleaved=(channel, channel + 1))
    assert setup.status == 200, "SETUP 失败：%s" % setup
    play = client.play()
    assert play.status == 200, "PLAY 失败：%s" % play
    return client


def collect_interleaved(client, seconds=2.5):
    """收一段 interleaved 数据，按通道统计包数 / 字节数，并解析 RTP 时间戳。"""
    stats = {"channels": {}, "timestamps": [], "bytes": 0, "packets": 0}
    deadline = time.time() + seconds
    while time.time() < deadline:
        frame = client.read_interleaved(timeout=max(0.1, deadline - time.time()))
        if frame is None:
            continue
        channel, payload = frame
        bucket = stats["channels"].setdefault(channel, {"packets": 0, "bytes": 0})
        bucket["packets"] += 1
        bucket["bytes"] += len(payload)
        stats["packets"] += 1
        stats["bytes"] += len(payload)
        if channel % 2 == 0:
            try:
                stats["timestamps"].append(rtsp.parse_rtp(payload)["timestamp"])
            except ValueError:
                pass
    return stats


# ========================================================================
# A. 发现
# ========================================================================

def check_discovery_dialect(env):
    """A1：强制用 2009/01 方言回复，只认 1.0 的客户端直接丢包。"""
    enable(env, "discovery.dialect", value="2009")
    replies = probe_replies(env, timeout=3.0, dialect="2005")
    assert replies, "开了 A1 之后仍然该回 ProbeMatch，只是命名空间不对"
    assert wsd.dialect_of(replies[0]["text"]) == "2009", \
        "应答还是 1.0 方言，A1 没生效"


def check_discovery_no_xaddrs(env):
    """A2：ProbeMatch 不带 XAddrs，逼客户端补发 Resolve。"""
    enable(env, "discovery.no_xaddrs")
    replies = probe_replies(env, timeout=3.0)
    assert replies, "该照常回 ProbeMatch"
    assert not wsd.xaddrs_of(replies[0]["text"]), "XAddrs 还在"


def check_discovery_no_metadata_version(env):
    """A3：缺 MetadataVersion，wsdiscovery 2.1.2 解析时整包丢弃。"""
    enable(env, "discovery.no_metadata_version")
    replies = probe_replies(env, timeout=3.0)
    assert replies, "该照常回 ProbeMatch"
    assert not wsd.has_metadata_version(replies[0]["text"]), "MetadataVersion 还在"


def check_discovery_bad_xaddr_ip(env):
    """A4：把不可达地址放 XAddrs 第一位（客户端只取 [0]）。"""
    enable(env, "discovery.bad_xaddr_ip", address="192.0.2.99", first=True)
    replies = probe_replies(env, timeout=3.0)
    assert replies
    xaddrs = wsd.xaddrs_of(replies[0]["text"])
    assert xaddrs, "XAddrs 不该空"
    assert "192.0.2.99" in xaddrs[0], "坏地址没排在第一位：%s" % xaddrs


def check_discovery_no_reply(env):
    """不回 ProbeMatch：设备在线但发现不到，只能手工填 IP。"""
    baseline = probe_replies(env, timeout=2.5)
    assert baseline, "基线状态就搜不到，对比没意义"
    enable(env, "discovery.no_reply")
    assert not probe_replies(env, timeout=2.5), "开了之后还在回应答"


def check_discovery_reply_delay(env):
    """延迟回复：超过客户端的搜索窗口就等于没回。"""
    enable(env, "discovery.reply_delay", ms=2500)
    assert not probe_replies(env, timeout=1.0), "延迟 2.5s，1s 窗口内不该收到"
    assert probe_replies(env, timeout=4.5), "窗口开够了却还是没收到"


def check_discovery_reply_twice(env):
    """同一个 Probe 回两次：客户端按 EPR 去重，后到的覆盖先到的。"""
    enable(env, "discovery.reply_twice", differing_xaddrs=True)
    replies = probe_replies(env, timeout=3.0)
    assert len(replies) >= 2, "只收到 %d 份应答" % len(replies)
    first = wsd.xaddrs_of(replies[0]["text"])
    second = wsd.xaddrs_of(replies[1]["text"])
    assert first != second, "两份 XAddrs 一样，differing_xaddrs 没生效"


def check_discovery_scopes_no_name(env):
    """Scopes 里不带 onvif.org/name/，相机在客户端里显示为无名。"""
    enable(env, "discovery.scopes_no_name")
    replies = probe_replies(env, timeout=3.0)
    assert replies
    scopes = wsd.scopes_of(replies[0]["text"])
    assert scopes, "Scopes 整个空了，这不是本条 quirk 的行为"
    assert not wsd.scope_value(scopes, "name"), "name scope 还在：%s" % scopes


# ========================================================================
# A / D. 建连与鉴权
# ========================================================================

def check_xaddr_odd_port(env):
    """A5：XAddr 报 :2020/onvif/service，实际服务却在别的端口上。"""
    enable(env, "connect.xaddr_odd_port", port=2020, path="/onvif/service")
    xaddrs = soap.capability_xaddrs(env.device_url, env.user, env.password)
    assert ":2020" in xaddrs["device"], "端口没改：%s" % xaddrs["device"]
    assert xaddrs["device"].endswith("/onvif/service"), \
        "path 没改：%s" % xaddrs["device"]


def check_media2_first(env):
    """A6：GetServices 里 Media2 排在 Media 前面，只按名字匹配的客户端会解析错。"""
    enable(env, "connect.media2_first")
    order = [namespace for namespace, _ in
             soap.get_services(env.device_url, env.user, env.password)
             if "/media/wsdl" in namespace]
    assert len(order) >= 2, "该同时列出两个 media 命名空间：%s" % order
    assert "ver20" in order[0], "Media2 没排在前面：%s" % order


def check_no_get_services(env):
    """A7：不实现 GetServices，客户端必须能回落到 GetCapabilities。"""
    enable(env, "connect.no_get_services")
    with pytest.raises(SoapFault):
        soap.get_services(env.device_url, env.user, env.password)
    assert "media" in soap.capability_xaddrs(env.device_url, env.user, env.password), \
        "回落路径也断了，那这条 quirk 就没法用了"


def check_tight_time_window(env):
    """A8：Created 时间窗收紧 + 时钟偏移 → 不做时钟补偿的客户端全线 401。"""
    enable(env, "auth.tight_time_window", seconds=2, clock_skew=600)
    response = device_info_raw(env)
    if response.status_code == 401:
        return
    with pytest.raises(SoapFault):
        soap.parse(response)


def check_subscription_never_expires(env):
    """A9：关掉过期回收 → 订阅只增不减，复现槽位泄漏。"""
    enable(env, "auth.subscription_never_expires")
    pullpoint.create(env.events_url, env.user, env.password,
                     initial_termination="PT1S")
    time.sleep(3.0)
    sessions = env.control.sessions(env.camera_id)
    assert sessions["subscriptions"], "PT1S 的订阅过了 3 秒仍然不该被回收"


def check_device_info_missing(env):
    """A10：GetDeviceInformation 省掉部分字段。"""
    before = soap.get_device_information(env.device_url, env.user, env.password)
    assert before["HardwareId"], "基线状态就没有 HardwareId"
    enable(env, "connect.device_info_missing", fields="HardwareId,SerialNumber")
    after = soap.get_device_information(env.device_url, env.user, env.password)
    assert not after["HardwareId"] and not after["SerialNumber"], after
    assert after["Manufacturer"] == before["Manufacturer"], "没被点名的字段不该跟着丢"


def check_password_text_only(env):
    """只接受 PasswordText —— 一律发 PasswordDigest 的客户端会全线失败。"""
    enable(env, "auth.password_text_only")
    with pytest.raises((SoapFault, ValueError)):
        soap.get_device_information(env.device_url, env.user, env.password)
    # 换成 PasswordText 就该通。
    root = soap.call(env.device_url, "<tds:GetDeviceInformation/>",
                     env.user, env.password, digest=False)
    assert soap.find(root, "Manufacturer") is not None


def check_nonce_strict_once(env):
    """nonce 严格一次性：同一个 nonce 重放就 401。"""
    enable(env, "auth.nonce_strict_once", cache_seconds=300)
    nonce = b"onvifsim-e2e-nonce"
    created = soap.iso(soap.utc_now())
    first = soap.post(env.device_url, "<tds:GetDeviceInformation/>",
                      env.user, env.password, nonce=nonce, created=created)
    assert first.status_code == 200, "第一次就被拒了：%s" % first.status_code
    soap.parse(first)

    second = soap.post(env.device_url, "<tds:GetDeviceInformation/>",
                       env.user, env.password, nonce=nonce, created=created)
    if second.status_code == 401:
        return
    with pytest.raises(SoapFault):
        soap.parse(second)


def check_preauth_required(env):
    """连 PRE_AUTH 操作也要鉴权（规范允许匿名调 GetSystemDateAndTime）。"""
    anonymous = soap.post(env.device_url, "<tds:GetSystemDateAndTime/>")
    assert anonymous.status_code == 200, "基线状态下匿名该能调：%s" % anonymous.status_code
    soap.parse(anonymous)

    enable(env, "auth.preauth_required")
    blocked = soap.post(env.device_url, "<tds:GetSystemDateAndTime/>")
    if blocked.status_code == 401:
        return
    with pytest.raises(SoapFault):
        soap.parse(blocked)


def check_http_401_not_fault(env):
    """鉴权失败直接回 HTTP 401 + WWW-Authenticate，而不是 SOAP Fault。"""
    enable(env, "auth.http_401_not_fault")
    response = soap.post(env.device_url, "<tds:GetDeviceInformation/>",
                         env.user, "wrong-password")
    assert response.status_code == 401, "该是 401，实际 %s" % response.status_code
    assert "www-authenticate" in {key.lower() for key in response.headers}, \
        "401 却没带 WWW-Authenticate：%s" % dict(response.headers)


def check_fault_http_status(env):
    """D7：SOAP Fault 走 HTTP 200 —— 只看状态码的客户端会以为成功。"""
    enable(env, "auth.fault_http_status", value="200")
    response = soap.post(env.device_url, "<tds:GetDeviceInformation/>",
                         env.user, "wrong-password")
    assert response.status_code == 200, "该走 200，实际 %s" % response.status_code
    assert b"Fault" in response.content, "200 的响应体里没有 Fault"


def check_fault_wording(env):
    """D8：鉴权 Fault 的措辞变体，客户端靠关键词表识别。"""
    enable(env, "auth.fault_wording", value="SenderNotAuthorized")
    response = soap.post(env.device_url, "<tds:GetDeviceInformation/>",
                         env.user, "wrong-password")
    blob = response.text.lower().replace(" ", "")
    assert "sendernotauthorized" in blob, "措辞没换：%s" % response.text[:400]


# ========================================================================
# B. Media 与快照
# ========================================================================

def check_profile_naming(env):
    """B1：profile 名不含 main / sub，客户端只能按顺序猜主子码流。"""
    before = [soap.text(soap.find(profile, "Name")) for profile in env.soap_profiles()]
    assert any("main" in (name or "").lower() for name in before), \
        "基线状态就没有含 main 的名字：%s" % before

    enable(env, "media.profile_naming", value="profile_n")
    after = [soap.text(soap.find(profile, "Name")) for profile in env.soap_profiles()]
    assert not any("main" in (name or "").lower() for name in after), \
        "换风格之后还有含 main 的名字：%s" % after
    assert any((name or "").lower().startswith("profile") for name in after), after


def check_stream_uri_userinfo(env):
    """B2：GetStreamUri 返回的 URI 自带 user:pass@，客户端要丢弃重注入。"""
    enable(env, "media.stream_uri_userinfo", userinfo="admin:wrongpass")
    root = soap.get_stream_uri(env.media_url, env.user, env.password,
                               env.main_profile_token())
    uri = soap.text(soap.find(root, "Uri"))
    assert "admin:wrongpass@" in uri, uri
    assert "wrongpass" not in media.inject_credentials(uri, env.user, env.password)


def check_stream_uri_nested(env):
    """B3：URI 套在 MediaUri 下，两条客户端路径一认一不认。"""
    baseline = soap.post(env.media_url,
                         soap_stream_uri_body(env.main_profile_token()),
                         env.user, env.password).text
    enable(env, "media.stream_uri_nested")
    response = soap.post(env.media_url,
                         soap_stream_uri_body(env.main_profile_token()),
                         env.user, env.password)
    root = soap.parse(response)
    nested = soap.find(root, "MediaUri")
    assert nested is not None, "开了 B3 却没有 MediaUri 这一层：%s" % response.text[:400]
    assert soap.find(nested, "Uri") is not None, "MediaUri 下面没有 Uri"
    assert response.text != baseline, "响应形态和基线一模一样，B3 没生效"


def soap_stream_uri_body(profile_token):
    return ('<trt:GetStreamUri><trt:StreamSetup>'
            '<tt:Stream>RTP-Unicast</tt:Stream>'
            '<tt:Transport><tt:Protocol>RTSP</tt:Protocol></tt:Transport>'
            '</trt:StreamSetup><trt:ProfileToken>%s</trt:ProfileToken>'
            '</trt:GetStreamUri>' % profile_token)


def check_stream_uri_placeholder_ip(env):
    """URI 用占位 IP（多网口 / NAT 后的真机常见）。"""
    enable(env, "media.stream_uri_placeholder_ip", address="192.0.2.77")
    root = soap.get_stream_uri(env.media_url, env.user, env.password,
                               env.main_profile_token())
    uri = soap.text(soap.find(root, "Uri"))
    assert "192.0.2.77" in uri, "占位 IP 没生效：%s" % uri


def check_snapshot_empty_body(env):
    """B4：200 + image/jpeg + 空体，客户端注释写着「真踩过」。"""
    enable(env, "media.snapshot_empty_body", value="empty")
    response = requests.get(env.snapshot_url(),
                            auth=HTTPDigestAuth(env.user, env.password), timeout=10)
    assert response.status_code == 200, response.status_code
    assert response.headers.get("Content-Type", "").startswith("image/jpeg")
    assert len(response.content) == 0, "还有 %d 字节" % len(response.content)


def check_snapshot_auth(env):
    """B5：快照只接受 Basic，Digest 会被 401 顶回来。"""
    enable(env, "media.snapshot_auth", value="basic")
    url = env.snapshot_url()
    digest = requests.get(url, auth=HTTPDigestAuth(env.user, env.password), timeout=10)
    assert digest.status_code == 401, "配成只收 Basic，Digest 却给了 %d" % digest.status_code
    basic = requests.get(url, auth=HTTPBasicAuth(env.user, env.password), timeout=10)
    assert basic.status_code == 200 and basic.content[:2] == b"\xff\xd8", \
        "Basic 该通：%d" % basic.status_code


def check_snapshot_uri_rotates(env):
    """B6：快照 URI 定期失效，客户端要重探。"""
    # 窗口取 2 秒：边界随时可能落在 Digest 的两次往返之间，窗口越短撞上的概率越大。
    window = 2

    def fetch_fresh(tries=3):
        """重探 URI 再取图，过期就再探 —— 这正是 quirk 要求客户端做的事。"""
        response = None
        for _ in range(tries):
            response = requests.get(env.snapshot_url(),
                                    auth=HTTPDigestAuth(env.user, env.password),
                                    timeout=10)
            if response.status_code == 200:
                return response
        return response

    enable(env, "media.snapshot_uri_rotates", seconds=window)
    old = env.snapshot_url()
    assert fetch_fresh().status_code == 200, "刚探到的地址必须能用"
    time.sleep(window + 0.5)          # 睡过一整个窗口，保证 nonce 变过一次
    stale = requests.get(old, auth=HTTPDigestAuth(env.user, env.password), timeout=10)
    fresh_url = env.snapshot_url()
    assert fresh_url != old or stale.status_code != 200, "URI 没轮换"
    assert fetch_fresh().status_code == 200, "重探之后的新地址必须能用"


def check_no_get_snapshot_uri(env):
    """B7：GetSnapshotUri 不实现，客户端要退避而不是硬打。"""
    enable(env, "media.no_get_snapshot_uri")
    with pytest.raises(SoapFault):
        soap.get_snapshot_uri(env.media_url, env.user, env.password,
                              env.main_profile_token())


def check_codec_mismatch(env):
    """声明的音频编码与实发不一致 —— 所以客户端只信探流结果。"""
    enable(env, "media.codec_mismatch", declared="AAC", actual="PCMU")
    declared = profile_audio_encodings(env)
    assert declared, "GetProfiles 里没有音频配置"
    assert any((item["encoding"] or "").upper().startswith("AAC") for item in declared), \
        "声明的编码没变成 AAC：%s" % declared

    probed = media.probe(env.stream_url(0))
    audio = media.audio_info(probed)
    assert audio is not None, "探不到音频轨"
    assert audio["codec"] == "pcm_mulaw", \
        "实发该是 PCMU，探流结果是 %s" % audio["codec"]


def _soap_declared_resolution(env, index=0):
    """从 GetProfiles 里读声明的分辨率。

    不能用 REST 的 ``/api/cameras`` —— 那报的是模型里的真实值、不经过 quirk，
    拿它跟探流结果比等于自己跟自己比，永远看不出差异。
    """
    profile = env.soap_profiles()[index]
    resolution = soap.find(profile, "Resolution")
    return (int(soap.text(soap.find(resolution, "Width"))),
            int(soap.text(soap.find(resolution, "Height"))))


def check_resolution_mismatch(env):
    """声明分辨率与实际码流不符 —— 同样只有探流才看得出来。"""
    baseline = _soap_declared_resolution(env)
    probed = media.video_info(media.probe(env.stream_url(0)))
    assert probed is not None, "探不到视频轨"
    assert (probed["width"], probed["height"]) == baseline, \
        "基线状态下声明与实际就对不上（%s vs %s），没法验这条" % (baseline, probed)

    enable(env, "media.resolution_mismatch")
    declared = _soap_declared_resolution(env)
    assert declared != baseline, "开了 quirk，声明的分辨率却没变：%s" % (declared,)

    probed_after = media.video_info(media.probe(env.stream_url(0)))
    assert (probed_after["width"], probed_after["height"]) == baseline, \
        "这条只该改声明，实际码流不该动：%s" % (probed_after,)
    assert (probed_after["width"], probed_after["height"]) != declared, \
        "声明 %sx%s，实际也是 %sx%s，quirk 没生效" % (
            declared[0], declared[1], probed_after["width"], probed_after["height"])


# ========================================================================
# C. PTZ
# ========================================================================

def _ptz_tokens(env):
    return [soap.attr(profile, "token") for profile in env.soap_profiles()]


def check_ptz_config_on_sub_only(env):
    """C1：只有一路 profile 挂 PTZConfiguration，拿错就 Fault。"""
    enable(env, "ptz.config_on_sub_only")
    tokens = _ptz_tokens(env)
    configs = ptzlib.profile_ptz_config_tokens(env.soap_profiles())
    attached = [index for index, value in enumerate(configs) if value]
    assert len(attached) == 1, "该只有一条挂着：%s" % configs
    assert attached[0] != 0, "挂的该是子码流"

    unattached = [tokens[i] for i in range(len(tokens)) if i not in attached]
    with pytest.raises(SoapFault):
        ptzlib.continuous_move(env.ptz_url, env.user, env.password,
                               unattached[0], pan=0.5)


def check_ptz_usable_but_unadvertised(env):
    """C2：一个 profile 都不挂 PTZConfiguration，但 ContinuousMove 照常可用。"""
    enable(env, "ptz.usable_but_unadvertised")
    configs = ptzlib.profile_ptz_config_tokens(env.soap_profiles())
    assert not any(configs), "还有 profile 挂着配置：%s" % configs

    token = _ptz_tokens(env)[0]
    before = ptzlib.get_status(env.ptz_url, env.user, env.password, token)
    ptzlib.continuous_move(env.ptz_url, env.user, env.password, token, pan=0.6)
    time.sleep(0.8)
    after = ptzlib.get_status(env.ptz_url, env.user, env.password, token)
    ptzlib.stop(env.ptz_url, env.user, env.password, token)
    assert abs(after["pan"] - before["pan"]) > 0.01, "声明没挂但也真的动不了了"


def check_ptz_spaces_empty(env):
    """C3：SupportedPTZSpaces 为空，但八向 ContinuousMove 全可用。"""
    node_token = env.camera["ptzNode"]["nodeToken"]
    before = ptzlib.spaces_of(
        ptzlib.get_node(env.ptz_url, env.user, env.password, node_token))
    assert before, "基线状态就没有 Spaces"

    enable(env, "ptz.spaces_empty", value="empty")
    after = ptzlib.spaces_of(
        ptzlib.get_node(env.ptz_url, env.user, env.password, node_token))
    assert not after, "Spaces 还在：%s" % after

    token = _ptz_tokens(env)[0]
    start = ptzlib.get_status(env.ptz_url, env.user, env.password, token)
    ptzlib.continuous_move(env.ptz_url, env.user, env.password, token, pan=0.5)
    time.sleep(0.8)
    end = ptzlib.get_status(env.ptz_url, env.user, env.password, token)
    ptzlib.stop(env.ptz_url, env.user, env.password, token)
    assert abs(end["pan"] - start["pan"]) > 0.01, "Spaces 空了就该照样能动"


def check_ptz_range_min_equals_max(env):
    """C4：Range 里 Min == Max，客户端得放宽判据。"""
    enable(env, "ptz.range_min_equals_max")
    node = ptzlib.get_node(env.ptz_url, env.user, env.password,
                           env.camera["ptzNode"]["nodeToken"])
    pairs = ptzlib.ranges_of(node)
    assert pairs, "一个 Range 都没有"
    assert all(low == high for low, high in pairs), "还有 Min != Max 的：%s" % pairs


def check_ptz_zoom_malformed_response(env):
    """C5：非零 Zoom 让固件吐畸形 HTTP（回显请求字节 + 500）。"""
    enable(env, "ptz.zoom_malformed_response")
    token = _ptz_tokens(env)[0]
    body = ("<tptz:ContinuousMove><tptz:ProfileToken>%s</tptz:ProfileToken>"
            "<tptz:Velocity><tt:Zoom x=\"0.5\"/></tptz:Velocity>"
            "</tptz:ContinuousMove>" % token)
    try:
        response = soap.post(env.ptz_url, body, env.user, env.password)
    except requests.RequestException:
        return          # 连响应都读不成形，也算复现了畸形 HTTP
    assert b"ContinuousMoveResponse" not in response.content, \
        "非零 Zoom 居然给了一个正常响应"
    assert response.status_code >= 400, \
        "该是 5xx / 4xx，实际 %s" % response.status_code


def check_ptz_factory_300_presets(env):
    """C6：海康出厂预填 300 个预置位槽。"""
    enable(env, "ptz.factory_300_presets", settle=0.6, count=300)
    presets = ptzlib.get_presets(env.ptz_url, env.user, env.password,
                                 _ptz_tokens(env)[0])
    assert len(presets) >= 300, "只有 %d 个" % len(presets)


def check_ptz_preset_percent_encoded(env):
    """C7：预置位名百分号编码原样吐回。"""
    enable(env, "ptz.preset_percent_encoded")
    token = _ptz_tokens(env)[0]
    ptzlib.set_preset(env.ptz_url, env.user, env.password, token, name="大门 入口")
    names = [name for _, name in
             ptzlib.get_presets(env.ptz_url, env.user, env.password, token) if name]
    assert any("%" in name for name in names), "没有百分号编码的名字：%s" % names


def check_ptz_no_get_presets(env):
    """C8：GetPresets 未实现，客户端靠正则认 Fault 措辞。"""
    enable(env, "ptz.no_get_presets", value="action_not_supported")
    with pytest.raises(SoapFault) as excinfo:
        ptzlib.get_presets(env.ptz_url, env.user, env.password, _ptz_tokens(env)[0])
    blob = ("%s %s" % (excinfo.value.subcode, excinfo.value.reason)).lower()
    assert "actionnotsupported" in blob.replace(" ", "") or "not implemented" in blob, \
        blob


def check_ptz_set_preset_return_shape(env):
    """C9：SetPreset 返回裸字符串而不是带 PresetToken 的对象。"""
    enable(env, "ptz.set_preset_return_shape", value="bare_string")
    response = ptzlib.set_preset(env.ptz_url, env.user, env.password,
                                 _ptz_tokens(env)[0], name="shape-test")
    root = soap.parse(response)
    assert soap.find(root, "PresetToken") is None, \
        "还是对象形态：%s" % response.text[:400]
    bare = soap.text(soap.find(root, "SetPresetResponse"))
    assert bare, "既没有 PresetToken 也没有裸字符串：%s" % response.text[:400]


def check_ptz_response_jitter(env):
    """C10：SOAP RTT 抖动 —— 迟到的 ContinuousMove 会落在 Stop 之后，云台一直转。

    抖动加在哪个 PTZ 操作上由实现决定，所以只要求「至少有一个明显变慢」。
    """
    token = _ptz_tokens(env)[0]
    enable(env, "ptz.response_jitter", min_ms=400, max_ms=700)
    try:
        move_time, _ = elapsed(ptzlib.continuous_move, env.ptz_url, env.user,
                               env.password, token, 0.3)
        status_time, _ = elapsed(ptzlib.get_status, env.ptz_url, env.user,
                                 env.password, token)
    finally:
        ptzlib.stop(env.ptz_url, env.user, env.password, token)
    assert max(move_time, status_time) >= 0.35, \
        "PTZ 请求都没变慢：move %.3fs / status %.3fs" % (move_time, status_time)


def check_ptz_goto_preset_slow(env):
    """GotoPreset 特别慢，客户端的超时要够宽。"""
    token = _ptz_tokens(env)[0]
    ptzlib.set_preset(env.ptz_url, env.user, env.password, token, name="slow-goto")
    presets = ptzlib.get_presets(env.ptz_url, env.user, env.password, token)
    preset_token = [item[0] for item in presets if item[1] == "slow-goto"][0]

    enable(env, "ptz.goto_preset_slow", ms=1500)

    # 这条 quirk 的语义是「响应正常、机械动作慢」—— 真机也是立刻回 200 再慢慢转。
    # 所以要验的是 GetStatus 在这段时间里一直报 MOVING，而不是 SOAP 响应本身变慢。
    spent, _ = elapsed(ptzlib.goto_preset, env.ptz_url, env.user, env.password,
                       token, preset_token)
    assert spent < 1.0, "GotoPreset 的响应不该被拖慢（用了 %.2fs）" % spent

    deadline = time.time() + 1.2
    moving_seen = False
    while time.time() < deadline:
        status = ptzlib.get_status(env.ptz_url, env.user, env.password, token)
        if (status["panTiltStatus"] or "").upper() == "MOVING":
            moving_seen = True
            break
        time.sleep(0.1)
    assert moving_seen, "GotoPreset 之后 GetStatus 从没报过 MOVING，动作没慢下来"

    # 到位之后要回 IDLE，不能一直挂着。
    time.sleep(1.6)
    settled = ptzlib.get_status(env.ptz_url, env.user, env.password, token)
    assert (settled["panTiltStatus"] or "").upper() != "MOVING", \
        "1500ms 早该到位了，GetStatus 还在 MOVING"


def check_ptz_move_without_status_change(env):
    """移动了但 GetStatus 一直不变 —— 客户端会以为云台卡死。"""
    enable(env, "ptz.move_without_status_change")
    token = _ptz_tokens(env)[0]
    before = ptzlib.get_status(env.ptz_url, env.user, env.password, token)
    ptzlib.continuous_move(env.ptz_url, env.user, env.password, token, pan=0.8)
    time.sleep(1.0)
    after = ptzlib.get_status(env.ptz_url, env.user, env.password, token)
    ptzlib.stop(env.ptz_url, env.user, env.password, token)
    assert abs(after["pan"] - before["pan"]) < 0.001, \
        "位置变了：%s -> %s" % (before["pan"], after["pan"])


# ========================================================================
# D. 事件
# ========================================================================

def check_subscription_port_increment(env):
    """D1：订阅管理器开独立端口且每次递增。"""
    enable(env, "events.subscription_port_increment", base_port=21024)
    first = pullpoint.create(env.events_url, env.user, env.password)
    second = pullpoint.create(env.events_url, env.user, env.password)
    assert first.port != env.http_port, "还在设备端口上"
    assert second.port == first.port + 1, "%d -> %d" % (first.port, second.port)


def check_subscription_host_bad(env):
    """D2：订阅地址 host 是设备自报的内网地址，客户端必须 rehost。"""
    enable(env, "events.subscription_host_bad", address="10.254.254.1")
    subscription = pullpoint.create(env.events_url, env.user, env.password)
    assert subscription.host == "10.254.254.1", subscription.address
    usable = subscription.reachable_address(env.host, env.http_port)
    assert pullpoint.pull(usable, env.user, env.password, timeout="PT1S") == []


def check_subscription_slot_limit(env):
    """D3：订阅槽位只有几个，超了就 Fault。

    上限要按「当前已经有多少条」来定：订阅默认不回收（A9 复现的就是这个
    「只增不退」的真实故障），同一个 sim 进程里前面的用例会留下订阅，
    写死 max=2 会在第一条 create 就被挡下。
    """
    existing = len(env.control.sessions(env.camera_id)["subscriptions"])
    env.set_quirks({"events.subscription_slot_limit":
                    {"enabled": True,
                     "params": {"max": existing + 2, "on_overflow": "fault"}}})
    time.sleep(SETTLE)
    pullpoint.create(env.events_url, env.user, env.password)
    pullpoint.create(env.events_url, env.user, env.password)
    with pytest.raises(SoapFault):
        pullpoint.create(env.events_url, env.user, env.password)


def check_no_get_event_properties(env):
    """D4：GetEventProperties 不实现 / 返回空 TopicSet。"""
    enable(env, "events.no_get_event_properties", value="action_not_supported")
    with pytest.raises(SoapFault):
        soap.call(env.events_url, "<tev:GetEventProperties/>", env.user, env.password)


def check_event_properties_bad_xml(env):
    """D5：TP-Link 真机会吐属性值不加引号的非法 XML，解析器直接崩。"""
    enable(env, "events.bad_xml")
    response = pullpoint.get_event_properties_raw(env.events_url, env.user, env.password)
    assert response.status_code == 200, response.status_code
    with pytest.raises(ET.ParseError):
        ET.fromstring(response.content)
    assert re.search(r"<[^>!?]+\s[\w:]+=[^\"'\s>/]+", response.text), \
        "没找到不加引号的属性，D5 没生效"


def check_subscription_flat_address(env):
    """D6：Address 直接放响应下，不套 SubscriptionReference。"""
    enable(env, "events.flat_address")
    response = soap.post(
        env.events_url,
        "<tev:CreatePullPointSubscription>"
        "<tev:InitialTerminationTime>PT60S</tev:InitialTerminationTime>"
        "</tev:CreatePullPointSubscription>", env.user, env.password)
    root = soap.parse(response)
    assert soap.find(root, "SubscriptionReference") is None, \
        "还套着 SubscriptionReference：%s" % response.text[:400]
    assert soap.text(soap.find(root, "Address")), "连 Address 都没有"


def check_topic_naming_style(env):
    """D9：topic 命名与通用约定不同（TP-Link 的 LineCrossDetector/LineCross）。"""
    enable(env, "events.topic_style", value="tplink")
    response = pullpoint.get_event_properties_raw(env.events_url, env.user, env.password)
    assert "LineCrossDetector" in response.text, \
        "没换成 TP-Link 风格：%s" % response.text[:600]


def check_pull_always_empty(env):
    """D10：长轮询永远返回零条 —— 但零条本身是合法心跳，不是错误。"""
    enable(env, "events.pull_always_empty")
    subscription = pullpoint.create(env.events_url, env.user, env.password)
    address = subscription.reachable_address(env.host, env.http_port)
    env.control.trigger_event(env.camera_id, kind="Motion", duration=1)
    for _ in range(3):
        assert pullpoint.pull(address, env.user, env.password, timeout="PT1S") == [], \
            "开了 D10 却拉到了消息"


def check_event_state_not_paired(env):
    """D11：属性型事件只发 true，不补配对的 false。"""
    enable(env, "events.state_not_paired")
    subscription = pullpoint.create(env.events_url, env.user, env.password)
    address = subscription.reachable_address(env.host, env.http_port)
    env.control.trigger_event(env.camera_id, kind="Motion", duration=1)

    values = []
    for _ in range(5):
        for message in pullpoint.pull(address, env.user, env.password, timeout="PT1S"):
            values.extend(str(value).lower() for value in message.data.values())
    assert "true" in values, "连 true 都没发：%s" % values
    assert "false" not in values, "不该补 false：%s" % values


def check_renew_fails(env):
    """Renew 总是失败，客户端要能重建订阅而不是一直重试。"""
    enable(env, "events.renew_fails")
    subscription = pullpoint.create(env.events_url, env.user, env.password)
    address = subscription.reachable_address(env.host, env.http_port)
    with pytest.raises(SoapFault):
        pullpoint.renew(address, env.user, env.password)


def check_subscription_expires_at_once(env):
    """订阅建完立刻过期，后续 PullMessages 全部 Fault。"""
    enable(env, "events.subscription_expires_at_once")
    subscription = pullpoint.create(env.events_url, env.user, env.password)
    address = subscription.reachable_address(env.host, env.http_port)
    time.sleep(0.5)
    with pytest.raises(SoapFault):
        pullpoint.pull(address, env.user, env.password, timeout="PT1S")


def check_event_storm(env):
    """事件风暴：每秒几十条，压客户端的事件队列。"""
    enable(env, "events.storm", per_second=40)
    subscription = pullpoint.create(env.events_url, env.user, env.password)
    address = subscription.reachable_address(env.host, env.http_port)

    # PullMessages 一有消息就立刻回（这是对的，空拉取才是心跳），
    # 所以连着拉三次拿到的是三个瞬时快照、每次只有几条 —— 验不出风暴。
    # 要验的是「一秒能积多少」：先让它跑一秒再一次性拉。
    time.sleep(1.0)
    first = len(pullpoint.pull(address, env.user, env.password, timeout="PT1S", limit=100))
    assert first >= 10, "每秒 40 条的风暴，积一秒只拉到 %d 条" % first


def check_no_sync_point(env):
    """不支持 SetSynchronizationPoint。"""
    enable(env, "events.no_sync_point")
    subscription = pullpoint.create(env.events_url, env.user, env.password)
    address = subscription.reachable_address(env.host, env.http_port)
    with pytest.raises(SoapFault):
        pullpoint.set_synchronization_point(address, env.user, env.password)


# ========================================================================
# E. RTSP 与对讲
# ========================================================================

def _open_backchannel(env, channel=0):
    """DESCRIBE(Require) → SETUP(interleaved) → PLAY，返回 (client, setup)。"""
    client = env.rtsp()
    describe = client.describe(require_backchannel=True)
    assert describe.status == 200, "DESCRIBE 失败：%s" % describe
    parsed = rtsp.parse_sdp(describe.sdp)
    track = None
    for entry in parsed["media"]:
        if entry["m"].startswith("audio") and rtsp.is_backchannel(entry):
            track = entry
            break
    assert track is not None, "SDP 里没有 sendonly 的对讲轨：\n%s" % describe.sdp
    control = rtsp.media_control(track) or rtsp.session_control(parsed)
    setup = client.setup(control, interleaved=(channel, channel + 1),
                         require_backchannel=True)
    assert setup.status == 200, "SETUP 失败：%s" % setup
    play = client.play(require_backchannel=True)
    assert play.status == 200, "PLAY 失败：%s" % play
    return client, setup


def check_audio_capability_lie(env):
    """E1：capability 列了 G.711 / G.722 / AAC，backchannel SDP 只列 PCMU。"""
    enable(env, "rtsp.audio_capability_lie")
    options = audio_decoder_options_raw(env).text.upper()
    assert "AAC" in options or "G722" in options, \
        "能力里没列出 AAC / G722，撒不了谎：%s" % options[:400]

    _response, parsed = describe_sdp(env, require=True)
    codecs = sdp_codecs(parsed, "audio")
    assert codecs, "SDP 里没有音频 rtpmap"
    assert codecs <= {"PCMU"}, "SDP 该只列 PCMU，实际 %s" % codecs


SAMPLE_RATE_FIELDS = ("SampleRateRange", "SampleRateList", "SampleRate")
BITRATE_FIELDS = ("Bitrate", "BitrateList", "BitrateRange")


def check_g722_sample_rate_8000(env):
    """E2：ONVIF 把 G.722 的 SampleRate 错写成 8000（真值是 16000）。"""
    _node, before = decoder_option_values(audio_decoder_options_raw(env).text,
                                          "G722", *SAMPLE_RATE_FIELDS)
    assert set(before) == {16000}, "基线的 G722 采样率本该是 16000：%s" % before

    enable(env, "rtsp.g722_sample_rate_8000")
    _node, after = decoder_option_values(audio_decoder_options_raw(env).text,
                                         "G722", *SAMPLE_RATE_FIELDS)
    assert set(after) == {8000}, "G722 的 SampleRate 没被写成 8000：%s" % after


def check_bitrate_unit(env):
    """E3：Bitrate 单位不定（64 kbps vs 64000 bps）。

    落点是 GetAudioDecoderConfigurationOptions —— 参照客户端的
    「< 1024 当 kbps，否则 // 1000」这条启发式就长在这条响应上（facts §4.1），
    plan §4.4 也把 E3 记在这个操作名下。
    原来这条断言看的是 GetProfiles 里的 AudioEncoderConfiguration/Bitrate：
    那个字段 ONVIF 规范定死了是 kbps，参照客户端压根不读它，quirk 也没动它，
    所以它测的不是 E3。
    """
    _node, before = decoder_option_values(audio_decoder_options_raw(env).text,
                                          "G711", *BITRATE_FIELDS)
    assert before and max(before) < 1024, "基线的码率本该是 kbps 量级：%s" % before

    enable(env, "rtsp.bitrate_unit", value="bps")
    _node, after = decoder_option_values(audio_decoder_options_raw(env).text,
                                         "G711", *BITRATE_FIELDS)
    assert min(after) >= 1024, "换成 bps 之后该是 64000 这个量级：%s" % after
    assert after == [value * 1000 for value in before], \
        "单位没按 1000 倍换算：%s -> %s" % (before, after)


def check_audio_decoder_options_shape(env):
    """E4：GetAudioDecoderConfigurationOptions 两种响应形态。"""
    baseline = audio_decoder_options_raw(env).text
    enable(env, "rtsp.audio_decoder_options_shape", value="b")
    variant = audio_decoder_options_raw(env).text
    assert variant != baseline, "形态没变，E4 没生效"
    ET.fromstring(variant)          # 两种形态都必须是合法 XML


def check_sample_rate_shape(env):
    """E5：SampleRate 形态不定（range / list / 单值）。

    默认形态本来就是 list —— ONVIF schema 里 SampleRateRange 的类型是 IntList
    （一串 tt:Items），模拟器关着 quirk 时按规范写。原来这条断言拿 value="list"
    当变体，跟基线一模一样，自然「形态没变」；它测的是自己选错的档位，不是实现。
    """
    baseline = audio_decoder_options_raw(env).text
    node, rates = decoder_option_values(baseline, "G711", *SAMPLE_RATE_FIELDS)
    assert rates == [8000], "基线的 G711 采样率该是 8000：%s" % rates
    assert soap.findall(node, "Items"), "基线该是 IntList 形态（tt:Items）"

    enable(env, "rtsp.sample_rate_shape", value="range")
    ranged = audio_decoder_options_raw(env).text
    assert ranged != baseline, "range 档没生效"
    node, rates = decoder_option_values(ranged, "G711", *SAMPLE_RATE_FIELDS)
    field = soap.find(node, "SampleRateRange")
    assert soap.find(field, "Min") is not None and soap.find(field, "Max") is not None, \
        "range 档该写成 Min/Max：\n%s" % ranged
    assert set(rates) == {8000}, "range 档的取值变了：%s" % rates

    enable(env, "rtsp.sample_rate_shape", value="single")
    single = audio_decoder_options_raw(env).text
    assert single != baseline, "single 档没生效"
    node, rates = decoder_option_values(single, "G711", *SAMPLE_RATE_FIELDS)
    field = soap.find(node, "SampleRateRange")
    assert len(list(field)) == 0 and (soap.text(field) or "").isdigit(), \
        "single 档该是不带子元素的裸数字：\n%s" % single
    assert rates == [8000], "single 档的取值变了：%s" % rates

    enable(env, "rtsp.sample_rate_shape", value="list")
    assert audio_decoder_options_raw(env).text == baseline, \
        "list 档就是规范形态，输出该和基线一字不差"


def check_talkback_dual_track(env):
    """E6：大华同时给麦克风轨（recvonly）+ 对讲轨（sendonly）。"""
    enable(env, "rtsp.talkback_dual_track")
    _response, parsed = describe_sdp(env, require=True)
    audio = [entry for entry in parsed["media"] if entry["m"].startswith("audio")]
    assert len(audio) >= 2, "只有 %d 条音频轨" % len(audio)
    assert any(rtsp.is_backchannel(entry) for entry in audio), "缺 sendonly"
    assert any("recvonly" in entry["attrs"] for entry in audio), "缺 recvonly"


def check_talkback_busy_slot(env):
    """E7：上条会话槽位没释放，新的 DESCRIBE 回 401（海康球机要 3~4 秒）。"""
    enable(env, "rtsp.talkback_busy_slot", seconds=3)
    client, _setup = _open_backchannel(env)
    client.teardown()
    client.close()

    with env.rtsp() as second:
        blocked = second.describe(require_backchannel=True, retry_auth=False)
        assert blocked.status == 401, "槽位没占住，DESCRIBE 给了 %d" % blocked.status

    time.sleep(3.5)
    with env.rtsp() as third:
        assert third.describe(require_backchannel=True).status == 200, "没恢复"


def check_auth_dual_challenge(env):
    """E8：同时发 Basic + Digest 两条 WWW-Authenticate。"""
    enable(env, "rtsp.auth_dual_challenge", order="basic_first")
    client = rtsp.RtspClient(env.host, env.rtsp_port, env.stream_path(0))
    with client:
        response = client.describe(retry_auth=False)
        assert response.status == 401, "该先要鉴权：%d" % response.status
        challenges = rtsp.RtspClient.parse_challenges(response)
        schemes = [scheme for scheme, _ in challenges]
        assert len(schemes) >= 2, "只发了 %s" % schemes
        assert set(schemes) >= {"basic", "digest"}, schemes
        assert schemes[0] == "basic", "order=basic_first 时 Basic 该排前面：%s" % schemes


def check_auth_strict_digest_params(env):
    """E9：Authorization 多带一个挑战没要求的参数（algorithm=MD5）就 401。"""
    enable(env, "rtsp.auth_strict_digest_params")
    with env.rtsp() as client:
        first = client.describe()
        assert first.status == 200, "规规矩矩的 Digest 该通过：%d" % first.status
        strict = client.describe(auth_extra={"algorithm": "MD5"}, retry_auth=False)
        assert strict.status == 401, "多带了 algorithm 却还是通过了：%d" % strict.status


def check_sdp_nonstandard_codec(env):
    """E10：SDP 声明 G7221 / G726 这类不规范 codec 名。"""
    enable(env, "rtsp.sdp_nonstandard_codec", name="G7221")
    response, parsed = describe_sdp(env, require=True)
    assert "G7221" in response.sdp.upper(), "SDP 里没有 G7221：\n%s" % response.sdp


def audio_tracks(parsed):
    return [entry for entry in parsed["media"] if entry["m"].startswith("audio")]


def has_rtpmap(entry):
    return any(attr.startswith("rtpmap") for attr in entry["attrs"])


def check_sdp_no_rtpmap(env):
    """E11：SDP 不列 rtpmap，只给静态 payload type。

    「只给静态 PT」是这条 quirk 的定义（facts E11、plan §4.9），所以它只作用于
    RFC 3551 静态表里查得到的 PT（音频 0 / 8 / 9）。视频用的是动态 PT 96，
    静态表里根本没有，抽掉 rtpmap 客户端就完全无法解码 —— 真机不会这么干，
    实现也就故意不动它（见 src/rtsp/Sdp.cpp 的 omitRtpmap）。
    原来那条断言是对整份 SDP 查 "a=rtpmap"，等于要求连视频轨也一起坏掉。
    """
    _response, parsed = describe_sdp(env, require=True)
    assert all(has_rtpmap(entry) for entry in audio_tracks(parsed)), \
        "基线的音频轨本来就该有 rtpmap"

    enable(env, "rtsp.sdp_no_rtpmap")
    response, parsed = describe_sdp(env, require=True)
    tracks = audio_tracks(parsed)
    assert tracks, "SDP 里没有音频轨：\n%s" % response.sdp
    for entry in tracks:
        payload = int(entry["m"].split()[-1])
        assert payload < 96, "音频轨该退回静态 PT，实际 %d：\n%s" % (payload, response.sdp)
        assert not has_rtpmap(entry), "静态 PT 的音频轨还留着 rtpmap：\n%s" % response.sdp

    video = first_video_track(parsed)
    assert video is not None, "SDP 里没有视频轨：\n%s" % response.sdp
    assert has_rtpmap(video), \
        "视频用的是动态 PT，不该被 E11 波及（去掉就没法解码了）：\n%s" % response.sdp


def check_sdp_session_level_control(env):
    """E12：只有 session 级 a=control，媒体级一个都没有。"""
    enable(env, "rtsp.sdp_session_level_control")
    _response, parsed = describe_sdp(env, require=True)
    assert rtsp.session_control(parsed), "session 级 a=control 也没有"
    assert all(rtsp.media_control(entry) is None for entry in parsed["media"]), \
        "媒体级还留着 a=control"


def check_talkback_require_marker(env):
    """E13：talkspurt 首包 marker=0 → 冷启动的相机丢掉整个 talkburst。"""
    enable(env, "rtsp.talkback_require_marker")
    client, _setup = _open_backchannel(env)
    try:
        talker = rtsp.Talker(client, channel=0)
        talker.burst(packets=20, marker_on_first=False)
        time.sleep(0.5)
        stats = env.control.talkback(env.camera_id)["sessions"][0]
        assert stats["markerMissing"] >= 1, "markerMissing 是 0：%s" % stats
    finally:
        client.teardown()
        client.close()


def check_setup_no_session_header(env):
    """E14：SETUP 响应不带 Session 头，客户端后续请求就没法带上。"""
    enable(env, "rtsp.setup_no_session_header")
    with env.rtsp() as client:
        describe = client.describe()
        parsed = rtsp.parse_sdp(describe.sdp)
        track = first_video_track(parsed)
        control = rtsp.media_control(track) or rtsp.session_control(parsed)
        setup = client.setup(control, interleaved=(0, 1))
        assert setup.status == 200, setup
        assert setup.header("Session") is None, \
            "还带着 Session：%s" % setup.header("Session")


def check_ignore_backchannel_require(env):
    """E15：相机对 Require: backchannel 要么装没看见，要么严格回 551。

    ``ignore`` 档的语义是「不看 Require 照给 sendonly 轨」（quirk 表原文，
    plan §4.9 写的是「忽略该头照样返回一样的 SDP」），所以要拿**不带** Require 的
    DESCRIBE 去验：基线不带 Require 时没有对讲轨，开了 ignore 就有了。
    原来那条断言方向反了 —— 它带着 Require 去 DESCRIBE，却要求「没有对讲轨」，
    那是「相机不支持对讲」的行为，不是 E15。
    """
    _response, parsed = describe_sdp(env, require=False)
    assert not any(rtsp.is_backchannel(entry) for entry in parsed["media"]), \
        "基线不带 Require 就不该有 sendonly 轨"

    enable(env, "rtsp.ignore_backchannel_require", value="strict_551")
    with env.rtsp() as client:
        response = client.describe(require_backchannel=True)
        assert response.status == 551, "strict_551 时该回 551，实际 %d" % response.status

    enable(env, "rtsp.ignore_backchannel_require", value="ignore")
    with env.rtsp() as client:
        response = client.describe(require_backchannel=False)
        assert response.status == 200, "ignore 时该照常 200，实际 %d" % response.status
        parsed = rtsp.parse_sdp(response.sdp)
        assert any(rtsp.is_backchannel(entry) for entry in parsed["media"]), \
            "不带 Require 也该照给 sendonly 轨，SDP 里却没有：\n%s" % response.sdp


def check_no_audio_output_config(env):
    """E16：GetCompatibleAudioOutputConfigurations 不实现，客户端静默跳过。"""
    enable(env, "rtsp.no_audio_output_config")
    with pytest.raises(SoapFault):
        soap.parse(audio_output_configs_raw(env))


def check_talkback_stop_draining(env):
    """E19：推流期相机停止排空，客户端的 sendall 会卡死。"""
    enable(env, "rtsp.talkback_stop_draining", after_ms=400)
    client, _setup = _open_backchannel(env)
    try:
        talker = rtsp.Talker(client, channel=0)
        talker.burst(packets=100, marker_on_first=True, pace=True)
        time.sleep(0.5)
        stats = env.control.talkback(env.camera_id)["sessions"][0]
        assert stats["bytes"] < talker.bytes_sent, \
            "推了 %d 字节，相机居然全收了（%d），没停止排空" \
            % (talker.bytes_sent, stats["bytes"])
    finally:
        try:
            client.teardown()
        except Exception:                       # noqa: BLE001 —— 卡死了本来就可能收不到响应
            pass
        client.close()


def wait_rtsp_idle(env, timeout=5.0):
    """等这台相机上一条 RTSP 会话都不剩。

    会话槽位是本条断言的分母，起点不干净就测不出上限。
    上一条用例（E19 停止排空）会留下一条服务端还没发现对端已断开的会话，
    要等 quirk 被关掉、服务端恢复排空并读到 EOF 才会消失。
    """
    deadline = time.time() + timeout
    while True:
        sessions = env.control.sessions(env.camera_id)["rtsp"]
        if not sessions:
            return []
        if time.time() >= deadline:
            return sessions
        time.sleep(0.1)


def check_rtsp_max_sessions(env):
    """并发会话上限，超过回 453 Not Enough Bandwidth。"""
    enable(env, "rtsp.max_sessions", max=1)
    leftover = wait_rtsp_idle(env)
    assert not leftover, "开始前还有别的 RTSP 会话占着槽位：%s" % leftover
    first = play_video(env)
    try:
        with env.rtsp() as second:
            describe = second.describe()
            if describe.status == 453:
                return
            parsed = rtsp.parse_sdp(describe.sdp)
            track = first_video_track(parsed)
            control = rtsp.media_control(track) or rtsp.session_control(parsed)
            setup = second.setup(control, interleaved=(0, 1))
            assert setup.status == 453, "第二条会话该被 453 顶回来，实际 %d" % setup.status
    finally:
        first.teardown()
        first.close()


def check_rtsp_periodic_teardown(env):
    """每 N 秒相机主动 TEARDOWN，客户端要能自动重连。"""
    enable(env, "rtsp.periodic_teardown", seconds=2)
    client = play_video(env)
    try:
        deadline = time.time() + 8.0
        while time.time() < deadline:
            time.sleep(0.5)
            if not env.control.sessions(env.camera_id)["rtsp"]:
                return
        raise AssertionError("等了 8 秒也没见相机主动断开会话")
    finally:
        client.close()


def check_rtp_packet_loss(env):
    """丢包：同样时长里收到的 RTP 包数明显变少。"""
    baseline_client = play_video(env)
    try:
        baseline = collect_interleaved(baseline_client, seconds=2.0)
    finally:
        baseline_client.teardown()
        baseline_client.close()
    assert baseline["packets"] > 10, "基线只收到 %d 包，样本太小" % baseline["packets"]

    enable(env, "rtsp.rtp_packet_loss", percent=80.0)
    lossy_client = play_video(env)
    try:
        lossy = collect_interleaved(lossy_client, seconds=2.0)
    finally:
        lossy_client.teardown()
        lossy_client.close()

    assert lossy["packets"] < baseline["packets"] * 0.6, \
        "丢包 80%% 之后仍收到 %d 包（基线 %d）" % (lossy["packets"], baseline["packets"])


def check_rtp_timestamp_jump(env):
    """时间戳跳变：解码端的时钟会被带飞。"""
    enable(env, "rtsp.rtp_timestamp_jump", seconds=1, delta_ms=5000)
    client = play_video(env)
    try:
        stats = collect_interleaved(client, seconds=4.0)
    finally:
        client.teardown()
        client.close()

    stamps = stats["timestamps"]
    assert len(stamps) > 5, "只收到 %d 个 RTP 包" % len(stamps)
    # 90 kHz 时钟下 5 秒 = 450000；正常帧间隔只有几千。
    jumps = [abs(second - first) for first, second in zip(stamps, stamps[1:])]
    assert max(jumps) > 90000, "最大时间戳跳变只有 %d，没跳起来" % max(jumps)


def check_sps_pps_placement(env):
    """SPS/PPS 只在 SDP / 只在带内 / 两边都有。"""
    baseline, _parsed = describe_sdp(env, require=False)
    assert "sprop-parameter-sets" in baseline.sdp, \
        "基线 SDP 里就没有 sprop-parameter-sets"

    enable(env, "rtsp.sps_pps_placement", value="inband_only")
    variant, _parsed = describe_sdp(env, require=False)
    assert "sprop-parameter-sets" not in variant.sdp, \
        "inband_only 时 SDP 不该再带 sprop-parameter-sets：\n%s" % variant.sdp


def check_video_fps_change(env):
    """帧率突变：声明 15 fps，实发换成别的。

    要看的是**实测**帧率（ffprobe 的 r_frame_rate，按包到达时间戳算出来的），
    不是 avg_frame_rate —— 后者是码流自己声明的（H.264 SPS 里的 VUI timing，
    内嵌样片烧死在 15），相机换什么节奏发它都不会动。原来那条断言读的正是它，
    于是不管实现对不对都恒等于 15。
    """
    declared = profile_frame_rate_limit(env)
    baseline = media.video_info(media.probe(env.stream_url(0)))
    assert baseline is not None and baseline["fps_measured"], "探不到基线视频轨"
    assert baseline["fps_measured"] > declared * 0.8, \
        "基线实测帧率(%.2f)该贴着声明值(%.2f)" % (baseline["fps_measured"], declared)

    enable(env, "rtsp.video_fps_change", fps=5.0)
    probed = media.video_info(media.probe(env.stream_url(0)))
    assert probed is not None and probed["fps_measured"], "探不到视频轨"
    assert abs(probed["fps_measured"] - 5.0) < 1.0, \
        "实发帧率该落在 5 fps 附近，实际 %.2f" % probed["fps_measured"]
    # 「实际与声明不符」是这条 quirk 的全部意义，所以声明值必须原样不动。
    assert profile_frame_rate_limit(env) == declared, "声明帧率不该跟着变"


# RTCP SR 每 5 秒一条，首条按 RFC 3550 §6.2 只等半个间隔（2.5s）。
# 收流窗口必须比 2.5s 宽出一截，否则基线里一条 SR 都收不到，无从对比。
RTCP_WINDOW = 4.0


def check_no_rtcp(env):
    """不发 RTCP SR，靠 RTCP 对时的客户端会失准。

    interleaved 模式下 RTCP 走奇数通道，直接数就行。
    """
    baseline_client = play_video(env)
    try:
        baseline = collect_interleaved(baseline_client, seconds=RTCP_WINDOW)
    finally:
        baseline_client.teardown()
        baseline_client.close()
    assert baseline["channels"].get(1, {}).get("packets", 0) > 0, \
        "基线状态下就没有 RTCP，这条没法对比"

    enable(env, "rtsp.no_rtcp")
    quiet_client = play_video(env)
    try:
        quiet = collect_interleaved(quiet_client, seconds=RTCP_WINDOW)
    finally:
        quiet_client.teardown()
        quiet_client.close()
    assert quiet["channels"].get(1, {}).get("packets", 0) == 0, \
        "还在发 RTCP：%s" % quiet["channels"]
    assert quiet["channels"].get(0, {}).get("packets", 0) > 0, \
        "连 RTP 都停了，那不是这条 quirk 的行为"


# ========================================================================
# F. 传输与设备
# ========================================================================

def wait_offline(env, timeout=6.0):
    """等相机进入离线状态。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            if env.listing["status"]["offline"]:
                return True
        except Exception:                       # noqa: BLE001 —— 控制面本身不受影响，但保险
            pass
        time.sleep(0.2)
    return False


def check_malformed_http(env):
    """F1：固件回畸形 HTTP，客户端只能把原始字节塞进异常文本。"""
    enable(env, "transport.malformed_http", value="echo_500", percent=100.0)
    try:
        response = soap.post(env.device_url, "<tds:GetDeviceInformation/>",
                             env.user, env.password, timeout=8.0)
    except requests.RequestException:
        return          # 连 HTTP 都解析不出来，正是这条 quirk 想要的效果
    assert b"GetDeviceInformationResponse" not in response.content, \
        "居然给了个正常响应"
    assert response.status_code >= 400 or b"GetDeviceInformation" in response.content, \
        "既不是错误码也没回显请求：%s / %s" % (response.status_code,
                                              response.content[:200])


def check_self_signed_tls(env):
    """F2：VIGI 的私有接口在自签 HTTPS 端口上，客户端只能 verify=False。

    这条 quirk 依附于 VIGI 的私有 API —— generic 预设根本没装那个服务，
    在它身上开等于什么都不会发生。运行时换预设也不会重建厂商桩，
    所以专门建一台 vigi 相机来验，验完删掉。
    """
    created = env.control.add_camera(preset="vigi")
    camera_id = created["id"]
    try:
        _check_self_signed_tls_on(env, camera_id)
    finally:
        with contextlib.suppress(Exception):
            env.control.remove_camera(camera_id)


def _check_self_signed_tls_on(env, camera_id):
    port = free_port(env.host)
    env.control.set_quirks(camera_id, {
        "transport.self_signed_tls": {"enabled": True, "params": {"port": port}}})
    time.sleep(0.8)
    context = ssl.create_default_context()
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE
    with socket.create_connection((env.host, port), timeout=5) as raw:
        with context.wrap_socket(raw) as tls:
            certificate = tls.getpeercert(binary_form=True)
    assert certificate, "TLS 握手成功了却拿不到证书"

    # 默认校验必须失败 —— 自签就是自签。
    strict = ssl.create_default_context()
    with socket.create_connection((env.host, port), timeout=5) as raw:
        try:
            with strict.wrap_socket(raw, server_hostname=env.host):
                raise AssertionError("自签证书居然通过了默认校验")
        except ssl.SSLError:
            pass


def check_overload_reboot(env):
    """F3：廉价固件遇并发 SOAP 风暴会超时甚至重启。"""
    import threading

    enable(env, "transport.overload_reboot", threshold=4, offline_seconds=4)

    def hammer():
        try:
            soap.post(env.device_url, "<tds:GetDeviceInformation/>",
                      env.user, env.password, timeout=5.0)
        except Exception:                       # noqa: BLE001 —— 打挂了本来就该失败
            pass

    threads = [threading.Thread(target=hammer) for _ in range(12)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(timeout=8)

    assert wait_offline(env), "并发风暴之后相机该短暂离线"


def check_system_reboot_real(env):
    """SystemReboot 真离线：Bye → 全端口关 N 秒 → Hello。"""
    enable(env, "transport.system_reboot_real", seconds=4)
    try:
        soap.post(env.device_url, "<tds:SystemReboot/>", env.user, env.password,
                  timeout=5.0)
    except requests.RequestException:
        pass                                    # 端口说关就关，响应可能收不全
    assert wait_offline(env), "SystemReboot 之后相机没真的离线"


def check_random_dropout(env):
    """随机掉线：percent=100 就是必掉，用来把随机性钉死。"""
    enable(env, "transport.random_dropout", percent=100.0, seconds=3)
    try:
        soap.post(env.device_url, "<tds:GetDeviceInformation/>", env.user,
                  env.password, timeout=5.0)
    except requests.RequestException:
        return
    assert wait_offline(env), "percent=100 却既没断连接也没离线"


def check_huge_response(env):
    """超大响应体：客户端的缓冲区与超时都会被拉爆。"""
    enable(env, "transport.huge_response", kb=512)
    response = soap.post(env.device_url, "<tds:GetDeviceInformation/>",
                         env.user, env.password, timeout=30.0)
    assert len(response.content) >= 400 * 1024, \
        "响应只有 %d 字节" % len(response.content)


def check_slowloris(env):
    """慢发送：一次几十字节、隔几十毫秒，把客户端的超时逼出来。"""
    baseline, _ = elapsed(soap.post, env.device_url, "<tds:GetDeviceInformation/>",
                          env.user, env.password)
    enable(env, "transport.slowloris", chunk_bytes=64, interval_ms=30)
    spent, response = elapsed(soap.post, env.device_url, "<tds:GetDeviceInformation/>",
                              env.user, env.password, timeout=60.0)
    assert spent > baseline + 0.3, "慢发送没起作用：基线 %.3fs，实际 %.3fs" \
        % (baseline, spent)
    assert response.status_code == 200, "慢归慢，内容还是要完整送到"


def check_global_delay(env):
    """整机响应延迟：所有 SOAP 都慢 N 毫秒。"""
    enable(env, "transport.global_delay", ms=900)
    spent, _ = elapsed(soap.post, env.device_url, "<tds:GetDeviceInformation/>",
                       env.user, env.password, timeout=30.0)
    assert spent >= 0.8, "只用了 %.3fs，延迟没生效" % spent


def check_rtp_rate_limit(env):
    """RTP 限速：把码率压到指定值以下，模拟窄带链路。"""
    enable(env, "transport.rtp_rate_limit", kbps=128)
    client = play_video(env)
    try:
        stats = collect_interleaved(client, seconds=3.0)
    finally:
        client.teardown()
        client.close()
    kbps = stats["bytes"] * 8 / 1000.0 / 3.0
    assert stats["bytes"] > 0, "限速之后一个字节都没收到"
    assert kbps < 128 * 2.5, "限到 128 kbps，实测 %.0f kbps" % kbps


# ========================================================================
# 断言表
# ========================================================================

DISCOVERY = (pytest.mark.discovery,)
SLOW = (pytest.mark.slow,)
STREAM = (pytest.mark.stream,)
SLOW_STREAM = (pytest.mark.slow, pytest.mark.stream)

CASES = [
    # ---- A. 发现 ----
    Case("discovery.dialect", check_discovery_dialect, DISCOVERY),
    Case("discovery.no_xaddrs", check_discovery_no_xaddrs, DISCOVERY),
    Case("discovery.no_metadata_version", check_discovery_no_metadata_version, DISCOVERY),
    Case("discovery.bad_xaddr_ip", check_discovery_bad_xaddr_ip, DISCOVERY),
    Case("discovery.no_reply", check_discovery_no_reply, DISCOVERY),
    Case("discovery.reply_delay", check_discovery_reply_delay, DISCOVERY + SLOW),
    Case("discovery.reply_twice", check_discovery_reply_twice, DISCOVERY),
    Case("discovery.scopes_no_name", check_discovery_scopes_no_name, DISCOVERY),

    # ---- A / D. 建连与鉴权 ----
    Case("connect.xaddr_odd_port", check_xaddr_odd_port),
    Case("connect.media2_first", check_media2_first),
    Case("connect.no_get_services", check_no_get_services),
    Case("auth.tight_time_window", check_tight_time_window),
    Case("auth.subscription_never_expires", check_subscription_never_expires, SLOW),
    Case("connect.device_info_missing", check_device_info_missing),
    Case("auth.password_text_only", check_password_text_only),
    Case("auth.nonce_strict_once", check_nonce_strict_once),
    Case("auth.preauth_required", check_preauth_required),
    Case("auth.http_401_not_fault", check_http_401_not_fault),
    Case("auth.fault_http_status", check_fault_http_status),
    Case("auth.fault_wording", check_fault_wording),

    # ---- B. Media 与快照 ----
    Case("media.profile_naming", check_profile_naming),
    Case("media.stream_uri_userinfo", check_stream_uri_userinfo),
    Case("media.stream_uri_nested", check_stream_uri_nested),
    Case("media.stream_uri_placeholder_ip", check_stream_uri_placeholder_ip),
    Case("media.snapshot_empty_body", check_snapshot_empty_body),
    Case("media.snapshot_auth", check_snapshot_auth),
    Case("media.snapshot_uri_rotates", check_snapshot_uri_rotates, SLOW),
    Case("media.no_get_snapshot_uri", check_no_get_snapshot_uri),
    Case("media.codec_mismatch", check_codec_mismatch, STREAM),
    Case("media.resolution_mismatch", check_resolution_mismatch, STREAM),

    # ---- C. PTZ ----
    Case("ptz.config_on_sub_only", check_ptz_config_on_sub_only),
    Case("ptz.usable_but_unadvertised", check_ptz_usable_but_unadvertised),
    Case("ptz.spaces_empty", check_ptz_spaces_empty),
    Case("ptz.range_min_equals_max", check_ptz_range_min_equals_max),
    Case("ptz.zoom_malformed_response", check_ptz_zoom_malformed_response),
    Case("ptz.factory_300_presets", check_ptz_factory_300_presets),
    Case("ptz.preset_percent_encoded", check_ptz_preset_percent_encoded),
    Case("ptz.no_get_presets", check_ptz_no_get_presets),
    Case("ptz.set_preset_return_shape", check_ptz_set_preset_return_shape),
    Case("ptz.response_jitter", check_ptz_response_jitter),
    Case("ptz.goto_preset_slow", check_ptz_goto_preset_slow, SLOW),
    Case("ptz.move_without_status_change", check_ptz_move_without_status_change),

    # ---- D. 事件 ----
    Case("events.subscription_port_increment", check_subscription_port_increment),
    Case("events.subscription_host_bad", check_subscription_host_bad),
    Case("events.subscription_slot_limit", check_subscription_slot_limit),
    Case("events.no_get_event_properties", check_no_get_event_properties),
    Case("events.bad_xml", check_event_properties_bad_xml),
    Case("events.flat_address", check_subscription_flat_address),
    Case("events.topic_style", check_topic_naming_style),
    Case("events.pull_always_empty", check_pull_always_empty, SLOW),
    Case("events.state_not_paired", check_event_state_not_paired, SLOW),
    Case("events.renew_fails", check_renew_fails),
    Case("events.subscription_expires_at_once", check_subscription_expires_at_once),
    Case("events.storm", check_event_storm, SLOW),
    Case("events.no_sync_point", check_no_sync_point),

    # ---- E. RTSP 与对讲 ----
    Case("rtsp.audio_capability_lie", check_audio_capability_lie),
    Case("rtsp.g722_sample_rate_8000", check_g722_sample_rate_8000),
    Case("rtsp.bitrate_unit", check_bitrate_unit),
    Case("rtsp.audio_decoder_options_shape", check_audio_decoder_options_shape),
    Case("rtsp.sample_rate_shape", check_sample_rate_shape),
    Case("rtsp.talkback_dual_track", check_talkback_dual_track),
    Case("rtsp.talkback_busy_slot", check_talkback_busy_slot, SLOW),
    Case("rtsp.auth_dual_challenge", check_auth_dual_challenge),
    Case("rtsp.auth_strict_digest_params", check_auth_strict_digest_params),
    Case("rtsp.sdp_nonstandard_codec", check_sdp_nonstandard_codec),
    Case("rtsp.sdp_no_rtpmap", check_sdp_no_rtpmap),
    Case("rtsp.sdp_session_level_control", check_sdp_session_level_control),
    Case("rtsp.talkback_require_marker", check_talkback_require_marker),
    Case("rtsp.setup_no_session_header", check_setup_no_session_header),
    Case("rtsp.ignore_backchannel_require", check_ignore_backchannel_require),
    Case("rtsp.no_audio_output_config", check_no_audio_output_config),
    Case("rtsp.talkback_stop_draining", check_talkback_stop_draining, SLOW),
    Case("rtsp.max_sessions", check_rtsp_max_sessions),
    Case("rtsp.periodic_teardown", check_rtsp_periodic_teardown, SLOW),
    Case("rtsp.rtp_packet_loss", check_rtp_packet_loss, SLOW),
    Case("rtsp.rtp_timestamp_jump", check_rtp_timestamp_jump, SLOW),
    Case("rtsp.sps_pps_placement", check_sps_pps_placement),
    Case("rtsp.video_fps_change", check_video_fps_change, STREAM),
    Case("rtsp.video_black", check_video_black, SLOW),
    Case("rtsp.video_freeze", check_video_freeze, SLOW),
    Case("rtsp.no_rtcp", check_no_rtcp, SLOW),

    # ---- F. 传输与设备 ----
    Case("transport.malformed_http", check_malformed_http),
    Case("transport.self_signed_tls", check_self_signed_tls),
    Case("transport.overload_reboot", check_overload_reboot, SLOW),
    Case("transport.system_reboot_real", check_system_reboot_real, SLOW),
    Case("transport.random_dropout", check_random_dropout, SLOW),
    Case("transport.huge_response", check_huge_response),
    Case("transport.slowloris", check_slowloris, SLOW),
    Case("transport.global_delay", check_global_delay),
    Case("transport.rtp_rate_limit", check_rtp_rate_limit, SLOW),
]

COVERED_KEYS = [case.key for case in CASES]


def duplicate_keys():
    seen = set()
    duplicates = set()
    for key in COVERED_KEYS:
        if key in seen:
            duplicates.add(key)
        seen.add(key)
    return sorted(duplicates)
