"""裸 SOAP 客户端。

onvif-zeep 是主力（它按官方 WSDL 严格解析，正好当 schema 校验器），
但有些 quirk 就是要吐 onvif-zeep 解析不了的东西 ——
非法 XML（D5）、裸字符串的 SetPreset 返回值（C9）、扁平的 Address（D6）……
那些必须用这个模块看原始报文。

同时它也用来做 onvif-zeep 不方便做的事：
往指定地址（而不是 GetCapabilities 报的地址）发 PullMessages、
故意重放同一个 nonce、故意不带 WS-Security 头。
"""

import base64
import hashlib
import os
import re
import xml.etree.ElementTree as ET
from datetime import datetime, timedelta, timezone

import requests

# ---- 命名空间 ----------------------------------------------------------

NS = {
    "s12": "http://www.w3.org/2003/05/soap-envelope",
    "s11": "http://schemas.xmlsoap.org/soap/envelope/",
    "wsse": "http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd",
    "wsu": "http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd",
    "wsa": "http://www.w3.org/2005/08/addressing",
    "wsnt": "http://docs.oasis-open.org/wsn/b-2",
    "wstop": "http://docs.oasis-open.org/wsn/t-1",
    "tds": "http://www.onvif.org/ver10/device/wsdl",
    "trt": "http://www.onvif.org/ver10/media/wsdl",
    "tr2": "http://www.onvif.org/ver20/media/wsdl",
    "tptz": "http://www.onvif.org/ver20/ptz/wsdl",
    "tev": "http://www.onvif.org/ver10/events/wsdl",
    "timg": "http://www.onvif.org/ver20/imaging/wsdl",
    "tan": "http://www.onvif.org/ver20/analytics/wsdl",
    "tmd": "http://www.onvif.org/ver10/deviceIO/wsdl",
    "tt": "http://www.onvif.org/ver10/schema",
}

_NS_DECL = " ".join('xmlns:%s="%s"' % (prefix, uri) for prefix, uri in NS.items()
                    if prefix not in ("s11",))

PASSWORD_TYPE_DIGEST = (
    "http://docs.oasis-open.org/wss/2004/01/"
    "oasis-200401-wss-username-token-profile-1.0#PasswordDigest")
PASSWORD_TYPE_TEXT = (
    "http://docs.oasis-open.org/wss/2004/01/"
    "oasis-200401-wss-username-token-profile-1.0#PasswordText")
NONCE_ENCODING = (
    "http://docs.oasis-open.org/wss/2004/01/"
    "oasis-200401-wss-soap-message-security-1.0#Base64Binary")


class SoapFault(Exception):
    """SOAP Fault。``http_status`` 保留下来是因为 quirk D7 专门在改它。"""

    def __init__(self, http_status, code, subcode, reason, raw):
        self.http_status = http_status
        self.code = code
        self.subcode = subcode
        self.reason = reason
        self.raw = raw
        super().__init__("HTTP %s / %s / %s：%s" % (http_status, code, subcode, reason))


# ---- 报文构造 ----------------------------------------------------------

def utc_now(skew_seconds=0):
    return datetime.now(timezone.utc) + timedelta(seconds=skew_seconds)


def iso(moment):
    return moment.strftime("%Y-%m-%dT%H:%M:%SZ")


def password_digest(nonce, created, password):
    """base64(sha1(nonce + created + password))"""
    sha = hashlib.sha1()
    sha.update(nonce)
    sha.update(created.encode("utf-8"))
    sha.update(password.encode("utf-8"))
    return base64.b64encode(sha.digest()).decode("ascii")


