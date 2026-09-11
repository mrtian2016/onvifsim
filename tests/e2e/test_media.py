"""Media：GetProfiles / GetStreamUri，然后 ffprobe **真的把流拉下来**验。

参照客户端不信 ``VideoEncoderConfiguration`` 里写的分辨率 / 编码 / 帧率，
一律靠探流拿真值。所以这里的判据也是探流结果，
声明值只用来做「声明与实际是否一致」的对照（quirk media.resolution_mismatch）。
"""

import time

import pytest

from helpers import media, soap
from helpers.soap import SoapFault

SCENARIO = "single-camera"


# ---- GetProfiles ------------------------------------------------------

def test_get_profiles_via_onvif_zeep(env, require_onvif_zeep):
    """onvif-zeep 严格解析 GetProfiles —— 能解析出来就说明 XML 是合规的。"""
    camera = env.onvif()
    service = camera.create_media_service()
    profiles = service.GetProfiles()
    assert profiles, "一条 profile 都没有"

    for profile in profiles:
        assert getattr(profile, "token", None), "profile 缺 token"
        assert getattr(profile, "Name", None), "profile 缺 Name"
        encoder = profile.VideoEncoderConfiguration
        assert encoder is not None, "profile 缺 VideoEncoderConfiguration"
        assert encoder.Resolution.Width > 0 and encoder.Resolution.Height > 0


def test_profile_names_carry_main_sub_semantics(env, require_onvif_zeep):
    """默认命名风格下，主 / 子码流要能按 Name 字串判出来（B1 的基线）。"""
    from helpers import onvifclient

    camera = env.onvif()
    profiles = camera.create_media_service().GetProfiles()
    names = [(profile.Name or "").lower() for profile in profiles]
    assert any("main" in name for name in names), \
        "默认该有个名字含 main 的 profile：%s" % names

    chosen = onvifclient.main_profile(profiles)
    assert "main" in (chosen.Name or "").lower()


def test_get_video_sources(env):
    """P0 清单里的 GetVideoSources。"""
    root = soap.call(env.media_url, "<trt:GetVideoSources/>", env.user, env.password)
    sources = soap.findall(root, "VideoSources")
    assert sources, "GetVideoSources 返回空"


# ---- GetStreamUri -----------------------------------------------------

def test_get_stream_uri(env, require_onvif_zeep):
    """GetStreamUri(RTP-Unicast / RTSP) 要给出一个能连的 rtsp:// 地址。"""
    from helpers import onvifclient

    camera = env.onvif()
    profiles = camera.create_media_service().GetProfiles()
    result = onvifclient.stream_uri(camera, profiles[0].token)
    uri = result.Uri
    assert uri.startswith("rtsp://"), uri
    assert "@" not in uri.split("//", 1)[1].split("/", 1)[0], \
        "默认不该在 URI 里带 userinfo（那是 quirk B2 的事）：%s" % uri


def test_stream_uri_matches_control_api(env):
    """SOAP 报的流地址与 REST ``streamUris`` 应当指向同一个 path。"""
    token = env.main_profile_token()
    root = soap.get_stream_uri(env.media_url, env.user, env.password, token)
    uri = soap.text(soap.find(root, "Uri"))
    assert uri
    assert env.stream_path(0) in uri, "%s 不含 %s" % (uri, env.stream_path(0))


# ---- ffprobe 真的拉流 -------------------------------------------------

@pytest.mark.stream
def test_ffprobe_reads_main_stream(env, require_ffprobe):
    """主码流：codec / 分辨率 / fps / 音频编码，全部以探流结果为准。"""
    declared = env.camera["profiles"][0]
    result = media.probe(env.stream_url(0))

    video = media.video_info(result)
    assert video is not None, "探不到视频轨"
    assert video["codec"] == "h264", "编码该是 h264，实际 %s" % video["codec"]
    assert video["width"] == declared["videoEncoder"]["width"], \
        "宽度：声明 %s 实际 %s" % (declared["videoEncoder"]["width"], video["width"])
    assert video["height"] == declared["videoEncoder"]["height"], \
        "高度：声明 %s 实际 %s" % (declared["videoEncoder"]["height"], video["height"])

    expected_fps = float(declared["videoEncoder"]["frameRate"])
    assert video["fps"] is not None
    assert abs(video["fps"] - expected_fps) <= max(2.0, expected_fps * 0.25), \
        "帧率：声明 %.1f 实际 %.2f" % (expected_fps, video["fps"])

    audio = media.audio_info(result)
    assert audio is not None, "探不到音频轨"
    assert audio["codec"] in ("pcm_mulaw", "pcm_alaw", "aac", "g722"), \
        "音频编码不认识：%s" % audio["codec"]


@pytest.mark.stream
@pytest.mark.parametrize("index", [0, 1, 2])
def test_ffprobe_reads_every_profile(env, require_ffprobe, index):
    """三档码流（1080p / 720p / 360p）都要真的能拉，且分辨率各不相同。"""
    profiles = env.camera["profiles"]
    if index >= len(profiles):
        pytest.skip("这台相机只有 %d 条 profile" % len(profiles))
    declared = profiles[index]

    result = media.probe(env.stream_url(index))
    video = media.video_info(result)
    assert video is not None, "第 %d 条 profile 探不到视频轨" % index
    assert video["width"] == declared["videoEncoder"]["width"]
    assert video["height"] == declared["videoEncoder"]["height"]


@pytest.mark.stream
def test_stream_uri_from_soap_is_pullable(env, require_ffprobe):
    """整条链路：GetStreamUri 拿到的地址，注入凭据之后 ffprobe 能直接拉。"""
    token = env.main_profile_token()
    root = soap.get_stream_uri(env.media_url, env.user, env.password, token)
    uri = soap.text(soap.find(root, "Uri"))
    # 和参照客户端一样：只接管 host:port、保留 path，再重注入凭据。
    usable = media.inject_credentials(
        soap.rehost(uri, env.host, env.rtsp_port), env.user, env.password)

    result = media.probe(usable)
    assert media.video_info(result) is not None


# ---- B7 / B2 / B3：Media 层的几条 quirk ------------------------------

def test_b2_stream_uri_with_userinfo(env):
    """B2：真机会把 user:pass 直接写进 URI，客户端要丢弃再重注入。"""
    env.set_quirk("media.stream_uri_userinfo", userinfo="admin:wrongpass")
    time.sleep(0.2)

    token = env.main_profile_token()
    root = soap.get_stream_uri(env.media_url, env.user, env.password, token)
    uri = soap.text(soap.find(root, "Uri"))
    assert "admin:wrongpass@" in uri, "B2 开了但 URI 里没有 userinfo：%s" % uri

    # 重注入之后的地址不该再带旧凭据。
    clean = media.inject_credentials(uri, env.user, env.password)
    assert "wrongpass" not in clean


def test_b7_no_get_snapshot_uri(env):
    """B7：GetSnapshotUri 不实现，客户端要退避而不是死循环。"""
    env.set_quirk("media.no_get_snapshot_uri")
    time.sleep(0.2)
    with pytest.raises(SoapFault):
        soap.get_snapshot_uri(env.media_url, env.user, env.password,
                              env.main_profile_token())
