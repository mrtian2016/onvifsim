"""PullPoint 订阅全流程，走裸 SOAP。

为什么不用 onvif-zeep 的 ``create_pullpoint_service()``：
它只认 GetCapabilities 报出来的 events XAddr，而真机（TL-IPC652P-A4，quirk D1）
会把订阅地址换到另一个端口、还每次递增。要测这条就必须自己往
``SubscriptionReference/Address`` 里那个地址发 PullMessages。
"""

import re
from urllib.parse import urlparse

from . import soap

CONCRETE_SET = "http://www.onvif.org/ver10/tev/topicExpression/ConcreteSet"

ACTION_RENEW = "http://docs.oasis-open.org/wsn/bw-2/SubscriptionManager/RenewRequest"
ACTION_UNSUBSCRIBE = (
    "http://docs.oasis-open.org/wsn/bw-2/SubscriptionManager/UnsubscribeRequest")
ACTION_PULL = "http://www.onvif.org/ver10/events/wsdl/PullPointSubscription/PullMessagesRequest"


class Subscription:
    """一条 PullPoint 订阅。``address`` 是设备自报的地址，可能不可达（D2）。"""

    def __init__(self, address, current_time, termination_time, raw, root):
        self.address = address
        self.current_time = current_time
        self.termination_time = termination_time
        self.raw = raw
        self.root = root

    @property
    def port(self):
        parsed = urlparse(self.address)
        if parsed.port:
            return parsed.port
        return 443 if parsed.scheme == "https" else 80

    @property
    def host(self):
        return urlparse(self.address).hostname

    def reachable_address(self, host, port):
        """只接管 host:port、保留 path —— 和参照客户端的 rehost 一致。"""
        return soap.rehost(self.address, host, port)

    def __repr__(self):
        return "<Subscription %s>" % self.address


class Message:
    """一条通知。``data`` / ``source`` 是 SimpleItem 的 Name→Value。"""

    def __init__(self, topic, data, source, property_operation, utc_time, element):
        self.topic = topic
        self.data = data
        self.source = source
        self.property_operation = property_operation
        self.utc_time = utc_time
        self.element = element

    def __repr__(self):
        return "<Message %s %s>" % (self.topic, self.data)


def create(events_url, user, password, initial_termination="PT60S",
           topic_filter=None, **kwargs):
    """CreatePullPointSubscription。``topic_filter`` 形如 ``tns1:RuleEngine//.``。"""
    filter_xml = ""
    if topic_filter:
        filter_xml = ('<tev:Filter><wsnt:TopicExpression Dialect="%s">%s'
                      '</wsnt:TopicExpression></tev:Filter>'
                      % (CONCRETE_SET, topic_filter))
    body = ('<tev:CreatePullPointSubscription>%s'
            '<tev:InitialTerminationTime>%s</tev:InitialTerminationTime>'
            '</tev:CreatePullPointSubscription>' % (filter_xml, initial_termination))
    response = soap.post(events_url, body, user, password, **kwargs)
    root = soap.parse(response)
    # D6：有的固件把 Address 直接放在响应下面，不套 SubscriptionReference。
    # 这里的查找是命名空间与层级无关的，两种形态都能拿到。
    address = soap.text(soap.find(root, "Address"))
    if not address:
        raise AssertionError("CreatePullPointSubscription 响应里没有订阅地址：\n%s"
                             % response.text[:800])
    return Subscription(address,
                        soap.text(soap.find(root, "CurrentTime")),
                        soap.text(soap.find(root, "TerminationTime")),
                        response.text, root)


def pull(address, user, password, timeout="PT2S", limit=10, **kwargs):
    """PullMessages → Message 列表。零条是正常心跳（D10），不是错误。"""
    body = ('<tev:PullMessages><tev:Timeout>%s</tev:Timeout>'
            '<tev:MessageLimit>%d</tev:MessageLimit></tev:PullMessages>'
            % (timeout, limit))
    kwargs.setdefault("extra_headers",
                      soap.addressing_header(address, ACTION_PULL))
    root = soap.call(address, body, user, password, **kwargs)
    return parse_messages(root)


def parse_messages(root):
    messages = []
    for notification in soap.findall(root, "NotificationMessage"):
        topic = soap.text(soap.find(notification, "Topic")) or ""
        message = soap.find(notification, "Message")
        # <wsnt:Message><tt:Message UtcTime=...> 是两层同名元素，取里面那层。
        inner = None
        for candidate in soap.findall(notification, "Message"):
            if soap.attr(candidate, "UtcTime") is not None:
                inner = candidate
                break
        inner = inner if inner is not None else message
        data = {}
        source = {}
        data_node = soap.find(inner, "Data")
        source_node = soap.find(inner, "Source")
        for item in soap.findall(data_node, "SimpleItem"):
            data[soap.attr(item, "Name")] = soap.attr(item, "Value")
        for item in soap.findall(source_node, "SimpleItem"):
            source[soap.attr(item, "Name")] = soap.attr(item, "Value")
        messages.append(Message(topic, data, source,
                                soap.attr(inner, "PropertyOperation"),
                                soap.attr(inner, "UtcTime"), notification))
    return messages


def renew(address, user, password, termination="PT60S", **kwargs):
    body = ('<wsnt:Renew><wsnt:TerminationTime>%s</wsnt:TerminationTime>'
            '</wsnt:Renew>' % termination)
    kwargs.setdefault("extra_headers",
                      soap.addressing_header(address, ACTION_RENEW))
    return soap.call(address, body, user, password, **kwargs)


def unsubscribe(address, user, password, **kwargs):
    kwargs.setdefault("extra_headers",
                      soap.addressing_header(address, ACTION_UNSUBSCRIBE))
    return soap.call(address, '<wsnt:Unsubscribe/>', user, password, **kwargs)


def set_synchronization_point(address, user, password, **kwargs):
    return soap.call(address, '<tev:SetSynchronizationPoint/>', user, password, **kwargs)


def get_event_properties_raw(events_url, user, password, **kwargs):
    """GetEventProperties，返回原始 ``requests.Response``。

    D5 会让响应变成属性值不加引号的非法 XML，那时候根本解析不出树来，
    所以这里刻意不解析。
    """
    return soap.post(events_url, '<tev:GetEventProperties/>', user, password, **kwargs)


def topics_of(response_text):
    """从 GetEventProperties 的响应文本里粗略抠出 topic 路径。

    正常响应用 XML 解析；D5 那种非法 XML 就退化成正则扫元素名。
    """
    try:
        import xml.etree.ElementTree as ET
        root = ET.fromstring(response_text)
    except Exception:                              # noqa: BLE001 —— D5 的非法 XML
        return sorted(set(re.findall(r"<(?:\w+:)?(\w+)[^>]*topic=", response_text)))
    topic_set = soap.find(root, "TopicSet")
    if topic_set is None:
        return []
    found = []

    def walk(node, path):
        for child in node:
            name = soap.localname(child)
            new_path = path + [name]
            if soap.attr(child, "topic") in ("true", "1"):
                found.append("/".join(new_path))
            walk(child, new_path)

    walk(topic_set, [])
    return found
