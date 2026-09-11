"""WS-Discovery：设备能不能被发现，以及两条发现层 quirk 的行为差异。

两套客户端同时用：
- wsdiscovery 2.1.2 —— 参照客户端用的那个库，它说找到才算真找到；
- 裸 UDP Probe —— 看未经库过滤的原始应答，用来区分「没回」和「回了但字段缺」。
"""

import time

import pytest

from helpers import wsd

SCENARIO = "single-camera"

pytestmark = pytest.mark.discovery


def _epr(env):
    return env.camera["identity"]["endpointReference"]


def _replies_for(env, **kwargs):
    """裸 Probe，只留 EPR 对得上这台相机的应答（局域网里可能有真相机）。"""
    target = _epr(env)
    replies = wsd.raw_probe(**kwargs)
    return [reply for reply in replies if wsd.epr_of(reply["text"]) == target]


def _search_epr(env, timeout=4.0):
    target = _epr(env)
    return [service for service in wsd.search_services(timeout=timeout)
            if service["epr"] == target]


# ---- 基本可发现性 ------------------------------------------------------

def test_wsdiscovery_library_finds_camera(env, require_discovery):
    """参照客户端同款库能搜到这台相机，且 XAddrs 指向它自己的 HTTP 端口。"""
    found = _search_epr(env)
    assert found, ("wsdiscovery 2.1.2 没搜到相机 %s。"
                   "多播不通的话用 ONVIFSIM_SKIP_DISCOVERY=1 跳过。" % _epr(env))
    assert len(found) == 1, "同一个 EPR 出现了 %d 次，去重没生效" % len(found)

    xaddrs = found[0]["xaddrs"]
    assert xaddrs, "ProbeMatch 没带 XAddrs"
    assert any(str(env.http_port) in address for address in xaddrs), \
        "XAddrs %s 里没有相机自己的 HTTP 端口 %d" % (xaddrs, env.http_port)


def test_probe_match_carries_required_fields(env, require_discovery):
    """ProbeMatch 该有的字段一个不能少 —— 缺哪个客户端都会整包丢。"""
    replies = _replies_for(env, timeout=3.0)
    assert replies, "没收到 ProbeMatch"
    text = replies[0]["text"]

    assert wsd.has_metadata_version(text), "缺 MetadataVersion"
    assert wsd.xaddrs_of(text), "缺 XAddrs"
    types = wsd.types_of(text)
    assert any("NetworkVideoTransmitter" in item for item in types), \
        "Types 里没有 NetworkVideoTransmitter：%s" % types

    scopes = wsd.scopes_of(text)
    assert wsd.scope_value(scopes, "name"), "Scopes 里缺 onvif.org/name/：%s" % scopes
    assert wsd.scope_value(scopes, "hardware"), "Scopes 里缺 onvif.org/hardware/：%s" % scopes
    assert wsd.dialect_of(text) == "2005", \
        "默认该照抄 Probe 的 1.0 方言，实际是 %s" % wsd.dialect_of(text)


def test_four_probes_are_deduplicated(env, require_discovery):
    """wsdiscovery 每次搜索发 4 次 Probe，客户端按 EPR 去重 —— 只应出现一条。

    设备该对每次 Probe 都回（这里断言收到多份原始应答），
    但库层面必须收敛成一台设备。
    """
    replies = _replies_for(env, timeout=3.0, repeats=4)
    assert len(replies) >= 2, \
        "4 次 Probe 只收到 %d 份应答，设备可能漏回了" % len(replies)

    found = _search_epr(env)
    assert len(found) == 1, "库层面没去重，同一 EPR 出现了 %d 次" % len(found)


# ---- A3：缺 MetadataVersion ------------------------------------------

