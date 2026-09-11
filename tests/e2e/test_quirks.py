"""故障注入：每一条 quirk 至少一条断言。

断言表在 ``helpers/quirk_cases.py``，这里只负责把它参数化跑起来。
用例 id 就是 quirk 的 key，所以想单跑一条很直接：

    pytest -k "rtsp.talkback_require_marker"
    pytest -m "quirk and not slow"

覆盖率由 ``test_quirk_coverage.py`` 拿 ``GET /api/quirks`` 对账，
新增 quirk 却忘了写断言会立刻失败。
"""

import pytest

from helpers.quirk_cases import CASES, UNCOVERED

SCENARIO = "single-camera"

pytestmark = pytest.mark.quirk

PARAMS = [pytest.param(case, marks=list(case.marks), id=case.key) for case in CASES]


@pytest.mark.parametrize("case", PARAMS)
def test_quirk(case, env):
    """打开这条 quirk，验证它真的改变了设备行为。

    检查函数自己负责开 quirk —— 不少断言要先取一份「关着的时候」的基线。
    进来时相机的 quirk 一定是空的（``env`` fixture 每个用例结束都会清）。
    """
    case.check(env)


@pytest.mark.parametrize("key", sorted(UNCOVERED))
def test_uncovered_quirk_is_documented(key):
    """还没覆盖的 quirk 必须写明原因，不许悄悄挂账。"""
    reason = UNCOVERED[key]
    assert reason and len(reason) > 10, "%s 的未覆盖原因写得太潦草：%r" % (key, reason)
