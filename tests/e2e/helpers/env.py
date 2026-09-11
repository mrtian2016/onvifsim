"""把「一台相机 + 一个跑着的模拟器」包成一个顺手的对象。

测试里反复要做的几件事都在这儿：
拿可连的服务地址（而不是被 quirk 改过的对外宣称地址）、
开关 quirk、建 onvif-zeep 连接、开 RTSP 会话。
"""

from urllib.parse import urlparse

from . import onvifclient, rtsp, soap

DEFAULT_USER = "admin"
DEFAULT_PASSWORD = "admin123"

# GetCapabilities 里没报出来的服务，按 generic 预设的路径模板兜底。
FALLBACK_PATHS = {
    "device": "/onvif/device_service",
    "media": "/onvif/media_service",
    "media2": "/onvif/media2_service",
    "ptz": "/onvif/ptz_service",
    "events": "/onvif/events_service",
    "imaging": "/onvif/imaging_service",
    "analytics": "/onvif/analytics_service",
    "deviceio": "/onvif/deviceio_service",
}


class CameraEnv:
    """一台相机的测试门面。"""

    def __init__(self, sim, camera_id=None, user=DEFAULT_USER, password=DEFAULT_PASSWORD):
        self.sim = sim
        self.control = sim.control
        self.user = user
        self.password = password
        snapshot = sim.first_camera() if camera_id is None else sim.control.camera(camera_id)
        self.camera_id = snapshot["id"]
        self._cached_xaddrs = None

    # ---- 基础信息 ------------------------------------------------------

    @property
    def camera(self):
        """每次都重新拉，避免拿到改 quirk 之前的旧快照。"""
        return self.control.camera(self.camera_id)

    @property
    def listing(self):
        """/api/cameras 里这台相机那一项（比单条 GET 多 status / streamUris）。"""
        for item in self.control.cameras():
            if item["id"] == self.camera_id:
                return item
        raise AssertionError("相机 %s 不在列表里了" % self.camera_id)

    @property
    def host(self):
        return self.sim.host

    @property
    def http_port(self):
        return int(self.camera["network"]["httpPort"])

    @property
    def rtsp_port(self):
        return int(self.camera["network"]["rtspPort"])

    @property
    def device_url(self):
        return "http://%s:%d/onvif/device_service" % (self.host, self.http_port)

    # ---- 服务地址 ------------------------------------------------------

    def xaddrs(self, refresh=False):
        """GetCapabilities(Category=All) 报出来的服务地址，已 rehost 成可连的。"""
        if self._cached_xaddrs is None or refresh:
            self._cached_xaddrs = soap.capability_xaddrs(
                self.device_url, self.user, self.password,
                host=self.host, port=self.http_port)
        return self._cached_xaddrs

    def service_url(self, name, refresh=False):
        name = name.lower()
        xaddrs = self.xaddrs(refresh)
        if name in xaddrs:
            return xaddrs[name]
        path = FALLBACK_PATHS.get(name)
        if not path:
            raise KeyError("不知道服务 %s 的地址" % name)
        return "http://%s:%d%s" % (self.host, self.http_port, path)

    @property
    def media_url(self):
        return self.service_url("media")

    @property
    def ptz_url(self):
        return self.service_url("ptz")

    @property
    def events_url(self):
        return self.service_url("events")

    @property
    def imaging_url(self):
        return self.service_url("imaging")

    # ---- quirk ---------------------------------------------------------

    def set_quirk(self, key, **params):
        """只开这一条 quirk，其余全关。

        REST 的 PATCH 是合并语义，所以这里先把当前开着的显式关掉再设 ——
        测试要的是一个干净起点，不然上一条用例的残留会串味。
        """
        value = {"enabled": True, "params": params} if params else True
        return self.control.replace_quirks(self.camera_id, {key: value})

    def set_quirks(self, mapping):
        """只开给定的这些 quirk，其余全关。"""
        return self.control.replace_quirks(self.camera_id, mapping)

    def patch_quirks(self, mapping):
        """按 REST 的原生语义做部分更新：只动提到的那些。"""
        return self.control.set_quirks(self.camera_id, mapping)

    def clear_quirks(self):
        self._cached_xaddrs = None
        return self.control.clear_quirks(self.camera_id)

    # ---- 客户端 --------------------------------------------------------

    def onvif(self, **kwargs):
        """建一个 onvif-zeep 连接（构造时就会打 GetCapabilities）。"""
        return onvifclient.make_camera(self.host, self.http_port,
                                       self.user, self.password, **kwargs)

    def soap_profiles(self):
        return soap.get_profiles(self.media_url, self.user, self.password)

    def profile_tokens(self):
        # profile 的 token 是**属性**（<trt:Profiles token="Profile_1">），不是子元素。
        return [soap.attr(profile, "token") for profile in self.soap_profiles()]

    def main_profile_token(self):
        model = self.camera
        profiles = model.get("profiles") or []
        if profiles:
            return profiles[0]["token"]
        tokens = self.profile_tokens()
        return tokens[0] if tokens else None

    # ---- RTSP ----------------------------------------------------------

    def stream_paths(self):
        """从 /api/cameras 的 streamUris 里取出 path 部分。"""
        paths = []
        for uri in self.listing.get("streamUris") or []:
            parsed = urlparse(uri)
            path = parsed.path or "/"
            if parsed.query:
                path += "?" + parsed.query
            paths.append(path)
        return paths

    def stream_path(self, index=0):
        paths = self.stream_paths()
        if not paths:
            return "/profile1"
        return paths[min(index, len(paths) - 1)]

    def stream_url(self, index=0, with_credentials=True):
        path = self.stream_path(index)
        if with_credentials:
            return "rtsp://%s:%s@%s:%d%s" % (self.user, self.password, self.host,
                                             self.rtsp_port, path)
        return "rtsp://%s:%d%s" % (self.host, self.rtsp_port, path)

    def rtsp(self, index=0, path=None, **kwargs):
        """开一条 RTSP 连接（调用方负责 close，或者用 with）。"""
        kwargs.setdefault("user", self.user)
        kwargs.setdefault("password", self.password)
        client = rtsp.RtspClient(self.host, self.rtsp_port,
                                 path or self.stream_path(index), **kwargs)
        return client.connect()

    # ---- 快照 ----------------------------------------------------------

    def snapshot_url(self, profile_token=None):
        token = profile_token or self.main_profile_token()
        root = soap.get_snapshot_uri(self.media_url, self.user, self.password, token)
        uri = soap.text(soap.find(root, "Uri"))
        if not uri:
            raise AssertionError("GetSnapshotUri 没返回 Uri")
        return soap.rehost(uri, self.host, self.http_port)
