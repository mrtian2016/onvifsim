"""手写的最小 RTSP / RTP 客户端。

刻意不引 live555 / ffmpeg 之类 —— 对讲测试要精确控制发出去的字节
（marker 位、talkspurt 边界、包间隔），也要看到 SDP 与响应头的原文，
库反而挡路。几十行就够。

支持：OPTIONS / DESCRIBE（可带 Require: backchannel）/ SETUP（interleaved）/
PLAY / TEARDOWN，Basic 与 Digest 鉴权，以及 interleaved 通道上的 RTP 收发。
"""

import base64
import hashlib
import os
import re
import socket
import struct
import time

RTSP_VERSION = "RTSP/1.0"
BACKCHANNEL_REQUIRE = "www.onvif.org/ver20/backchannel"


class RtspError(RuntimeError):
    pass


class Response:
    def __init__(self, status, reason, headers, body):
        self.status = status
        self.reason = reason
        self.headers = headers          # 小写键 → 值（同名头取最后一条）
        self.header_list = []           # [(原始键, 值)]，双挑战 E8 要看全部
        self.body = body

    def header(self, name, default=None):
        return self.headers.get(name.lower(), default)

    def headers_named(self, name):
        lowered = name.lower()
        return [value for key, value in self.header_list if key.lower() == lowered]

    @property
    def sdp(self):
        return self.body.decode("utf-8", "replace")

    def __repr__(self):
        return "<RTSP %d %s>" % (self.status, self.reason)


def parse_sdp(text):
    """把 SDP 拆成 ``{"session": [(key, value)], "media": [{...}]}``。"""
    session = []
    media = []
    current = None
    for raw in text.splitlines():
        line = raw.strip()
        if not line or "=" not in line:
            continue
        key, value = line.split("=", 1)
        if key == "m":
            current = {"m": value, "attrs": [], "lines": []}
            media.append(current)
            continue
        target = current if current is not None else None
        if target is None:
            session.append((key, value))
        else:
            target["lines"].append((key, value))
            if key == "a":
                target["attrs"].append(value)
    return {"session": session, "media": media}


def sdp_attrs(entries):
    return [value for key, value in entries if key == "a"]


def media_control(entry):
    for attr in entry["attrs"]:
        if attr.startswith("control:"):
            return attr.split(":", 1)[1]
    return None


def session_control(parsed):
    for attr in sdp_attrs(parsed["session"]):
        if attr.startswith("control:"):
            return attr.split(":", 1)[1]
    return None


def is_backchannel(entry):
    return any(attr.strip() == "sendonly" for attr in entry["attrs"])


def rtp_packet(payload, sequence, timestamp, ssrc, payload_type=0, marker=False):
    """RFC 3550 的 12 字节固定头 + 载荷。"""
    first = 0x80                                   # V=2, P=0, X=0, CC=0
    second = (0x80 if marker else 0x00) | (payload_type & 0x7F)
    header = struct.pack("!BBHII", first, second, sequence & 0xFFFF,
                         timestamp & 0xFFFFFFFF, ssrc & 0xFFFFFFFF)
    return header + payload


def parse_rtp(packet):
    """拆 RTP 头 → dict。载荷长度不足就抛。"""
    if len(packet) < 12:
        raise ValueError("RTP 包不足 12 字节：%d" % len(packet))
    first, second, sequence, timestamp, ssrc = struct.unpack("!BBHII", packet[:12])
    csrc_count = first & 0x0F
    offset = 12 + 4 * csrc_count
    return {
        "version": first >> 6,
        "marker": bool(second & 0x80),
        "payload_type": second & 0x7F,
        "sequence": sequence,
        "timestamp": timestamp,
        "ssrc": ssrc,
        "payload": packet[offset:],
    }


def pcmu_silence(samples=160):
    """PCMU 的静音是 0xFF。一包 160 样点 = 20 ms @ 8 kHz。"""
    return b"\xff" * samples


