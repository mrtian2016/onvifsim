"""e2e 的公共 fixture。

核心是 ``sim``：起一个 headless 的 onvifsim 进程，等控制面
``/api/cameras`` 可达之后 yield，测试结束时干净退出。

场景可以按模块指定，也可以按用例参数化：

    SCENARIO = "talkback"                       # 模块级默认

    @pytest.mark.parametrize("scenario_sim", ["talkback", "legacy-firmware"],
                             indirect=True)
    def test_x(scenario_sim): ...
"""

import contextlib
import itertools
import json
import os
import sys
import time
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from helpers import wsd                                    # noqa: E402
from helpers.env import CameraEnv, DEFAULT_PASSWORD, DEFAULT_USER   # noqa: E402
from helpers.media import ffprobe_available                # noqa: E402
from helpers.onvifclient import ONVIF_AVAILABLE            # noqa: E402
from helpers.ports import free_port, free_port_block       # noqa: E402
from helpers.sim import SimProcess, find_binary, patch_scenario     # noqa: E402

DEFAULT_SCENARIO = "single-camera"


# ---- 命令行开关 --------------------------------------------------------

def pytest_addoption(parser):
    group = parser.getgroup("onvifsim")
    group.addoption("--onvifsim-binary", action="store", default=None,
                    help="onvifsim 可执行文件路径（默认自动在 build/ 下找）")
    group.addoption("--sim-verbose", action="store_true", default=False,
                    help="给模拟器加 --verbose，日志里带完整报文")
    group.addoption("--keep-sim-logs", action="store_true", default=False,
                    help="用例结束后打印模拟器日志尾部，不管有没有失败")


@pytest.hookimpl(tryfirst=True, hookwrapper=True)
def pytest_runtest_makereport(item, call):
    """把每个阶段的结果挂到 item 上，fixture 拆解时好判断要不要打日志。"""
    outcome = yield
    report = outcome.get_result()
    setattr(item, "rep_" + report.when, report)


# ---- 路径 --------------------------------------------------------------

@pytest.fixture(scope="session")
def repo_root():
    return Path(__file__).resolve().parents[2]


@pytest.fixture(scope="session")
def scenarios_dir(repo_root):
    directory = repo_root / "assets" / "scenarios"
    if not directory.is_dir():
        pytest.fail("找不到场景目录：%s" % directory)
    return directory


@pytest.fixture(scope="session")
def binary_path(repo_root, pytestconfig):
    """定位可执行文件。找不到就直接失败 —— CI 上不能静默跳过。"""
    return find_binary(repo_root, pytestconfig.getoption("--onvifsim-binary"))


# ---- 起进程 ------------------------------------------------------------

@pytest.fixture(scope="session")
def sim_factory(binary_path, scenarios_dir, tmp_path_factory, pytestconfig):
    """返回一个上下文管理器，用它按场景起模拟器。"""
    counter = itertools.count()

    @contextlib.contextmanager
    def launch(scenario=DEFAULT_SCENARIO, discovery=None, extra_args=(), token=None):
        source = scenarios_dir / ("%s.json" % scenario)
        if not source.is_file():
            raise AssertionError("没有这个场景：%s" % source)
        index = next(counter)
        workdir = tmp_path_factory.mktemp("sim-%d-%s" % (index, scenario))

        data = json.loads(source.read_text(encoding="utf-8"))
        camera_count = max(1, len(data.get("cameras") or []))
        # 多留几个空位：测试里可能通过 REST 现加相机。
        http_base = free_port_block(camera_count + 4)
        rtsp_base = free_port_block(camera_count + 4)
        control_port = free_port()

        target = workdir / source.name
        patch_scenario(source, target, http_base, rtsp_base, control_port,
                       discovery=discovery)

        process = SimProcess(binary_path, target, workdir,
                             control_port=control_port, http_base=http_base,
                             rtsp_base=rtsp_base, token=token,
                             extra_args=extra_args,
                             verbose=pytestconfig.getoption("--sim-verbose"))
        process.scenario_name = scenario
        process.start()
        try:
            yield process
        finally:
            process.stop()

    return launch


def _report_logs(request, process):
    """用例失败时把模拟器日志尾巴打出来，省得再复现一遍。"""
    failed = any(getattr(request.node, "rep_" + phase, None) is not None
                 and getattr(request.node, "rep_" + phase).failed
                 for phase in ("setup", "call", "teardown"))
    if failed or request.config.getoption("--keep-sim-logs"):
        print("\n[onvifsim 日志 %s]\n%s" % (process.scenario_name, process.tail_logs()))


