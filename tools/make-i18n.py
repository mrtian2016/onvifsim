#!/usr/bin/env python3
"""从 core 的数据表生成可被 lupdate 扫到的翻译占位文件。

为什么要有这个脚本
------------------
界面自己的按钮标签走 tr()，lupdate 扫 src/gui 就够了。但**故障注入表**
（90 条 quirk 的标题 / 说明 / 参数说明，共两百多条）是 core 的数据，写在
Quirks.cpp 里一张跨行拼接的大表中：lupdate 扫不到，硬按参数位置去包
QT_TRANSLATE_NOOP 又要改几百处跨行字面量，风险远大于收益。

所以反过来做：**从运行中的模拟器把表读出来**（`GET /api/quirks` 就是照着
那张表生成的），生成一份只含 QT_TRANSLATE_NOOP 的 .cpp。这样：

  * lupdate 像对待普通源码一样扫得到，**不会再把它们当废弃条目删掉**
    —— 直接往 .ts 里注入会被下一次 lupdate 清空，踩过；
  * 表永远是唯一真源，加了 quirk 跑一遍就会多出待翻条目；
  * tst_i18n 会校验每条都有英文译文，忘了跑这个脚本测试就会挂。

用法：

    tools/make-i18n.py                       # 生成 .cpp
    lupdate6 -I src src -locations none -no-obsolete \
        -ts assets/i18n/onvifsim_zh_CN.ts assets/i18n/onvifsim_en.ts
"""

from __future__ import annotations

import argparse
import json
import pathlib
import socket
import subprocess
import sys
import time
import urllib.request

OUTPUT = pathlib.Path("src/core/QuirkStrings.cpp")
CONTEXT = "onvifsim::quirks"


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def fetch_quirks(binary: pathlib.Path) -> list[dict]:
    """起一个临时的无界面实例，把 quirk 表读回来。"""
    control, http, rtsp = free_port(), free_port(), free_port()
    proc = subprocess.Popen(
        [str(binary), "--headless", "--cameras", "1", "--no-discovery",
         "--bind", "127.0.0.1", "--http-port", str(http), "--rtsp-port", str(rtsp),
         "--control-port", str(control)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        url = "http://127.0.0.1:%d/api/quirks" % control
        deadline = time.time() + 20
        while time.time() < deadline:
            try:
                with urllib.request.urlopen(url, timeout=1) as r:
                    return json.load(r)
            except Exception:
                if proc.poll() is not None:
                    raise SystemExit("模拟器起不来，先确认可执行文件能跑")
                time.sleep(0.2)
        raise SystemExit("等控制面超时")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()


def sources(quirks: list[dict]) -> list[str]:
    """表里所有会显示在界面上的字串，按出现顺序去重。"""
    out: list[str] = []
    seen: set[str] = set()

    def take(text: str | None) -> None:
        if text and text not in seen:
            seen.add(text)
            out.append(text)

    for q in quirks:
        take(q.get("groupTitle"))
        take(q.get("title"))
        take(q.get("description"))
        for p in q.get("params") or []:
            take(p.get("description"))
    return out


def escape(text: str) -> str:
    return text.replace("\\", "\\\\").replace('"', '\\"')


def render(strings: list[str]) -> str:
    lines = [
        "// 生成物，不要手改。由 tools/make-i18n.py 从 src/core/Quirks.cpp 的表生成。",
        "//",
        "// 存在的唯一理由是让 lupdate 扫得到故障注入表里的字串 —— 那张表是数据、",
        "// 不经过 tr()，而界面（QuirksTab）显示前会经 gui/I18n.h 查一次译文。",
        "// 改了 quirk 之后跑一遍 tools/make-i18n.py 再跑 lupdate，",
        "// 否则 tst_i18n 会因为缺英文译文而失败。",
        "",
        '#include "core/QuirkStrings.h"',
        "",
        "#include <QtCore/QCoreApplication>",
        "",
        "namespace onvifsim {",
        "",
        "QStringList quirkTranslatableStrings()",
        "{",
        "    return {",
    ]
    for s in strings:
        lines.append('        QStringLiteral(QT_TRANSLATE_NOOP("%s", "%s")),' % (CONTEXT, escape(s)))
    lines += [
        "    };",
        "}",
        "",
        "} // namespace onvifsim",
        "",
    ]
    return "\n".join(lines)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default="build/conda-linux/bin/onvifsim", type=pathlib.Path)
    args = ap.parse_args()
    if not args.binary.is_file():
        sys.exit("找不到可执行文件：%s" % args.binary)

    quirks = fetch_quirks(args.binary.resolve())
    strings = sources(quirks)
    OUTPUT.write_text(render(strings), encoding="utf-8")
    print("-- %s：%d 条字串（%d 条 quirk）" % (OUTPUT, len(strings), len(quirks)))
    print("-- 接着跑：lupdate6 -I src src -locations none -no-obsolete \\")
    print("       -ts assets/i18n/onvifsim_zh_CN.ts assets/i18n/onvifsim_en.ts")


if __name__ == "__main__":
    main()
