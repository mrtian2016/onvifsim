"""PTZ：四档能力声明、移动后 GetStatus 真的变、出厂 300 预置位。

「四档」指参照客户端在真机上遇到过的四种 PTZ 能力声明形态，
它们的共同点是：**声明看起来都不对，但云台其实都能动**。
所以判据永远是「ContinuousMove 之后位置有没有变」，而不是声明本身。
"""

import time

import pytest

from helpers import ptz, soap
from helpers.soap import SoapFault

SCENARIO = "single-camera"


def _tokens(env):
    profiles = env.soap_profiles()
    return [soap.attr(profile, "token") for profile in profiles]


def _wait_position_change(env, token, before, timeout=3.0, threshold=0.01):
    """等 GetStatus 的位置动起来（PTZ 是积分模型，要给它一点时间）。"""
    deadline = time.time() + timeout
    latest = before
    while time.time() < deadline:
        latest = ptz.get_status(env.ptz_url, env.user, env.password, token)
        moved = (abs(latest["pan"] - before["pan"]) > threshold
                 or abs(latest["tilt"] - before["tilt"]) > threshold
                 or abs(latest["zoom"] - before["zoom"]) > threshold)
        if moved:
            return latest
        time.sleep(0.15)
    return latest


# ---- 基线：节点与配置 --------------------------------------------------

def test_get_nodes(env, require_onvif_zeep):
    """onvif-zeep 严格解析 GetNodes —— 能解析出来就说明 XML 合规。"""
    camera = env.onvif()
    service = camera.create_ptz_service()
    nodes = service.GetNodes()
    assert nodes, "一个 PTZ 节点都没有"
    assert getattr(nodes[0], "token", None)


def test_get_node_declares_spaces(env):
    """默认状态下 SupportedPTZSpaces 要有内容（C3 的对照组）。"""
    node_token = env.camera["ptzNode"]["nodeToken"]
    node = ptz.get_node(env.ptz_url, env.user, env.password, node_token)
    assert node is not None
    spaces = ptz.spaces_of(node)
    assert spaces, "默认不该是空的 SupportedPTZSpaces"


# ---- 移动 -------------------------------------------------------------

def test_continuous_move_changes_status(env):
    """ContinuousMove 之后 GetStatus 的位置必须变 —— 这是 PTZ 唯一真判据。"""
    token = _tokens(env)[0]
    before = ptz.get_status(env.ptz_url, env.user, env.password, token)

    ptz.continuous_move(env.ptz_url, env.user, env.password, token, pan=0.6, tilt=0.3)
    after = _wait_position_change(env, token, before)
    ptz.stop(env.ptz_url, env.user, env.password, token)

    assert abs(after["pan"] - before["pan"]) > 0.01, \
        "pan 没动：%s -> %s" % (before["pan"], after["pan"])
    assert after["panTiltStatus"] in ("MOVING", "IDLE", "UNKNOWN", None)


def test_stop_halts_motion(env):
    """Stop 之后位置就该定住。"""
    token = _tokens(env)[0]
    ptz.continuous_move(env.ptz_url, env.user, env.password, token, pan=0.5)
    time.sleep(0.4)
    ptz.stop(env.ptz_url, env.user, env.password, token)
    time.sleep(0.3)

    first = ptz.get_status(env.ptz_url, env.user, env.password, token)
    time.sleep(0.6)
    second = ptz.get_status(env.ptz_url, env.user, env.password, token)
    assert abs(second["pan"] - first["pan"]) < 0.02, \
        "Stop 之后还在动：%s -> %s" % (first["pan"], second["pan"])


def test_rest_and_soap_agree_on_position(env):
    """REST 的 ``/api/cameras/{id}/ptz`` 与 SOAP 的 GetStatus 必须是同一份状态。"""
    token = _tokens(env)[0]
    ptz.continuous_move(env.ptz_url, env.user, env.password, token, pan=0.4)
    time.sleep(0.5)
    ptz.stop(env.ptz_url, env.user, env.password, token)
    time.sleep(0.2)

    soap_status = ptz.get_status(env.ptz_url, env.user, env.password, token)
    rest_status = env.control.ptz_status(env.camera_id)
    assert abs(soap_status["pan"] - rest_status["pan"]) < 0.02, \
        "SOAP %s vs REST %s" % (soap_status["pan"], rest_status["pan"])


# ---- 四档能力声明 -----------------------------------------------------

CAPABILITY_TIERS = [
    ("baseline", None, {}),
    ("C1 只挂子码流", "ptz.config_on_sub_only", {}),
    ("C2 一个都不挂", "ptz.usable_but_unadvertised", {}),
    ("C3 Spaces 为空", "ptz.spaces_empty", {"value": "empty"}),
    ("C4 Min == Max", "ptz.range_min_equals_max", {}),
]


@pytest.mark.parametrize("label,key,params",
                         CAPABILITY_TIERS, ids=[t[0] for t in CAPABILITY_TIERS])
