"""onvif-zeep 0.2.12 的薄封装。

这是参照客户端用的那个库。它按官方 WSDL **严格**解析响应，
少一个必填字段、类型对不上就直接抛异常 —— 所以它同时是
「最挑剔的客户端」和「手写 XML 的 schema 校验器」。

构造 ``ONVIFCamera`` 本身就会打一次 ``GetCapabilities(Category=All)``，
所以只要能构造出来，建连这条路就是通的。
"""

import inspect
import os

try:
    import onvif
    from onvif import ONVIFCamera
    ONVIF_AVAILABLE = True
except Exception:                       # noqa: BLE001
    onvif = None
    ONVIFCamera = None
    ONVIF_AVAILABLE = False

try:
    from onvif import ONVIFError
except Exception:                       # noqa: BLE001
    ONVIFError = Exception


def wsdl_dir():
    """找 onvif-zeep 自带的 WSDL 目录。不同发行版装的位置不一样。"""
    if onvif is None:
        return None
    package = os.path.dirname(os.path.abspath(onvif.__file__))
    candidates = [
        os.path.join(package, "wsdl"),
        os.path.join(os.path.dirname(package), "wsdl"),
        "/etc/onvif/wsdl",
        os.path.join(os.path.dirname(os.path.dirname(package)), "wsdl"),
    ]
    for candidate in candidates:
        if os.path.isfile(os.path.join(candidate, "devicemgmt.wsdl")):
            return candidate
    return None


def make_camera(host, port, user="admin", password="admin123", **kwargs):
    """构造 ONVIFCamera。

    只传这个版本真有的关键字参数 —— onvif-zeep 各版本签名不一致，
    盲传会 TypeError，那就把 quirk 的失败和环境问题混在一起了。
    """
    if not ONVIF_AVAILABLE:
        raise RuntimeError("没装 onvif-zeep，先按 tests/e2e/README.md 建 venv")
    accepted = set(inspect.signature(ONVIFCamera.__init__).parameters)
    options = {}
    directory = wsdl_dir()
    if directory and "wsdl_dir" in accepted:
        options["wsdl_dir"] = directory
    # 参照客户端从不做时钟补偿（adjust_time 从不传 True），这里保持一致：
    # quirk A8 靠的就是这个默认值才复现得出「时钟偏了全线 401」。
    for key, value in kwargs.items():
        if key in accepted:
            options[key] = value
    return ONVIFCamera(host, int(port), user, password, **options)


def device_information(camera):
    """GetDeviceInformation → dict。缺字段（A10）会是 None，不是抛异常。"""
    info = camera.devicemgmt.GetDeviceInformation()
    return {
        "Manufacturer": getattr(info, "Manufacturer", None),
        "Model": getattr(info, "Model", None),
        "FirmwareVersion": getattr(info, "FirmwareVersion", None),
        "SerialNumber": getattr(info, "SerialNumber", None),
        "HardwareId": getattr(info, "HardwareId", None),
    }


def capability_xaddrs(camera):
    """构造时缓存下来的 capabilities → {服务名小写: XAddr}。"""
    capabilities = camera.devicemgmt.GetCapabilities({"Category": "All"})
    result = {}
    for name in ("Analytics", "Device", "Events", "Imaging", "Media", "PTZ", "Extension"):
        section = getattr(capabilities, name, None)
        xaddr = getattr(section, "XAddr", None) if section is not None else None
        if xaddr:
            result[name.lower()] = xaddr
    extension = getattr(capabilities, "Extension", None)
    if extension is not None:
        for name in ("DeviceIO", "Recording", "Replay", "Search"):
            section = getattr(extension, name, None)
            xaddr = getattr(section, "XAddr", None) if section is not None else None
            if xaddr:
                result[name.lower()] = xaddr
    return result


def stream_uri(camera, profile_token, protocol="RTSP", stream="RTP-Unicast"):
    media = camera.create_media_service()
    request = media.create_type("GetStreamUri")
    request.ProfileToken = profile_token
    request.StreamSetup = {"Stream": stream, "Transport": {"Protocol": protocol}}
    return media.GetStreamUri(request)


def snapshot_uri(camera, profile_token):
    media = camera.create_media_service()
    request = media.create_type("GetSnapshotUri")
    request.ProfileToken = profile_token
    return media.GetSnapshotUri(request)


def main_profile(profiles):
    """参照客户端的主 / 子码流判定：**只看 Name 字串**，判不出就按顺序取第一个。

    quirk B1 换掉命名风格后，这个函数就会和真客户端一样判错 —— 那正是重点。
    """
    main_words = ("main", "primary", "high")
    sub_words = ("sub", "secondary", "low")
    for profile in profiles:
        name = (getattr(profile, "Name", "") or "").lower()
        if any(word in name for word in main_words):
            return profile
    for profile in profiles:
        name = (getattr(profile, "Name", "") or "").lower()
        if not any(word in name for word in sub_words):
            return profile
    return profiles[0] if profiles else None
