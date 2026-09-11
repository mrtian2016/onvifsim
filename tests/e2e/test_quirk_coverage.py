"""覆盖率守卫：``GET /api/quirks`` 与 e2e 断言表必须对得上。

CLAUDE.md 定的规矩是「新增一个 quirk 必须同时补齐 Quirks 表条目、REST 字段、
GUI 复选框和一条 e2e 断言」。这个文件盯的就是最后一条 ——
只要有人往 ``Quirks`` 表里加了东西却没写断言，这里立刻红。
"""

import pytest

from helpers.quirk_cases import CASES, COVERED_KEYS, UNCOVERED, duplicate_keys

SCENARIO = "single-camera"


@pytest.fixture(scope="module")
def registry(control):
    """设备侧的 quirk 元数据，键 → 完整定义。"""
    return {item["key"]: item for item in control.quirks_meta()}


def test_every_quirk_has_an_assertion_or_a_reason(registry):
    """每条 quirk 要么有断言，要么在 UNCOVERED 里写清楚为什么没有。"""
    accounted = set(COVERED_KEYS) | set(UNCOVERED)
    missing = sorted(set(registry) - accounted)
    assert not missing, (
        "这 %d 条 quirk 既没有 e2e 断言，也没在 helpers/quirk_cases.py 的 "
        "UNCOVERED 里登记：\n  %s" % (len(missing), "\n  ".join(missing)))


def test_no_stale_assertions(registry):
    """断言表里不该有设备侧已经不存在的键（改名 / 删除后要跟着改）。"""
    stale = sorted((set(COVERED_KEYS) | set(UNCOVERED)) - set(registry))
    assert not stale, (
        "断言表里这些键在 /api/quirks 里已经不存在了：\n  %s" % "\n  ".join(stale))


def test_no_duplicate_cases():
    """同一条 quirk 只写一条断言，重复了说明表被改乱了。"""
    duplicates = duplicate_keys()
    assert not duplicates, "断言表里有重复的键：%s" % duplicates


def test_registry_metadata_is_complete(registry):
    """每条 quirk 的元数据要能直接喂给 GUI 与文档生成。"""
    for key, item in sorted(registry.items()):
        assert item.get("group"), "%s 没有分组" % key
        assert item.get("groupTitle"), "%s 没有分组标题" % key
        assert item.get("title"), "%s 没有中文标题" % key
        assert item.get("description"), "%s 没有说明" % key
        for param in item.get("params", []):
            assert param.get("name"), "%s 的参数缺 name" % key
            assert "default" in param, "%s 的参数 %s 缺默认值" % (key, param.get("name"))
            assert param.get("description"), \
                "%s 的参数 %s 缺说明" % (key, param.get("name"))
            choices = param.get("choices")
            if choices:
                assert param["default"] in choices, \
                    "%s 的参数 %s 默认值 %r 不在可选项 %s 里" % (
                        key, param["name"], param["default"], choices)


def test_source_ids_point_at_the_facts_document(registry):
    """带编号的 quirk，编号形态要合规（A1 / D10 / E19 这种）。

    编号指向 docs/reference-client-facts.md §9。没有编号的（比如「黑屏」
    这类通用故障）留空是允许的，但不能写成别的样子。
    """
    import re

    pattern = re.compile(r"^[A-F]\d{1,2}$")
    bad = {key: item["sourceId"] for key, item in registry.items()
           if item.get("sourceId") and not pattern.match(item["sourceId"])}
    assert not bad, "出处编号格式不对：%s" % bad


def test_coverage_summary(registry):
    """打印一份覆盖率小结，跑完一眼能看到还差多少。"""
    total = len(registry)
    covered = len(set(COVERED_KEYS) & set(registry))
    known_gaps = len(set(UNCOVERED) & set(registry))
    print("\nquirk 覆盖率：%d / %d 有断言，%d 条已登记为待实现"
          % (covered, total, known_gaps))
    for key in sorted(set(UNCOVERED) & set(registry)):
        print("  待实现  %s —— %s" % (key, UNCOVERED[key]))
    assert covered + known_gaps == total


def test_cases_cover_every_group(registry):
    """每个分组都得有断言，别整组漏掉。"""
    groups = {}
    for key, item in registry.items():
        groups.setdefault(item["group"], []).append(key)
    covered = set(COVERED_KEYS)
    for group, keys in sorted(groups.items()):
        assert covered & set(keys), "分组 %s 一条断言都没有" % group


def test_case_table_is_ordered_like_the_registry(registry):
    """断言表的分组顺序要跟着 Quirks 表走，方便对照阅读（顺序错了只提示，不算失败）。"""
    order = {item["key"]: index for index, item in enumerate(registry.values())}
    indices = [order[case.key] for case in CASES if case.key in order]
    inversions = sum(1 for a, b in zip(indices, indices[1:]) if a > b)
    if inversions:
        print("\n提示：断言表有 %d 处与 /api/quirks 的顺序不一致" % inversions)
