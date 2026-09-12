"""快照：三种鉴权、空体被拒、两次快照内容不同。

参照客户端拿快照的顺序是 **Digest → Basic → 无**，
而且只有收到 401 才换下一种。B5 就是在这条顺序上做文章。
"""

import time

import pytest
import requests
from requests.auth import HTTPBasicAuth, HTTPDigestAuth

from helpers import soap

SCENARIO = "single-camera"

JPEG_SOI = b"\xff\xd8"


def fetch(url, user=None, password=None, mode="digest", timeout=10):
    """按指定方式取一次快照。"""
    auth = None
    if mode == "digest" and user:
        auth = HTTPDigestAuth(user, password)
    elif mode == "basic" and user:
        auth = HTTPBasicAuth(user, password)
    return requests.get(url, auth=auth, timeout=timeout)


def fetch_like_client(url, user, password):
    """复刻参照客户端的顺序：Digest → Basic → 无，401 才往下走。

    返回 ``(成功的方式, 响应)``；全都 401 就返回 ``(None, 最后一个响应)``。
    """
    last = None
    for mode in ("digest", "basic", "none"):
        response = fetch(url, user, password, mode)
        last = response
        if response.status_code != 401:
            return mode, response
    return None, last


# ---- 基线 -------------------------------------------------------------

def test_snapshot_returns_jpeg(env):
    """默认配置下要拿到一张真 JPEG。"""
    url = env.snapshot_url()
    mode, response = fetch_like_client(url, env.user, env.password)
    assert mode is not None, "三种鉴权都被 401 顶回来了"
    assert response.status_code == 200, response.status_code
    assert response.headers.get("Content-Type", "").startswith("image/jpeg"), \
        response.headers.get("Content-Type")
    assert response.content[:2] == JPEG_SOI, "响应体不是 JPEG：%r" % response.content[:16]
    assert len(response.content) > 1024, "JPEG 只有 %d 字节，像是占位" % len(response.content)


def test_snapshot_requires_credentials(env):
    """默认不该匿名放行 —— 否则 B5 的四档就没有对照组。"""
    response = fetch(env.snapshot_url(), mode="none")
    assert response.status_code in (401, 403), \
        "匿名居然拿到了 HTTP %d" % response.status_code


def test_two_snapshots_differ(env):
    """两次快照内容必须不同（画面上带时间戳），否则客户端会以为画面卡死。"""
    url = env.snapshot_url()
    first = fetch(url, env.user, env.password, "digest")
    assert first.status_code == 200
    time.sleep(1.2)
    second = fetch(url, env.user, env.password, "digest")
    assert second.status_code == 200
    assert first.content != second.content, "两次快照字节完全一样"


# ---- B5：四档鉴权 -----------------------------------------------------

@pytest.mark.parametrize("mode", ["none", "basic", "digest"])
def test_b5_snapshot_auth_modes(env, mode):
    """B5：只接受 Basic / 只接受 Digest / 完全不鉴权，客户端都要能拿到图。"""
    env.set_quirk("media.snapshot_auth", value=mode)
    time.sleep(0.2)

    url = env.snapshot_url()
    accepted, response = fetch_like_client(url, env.user, env.password)
    assert accepted is not None, "配成 %s 之后客户端一种都试不通" % mode
    assert response.status_code == 200
    assert response.content[:2] == JPEG_SOI

    if mode == "digest":
        assert accepted == "digest"
        assert fetch(url, env.user, env.password, "basic").status_code == 401, \
            "配成只收 Digest，Basic 却通了"
    elif mode == "basic":
        assert accepted in ("basic", "none") and accepted != "digest", \
            "配成只收 Basic，Digest 却先通了"
    else:
        # 不鉴权时服务器根本不看 Authorization 头，所以客户端第一次带 Digest
        # 的尝试就直接 200 了 —— 真机也是这个表现，accepted 会是 "digest"。
        # 真正要验的是「一点凭据都不带也能拿到图」。
        bare = fetch(url, None, None, "none")
        assert bare.status_code == 200, "配成不鉴权，裸请求却拿不到图"
        assert bare.content[:2] == JPEG_SOI