class RtspClient:
    """一条 RTSP TCP 连接。

    用法::

        with RtspClient(host, port, "/profile1", "admin", "admin123") as client:
            client.options()
            response = client.describe(require_backchannel=True)
            client.setup(control_url, interleaved=(0, 1))
            client.play()
    """

    def __init__(self, host, port, path="/", user=None, password=None,
                 timeout=8.0, user_agent="onvifsim-e2e/1.0"):
        self.host = host
        self.port = int(port)
        self.path = path if path.startswith("/") else "/" + path
        self.user = user
        self.password = password
        self.timeout = timeout
        self.user_agent = user_agent

        self.socket = None
        self.buffer = b""
        self.cseq = 0
        self.session_id = None
        self.auth = None                # ("digest", {...}) / ("basic", realm)
        self.last_response = None
        self.pending_frames = []        # 响应中间夹进来的 interleaved 帧

    # ---- 连接 ----------------------------------------------------------

    @property
    def base_url(self):
        return "rtsp://%s:%d%s" % (self.host, self.port, self.path)

    def connect(self):
        self.socket = socket.create_connection((self.host, self.port), self.timeout)
        self.socket.settimeout(self.timeout)
        return self

    def close(self):
        if self.socket is not None:
            try:
                self.socket.close()
            finally:
                self.socket = None

    def __enter__(self):
        return self.connect()

    def __exit__(self, *exc):
        self.close()
        return False

    # ---- 鉴权 ----------------------------------------------------------

    @staticmethod
    def parse_challenges(response):
        """把所有 WWW-Authenticate 拆成 [(scheme, params)]。

        E8 会同时发 Basic 与 Digest 两条，所以必须收全，不能只取第一条。
        """
        challenges = []
        for value in response.headers_named("WWW-Authenticate"):
            value = value.strip()
            scheme, _, rest = value.partition(" ")
            params = dict(re.findall(r'(\w+)\s*=\s*"([^"]*)"', rest))
            params.update(dict(re.findall(r'(\w+)\s*=\s*([^",\s]+)', rest)))
            challenges.append((scheme.lower(), params))
        return challenges

    def authorization(self, method, url, extra_params=None):
        """按已缓存的挑战算 Authorization 头。

        ``extra_params`` 用来测 E9：多塞一个挑战没要求的参数（比如
        ``algorithm=MD5``）就该被 401 顶回来。
        """
        if not self.auth or not self.user:
            return None
        scheme, params = self.auth
        if scheme == "basic":
            token = base64.b64encode(
                ("%s:%s" % (self.user, self.password or "")).encode()).decode()
            return "Basic " + token
        realm = params.get("realm", "")
        nonce = params.get("nonce", "")
        ha1 = hashlib.md5(
            ("%s:%s:%s" % (self.user, realm, self.password or "")).encode()).hexdigest()
        ha2 = hashlib.md5(("%s:%s" % (method, url)).encode()).hexdigest()
        digest = hashlib.md5(("%s:%s:%s" % (ha1, nonce, ha2)).encode()).hexdigest()
        fields = {
            "username": self.user,
            "realm": realm,
            "nonce": nonce,
            "uri": url,
            "response": digest,
        }
        if extra_params:
            fields.update(extra_params)
        rendered = ", ".join('%s="%s"' % (key, value) for key, value in fields.items())
        return "Digest " + rendered

    def prefer_challenge(self, response, scheme=None):
        """从 401 里挑一条挑战缓存起来。默认优先 Digest。"""
        challenges = self.parse_challenges(response)
        if not challenges:
            return False
        if scheme:
            for candidate in challenges:
                if candidate[0] == scheme:
                    self.auth = candidate
                    return True
            return False
        for candidate in challenges:
            if candidate[0] == "digest":
                self.auth = candidate
                return True
        self.auth = challenges[0]
        return True

    # ---- 请求 ----------------------------------------------------------

    def request(self, method, url=None, headers=None, body=b"", retry_auth=True,
                auth_extra=None, auth_scheme=None):
        if self.socket is None:
            self.connect()
        url = url or self.base_url
        self.cseq += 1
        lines = ["%s %s %s" % (method, url, RTSP_VERSION),
                 "CSeq: %d" % self.cseq,
                 "User-Agent: %s" % self.user_agent]
        if self.session_id:
            lines.append("Session: %s" % self.session_id)
        authorization = self.authorization(method, url, auth_extra)
        if authorization:
            lines.append("Authorization: %s" % authorization)
        for key, value in (headers or {}).items():
            lines.append("%s: %s" % (key, value))
        if body:
            lines.append("Content-Length: %d" % len(body))
        payload = ("\r\n".join(lines) + "\r\n\r\n").encode("utf-8")
        if body:
            payload += body
        self.socket.sendall(payload)

        response = self.read_response()
        if (response.status == 401 and retry_auth and self.user
                and self.prefer_challenge(response, auth_scheme)):
            self.cseq += 1
            lines[1] = "CSeq: %d" % self.cseq
            authorization = self.authorization(method, url, auth_extra)
            retry = [line for line in lines if not line.startswith("Authorization:")]
            retry.append("Authorization: %s" % authorization)
            payload = ("\r\n".join(retry) + "\r\n\r\n").encode("utf-8")
            if body:
                payload += body
            self.socket.sendall(payload)
            response = self.read_response()
        self.last_response = response
        return response

    # ---- 收报文 --------------------------------------------------------

    def _fill(self, timeout=None):
        if timeout is not None:
            self.socket.settimeout(timeout)
        try:
            chunk = self.socket.recv(65536)
        finally:
            if timeout is not None:
                self.socket.settimeout(self.timeout)
        if not chunk:
            raise RtspError("对端关闭了连接")
        self.buffer += chunk

    def read_response(self, timeout=None):
        """读一条 RTSP 响应，途中遇到的 interleaved 帧存进 pending_frames。"""
        deadline = time.time() + (timeout or self.timeout)
        while True:
            if self.buffer.startswith(b"$"):
                frame = self._take_interleaved()
                if frame is None:
                    self._fill(max(0.05, deadline - time.time()))
                    continue
                self.pending_frames.append(frame)
                continue
            end = self.buffer.find(b"\r\n\r\n")
            if end < 0:
                if time.time() > deadline:
                    raise RtspError("等 RTSP 响应超时，已收到：%r" % self.buffer[:200])
                self._fill(max(0.05, deadline - time.time()))
                continue
            head = self.buffer[:end].decode("utf-8", "replace")
            rest = end + 4
            lines = head.split("\r\n")
            match = re.match(r"RTSP/1\.\d\s+(\d+)\s*(.*)", lines[0])
            if not match:
                raise RtspError("响应的状态行不合法：%r" % lines[0])
            status = int(match.group(1))
            reason = match.group(2)
            headers = {}
            header_list = []
            for line in lines[1:]:
                if ":" not in line:
                    continue
                key, value = line.split(":", 1)
                headers[key.strip().lower()] = value.strip()
                header_list.append((key.strip(), value.strip()))
            length = int(headers.get("content-length", "0") or 0)
            while len(self.buffer) - rest < length:
                if time.time() > deadline:
                    raise RtspError("等响应体超时（还差 %d 字节）"
                                    % (length - (len(self.buffer) - rest)))
                self._fill(max(0.05, deadline - time.time()))
            body = self.buffer[rest:rest + length]
            self.buffer = self.buffer[rest + length:]
            response = Response(status, reason, headers, body)
            response.header_list = header_list
            if "session" in headers and not self.session_id:
                self.session_id = headers["session"].split(";")[0].strip()
            return response

    def _take_interleaved(self):
        if len(self.buffer) < 4:
            return None
        channel = self.buffer[1]
        length = struct.unpack("!H", self.buffer[2:4])[0]
        if len(self.buffer) < 4 + length:
            return None
        payload = self.buffer[4:4 + length]
        self.buffer = self.buffer[4 + length:]
        return channel, payload

    def read_interleaved(self, timeout=2.0):
        """读一个 interleaved 帧，返回 ``(channel, payload)``；超时返回 None。"""
        if self.pending_frames:
            return self.pending_frames.pop(0)
        deadline = time.time() + timeout
        while time.time() < deadline:
            frame = self._take_interleaved()
            if frame is not None:
                return frame
            if self.buffer and not self.buffer.startswith(b"$"):
                # 服务端插了一条请求 / 响应（比如主动 TEARDOWN），吃掉它。
                end = self.buffer.find(b"\r\n\r\n")
                if end >= 0:
                    self.buffer = self.buffer[end + 4:]
                    continue
            try:
                self._fill(max(0.05, deadline - time.time()))
            except (socket.timeout, OSError):
                return None
        return None

    def send_interleaved(self, channel, payload):
        """往 interleaved 通道写一帧（对讲就是走这条）。"""
        self.socket.sendall(b"$" + bytes([channel])
                            + struct.pack("!H", len(payload)) + payload)

    # ---- 方法 ----------------------------------------------------------

    def options(self, **kwargs):
        return self.request("OPTIONS", **kwargs)

    def describe(self, require_backchannel=False, headers=None, **kwargs):
        merged = {"Accept": "application/sdp"}
        if require_backchannel:
            merged["Require"] = BACKCHANNEL_REQUIRE
        merged.update(headers or {})
        return self.request("DESCRIBE", headers=merged, **kwargs)

    def setup(self, control_url=None, interleaved=(0, 1), transport=None,
              headers=None, require_backchannel=False, **kwargs):
        url = control_url or (self.base_url + "/trackID=0")
        if not url.startswith("rtsp://"):
            url = self.base_url.rstrip("/") + "/" + url.lstrip("/")
        merged = {"Transport": transport or
                  ("RTP/AVP/TCP;unicast;interleaved=%d-%d" % interleaved)}
        if require_backchannel:
            merged["Require"] = BACKCHANNEL_REQUIRE
        merged.update(headers or {})
        return self.request("SETUP", url=url, headers=merged, **kwargs)

    def play(self, headers=None, require_backchannel=False, **kwargs):
        merged = {"Range": "npt=0.000-"}
        if require_backchannel:
            merged["Require"] = BACKCHANNEL_REQUIRE
        merged.update(headers or {})
        return self.request("PLAY", headers=merged, **kwargs)

    def teardown(self, **kwargs):
        response = self.request("TEARDOWN", **kwargs)
        self.session_id = None
        return response


