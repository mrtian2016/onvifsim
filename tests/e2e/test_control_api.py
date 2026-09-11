"""REST 控制面自身的冒烟测试。

``docs/control-api.md`` 是照着 ``src/control/ControlApi.cpp`` 写的，
这里把每个端点都打一遍，文档和实现漂移了就会红。
"""

import pytest

from helpers.control import ControlError

SCENARIO = "single-camera"


def test_list_cameras(control):
    """GET /api/cameras —— 列表里带 status / xaddr / streamUris / quirks。"""
    cameras = control.cameras()
    assert cameras, "一台相机都没有"
    camera = cameras[0]
    for field in ("id", "identity", "network", "profiles", "capabilities",
                  "status", "xaddr", "streamUris", "quirks"):
        assert field in camera, "列表项缺字段 %s：%s" % (field, sorted(camera))
    assert camera["status"]["running"] is True


def test_get_single_camera(control):
    """GET /api/cameras/{id}"""
    camera_id = control.cameras()[0]["id"]
    camera = control.camera(camera_id)
    assert camera["id"] == camera_id
    assert "quirks" in camera and "xaddr" in camera


def test_unknown_camera_is_404(control):
    with pytest.raises(ControlError) as excinfo:
        control.camera("no-such-camera")
    assert excinfo.value.status == 404


def test_create_and_delete_camera(control):
    """POST /api/cameras（201）与 DELETE /api/cameras/{id}。"""
    before = len(control.cameras())
    created = control.add_camera(preset="hikvision")
    assert created["persona"] == "hikvision"
    assert created["identity"]["manufacturer"]
    assert len(control.cameras()) == before + 1

    control.remove_camera(created["id"])
    assert len(control.cameras()) == before


def test_patch_quirks_merges(control):
    """PATCH 的 quirks 是**部分更新**：只动提到的，没提到的保持不变。

    这条语义很关键 —— 文档和 GUI 都按它写，改了会连累一大片。
    """
    camera_id = control.cameras()[0]["id"]
    control.clear_quirks(camera_id)

    control.set_quirks(camera_id, {"media.stream_uri_nested": True,
                                   "rtsp.sdp_no_rtpmap": True})
    enabled = {key for key, value in control.camera(camera_id)["quirks"].items()
               if value.get("enabled")}
    assert enabled == {"media.stream_uri_nested", "rtsp.sdp_no_rtpmap"}

    # 只提一条，另一条不该被牵连。
    control.set_quirks(camera_id, {"events.bad_xml": True})
    enabled = {key for key, value in control.camera(camera_id)["quirks"].items()
               if value.get("enabled")}
    assert enabled == {"media.stream_uri_nested", "rtsp.sdp_no_rtpmap",
                       "events.bad_xml"}, "PATCH 该是合并语义"

    # 关掉一条要显式写 false。
    control.set_quirks(camera_id, {"events.bad_xml": False})
    enabled = {key for key, value in control.camera(camera_id)["quirks"].items()
               if value.get("enabled")}
    assert "events.bad_xml" not in enabled

    control.clear_quirks(camera_id)
    enabled = {key for key, value in control.camera(camera_id)["quirks"].items()
               if value.get("enabled")}
    assert not enabled, "clear 之后不该还有开着的"


def test_unknown_quirk_key_is_reported(control):
    """未知的 quirk 键会进 warnings，但不会让整个请求失败。"""
    camera_id = control.cameras()[0]["id"]
    result = control.patch_camera(camera_id, {"quirks": {"no.such.quirk": True}})
    assert result.get("warnings"), "未知键该给告警：%s" % result
    control.clear_quirks(camera_id)


def test_quirks_metadata(control):
    """GET /api/quirks —— 文档与 GUI 都从这里取。"""
    quirks = control.quirks_meta()
    assert len(quirks) > 50, "只有 %d 条 quirk，像是没加载全" % len(quirks)
    groups = {item["group"] for item in quirks}
    assert len(groups) >= 5, groups


def test_presets(control):
    """GET /api/presets —— 品牌预设（plan.md §4.15 的表里漏了这个端点）。"""
    presets = control.presets()
    keys = {item["key"] for item in presets}
    assert {"generic", "hikvision", "dahua", "tplink"} <= keys, keys


def test_ptz_endpoints(control):
    """GET / POST /api/cameras/{id}/ptz"""
    camera_id = control.cameras()[0]["id"]
    status = control.ptz_status(camera_id)
    for field in ("pan", "tilt", "zoom", "panTiltStatus", "zoomStatus", "presets"):
        assert field in status, sorted(status)

    control.ptz(camera_id, "continuous", pan=0.4)
    control.ptz(camera_id, "stop")
    control.ptz(camera_id, "absolute", pan=0.0, tilt=0.0)

    with pytest.raises(ControlError) as excinfo:
        control.ptz(camera_id, "nonsense")
    assert excinfo.value.status == 400


def test_sessions_and_talkback(control):
    """GET /api/cameras/{id}/sessions 与 /talkback 在空闲时也要有稳定结构。"""
    camera_id = control.cameras()[0]["id"]
    sessions = control.sessions(camera_id)
    assert set(sessions) >= {"rtsp", "subscriptions", "httpClients"}

    talkback = control.talkback(camera_id)
    assert set(talkback) >= {"sessions", "totalBytes"}
    assert talkback["totalBytes"] == 0


def test_events_endpoint(control):
    """POST /api/cameras/{id}/events —— 按 kind 或按 topic 触发。"""
    camera_id = control.cameras()[0]["id"]
    assert control.trigger_event(camera_id, kind="Motion", duration=1)["ok"]

    with pytest.raises(ControlError) as excinfo:
        control.trigger_event(camera_id, kind="NoSuchKind")
    assert excinfo.value.status == 400


def test_offline_endpoint(control):
    """POST /api/cameras/{id}/offline?seconds=N"""
    camera_id = control.cameras()[0]["id"]
    assert control.go_offline(camera_id, seconds=1)["ok"]


def test_scenario_export(control):
    """GET /api/scenario —— 导出的场景要能直接当输入用。"""
    scenario = control.scenario()
    assert scenario["formatVersion"] >= 1
    assert "config" in scenario and "cameras" in scenario
    assert scenario["cameras"], "导出的场景里没有相机"


def test_network_endpoint(control):
    """GET /api/network —— 网卡列表与当前网络模式。"""
    network = control.network()
    assert "interfaces" in network and network["interfaces"]
    assert network["mode"] in ("ports", "ip_alias", "external")
    assert "needsElevation" in network


def test_metrics_is_prometheus_text(control):
    """GET /metrics —— Prometheus 文本，压测时直接抓。"""
    text = control.metrics()
    assert "onvifsim_cameras" in text
    assert "onvifsim_rtsp_sessions" in text
    assert "# TYPE" in text


def test_openapi_lists_endpoints(control):
    """GET /openapi.json —— 端点清单从实际路由生成。"""
    spec = control.openapi()
    assert spec["openapi"].startswith("3.")
    paths = set(spec["paths"])
    assert "/api/cameras" in paths and "/api/quirks" in paths and "/metrics" in paths


def test_unknown_endpoint_is_404(control):
    with pytest.raises(ControlError) as excinfo:
        control.get("/api/nope")
    assert excinfo.value.status == 404