# ---- B4：空体 ---------------------------------------------------------

def test_b4_empty_body_must_be_rejected(env):
    """B4：``200 + image/jpeg + 空体``。客户端注释写着「真踩过」。

    这条的判据是：HTTP 层完全正常，只有内容是空的 ——
    只看 status_code 的客户端会把空图当成有效帧。
    """
    env.set_quirk("media.snapshot_empty_body", value="empty")
    time.sleep(0.2)

    response = fetch(env.snapshot_url(), env.user, env.password, "digest")
    assert response.status_code == 200, "B4 就是要 200，不是错误码"
    assert response.headers.get("Content-Type", "").startswith("image/jpeg")
    assert len(response.content) == 0, \
        "B4 开了但响应体有 %d 字节" % len(response.content)


@pytest.mark.parametrize("variant", ["short_text", "html_login", "wrong_content_type"])
def test_b4_other_bad_body_variants(env, variant):
    """B4 的另外三种形态：短文本 / 登录页 HTML / Content-Type 撒谎。"""
    env.set_quirk("media.snapshot_empty_body", value=variant)
    time.sleep(0.2)

    response = fetch(env.snapshot_url(), env.user, env.password, "digest")
    assert response.status_code == 200
    is_jpeg = response.content[:2] == JPEG_SOI
    content_type = response.headers.get("Content-Type", "")
    if variant == "wrong_content_type":
        assert not content_type.startswith("image/jpeg"), \
            "该报一个错的 Content-Type，实际 %s" % content_type
    else:
        assert not is_jpeg, "%s 变体不该给出真 JPEG" % variant


# ---- B6 / B7 ----------------------------------------------------------

# B6 的轮换窗口。取 2 秒而不是 1 秒：窗口边界随时可能落在 Digest 的两次往返
# （401 挑战 + 带 nonce 重发）之间，窗口越短撞上的概率越大。
B6_WINDOW_SECONDS = 2


def fetch_fresh_snapshot(env, tries=3):
    """重探 URI 再取图，过期就再探一次。

    B6 打开时，刚拿到的 URI 也可能在 Digest 的两次往返之间跨过窗口边界而 404 ——
    这正是 quirk 要模拟的行为，**客户端本来就该重探**。所以这里也重探，
    而不是把它当失败。不这么写这条断言会偶发性地红（踩过）。
    """
    response = None
    for _ in range(tries):
        response = fetch(env.snapshot_url(), env.user, env.password, "digest")
        if response.status_code == 200:
            return response
    return response


@pytest.mark.slow
def test_b6_snapshot_uri_rotates(env):
    """B6：快照 URI 会随重配 / 重启失效，客户端要重探而不是一直用旧的。"""
    env.set_quirk("media.snapshot_uri_rotates", seconds=B6_WINDOW_SECONDS)
    time.sleep(0.2)

    old_url = env.snapshot_url()
    assert fetch_fresh_snapshot(env).status_code == 200, "刚探到的地址必须能用"

    # 睡过一整个窗口，保证 floor(now/window) 变过一次。
    time.sleep(B6_WINDOW_SECONDS + 0.5)
    stale = fetch(old_url, env.user, env.password, "digest")
    fresh_url = env.snapshot_url()
    assert fresh_url != old_url or stale.status_code != 200, \
        "URI 没轮换，旧地址也照样能用"

    fresh = fetch_fresh_snapshot(env)
    assert fresh.status_code == 200 and fresh.content[:2] == JPEG_SOI, \
        "重探之后新地址必须能用"


def test_snapshot_uri_matches_persona_path(env):
    """GetSnapshotUri 报的 path 要和品牌预设一致（客户端会硬记这些路径）。"""
    root = soap.get_snapshot_uri(env.media_url, env.user, env.password,
                                 env.main_profile_token())
    uri = soap.text(soap.find(root, "Uri"))
    assert uri.startswith("http"), uri
    assert "/" in uri.split("//", 1)[1], "快照 URI 没有 path：%s" % uri