def test_ptz_capability_tiers_still_movable(env, label, key, params):
    """四档声明形态各不相同，但云台一律得能动。

    客户端的判据被真机逼得越来越松：既然声明信不过，那就试着动一下看结果。
    """
    if key:
        env.set_quirk(key, **params)
        time.sleep(0.3)

    tokens = _tokens(env)
    assert tokens, "没有 profile"

    profiles = env.soap_profiles()
    config_tokens = ptz.profile_ptz_config_tokens(profiles)
    if key == "ptz.config_on_sub_only":
        attached = [index for index, value in enumerate(config_tokens) if value]
        assert len(attached) == 1, "C1 该只有一条 profile 挂 PTZConfiguration：%s" % config_tokens
        assert attached[0] != 0, "C1 挂的该是子码流而不是主码流"
    elif key == "ptz.usable_but_unadvertised":
        assert not any(config_tokens), "C2 该一条都不挂：%s" % config_tokens
    elif key == "ptz.spaces_empty":
        node_token = env.camera["ptzNode"]["nodeToken"]
        node = ptz.get_node(env.ptz_url, env.user, env.password, node_token)
        assert not ptz.spaces_of(node), "C3 该让 SupportedPTZSpaces 为空"
    elif key == "ptz.range_min_equals_max":
        node_token = env.camera["ptzNode"]["nodeToken"]
        node = ptz.get_node(env.ptz_url, env.user, env.password, node_token)
        pairs = ptz.ranges_of(node)
        assert pairs and all(low == high for low, high in pairs), \
            "C4 该让每个 Range 的 Min == Max：%s" % pairs

    # 不管声明成什么样，动一下都得真的动。
    movable_token = tokens[0]
    if key == "ptz.config_on_sub_only":
        attached = [index for index, value in enumerate(config_tokens) if value]
        movable_token = tokens[attached[0]]

    before = ptz.get_status(env.ptz_url, env.user, env.password, movable_token)
    ptz.continuous_move(env.ptz_url, env.user, env.password, movable_token, pan=0.5)
    after = _wait_position_change(env, movable_token, before)
    ptz.stop(env.ptz_url, env.user, env.password, movable_token)
    assert abs(after["pan"] - before["pan"]) > 0.01, \
        "%s：ContinuousMove 之后位置没变" % label


def test_c1_wrong_profile_faults(env):
    """C1 的痛点：拿错 profile 就吃 ``does not reference a PTZ configuration``。"""
    env.set_quirk("ptz.config_on_sub_only")
    time.sleep(0.3)

    tokens = _tokens(env)
    config_tokens = ptz.profile_ptz_config_tokens(env.soap_profiles())
    unattached = [tokens[index] for index, value in enumerate(config_tokens) if not value]
    assert unattached, "C1 之后总该有没挂配置的 profile"

    with pytest.raises(SoapFault):
        ptz.continuous_move(env.ptz_url, env.user, env.password, unattached[0], pan=0.5)


# ---- 预置位 -----------------------------------------------------------

def test_preset_roundtrip(env):
    """SetPreset → GetPresets → GotoPreset → RemovePreset 全流程。"""
    token = _tokens(env)[0]
    response = ptz.set_preset(env.ptz_url, env.user, env.password, token,
                              name="e2e-preset")
    assert response.status_code == 200, response.text[:400]

    presets = ptz.get_presets(env.ptz_url, env.user, env.password, token)
    match = [item for item in presets if item[1] == "e2e-preset"]
    assert match, "新建的预置位没出现在 GetPresets 里：%s" % presets

    preset_token = match[0][0]
    ptz.goto_preset(env.ptz_url, env.user, env.password, token, preset_token)
    ptz.remove_preset(env.ptz_url, env.user, env.password, token, preset_token)

    after = ptz.get_presets(env.ptz_url, env.user, env.password, token)
    assert preset_token not in [item[0] for item in after], "RemovePreset 没删掉"


def test_c6_factory_300_presets(env):
    """C6：海康出厂预填 300 个预置位，共享同一个假 PTZPosition，还夹着功能槽。

    客户端拿到 300 条会直接把预置位列表撑爆，所以必须能复现。
    """
    env.set_quirk("ptz.factory_300_presets", count=300)
    time.sleep(0.5)

    token = _tokens(env)[0]
    presets = ptz.get_presets(env.ptz_url, env.user, env.password, token)
    assert len(presets) >= 300, "C6 开了却只有 %d 个预置位" % len(presets)

    rest = env.control.ptz_status(env.camera_id)
    assert len(rest["presets"]) >= 300, "REST 里也该看得到 300 个"


def test_c7_percent_encoded_preset_name(env):
    """C7：预置位名百分号编码原样吐回（同品牌两台行为还不一样）。"""
    env.set_quirk("ptz.preset_percent_encoded")
    time.sleep(0.2)

    token = _tokens(env)[0]
    ptz.set_preset(env.ptz_url, env.user, env.password, token, name="大门 入口")
    presets = ptz.get_presets(env.ptz_url, env.user, env.password, token)
    names = [name for _, name in presets if name]
    assert any("%" in name for name in names), \
        "C7 开了但没有一个名字是百分号编码的：%s" % names


def test_c8_no_get_presets(env):
    """C8：GetPresets 未实现 → 客户端靠正则认 ``ActionNotSupported`` / ``not implemented``。"""
    env.set_quirk("ptz.no_get_presets", value="action_not_supported")
    time.sleep(0.2)

    token = _tokens(env)[0]
    with pytest.raises(SoapFault) as excinfo:
        ptz.get_presets(env.ptz_url, env.user, env.password, token)
    blob = ("%s %s" % (excinfo.value.subcode, excinfo.value.reason)).lower()
    assert "actionnotsupported" in blob.replace(" ", "") or "not implemented" in blob, \
        "Fault 措辞客户端认不出来：%s" % blob