def security_header(user, password, created=None, nonce=None, digest=True):
    """WS-Security UsernameToken。

    ``nonce`` 显式传进来是为了测 ``auth.nonce_strict_once``（重放同一个 nonce）。
    """
    if created is None:
        created = iso(utc_now())
    if nonce is None:
        nonce = os.urandom(16)
    if digest:
        secret = password_digest(nonce, created, password)
        password_type = PASSWORD_TYPE_DIGEST
    else:
        secret = password
        password_type = PASSWORD_TYPE_TEXT
    return (
        '<wsse:Security s12:mustUnderstand="true">'
        '<wsse:UsernameToken>'
        '<wsse:Username>%s</wsse:Username>'
        '<wsse:Password Type="%s">%s</wsse:Password>'
        '<wsse:Nonce EncodingType="%s">%s</wsse:Nonce>'
        '<wsu:Created>%s</wsu:Created>'
        '</wsse:UsernameToken></wsse:Security>'
        % (user, password_type, secret, NONCE_ENCODING,
           base64.b64encode(nonce).decode("ascii"), created))


def envelope(body_xml, user=None, password=None, extra_headers="", **token_kwargs):
    """拼一个 SOAP 1.2 信封。给了 user 才带 WS-Security 头。"""
    header = extra_headers
    if user is not None:
        header += security_header(user, password or "", **token_kwargs)
    header_xml = "<s12:Header>%s</s12:Header>" % header if header else ""
    return ('<?xml version="1.0" encoding="UTF-8"?>'
            '<s12:Envelope %s>%s<s12:Body>%s</s12:Body></s12:Envelope>'
            % (_NS_DECL, header_xml, body_xml)).encode("utf-8")


def addressing_header(to, action, message_id=None):
    """WS-Addressing 头。PullPoint 的 Renew / Unsubscribe 要用。"""
    message_id = message_id or ("urn:uuid:%s" % os.urandom(16).hex())
    return ('<wsa:To s12:mustUnderstand="true">%s</wsa:To>'
            '<wsa:Action s12:mustUnderstand="true">%s</wsa:Action>'
            '<wsa:MessageID>%s</wsa:MessageID>'
            '<wsa:ReplyTo><wsa:Address>'
            'http://www.w3.org/2005/08/addressing/anonymous'
            '</wsa:Address></wsa:ReplyTo>' % (to, action, message_id))


# ---- 发送 --------------------------------------------------------------

def post(url, body_xml, user=None, password=None, timeout=15.0, action=None,
         extra_headers="", session=None, **token_kwargs):
    """发一个 SOAP 请求，返回原始的 ``requests.Response``（不解析、不抛 Fault）。"""
    data = envelope(body_xml, user, password, extra_headers, **token_kwargs)
    content_type = 'application/soap+xml; charset=utf-8'
    if action:
        content_type += '; action="%s"' % action
    caller = session or requests
    return caller.post(url, data=data, timeout=timeout,
                       headers={"Content-Type": content_type})


def parse(response):
    """把响应体解析成 ElementTree；Fault 转成 ``SoapFault`` 抛出。"""
    try:
        root = ET.fromstring(response.content)
    except ET.ParseError as exc:
        raise ValueError("响应不是合法 XML（HTTP %s）：%s\n%s"
                         % (response.status_code, exc,
                            response.text[:600])) from exc
    fault = find(root, "Fault")
    if fault is not None:
        # SOAP 1.2 是 Code/Value + Subcode/Value + Reason/Text；
        # 1.1 是扁平的 faultcode + faultstring。两种都要认 ——
        # 真机什么版本都有，quirk D8 还专门在措辞上做文章。
        code = text(find(fault, "Value")) or text(find(fault, "faultcode")) or ""
        subcode_node = find(fault, "Subcode")
        subcode = text(find(subcode_node, "Value")) if subcode_node is not None else ""
        reason = text(find(fault, "Text")) or text(find(fault, "faultstring")) or ""
        raise SoapFault(response.status_code, code, subcode or "", reason or "",
                        response.text)
    return root


def call(url, body_xml, user=None, password=None, **kwargs):
    """发请求 + 解析 + Fault 抛异常，一步到位。"""
    return parse(post(url, body_xml, user, password, **kwargs))


# ---- 命名空间无关的查找 ------------------------------------------------

def localname(element):
    tag = element.tag
    return tag.rsplit("}", 1)[-1] if "}" in tag else tag


def iter_all(root):
    yield root
    for child in root:
        for node in iter_all(child):
            yield node


