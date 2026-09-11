# tests/e2e —— 端到端测试

用**参照客户端同款的库**驱动一个真的在跑的 onvifsim 进程。
这是刻意的：onvif-zeep 0.2.12 按官方 WSDL 严格解析响应，字段错就抛异常，
所以它既是「最挑剔的客户端」，也是手写 XML 的免费 schema 校验器。

这套测试有自己的 `pyproject.toml` 和 venv，**不走 conda 环境**
（conda 环境里根本没有 Python）。

## 依赖

| 包 | 版本 | 为什么 |
|---|---|---|
| `onvif-zeep` | `0.2.12` | 参照客户端用的就是这个版本，行为要逐字对齐 |
| `zeep` | `4.3.2` | onvif-zeep 的底座 |
| `wsdiscovery` | `2.1.2` | 参照客户端的 WS-Discovery 库，缺 MetadataVersion 会整包丢 |
| `requests` | ≥ 2.31 | REST 控制面、快照、裸 SOAP |
| `pytest` | ≥ 7.4 | 跑测试 |
| `ffprobe` | 系统自带 | 真的把流拉下来验 codec / 分辨率 / fps / 音频 |

RTSP / RTP 客户端是自己用 socket 写的（`helpers/rtsp.py`），
不引 live555 之类 —— 对讲要精确控制 marker 位与包间隔，库反而挡路。

## 怎么跑

```bash
# 1. 先把模拟器构建出来（在 conda 环境里）
conda activate onvifsim
cmake --preset conda-linux && cmake --build --preset conda-linux

# 2. 建 e2e 自己的 venv（用系统 Python，不用 conda 的）
cd tests/e2e
python3 -m venv .venv
. .venv/bin/activate
pip install -e .

# 3. 跑
pytest                       # 全部
pytest -m "not slow"         # 跳过耗时用例
pytest -m "not discovery"    # 多播不通的环境（容器 / 某些 CI）
pytest test_talkback.py      # 单个文件
pytest -k "rtsp.sdp_no_rtpmap"   # 单条 quirk（用例 id 就是 quirk 的 key）
```

Windows 下把 `. .venv/bin/activate` 换成 `.venv\Scripts\activate`。

### 找不到可执行文件时

默认会在仓库的 `build/*/bin/onvifsim`、`build/bin/onvifsim` 等位置找。
不在那儿就显式指定：

```bash
pytest --onvifsim-binary /path/to/onvifsim
ONVIFSIM_BINARY=/path/to/onvifsim pytest
```

### 有用的开关

| 开关 | 作用 |
|---|---|
| `--onvifsim-binary PATH` | 指定可执行文件 |
| `--sim-verbose` | 给模拟器加 `--verbose`，日志里带完整 SOAP 报文 |
| `--keep-sim-logs` | 每个用例结束都打印模拟器日志尾部（不只失败时） |
| `FFPROBE=/path/to/ffprobe` | 指定 ffprobe |
| `ONVIFSIM_SKIP_DISCOVERY=1` | 跳过所有多播用例 |

用例失败时会自动把模拟器那一段的 stdout / stderr 尾部打出来，不用再复现一遍。

## 标记

| 标记 | 含义 |
|---|---|
| `discovery` | 需要 WS-Discovery 多播（3702 组播） |
| `stream` | 需要 ffprobe 真的拉流，慢 |
| `slow` | 单条用时超过 5 秒 |
| `quirk` | 故障注入断言 |

## 目录里都有什么

```
conftest.py              起 / 停模拟器的 fixture，场景参数化
helpers/
  sim.py                 headless 进程的生命周期 + 场景端口重写
  control.py             REST 控制面客户端
  env.py                 「一台相机」的门面：可连地址、开关 quirk、建各种客户端
  soap.py                裸 SOAP + WS-Security UsernameToken
  pullpoint.py           PullPoint 订阅全流程
  ptz.py                 PTZ 的裸 SOAP 调用
  rtsp.py                手写的最小 RTSP / RTP 客户端
  wsd.py                 裸 WS-Discovery Probe + wsdiscovery 2.1.2 封装
  onvifclient.py         onvif-zeep 薄封装
  media.py               ffprobe 探流
  quirk_cases.py         ★ 故障注入断言表（每条 quirk 一个检查函数）
test_discovery.py        发现、4 次重发去重、A2 / A3 的行为差异
test_connect.py          建连、GetCapabilities、设备信息、A5 / A6 / A7 / A10
test_media.py            GetProfiles / GetStreamUri + ffprobe 真拉流
test_snapshot.py         三种鉴权、B4 空体、两次快照内容不同
test_ptz.py              四档能力声明、移动改状态、出厂 300 预置位
test_events.py           PullPoint 全流程、D1 换端口、D3 槽位上限
test_talkback.py         DESCRIBE → SETUP → PLAY → 推 RTP → 查统计
test_quirks.py           把 quirk_cases 的表跑一遍
test_quirk_coverage.py   覆盖率守卫：/api/quirks 与断言表对账
test_control_api.py      REST 控制面本身的冒烟测试
```

## 端口怎么分配

场景文件里的端口是写死的 8000 / 8554 / 9000。fixture 每次起进程都会：

1. 复制一份场景 JSON 到临时目录；
2. 把 `config.httpBasePort` / `rtspBasePort` / `controlApiPort` 换成本次分配的空闲端口；
3. 每台相机的 `network.httpPort` / `rtspPort` 按同样的偏移平移，保持相对关系。

所以仓库里的 `assets/scenarios/*.json` 不会被改，多次运行也不会互相打架。

## 新增一条 quirk 要做什么

按 CLAUDE.md 的规矩，**四件事一个都不能少**：`Quirks` 表条目、REST 字段、
GUI 复选框、**一条 e2e 断言**。这里只管最后一条：

1. 在 `helpers/quirk_cases.py` 里写一个 `check_xxx(env)` 函数，
   自己负责 `enable(env, "your.key", ...)` 并断言行为真的变了；
2. 把它加进文件末尾的 `CASES` 表；
3. 实在暂时测不了，就登记进 `UNCOVERED` 并写清楚原因。

`test_quirk_coverage.py` 会拿 `GET /api/quirks` 和这张表对账，漏了立刻红。

## 已知未覆盖

见 `helpers/quirk_cases.py` 里的 `UNCOVERED`，跑一次
`pytest test_quirk_coverage.py -s` 也会把清单打出来。
