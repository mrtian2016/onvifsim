"""空闲端口分配。

e2e 会同时起 HTTP / RTSP / 控制面三组端口，而场景文件里写死的是 8000 / 8554 / 9000。
每次运行都换一段端口，既避免和开发机上正在跑的实例冲突，也让 CI 上并行 job 安全。
"""

import contextlib
import random
import socket


def is_free(port, host="127.0.0.1"):
    """探一个 TCP 端口能不能绑上。"""
    with contextlib.closing(socket.socket(socket.AF_INET, socket.SOCK_STREAM)) as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            s.bind((host, port))
        except OSError:
            return False
    return True


def free_port(host="127.0.0.1"):
    """让内核挑一个空闲端口。"""
    with contextlib.closing(socket.socket(socket.AF_INET, socket.SOCK_STREAM)) as s:
        s.bind((host, 0))
        return s.getsockname()[1]


def free_port_block(count, host="127.0.0.1", low=20000, high=59000):
    """找一段连续 count 个都空闲的端口，返回起始端口。

    相机的 HTTP / RTSP 端口是「基准端口 + 序号」递增的，所以必须连续。
    """
    if count < 1:
        raise ValueError("count 至少为 1")
    for _ in range(400):
        base = random.randint(low, high - count)
        if all(is_free(base + i, host) for i in range(count)):
            return base
    raise RuntimeError("找不到 %d 个连续的空闲端口" % count)
