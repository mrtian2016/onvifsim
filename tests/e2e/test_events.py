"""事件：PullPoint 全流程，含订阅换端口（D1）与槽位上限（D3）。

参照客户端的行为很有特点：每次建连都建一条 PullPoint 订阅、
**从不 Unsubscribe**，靠长轮询拉消息，零条返回当心跳。
订阅槽位泄漏是真实高发故障，所以这里的用例都围绕订阅生命周期打转。
"""

import time
from urllib.parse import urlparse

import pytest

from helpers import pullpoint, soap
from helpers.soap import SoapFault

SCENARIO = "single-camera"


def _create(env, **kwargs):
    return pullpoint.create(env.events_url, env.user, env.password, **kwargs)


def _pull(env, subscription, timeout="PT2S", limit=20):
    address = subscription.reachable_address(env.host, env.http_port)
    return pullpoint.pull(address, env.user, env.password, timeout=timeout, limit=limit)


def _pull_until(env, subscription, predicate, attempts=6, timeout="PT1S"):
    """长轮询拿零条是正常心跳（D10），所以要多拉几轮才能下结论。"""
    collected = []
    for _ in range(attempts):
        collected.extend(_pull(env, subscription, timeout=timeout))
        if predicate(collected):
            return collected
    return collected


# ---- 全流程 -----------------------------------------------------------

def test_pullpoint_full_cycle(env):
    """CreatePullPointSubscription → PullMessages → Renew → Unsubscribe。"""
    subscription = _create(env, initial_termination="PT60S")
    assert subscription.address.startswith("http"), subscription.address
    assert subscription.current_time, "响应里缺 CurrentTime"
    assert subscription.termination_time, "响应里缺 TerminationTime"

    sessions = env.control.sessions(env.camera_id)
    assert sessions["subscriptions"], "REST 里看不到刚建的订阅"

    address = subscription.reachable_address(env.host, env.http_port)
    # 空拉一次：零条是正常心跳，不该报错。
    assert pullpoint.pull(address, env.user, env.password, timeout="PT1S") == []

    pullpoint.renew(address, env.user, env.password, termination="PT120S")
    pullpoint.unsubscribe(address, env.user, env.password)

    time.sleep(0.4)
    after = env.control.sessions(env.camera_id)
    assert len(after["subscriptions"]) < len(sessions["subscriptions"]) or \
        not after["subscriptions"], "Unsubscribe 之后订阅还在"


def test_triggered_event_is_pulled(env):
    """通过 REST 触发一次运动，订阅方要能拉到对应的通知。"""
    subscription = _create(env)
    env.control.trigger_event(env.camera_id, kind="Motion", duration=1)

    messages = _pull_until(env, subscription, lambda items: len(items) > 0)
    assert messages, "触发了事件却一条都没拉到"

    topics = [message.topic for message in messages]
    assert any("Motion" in topic for topic in topics), \
        "拉到的 topic 里没有运动事件：%s" % topics

    motion = [message for message in messages if "Motion" in message.topic][0]
    assert motion.data, "通知里没有 Data/SimpleItem"


def test_property_event_sends_paired_state(env):
    """属性型事件要成对发：先 true，时长到了再补一条 false（D11 的对照组）。"""
    subscription = _create(env)
    env.control.trigger_event(env.camera_id, kind="Motion", duration=1)

    messages = _pull_until(
        env, subscription,
        lambda items: len({tuple(sorted(m.data.items())) for m in items}) >= 2,
        attempts=8)
    values = [value for message in messages for value in message.data.values()]
    assert "true" in [str(value).lower() for value in values], "没收到 state=true"
    assert "false" in [str(value).lower() for value in values], \
        "属性型事件该补一条 false 收尾，实际拿到：%s" % values


def test_get_event_properties_lists_topics(env):
    """GetEventProperties 要列出 topic 集，客户端靠它建过滤器。"""
    response = pullpoint.get_event_properties_raw(env.events_url, env.user, env.password)
    assert response.status_code == 200, response.status_code
    topics = pullpoint.topics_of(response.text)
    assert topics, "TopicSet 里一个 topic 都没有"
    assert any("Motion" in topic for topic in topics), topics


def test_topic_filter_narrows_results(env):
    """带 TopicExpression 的订阅只该收到匹配的 topic。"""
    subscription = _create(env, topic_filter="tns1:RuleEngine//.")
    env.control.trigger_event(env.camera_id, kind="Motion", duration=1)
    messages = _pull_until(env, subscription, lambda items: len(items) > 0)
    for message in messages:
        assert "RuleEngine" in message.topic, \
            "过滤器只要 RuleEngine，却收到了 %s" % message.topic


def test_set_synchronization_point(env):
    """SetSynchronizationPoint 让设备补发一遍当前状态。"""
    subscription = _create(env)
    address = subscription.reachable_address(env.host, env.http_port)
    pullpoint.set_synchronization_point(address, env.user, env.password)
    # 补发的内容不强求，能正常返回就说明这条路通了。


# ---- D1：订阅换端口 ---------------------------------------------------

