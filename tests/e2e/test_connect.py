"""建连：onvif-zeep 能不能连上、GetCapabilities 拿不拿得到各 XAddr。

onvif-zeep 0.2.12 构造 ``ONVIFCamera`` 时就会打一次
``GetCapabilities(Category=All)`` —— 构造成功本身就是一条断言。
它按官方 WSDL 严格解析，字段错就抛异常，等于免费的 schema 校验。
"""

import time

import pytest

from helpers import soap
from helpers.soap import SoapFault

SCENARIO = "single-camera"


# ---- P0：连得上 -------------------------------------------------------

def test_onvif_camera_constructs(env, require_onvif_zeep):
    """构造 ONVIFCamera 成功 = GetCapabilities 通了、响应能被 WSDL 解析。"""
    camera = env.onvif()
    assert camera is not None
    capabilities = camera.devicemgmt.GetCapabilities({"Category": "All"})
    assert capabilities is not None


def test_get_capabilities_returns_all_xaddrs(env, require_onvif_zeep):
    """参照客户端只用 GetCapabilities(Category=All) 拿 XAddr，从不用 GetServices。"""
    from helpers import onvifclient

    camera = env.onvif()
    xaddrs = onvifclient.capability_xaddrs(camera)
    for service in ("device", "media", "events"):
        assert service in xaddrs, "GetCapabilities 里没有 %s 的 XAddr：%s" % (service, xaddrs)
        assert xaddrs[service].startswith("http"), xaddrs[service]

    model = env.camera
    if model["capabilities"]["ptz"]:
        assert "ptz" in xaddrs, "声明了 PTZ 却没报 XAddr"
    if model["capabilities"]["imaging"]:
        assert "imaging" in xaddrs


def test_get_device_information_fields(env, require_onvif_zeep):
    """五个字段齐全，并且与 REST 里的相机模型一致。"""
    from helpers import onvifclient

    camera = env.onvif()
    info = onvifclient.device_information(camera)
    model = env.camera["identity"]

    assert info["Manufacturer"] == model["manufacturer"]
    assert info["Model"] == model["model"]
    assert info["FirmwareVersion"] == model["firmwareVersion"]
    assert info["SerialNumber"] == model["serialNumber"]
    assert info["HardwareId"] == model["hardwareId"]


def test_get_services_lists_device_and_media(env):
    """GetServices 有实现时，Device 与 Media 都要在，且顺序有意义（见 A6）。"""
    services = soap.get_services(env.device_url, env.user, env.password)
    namespaces = [namespace for namespace, _ in services]
    assert any("device/wsdl" in ns for ns in namespaces), namespaces
    assert any("ver10/media/wsdl" in ns for ns in namespaces), namespaces


def test_wrong_password_is_rejected(env):
    """密码错就该失败 —— 别把鉴权做成摆设。"""
    with pytest.raises(Exception):
        soap.get_device_information(env.device_url, env.user, "definitely-wrong")


# ---- A10：设备信息缺字段 ---------------------------------------------

def test_a10_device_info_missing_fields(env, require_onvif_zeep):
    """A10：真机常缺 HardwareId / SerialNumber。

    客户端全字段都写了 ``or None``，所以缺字段**不能**让解析崩 ——
    这条断言同时盯住「确实缺了」和「缺了也还能用」。
    """
    from helpers import onvifclient

    before = onvifclient.device_information(env.onvif())
    assert before["HardwareId"], "基线状态 HardwareId 就是空的，对比没意义"

    env.set_quirk("connect.device_info_missing", fields="HardwareId,SerialNumber")
    time.sleep(0.2)

    after = onvifclient.device_information(env.onvif())
    assert not after["HardwareId"], "A10 开了但 HardwareId 还在"
    assert not after["SerialNumber"], "A10 开了但 SerialNumber 还在"
    # 没被省掉的字段必须原样保留，否则客户端选不出厂商适配器。
    assert after["Manufacturer"] == before["Manufacturer"]
    assert after["Model"] == before["Model"]


# ---- A5：XAddr 报非常规端口 ------------------------------------------

def test_a5_xaddr_odd_port_is_advertised(env):
    """A5：真机 TL-IPC652P-A4 报 ``:2020/onvif/service``，实际连的却是 80。"""
    env.set_quirk("connect.xaddr_odd_port", port=2020, path="/onvif/service")
    time.sleep(0.2)

    xaddrs = soap.capability_xaddrs(env.device_url, env.user, env.password)
    device_xaddr = xaddrs["device"]
    assert ":2020" in device_xaddr, "A5 开了但 XAddr 端口没变：%s" % device_xaddr
    assert device_xaddr.endswith("/onvif/service"), \
        "A5 的 path 没生效：%s" % device_xaddr

    # REST 报的对外地址要和 SOAP 里一致。
    assert ":2020" in env.listing["xaddr"]


def test_a5_client_takes_over_host_port_keeps_path(env):
    """客户端只接管 host:port、保留 path —— 这才是 A5 能被扛过去的原因。"""
    env.set_quirk("connect.xaddr_odd_port", port=2020, path="/onvif/service")
    time.sleep(0.2)

    xaddrs = soap.capability_xaddrs(env.device_url, env.user, env.password)
    advertised = xaddrs["media"]
    usable = soap.rehost(advertised, env.host, env.http_port)

    assert usable.startswith("http://%s:%d" % (env.host, env.http_port))
    assert usable.endswith("/" + advertised.split("/", 3)[3]), \
        "rehost 不该动 path：%s -> %s" % (advertised, usable)
    profiles = soap.get_profiles(usable, env.user, env.password)
    assert profiles, "按 rehost 之后的地址应当能正常打 GetProfiles"


# ---- A6 / A7：GetServices 的两种坑 -----------------------------------

def test_a6_media2_listed_before_media(env):
    """A6：两个命名空间都含 ``/media/``，只按服务名匹配的客户端会解析到 Media2。"""
    env.set_quirk("connect.media2_first")
    time.sleep(0.2)

    services = soap.get_services(env.device_url, env.user, env.password)
    order = [namespace for namespace, _ in services if "/media/wsdl" in namespace]
    assert len(order) >= 2, "该同时列出 Media 与 Media2：%s" % order
    assert "ver20" in order[0], "A6 开了但 Media2 没排在前面：%s" % order


def test_a7_no_get_services(env):
    """A7：老固件不实现 GetServices，客户端必须能回落到 GetCapabilities。"""
    env.set_quirk("connect.no_get_services")
    time.sleep(0.2)

    with pytest.raises(SoapFault):
        soap.get_services(env.device_url, env.user, env.password)

    # 回落这条路必须还通着。
    xaddrs = soap.capability_xaddrs(env.device_url, env.user, env.password)
    assert "media" in xaddrs


# ---- 场景：老固件 ----------------------------------------------------

@pytest.mark.parametrize("scenario_sim", ["legacy-firmware"], indirect=True)
def test_legacy_firmware_still_connectable(scenario_sim, require_onvif_zeep):
    """老固件场景：只剩 GetCapabilities 一条路，客户端仍然要连得上。"""
    from helpers import onvifclient

    camera = scenario_sim.first_camera()
    port = scenario_sim.http_port_of(camera)
    client = onvifclient.make_camera(scenario_sim.host, port)
    xaddrs = onvifclient.capability_xaddrs(client)
    assert "media" in xaddrs, "老固件也得能从 GetCapabilities 拿到 Media 地址"

    info = onvifclient.device_information(client)
    assert info["Manufacturer"], "厂商字段不能连着一起缺"
    assert not info["HardwareId"], "场景里配了缺 HardwareId"
