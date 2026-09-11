"""起 / 停 headless 的 onvifsim 进程。

进程用 ``--scenario`` 启动。场景文件里的端口是写死的 8000 / 8554 / 9000，
所以每次都把场景 JSON 复制一份、改成本次分配的端口再喂给进程 ——
既不动仓库里的资产，也让并行运行互不冲突。
"""

import json
import os
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

import requests

from .control import ControlClient
from .ports import free_port


class SimStartupError(RuntimeError):
    pass


def find_binary(repo_root, explicit=None):
    """定位 onvifsim 可执行文件。

    顺序：显式参数 → 环境变量 ONVIFSIM_BINARY → build 目录下的常见位置。
    """
    # 一律转成绝对路径：SimProcess 起子进程时用的是临时工作目录（cwd=workdir），
    # 相对路径会在 chdir 之后才解析，于是 --onvifsim-binary=../../build/... 这种
    # 写法必然 FileNotFoundError。自动查找那条路返回的本来就是绝对路径，
    # 所以这个坑只在显式指定时踩得到 —— 而那正是本地手工跑测试的常用姿势。
    if explicit:
        path = Path(explicit).expanduser().resolve()
        if not path.exists():
            raise SimStartupError("指定的可执行文件不存在：%s" % path)
        return path

    env = os.environ.get("ONVIFSIM_BINARY")
    if env:
        path = Path(env).expanduser().resolve()
        if not path.exists():
            raise SimStartupError("环境变量 ONVIFSIM_BINARY 指向的文件不存在：%s" % path)
        return path

    names = ["onvifsim.exe", "onvifsim"] if os.name == "nt" else ["onvifsim"]
    patterns = [
        "build/*/bin/{name}",
        "build/*/{name}",
        "build/bin/{name}",
        "build/{name}",
        "build/*/bin/*/{name}",   # 多配置生成器（MSVC / Xcode）
    ]
    candidates = []
    for name in names:
        for pattern in patterns:
            candidates.extend(sorted(repo_root.glob(pattern.format(name=name))))
    for candidate in candidates:
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    searched = [str(repo_root / pattern.format(name=name))
                for name in names for pattern in patterns]
    raise SimStartupError(
        "找不到 onvifsim 可执行文件。先构建：\n"
        "  cmake --preset conda-linux && cmake --build --preset conda-linux\n"
        "或者用 --onvifsim-binary / ONVIFSIM_BINARY 指定路径。\n"
        "已经找过：\n  %s" % "\n  ".join(searched))


def patch_scenario(source, target, http_base, rtsp_base, control_port,
                   bind_address=None, discovery=None):
    """把场景 JSON 复制一份并换掉端口。

    相机自己的 ``network.httpPort`` / ``rtspPort`` 按与场景基准端口的偏移平移，
    这样 8 路混品牌那种「基准 + 序号」的排布关系不会被破坏。
    """
    data = json.loads(Path(source).read_text(encoding="utf-8"))
    config = data.setdefault("config", {})

    old_http = int(config.get("httpBasePort", 8000))
    old_rtsp = int(config.get("rtspBasePort", 8554))
    config["httpBasePort"] = int(http_base)
    config["rtspBasePort"] = int(rtsp_base)
    config["controlApiEnabled"] = True
    config["controlApiPort"] = int(control_port)
    if bind_address is not None:
        config["bindAddress"] = bind_address
    if discovery is not None:
        config["discoveryEnabled"] = bool(discovery)

    http_delta = int(http_base) - old_http
    rtsp_delta = int(rtsp_base) - old_rtsp
    for camera in data.get("cameras", []):
        network = camera.get("network")
        if not isinstance(network, dict):
            continue
        if "httpPort" in network:
            network["httpPort"] = int(network["httpPort"]) + http_delta
        if "rtspPort" in network:
            network["rtspPort"] = int(network["rtspPort"]) + rtsp_delta

    Path(target).write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n",
                            encoding="utf-8")
    return data


