"""对讲（RTSP backchannel）：DESCRIBE → SETUP → PLAY → 推 RTP → 查统计。

RTSP 客户端是自己用 socket 写的（``helpers/rtsp.py``）。
不引 live555 之类，是因为这里要精确控制发出去的字节 ——
marker 位、talkspurt 边界、包间隔，库反而挡路。

完整序列见 reference-client-facts.md §4.3。
"""

import time

import pytest

from helpers import rtsp

SCENARIO = "single-camera"


def backchannel_track(parsed):
    """从 SDP 里挑出 ``a=sendonly`` 的音频轨。"""
    for entry in parsed["media"]:
        if entry["m"].startswith("audio") and rtsp.is_backchannel(entry):
            return entry
    return None


def open_backchannel(env, channel=0, require=True):
    """把 DESCRIBE → SETUP → PLAY 走完，返回 (client, track, setup_response)。"""
    client = env.rtsp()
    client.options()
    describe = client.describe(require_backchannel=require)
    assert describe.status == 200, "DESCRIBE 失败：%s" % describe

    parsed = rtsp.parse_sdp(describe.sdp)
    track = backchannel_track(parsed)
    assert track is not None, "SDP 里没有 sendonly 的对讲轨：\n%s" % describe.sdp

    control = rtsp.media_control(track) or rtsp.session_control(parsed)
    setup = client.setup(control, interleaved=(channel, channel + 1),
                         require_backchannel=require)
    assert setup.status == 200, "SETUP 失败：%s" % setup

    play = client.play(require_backchannel=require)
    assert play.status == 200, "PLAY 失败：%s" % play
    return client, track, setup


# ---- 基线全流程 -------------------------------------------------------

def test_options_advertises_methods(env):
    """OPTIONS 要把 DESCRIBE / SETUP / PLAY / TEARDOWN 都列出来。"""
    with env.rtsp() as client:
        response = client.options()
        assert response.status == 200, response
        public = (response.header("Public") or "").upper()
        for method in ("DESCRIBE", "SETUP", "PLAY", "TEARDOWN"):
            assert method in public, "OPTIONS 没声明 %s：%s" % (method, public)


def test_describe_with_require_returns_backchannel_sdp(env):
    """带 ``Require: www.onvif.org/ver20/backchannel`` 的 DESCRIBE 要多出一条 sendonly 轨。"""
    with env.rtsp() as client:
        plain = client.describe(require_backchannel=False)
        assert plain.status == 200
        assert backchannel_track(rtsp.parse_sdp(plain.sdp)) is None, \
            "没带 Require 也给了对讲轨"

    with env.rtsp() as client:
        with_require = client.describe(require_backchannel=True)
        assert with_require.status == 200
        track = backchannel_track(rtsp.parse_sdp(with_require.sdp))
        assert track is not None, "带了 Require 却没有对讲轨"
        assert "sendonly" in track["attrs"]


def test_talkback_bytes_and_markers(env):
    """整条链路：推 25 包 PCMU 之后，REST 里的字节数与 marker 计数都要非零。"""
    client, _track, _setup = open_backchannel(env)
    try:
        talker = rtsp.Talker(client, channel=0)
        talker.burst(packets=25, marker_on_first=True)
        time.sleep(0.5)

        stats = env.control.talkback(env.camera_id)
        assert stats["totalBytes"] > 0, "推了 %d 字节，REST 却是 0" % talker.bytes_sent
        assert stats["sessions"], "talkback 里没有会话"

        session = stats["sessions"][0]
        assert session["packets"] >= 20, "只收到 %d 包（发了 %d 包）" \
            % (session["packets"], talker.packets_sent)
        assert session["markers"] >= 1, "marker 计数是 0，首包的 marker 位没被认出来"
        assert session["codec"], "没识别出对讲编码"
    finally:
        client.teardown()
        client.close()