def test_d1_subscription_port_increments(env):
    """D1：TL-IPC652P-A4 把订阅管理器开在独立端口，而且每建一条就递增。

    客户端不能想当然认为订阅地址和设备服务在同一个端口。
    """
    base_port = 21024
    env.set_quirk("events.subscription_port_increment", base_port=base_port)
    time.sleep(0.3)

    first = _create(env)
    second = _create(env)

    assert first.port != env.http_port, \
        "D1 开了但订阅地址还在设备端口 %d 上" % env.http_port
    assert second.port == first.port + 1, \
        "端口没有递增：%d -> %d" % (first.port, second.port)

    # 换了端口也得真的能拉 —— 端口是新开的监听，不是随口报一个。
    messages = pullpoint.pull(first.address, env.user, env.password, timeout="PT1S")
    assert messages == [] or messages, "换端口后的订阅地址不可用"


def test_d2_subscription_host_unreachable(env):
    """D2：订阅地址的 host 是设备自报的内网地址，客户端必须 rehost。"""
    env.set_quirk("events.subscription_host_bad", address="10.0.0.1")
    time.sleep(0.3)

    subscription = _create(env)
    assert urlparse(subscription.address).hostname == "10.0.0.1", \
        "D2 开了但 host 没变：%s" % subscription.address

    # 只接管 host:port、保留 path 之后必须能用。
    usable = subscription.reachable_address(env.host, env.http_port)
    assert pullpoint.pull(usable, env.user, env.password, timeout="PT1S") == []


# ---- D3：槽位上限 -----------------------------------------------------

def test_d3_subscription_slot_limit_faults(env):
    """D3：廉价固件只有 4~8 个订阅槽，超了直接 Fault。"""
    # 上限要按「当前已有多少条」来定：订阅默认不回收（A9 就是拿这个做文章的），
    # 同一个 sim 进程里前面的用例会留下订阅，写死 max=2 会在第一条就被挡。
    existing = len(env.control.sessions(env.camera_id)["subscriptions"])
    env.set_quirks({"events.subscription_slot_limit":
                    {"enabled": True,
                     "params": {"max": existing + 2, "on_overflow": "fault"}}})
    time.sleep(0.3)

    created = [_create(env), _create(env)]
    assert len(created) == 2

    with pytest.raises(SoapFault):
        _create(env)

    sessions = env.control.sessions(env.camera_id)
    assert len(sessions["subscriptions"]) <= existing + 2, \
        "槽位上限没兜住：%d 条订阅" % len(sessions["subscriptions"])


def test_d3_slot_limit_evicts_oldest(env):
    """D3 的另一种表现：不 Fault，直接把最老的订阅踢掉（并发订阅互踢）。"""
    env.set_quirks({"events.subscription_slot_limit":
                    {"enabled": True,
                     "params": {"max": 2, "on_overflow": "evict_oldest"}}})
    time.sleep(0.3)

    oldest = _create(env)
    _create(env)
    _create(env)

    time.sleep(0.3)
    sessions = env.control.sessions(env.camera_id)
    assert len(sessions["subscriptions"]) <= 2, \
        "该只留 2 条，实际 %d 条" % len(sessions["subscriptions"])

    with pytest.raises(SoapFault):
        pullpoint.pull(oldest.reachable_address(env.host, env.http_port),
                       env.user, env.password, timeout="PT1S")


# ---- A9：订阅只增不回收 ----------------------------------------------

@pytest.mark.slow
def test_a9_subscription_never_expires(env):
    """A9：onvif-zeep 建了订阅从不退订，关掉过期回收就能复现槽位泄漏。"""
    env.set_quirk("auth.subscription_never_expires")
    time.sleep(0.2)

    _create(env, initial_termination="PT1S")
    time.sleep(3.0)

    sessions = env.control.sessions(env.camera_id)
    assert sessions["subscriptions"], \
        "A9 开了，PT1S 的订阅过期之后也不该被回收"


# ---- 场景：事件风暴 ---------------------------------------------------

@pytest.mark.parametrize("scenario_sim", ["event-storm"], indirect=True)
def test_event_storm_scenario(scenario_sim):
    """事件风暴场景：每秒几十条，客户端的队列要顶得住。"""
    from helpers.env import CameraEnv

    env = CameraEnv(scenario_sim)
    subscription = pullpoint.create(env.events_url, env.user, env.password)
    address = subscription.reachable_address(env.host, env.http_port)

    # PullMessages 一有消息就立刻返回（这是对的，空拉取才是心跳），
    # 所以连着 pull 三次拿到的是三个瞬时快照、每次只有几条 —— 那验不出风暴。
    # 要验的是「一秒能积多少」：先让它跑一秒，再一次性拉。
    time.sleep(1.0)
    first = len(pullpoint.pull(address, env.user, env.password,
                               timeout="PT1S", limit=100))
    assert first >= 20, "每秒 80 条的风暴，积一秒只拉到 %d 条" % first

    # 再积一秒确认是持续的，不是只喷了一下。
    time.sleep(1.0)
    second = len(pullpoint.pull(address, env.user, env.password,
                                timeout="PT1S", limit=100))
    assert second >= 20, "风暴没能持续：第二秒只有 %d 条" % second
