"""图形界面版的存活测试。

协议层的用例全跑在 --headless 上，界面层一行都碰不到 —— 而界面自己有一套
每秒刷新的逻辑，它读的正是 RTSP 会话、订阅、对讲统计这些**会随客户端行为变化**
的东西。真出过一次事故：对讲页的 refresh() 拿「连接总数」去和「有对讲轨的会话」
下拉框比，普通取流一连上，两个函数就无限互调直接爆栈 —— 用户看到的是「一读流就闪退」。

所以这里专门起图形版（同一个可执行文件，只是不带 --headless），
做客户端会做的事，然后确认进程还活着。
"""

import time

import pytest

from helpers import media
from helpers.env import CameraEnv
from helpers.ports import free_port, free_port_block
from helpers.sim import SimProcess, patch_scenario


pytestmark = pytest.mark.slow


@pytest.fixture
def gui_sim(request, binary_path, tmp_path, scenarios_dir):
    """起一台图形界面版的模拟器。"""
    http_base = free_port_block(5)
    rtsp_base = free_port_block(5)
    control_port = free_port()
    target = tmp_path / "single-camera.json"
    patch_scenario(scenarios_dir / "single-camera.json", target,
                   http_base, rtsp_base, control_port)
    sim = SimProcess(binary_path, target, tmp_path,
                     control_port=control_port, http_base=http_base,
                     rtsp_base=rtsp_base, headless=False)
    try:
        sim.start()
    except Exception as exc:
        # 这里**只**放过「这台机器确实没有 offscreen 平台插件」这一种情况。
        # 原来是无差别 skip，结果是：可执行文件路径写错、程序一起来就崩，
        # 统统伪装成一条「大概缺插件」的跳过 —— 而这两条用例正是为
        # 「一拉流就闪退」那次事故写的，静默跳过比没有测试更糟。
        detail = "%s\n%s" % (exc, sim.tail_logs())
        if "platform plugin" in detail or "Qt platform plugin" in detail:
            pytest.skip("这台机器缺 offscreen 平台插件，图形版起不来")
        raise
    try:
        yield sim
    finally:
        sim.stop()


def test_gui_survives_repeated_streaming(gui_sim):
    """界面开着的时候反复拉流，进程必须一直活着。

    这条就是冲着那次「一拉流就闪退」去的：崩溃只在界面版出现，
    而且要真的建起 RTSP 会话才会触发。
    """
    if not media.ffprobe_available():
        pytest.skip("没有 ffprobe，拉不了流")

    env = CameraEnv(gui_sim)
    url = env.stream_url(0)

    for round_index in range(3):
        for transport in ("tcp", "udp"):
            result = media.probe(url, transport=transport)
            video = media.video_info(result)
            assert video is not None, \
                "第 %d 轮 %s 拉流没探到视频轨" % (round_index + 1, transport)
            assert gui_sim.process.poll() is None, \
                "第 %d 轮 %s 拉流之后界面版进程死了：\n%s" % (
                    round_index + 1, transport, gui_sim.tail_logs())

    # 会话建完又断完，界面的刷新循环还得再转几圈才算真稳。
    time.sleep(2.0)
    assert gui_sim.process.poll() is None, \
        "拉流结束后界面版才死：\n%s" % gui_sim.tail_logs()


def test_gui_survives_control_api_traffic(gui_sim):
    """界面开着时用 REST 频繁改状态，界面的刷新不能被带崩。"""
    env = CameraEnv(gui_sim)
    for _ in range(5):
        env.control.trigger_event(env.camera_id, kind="Motion", duration=200)
        env.control.set_quirks(env.camera_id, {"ptz.factory_300_presets": True})
        env.control.set_quirks(env.camera_id, {"ptz.factory_300_presets": False})
        time.sleep(0.3)
    assert gui_sim.process.poll() is None, \
        "REST 操作把界面版搞崩了：\n%s" % gui_sim.tail_logs()