def test_session_appears_in_rest(env):
    """PLAY 之后 REST 的 sessions 里要能看到这条带 backchannel 的会话。"""
    client, _track, setup = open_backchannel(env)
    try:
        session_id = setup.header("Session", "").split(";")[0]
        assert session_id, "SETUP 没返回 Session 头"

        sessions = env.control.sessions(env.camera_id)["rtsp"]
        assert sessions, "REST 里看不到 RTSP 会话"
        assert any(item["backchannel"] for item in sessions), \
            "会话没被标成 backchannel：%s" % sessions
    finally:
        client.teardown()
        client.close()


def test_teardown_releases_session(env):
    """TEARDOWN 之后会话要从列表里消失。"""
    client, _track, _setup = open_backchannel(env)
    client.teardown()
    client.close()
    time.sleep(0.4)
    sessions = env.control.sessions(env.camera_id)["rtsp"]
    assert not any(item["backchannel"] for item in sessions), \
        "TEARDOWN 之后还留着 backchannel 会话：%s" % sessions


# ---- E13：talkspurt 首包 marker --------------------------------------

def test_e13_missing_marker_is_counted(env):
    """E13：talkspurt 首包 marker=0，冷启动的相机会丢掉整个 talkburst。"""
    env.set_quirk("rtsp.talkback_require_marker")
    time.sleep(0.2)

    client, _track, _setup = open_backchannel(env)
    try:
        talker = rtsp.Talker(client, channel=0)
        talker.burst(packets=20, marker_on_first=False)
        time.sleep(0.5)

        stats = env.control.talkback(env.camera_id)["sessions"][0]
        assert stats["markerMissing"] >= 1, \
            "首包没带 marker，markerMissing 却是 0：%s" % stats

        # 补一个带 marker 的 talkspurt，这次就该正常收下。
        talker.burst(packets=20, marker_on_first=True)
        time.sleep(0.5)
        after = env.control.talkback(env.camera_id)["sessions"][0]
        assert after["markers"] >= 1, "补发的 talkspurt 带了 marker，却仍然没被认出来"
    finally:
        client.teardown()
        client.close()


# ---- E7：忙槽位 -------------------------------------------------------

@pytest.mark.slow
def test_e7_busy_slot_rejects_new_describe(env):
    """E7：上条会话槽位没释放时，新的 DESCRIBE 回 401（海康球机要 3~4 秒）。"""
    env.set_quirk("rtsp.talkback_busy_slot", seconds=3)
    time.sleep(0.2)

    client, _track, _setup = open_backchannel(env)
    client.teardown()
    client.close()

    with rtsp.RtspClient(env.host, env.rtsp_port, env.stream_path(0),
                         env.user, env.password) as second:
        blocked = second.describe(require_backchannel=True, retry_auth=False)
        assert blocked.status == 401, \
            "槽位还没释放，DESCRIBE 却给了 %d" % blocked.status

    time.sleep(3.5)
    with rtsp.RtspClient(env.host, env.rtsp_port, env.stream_path(0),
                         env.user, env.password) as third:
        recovered = third.describe(require_backchannel=True)
        assert recovered.status == 200, "等够时间之后该恢复，实际 %d" % recovered.status


# ---- 场景：对讲三连 ---------------------------------------------------

@pytest.mark.parametrize("scenario_sim", ["talkback"], indirect=True)
def test_talkback_scenario_dual_track(scenario_sim):
    """talkback 场景的 cam2 是大华风格：麦克风 recvonly + 对讲 sendonly 两条轨。"""
    from helpers.env import CameraEnv

    env = CameraEnv(scenario_sim, "cam2")
    with env.rtsp() as client:
        response = client.describe(require_backchannel=True)
        assert response.status == 200
        parsed = rtsp.parse_sdp(response.sdp)
        audio = [entry for entry in parsed["media"] if entry["m"].startswith("audio")]
        assert len(audio) >= 2, "大华双轨该有两条音频轨：%d" % len(audio)
        assert any(rtsp.is_backchannel(entry) for entry in audio), "缺 sendonly 轨"
        assert any("recvonly" in entry["attrs"] for entry in audio), "缺 recvonly 轨"
