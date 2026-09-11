"""REST 控制面客户端。

端点以 ``src/control/ControlApi.cpp`` 的实现为准（不是 plan.md 的表格）。
所有方法都在非 2xx 时抛 ``ControlError``，并把服务端返回的中文错误带上。
"""

import json

import requests


class ControlError(RuntimeError):
    def __init__(self, method, path, status, body):
        self.status = status
        self.body = body
        super().__init__("%s %s -> HTTP %s: %s" % (method, path, status, body))


class ControlClient:
    """``http://127.0.0.1:<port>`` 上的控制面。"""

    def __init__(self, base_url, token=None, timeout=10.0):
        self.base_url = base_url.rstrip("/")
        self.token = token
        self.timeout = timeout
        self.session = requests.Session()

    # ---- 底层 ----------------------------------------------------------

    def _headers(self):
        headers = {"Content-Type": "application/json"}
        if self.token:
            headers["Authorization"] = "Bearer " + self.token
        return headers

    def request(self, method, path, body=None, params=None, expect=(200, 201, 204)):
        url = self.base_url + path
        data = None if body is None else json.dumps(body).encode("utf-8")
        response = self.session.request(
            method, url, data=data, params=params,
            headers=self._headers(), timeout=self.timeout)
        if expect is not None and response.status_code not in expect:
            raise ControlError(method, path, response.status_code, response.text)
        return response

    def json(self, method, path, body=None, params=None, expect=(200, 201, 204)):
        response = self.request(method, path, body, params, expect)
        if not response.content:
            return None
        return response.json()

    def get(self, path, params=None):
        return self.json("GET", path, params=params)

    def post(self, path, body=None, params=None, expect=(200, 201, 204)):
        return self.json("POST", path, body=body, params=params, expect=expect)

    def patch(self, path, body=None, expect=(200,)):
        return self.json("PATCH", path, body=body, expect=expect)

    def delete(self, path, expect=(200, 204)):
        return self.json("DELETE", path, expect=expect)

    # ---- 相机 ----------------------------------------------------------

    def cameras(self):
        """GET /api/cameras —— 列表，每项含 status / xaddr / streamUris / quirks。"""
        return self.get("/api/cameras")

    def camera(self, camera_id):
        """GET /api/cameras/{id}"""
        return self.get("/api/cameras/%s" % camera_id)

    def add_camera(self, preset="generic", quirks=None):
        """POST /api/cameras —— 从预设创建，返回 201 的相机模型。"""
        body = {"preset": preset}
        if quirks:
            body["quirks"] = quirks
        return self.post("/api/cameras", body, expect=(201,))

    def remove_camera(self, camera_id):
        return self.delete("/api/cameras/%s" % camera_id)

    def patch_camera(self, camera_id, body):
        return self.patch("/api/cameras/%s" % camera_id, body)

    def set_quirks(self, camera_id, quirks):
        """PATCH /api/cameras/{id} 的 quirks 字段 —— **部分更新**。

        服务端走的是 ``Quirks::merge()``：只覆盖请求里提到的 quirk，
        没提到的原样保留。想关掉一条就显式写 ``false``。
        要「只开这一条、其余全关」用 :meth:`replace_quirks`。
        """
        result = self.patch_camera(camera_id, {"quirks": quirks})
        warnings = result.get("warnings") or []
        if warnings:
            raise AssertionError("设置 quirk 时服务端报了告警：%s" % warnings)
        return result

    def clear_quirks(self, camera_id):
        """把这台相机上所有开着的 quirk 显式关掉。

        PATCH 是合并语义，传 ``{}`` 什么也不会发生，
        所以要把当前生效的键一条条写成 ``false``。
        """
        current = self.camera(camera_id).get("quirks") or {}
        if not current:
            return None
        return self.patch_camera(
            camera_id, {"quirks": {key: False for key in current}})

    def replace_quirks(self, camera_id, quirks):
        """先全关再只开给定的这些 —— 测试里要的「干净起点」。"""
        self.clear_quirks(camera_id)
        if not quirks:
            return self.camera(camera_id)
        return self.set_quirks(camera_id, quirks)

    def set_enabled(self, camera_id, enabled):
        return self.patch_camera(camera_id, {"enabled": bool(enabled)})

    def go_offline(self, camera_id, seconds=10):
        """POST /api/cameras/{id}/offline?seconds=N"""
        return self.post("/api/cameras/%s/offline" % camera_id,
                         params={"seconds": int(seconds)})

    # ---- 事件 / PTZ / 会话 ----------------------------------------------

    def trigger_event(self, camera_id, kind=None, topic=None, state=True, duration=0):
        body = {"state": bool(state), "duration": int(duration)}
        if topic:
            body["topic"] = topic
        else:
            body["kind"] = kind or "Motion"
        return self.post("/api/cameras/%s/events" % camera_id, body)

    def ptz_status(self, camera_id):
        """GET /api/cameras/{id}/ptz —— 位置、移动状态、预置位列表。"""
        return self.get("/api/cameras/%s/ptz" % camera_id)

    def ptz(self, camera_id, action, **kwargs):
        body = {"action": action}
        body.update(kwargs)
        return self.post("/api/cameras/%s/ptz" % camera_id, body)

    def sessions(self, camera_id):
        """GET /api/cameras/{id}/sessions —— RTSP 会话 + 订阅 + HTTP 客户端数。"""
        return self.get("/api/cameras/%s/sessions" % camera_id)

    def talkback(self, camera_id):
        """GET /api/cameras/{id}/talkback —— 对讲接收统计。"""
        return self.get("/api/cameras/%s/talkback" % camera_id)

    # ---- 全局 ----------------------------------------------------------

    def quirks_meta(self):
        """GET /api/quirks —— 全部 quirk 的元数据。"""
        return self.get("/api/quirks")

    def quirk_keys(self):
        return [q["key"] for q in self.quirks_meta()]

    def presets(self):
        """GET /api/presets —— 全部品牌预设。"""
        return self.get("/api/presets")

    def network(self):
        return self.get("/api/network")

    def scenario(self):
        """GET /api/scenario —— 导出当前场景。"""
        return self.get("/api/scenario")

    def load_scenario(self, scenario=None, path=None):
        body = dict(scenario) if scenario else {}
        if path:
            body = {"path": path}
        return self.post("/api/scenario", body)

    def metrics(self):
        """GET /metrics —— Prometheus 文本。"""
        return self.request("GET", "/metrics").text

    def openapi(self):
        return self.get("/openapi.json")