@pytest.fixture(scope="module")
def sim(request, sim_factory):
    """模块级模拟器。模块里定义 ``SCENARIO = "xxx"`` 即可换场景。"""
    scenario = getattr(request.module, "SCENARIO", DEFAULT_SCENARIO)
    with sim_factory(scenario) as process:
        yield process


@pytest.fixture
def scenario_sim(request, sim_factory):
    """按用例参数化场景：``indirect=True`` 传场景名。"""
    scenario = getattr(request, "param", DEFAULT_SCENARIO)
    with sim_factory(scenario) as process:
        yield process


@pytest.fixture(scope="module")
def control(sim):
    return sim.control


def _wait_back_online(camera_env, timeout=25.0):
    """等相机从「离线」状态回来。

    F3 过载重启、SystemReboot、随机掉线这几条 quirk 会把整台相机的端口关掉几秒。
    同一个 sim 进程跑后续用例时如果不等它回来，后面全是 Connection refused ——
    看起来像一片实现 bug，其实只是上一条用例的余波。
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            if not camera_env.listing["status"]["offline"]:
                return True
        except Exception:                       # noqa: BLE001 —— 控制面还在，只是相机端口关着
            pass
        time.sleep(0.25)
    return False


@pytest.fixture
def env(request, sim):
    """默认相机的门面。每个用例结束后把 quirk 清空、等相机回到在线，互不影响。"""
    camera_env = CameraEnv(sim)
    try:
        yield camera_env
    finally:
        _report_logs(request, sim)
        try:
            camera_env.clear_quirks()
        except Exception:                       # noqa: BLE001 —— 进程可能已经不在了
            pass
        # 清 quirk 挡不住已经开始的离线倒计时，必须真的等它结束。
        try:
            _wait_back_online(camera_env)
        except Exception:                       # noqa: BLE001
            pass


@pytest.fixture
def env_factory(request, sim):
    """按 id 拿别的相机（8 路混品牌、对讲三连那类场景用）。"""
    created = []

    def make(camera_id=None, user=DEFAULT_USER, password=DEFAULT_PASSWORD):
        camera_env = CameraEnv(sim, camera_id, user, password)
        created.append(camera_env)
        return camera_env

    try:
        yield make
    finally:
        _report_logs(request, sim)
        for camera_env in created:
            try:
                camera_env.clear_quirks()
            except Exception:                   # noqa: BLE001
                pass


# ---- 环境依赖 ----------------------------------------------------------

@pytest.fixture(scope="session")
def require_onvif_zeep():
    if not ONVIF_AVAILABLE:
        pytest.skip("没装 onvif-zeep，见 tests/e2e/README.md")


@pytest.fixture(scope="session")
def require_ffprobe():
    if not ffprobe_available():
        pytest.skip("找不到 ffprobe（可用环境变量 FFPROBE 指定）")


@pytest.fixture(scope="session")
def require_discovery():
    if wsd.skip_discovery_requested():
        pytest.skip("ONVIFSIM_SKIP_DISCOVERY=1，跳过多播相关用例")


def pytest_collection_modifyitems(config, items):
    """没装 onvif-zeep / 没有 ffprobe 时，给相关用例自动加 skip 标记。"""
    if not ONVIF_AVAILABLE:
        skip = pytest.mark.skip(reason="没装 onvif-zeep")
        for item in items:
            if "require_onvif_zeep" in item.fixturenames:
                item.add_marker(skip)
    if not ffprobe_available():
        skip = pytest.mark.skip(reason="找不到 ffprobe")
        for item in items:
            if "stream" in item.keywords:
                item.add_marker(skip)
    if wsd.skip_discovery_requested():
        skip = pytest.mark.skip(reason="ONVIFSIM_SKIP_DISCOVERY=1")
        for item in items:
            if "discovery" in item.keywords:
                item.add_marker(skip)


def pytest_report_header(config):
    lines = ["onvifsim e2e：onvif-zeep=%s  ffprobe=%s"
             % ("有" if ONVIF_AVAILABLE else "无",
                "有" if ffprobe_available() else "无")]
    binary = os.environ.get("ONVIFSIM_BINARY") or config.getoption("--onvifsim-binary")
    if binary:
        lines.append("可执行文件：%s" % binary)
    return lines