def find(root, name):
    """按 localName 深度优先找第一个（不管命名空间）。"""
    if root is None:
        return None
    for node in iter_all(root):
        if localname(node) == name:
            return node
    return None


def findall(root, name):
    if root is None:
        return []
    return [node for node in iter_all(root) if localname(node) == name]


def text(element, default=None):
    if element is None or element.text is None:
        return default
    return element.text.strip()


def attr(element, name, default=None):
    """按 localName 取属性（属性可能带命名空间前缀）。"""
    if element is None:
        return default
    for key, value in element.attrib.items():
        if key.rsplit("}", 1)[-1] == name:
            return value
    return default


def rehost(xaddr, host, port):
    """只接管 host:port、保留 path —— 参照客户端就是这么干的。

    quirk A5（XAddr 报 :2020）能被客户端扛过去，靠的正是这一步。
    """
    match = re.match(r"^(\w+)://([^/]+)(/.*)?$", xaddr)
    if not match:
        return xaddr
    scheme, _authority, path = match.groups()
    return "%s://%s:%d%s" % (scheme, host, port, path or "")


# ---- 常用调用 ----------------------------------------------------------

def get_capabilities(url, user, password, category="All", **kwargs):
    """GetCapabilities(Category=All) —— 参照客户端唯一拿 XAddr 的路子。"""
    body = ('<tds:GetCapabilities><tds:Category>%s</tds:Category>'
            '</tds:GetCapabilities>' % category)
    return call(url, body, user, password, **kwargs)


def capability_xaddrs(url, user, password, host=None, port=None, **kwargs):
    """GetCapabilities 拿到的各服务 XAddr，键是小写服务名。

    传了 host/port 就顺手 rehost 一遍，直接可连。
    """
    root = get_capabilities(url, user, password, **kwargs)
    capabilities = find(root, "Capabilities")
    result = {}
    if capabilities is None:
        return result
    for child in capabilities:
        xaddr = find(child, "XAddr")
        if xaddr is None or not text(xaddr):
            continue
        value = text(xaddr)
        if host is not None and port is not None:
            value = rehost(value, host, port)
        result[localname(child).lower()] = value
    return result


def get_services(url, user, password, include_capability=False, **kwargs):
    """GetServices → [(namespace, xaddr)]，**保持响应里的顺序**。

    顺序是 A6（Media2 排在 Media 前）的关键。
    """
    body = ('<tds:GetServices><tds:IncludeCapability>%s</tds:IncludeCapability>'
            '</tds:GetServices>' % ("true" if include_capability else "false"))
    root = call(url, body, user, password, **kwargs)
    services = []
    for service in findall(root, "Service"):
        services.append((text(find(service, "Namespace")),
                         text(find(service, "XAddr"))))
    return services


def get_device_information(url, user, password, **kwargs):
    root = call(url, '<tds:GetDeviceInformation/>', user, password, **kwargs)
    fields = ("Manufacturer", "Model", "FirmwareVersion", "SerialNumber", "HardwareId")
    return {name: text(find(root, name)) for name in fields}


def get_profiles(media_url, user, password, **kwargs):
    """Media(ver10) GetProfiles → 原始的 Profiles 节点列表。"""
    root = call(media_url, '<trt:GetProfiles/>', user, password, **kwargs)
    return findall(root, "Profiles")


def get_stream_uri(media_url, user, password, profile_token,
                   protocol="RTSP", stream="RTP-Unicast", **kwargs):
    body = ('<trt:GetStreamUri><trt:StreamSetup>'
            '<tt:Stream>%s</tt:Stream><tt:Transport><tt:Protocol>%s</tt:Protocol>'
            '</tt:Transport></trt:StreamSetup>'
            '<trt:ProfileToken>%s</trt:ProfileToken></trt:GetStreamUri>'
            % (stream, protocol, profile_token))
    return call(media_url, body, user, password, **kwargs)


def get_snapshot_uri(media_url, user, password, profile_token, **kwargs):
    body = ('<trt:GetSnapshotUri><trt:ProfileToken>%s</trt:ProfileToken>'
            '</trt:GetSnapshotUri>' % profile_token)
    return call(media_url, body, user, password, **kwargs)
