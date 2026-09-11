"""WS-Discovery：裸 Probe 与 wsdiscovery 2.1.2 两条路子。

- **裸 Probe**（``raw_probe``）看的是未经任何库过滤的原始应答。
  A2 / A3 / 回两次 / Scopes 缺 name 这些必须这么看 ——
  wsdiscovery 2.1.2 遇到缺 MetadataVersion 会把整包丢掉，
  用库根本区分不出「没回」和「回了但字段缺」。
- **wsdiscovery 库**（``search_services``）是参照客户端用的那一个，
  验证的是「最挑剔的客户端到底能不能发现这台相机」。
"""

import os
import re
import socket
import struct
import time
import uuid

MULTICAST_GROUP = "239.255.255.250"
MULTICAST_PORT = 3702

# 1.0 方言（参照客户端与 wsdiscovery 2.1.2 用的就是这套）
NS_D_2005 = "http://schemas.xmlsoap.org/ws/2005/04/discovery"
NS_A_2004 = "http://schemas.xmlsoap.org/ws/2004/08/addressing"
# OASIS 方言
NS_D_2009 = "http://docs.oasis-open.org/ws-dd/ns/discovery/2009/01"
NS_A_2005 = "http://www.w3.org/2005/08/addressing"

NVT_TYPE = "dn:NetworkVideoTransmitter"
NVT_NS = "http://www.onvif.org/ver10/network/wsdl"

PROBE_TEMPLATE = """<?xml version="1.0" encoding="UTF-8"?>
<e:Envelope xmlns:e="http://www.w3.org/2003/05/soap-envelope"
            xmlns:w="{ns_a}" xmlns:d="{ns_d}" xmlns:dn="{nvt_ns}">
  <e:Header>
    <w:MessageID>uuid:{message_id}</w:MessageID>
    <w:To e:mustUnderstand="true">urn:schemas-xmlsoap-org:ws:2005:04:discovery</w:To>
    <w:Action e:mustUnderstand="true">{ns_d}/Probe</w:Action>
  </e:Header>
  <e:Body>
    <d:Probe>
      <d:Types>{types}</d:Types>
    </d:Probe>
  </e:Body>
</e:Envelope>"""


def build_probe(message_id=None, dialect="2005", types=NVT_TYPE):
    message_id = message_id or str(uuid.uuid4())
    ns_d = NS_D_2005 if dialect == "2005" else NS_D_2009
    ns_a = NS_A_2004 if dialect == "2005" else NS_A_2005
    return message_id, PROBE_TEMPLATE.format(
        ns_d=ns_d, ns_a=ns_a, nvt_ns=NVT_NS, message_id=message_id,
        types=types).encode("utf-8")


def raw_probe(timeout=3.0, dialect="2005", types=NVT_TYPE, repeats=1,
              interface_address="0.0.0.0", message_id=None):
    """发 Probe，在 timeout 内收集**全部**原始应答（不去重）。

    ``repeats`` 模拟 wsdiscovery 的 4 次重发：同一个 MessageID 发 4 遍，
    客户端按 EPR 去重，我们这里则要看设备到底回了几条。
    """
    message_id, payload = build_probe(message_id, dialect, types)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 4)
    if interface_address != "0.0.0.0":
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF,
                        socket.inet_aton(interface_address))
    sock.bind((interface_address, 0))
    sock.settimeout(0.3)
    replies = []
    try:
        for index in range(max(1, repeats)):
            sock.sendto(payload, (MULTICAST_GROUP, MULTICAST_PORT))
            if index + 1 < repeats:
                time.sleep(0.05)
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                data, peer = sock.recvfrom(65536)
            except socket.timeout:
                continue
            except OSError:
                break
            text = data.decode("utf-8", "replace")
            if "ProbeMatch" not in text and "Hello" not in text:
                continue
            replies.append({"peer": peer, "text": text})
    finally:
        sock.close()
    return replies


# ---- 原始应答的轻量解析（刻意不用 XML 解析器）------------------------

def _values(text, tag):
    return [match.strip() for match in
            re.findall(r"<(?:\w+:)?%s[^>]*>(.*?)</(?:\w+:)?%s>" % (tag, tag),
                       text, re.S)]


def xaddrs_of(text):
    values = _values(text, "XAddrs")
    return values[0].split() if values else []


def epr_of(text):
    values = _values(text, "Address")
    return values[0] if values else None


def scopes_of(text):
    values = _values(text, "Scopes")
    return values[0].split() if values else []


def types_of(text):
    values = _values(text, "Types")
    return values[0].split() if values else []


def has_metadata_version(text):
    return bool(_values(text, "MetadataVersion"))


def dialect_of(text):
    """应答用的是哪套命名空间。"""
    if NS_D_2009 in text:
        return "2009"
    if NS_D_2005 in text:
        return "2005"
    return "unknown"


def scope_value(scopes, prefix):
    """从 scopes 里取 ``onvif://www.onvif.org/<prefix>/`` 后面那段。"""
    marker = "onvif://www.onvif.org/%s/" % prefix
    for scope in scopes:
        if scope.startswith(marker):
            return scope[len(marker):]
    return None


def match_texts(replies):
    return [reply["text"] for reply in replies]


def filter_by_port(replies, ports):
    """只留 XAddrs 指向给定端口的应答 —— 局域网里可能有真相机在回。"""
    wanted = {str(port) for port in ports}
    result = []
    for reply in replies:
        addresses = xaddrs_of(reply["text"])
        if any(re.search(r":(\d+)", address) and
               re.search(r":(\d+)", address).group(1) in wanted
               for address in addresses):
            result.append(reply)
    return result


# ---- wsdiscovery 2.1.2 -------------------------------------------------

def wsdiscovery_available():
    try:
        import wsdiscovery  # noqa: F401
    except Exception:       # noqa: BLE001
        return False
    return True


def search_services(timeout=4.0):
    """用参照客户端同款的 wsdiscovery 2.1.2 搜一遍。

    返回 ``[{"epr":..., "xaddrs": [...], "scopes": [...]}]``。
    库内部会发 4 次 Probe 并按 EPR 去重，所以同一台相机只应出现一次。
    """
    from wsdiscovery.discovery import ThreadedWSDiscovery
    from wsdiscovery import QName

    discovery = ThreadedWSDiscovery()
    discovery.start()
    try:
        services = discovery.searchServices(
            types=[QName(NVT_NS, "NetworkVideoTransmitter")], timeout=timeout)
        result = []
        for service in services:
            result.append({
                "epr": service.getEPR(),
                "xaddrs": list(service.getXAddrs()),
                "scopes": [str(scope.getValue()) for scope in service.getScopes()],
                "types": [str(qname) for qname in service.getTypes()],
            })
        return result
    finally:
        discovery.stop()


def skip_discovery_requested():
    """容器 / CI 里多播不通时用 ONVIFSIM_SKIP_DISCOVERY=1 跳过。"""
    return os.environ.get("ONVIFSIM_SKIP_DISCOVERY", "").lower() in ("1", "true", "yes")