class Talker:
    """往 backchannel 上按 20 ms 一包推 PCMU 的小工具。

    ``marker_on_first`` 关掉就是 E13 复现的场景：talkspurt 首包 marker=0，
    严格校验 marker 的相机会把整个 talkburst 丢掉。
    """

    def __init__(self, client, channel=0, payload_type=0, ssrc=None,
                 samples_per_packet=160):
        self.client = client
        self.channel = channel
        self.payload_type = payload_type
        self.ssrc = ssrc if ssrc is not None else int.from_bytes(os.urandom(4), "big")
        self.samples = samples_per_packet
        self.sequence = 1
        self.timestamp = 0
        self.bytes_sent = 0
        self.packets_sent = 0

    def burst(self, packets=25, marker_on_first=True, pace=True, payload=None):
        for index in range(packets):
            data = payload if payload is not None else pcmu_silence(self.samples)
            packet = rtp_packet(data, self.sequence, self.timestamp, self.ssrc,
                                self.payload_type,
                                marker=bool(marker_on_first and index == 0))
            self.client.send_interleaved(self.channel, packet)
            self.sequence += 1
            self.timestamp += self.samples
            self.bytes_sent += len(packet)
            self.packets_sent += 1
            if pace:
                time.sleep(0.02)
        return self.packets_sent