def test_missing_metadata_version_hides_device(env, require_discovery):
    """A3：应答照发，但 wsdiscovery 2.1.2 解析不了 → 设备直接消失。

    这条 quirk 的价值就在这个反差：抓包看得见，客户端却搜不到。
    """
    baseline = _search_epr(env)
    assert baseline, "基线状态就搜不到，后面的对比没意义"

    env.set_quirk("discovery.no_metadata_version")
    time.sleep(0.3)

    replies = _replies_for(env, timeout=3.0)
    assert replies, "开了 A3 之后设备就该照常回 ProbeMatch，只是缺字段"
    assert not wsd.has_metadata_version(replies[0]["text"]), \
        "A3 开了却还带着 MetadataVersion"

    assert not _search_epr(env), "库居然还能解析出这台设备，A3 没生效"


# ---- A2：缺 XAddrs ---------------------------------------------------

def test_missing_xaddrs_forces_resolve(env, require_discovery):
    """A2：ProbeMatch 不带 XAddrs，客户端只能补发 Resolve 才拿得到地址。"""
    env.set_quirk("discovery.no_xaddrs")
    time.sleep(0.3)

    replies = _replies_for(env, timeout=3.0)
    assert replies, "开了 A2 之后仍然要回 ProbeMatch"
    assert not wsd.xaddrs_of(replies[0]["text"]), \
        "A2 开了却还带着 XAddrs：%s" % wsd.xaddrs_of(replies[0]["text"])

    # 设备本身仍然可发现（EPR / Types / Scopes 都在），只是地址要另外问。
    found = _search_epr(env)
    assert found, "A2 不该让设备整个消失，只是 XAddrs 空"


def test_two_quirks_differ(env, require_discovery):
    """把 A2 与 A3 摆在一起看：一个「有设备但没地址」，一个「设备直接不见」。"""
    env.set_quirk("discovery.no_xaddrs")
    time.sleep(0.3)
    with_a2 = _search_epr(env)

    env.set_quirk("discovery.no_metadata_version")
    time.sleep(0.3)
    with_a3 = _search_epr(env)

    assert with_a2 and not with_a3, \
        "A2 应当仍可见（%d 条），A3 应当不可见（%d 条）" % (len(with_a2), len(with_a3))


# ---- 场景文件驱动 ----------------------------------------------------

@pytest.mark.parametrize("scenario_sim", ["discovery-quirks"], indirect=True)
def test_discovery_quirks_scenario(scenario_sim, require_discovery):
    """discovery-quirks 场景：四台相机各带一种发现层毛病。

    cam1 缺 MetadataVersion、cam2 缺 XAddrs、cam3 坏 XAddr 排第一、
    cam4 回两次且 Scopes 不带 name。
    """
    cameras = {item["id"]: item for item in scenario_sim.cameras()}
    assert set(cameras) >= {"cam1", "cam2", "cam3", "cam4"}

    replies = wsd.raw_probe(timeout=4.0)
    by_epr = {}
    for reply in replies:
        by_epr.setdefault(wsd.epr_of(reply["text"]), []).append(reply["text"])

    def texts(camera_id):
        return by_epr.get(cameras[camera_id]["identity"]["endpointReference"], [])

    cam1 = texts("cam1")
    assert cam1 and not wsd.has_metadata_version(cam1[0]), "cam1 该缺 MetadataVersion"

    cam2 = texts("cam2")
    assert cam2 and not wsd.xaddrs_of(cam2[0]), "cam2 该不带 XAddrs"

    cam3 = texts("cam3")
    assert cam3, "cam3 没回应答"
    assert "192.168.99.99" in wsd.xaddrs_of(cam3[0])[0], \
        "cam3 的第一个 XAddr 该是那个不可达地址：%s" % wsd.xaddrs_of(cam3[0])

    cam4 = texts("cam4")
    assert len(cam4) >= 2, "cam4 该对同一个 Probe 回两次，实际 %d 次" % len(cam4)
    assert not wsd.scope_value(wsd.scopes_of(cam4[0]), "name"), \
        "cam4 的 Scopes 不该带 name"