class SimProcess:
    """一个跑着的 headless onvifsim。

    ``start()`` 会一直等到控制面的 ``/api/cameras`` 可达才返回，
    所以测试拿到它的时候相机一定已经在监听了。
    """

    def __init__(self, binary, scenario_path, workdir, control_port=None,
                 http_base=None, rtsp_base=None, token=None, extra_args=(),
                 verbose=False, headless=True):
        self.binary = Path(binary)
        self.scenario_path = Path(scenario_path)
        self.workdir = Path(workdir)
        self.workdir.mkdir(parents=True, exist_ok=True)
        self.control_port = control_port or free_port()
        self.http_base = http_base
        self.rtsp_base = rtsp_base
        self.token = token
        self.extra_args = list(extra_args)
        self.verbose = verbose
        # headless=False 起的是**图形界面版**（同一个可执行文件，只是不带 --headless）。
        # 界面层有自己的一套刷新逻辑，协议层测不到它 —— 曾经就有过
        # 「一拉流界面就无限递归爆栈」这种只在 GUI 下出现的崩溃。
        self.headless = headless

        self.process = None
        self.stdout_path = self.workdir / "onvifsim.stdout.log"
        self.stderr_path = self.workdir / "onvifsim.stderr.log"
        self._stdout = None
        self._stderr = None
        self.control = ControlClient(self.control_url, token=token)

    # ---- 地址 ----------------------------------------------------------

    @property
    def control_url(self):
        return "http://127.0.0.1:%d" % self.control_port

    @property
    def host(self):
        """相机在测试里一律按 127.0.0.1 连（绑的是 0.0.0.0）。"""
        return "127.0.0.1"

    # ---- 生命周期 ------------------------------------------------------

    def command(self):
        args = [str(self.binary)]
        if self.headless:
            args.append("--headless")
        args += ["--scenario", str(self.scenario_path),
                 "--control-port", str(self.control_port)]
        if self.token:
            args += ["--control-token", self.token]
        if self.verbose:
            args.append("--verbose")
        args += self.extra_args
        return args

    def start(self, timeout=30.0):
        if self.process is not None:
            raise SimStartupError("进程已经在跑了")
        self._stdout = self.stdout_path.open("wb")
        self._stderr = self.stderr_path.open("wb")
        creationflags = 0
        preexec = None
        if os.name == "nt":
            creationflags = getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
        else:
            preexec = os.setsid
        env = dict(os.environ)
        if not self.headless:
            # CI 与容器里没有显示器，图形版得靠 offscreen 平台插件才起得来。
            env.setdefault("QT_QPA_PLATFORM", "offscreen")
        self.process = subprocess.Popen(
            self.command(), stdout=self._stdout, stderr=self._stderr,
            cwd=str(self.workdir), creationflags=creationflags, preexec_fn=preexec,
            env=env)
        try:
            self.wait_ready(timeout)
        except Exception:
            self.stop()
            raise
        return self

    def wait_ready(self, timeout=30.0):
        deadline = time.time() + timeout
        last_error = None
        while time.time() < deadline:
            if self.process.poll() is not None:
                raise SimStartupError(
                    "onvifsim 启动即退出（退出码 %s）\n%s"
                    % (self.process.returncode, self.tail_logs()))
            try:
                self.control.cameras()
                return
            except Exception as exc:      # noqa: BLE001 —— 还没起来，继续等
                last_error = exc
                time.sleep(0.1)
        raise SimStartupError("等控制面 %s/api/cameras 超时（%.1fs）：%s\n%s"
                              % (self.control_url, timeout, last_error, self.tail_logs()))

    def stop(self, timeout=10.0):
        if self.process is None:
            return
        if self.process.poll() is None:
            try:
                if os.name == "nt":
                    self.process.send_signal(signal.CTRL_BREAK_EVENT)
                else:
                    os.killpg(os.getpgid(self.process.pid), signal.SIGTERM)
            except (OSError, ValueError):
                self.process.terminate()
            try:
                self.process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=timeout)
        self.process = None
        for handle in (self._stdout, self._stderr):
            if handle:
                handle.close()
        self._stdout = self._stderr = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.stop()
        return False

    # ---- 诊断 ----------------------------------------------------------

    def tail_logs(self, lines=40):
        chunks = []
        for label, path in (("stdout", self.stdout_path), ("stderr", self.stderr_path)):
            if not path.exists():
                continue
            text = path.read_text(encoding="utf-8", errors="replace").splitlines()
            if text:
                chunks.append("--- %s (最后 %d 行) ---\n%s"
                              % (label, lines, "\n".join(text[-lines:])))
        return "\n".join(chunks) or "（没有日志输出）"

    # ---- 便捷读取 ------------------------------------------------------

    def cameras(self):
        return self.control.cameras()

    def first_camera(self):
        cameras = self.cameras()
        if not cameras:
            raise AssertionError("场景里一台相机都没有")
        return cameras[0]

    def http_port_of(self, camera):
        return int(camera["network"]["httpPort"])

    def rtsp_port_of(self, camera):
        return int(camera["network"]["rtspPort"])

    def device_url(self, camera):
        """相机 Device 服务的**可连**地址。

        注意不要用 ``camera["xaddr"]`` —— 那是对外宣称的地址，
        quirk A5 会故意把它改成 :2020 这种连不上的端口。
        """
        return "http://%s:%d/onvif/device_service" % (self.host, self.http_port_of(camera))
