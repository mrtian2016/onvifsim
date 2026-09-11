#!/usr/bin/env python3
"""把 assets/logo/*.svg 打包成 .ico / .icns。

ICO 和 ICNS 都只是「一串 PNG 加一个索引表」，格式简单到不值得为它拉一个
Pillow/ImageMagick 依赖进来 —— 这个仓库其余部分也是零第三方库的。
位图本身由 tools/svg2png（Qt 渲染）生成，这里只负责封装。
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

# ICO 目录项里宽高各占一个字节，256 只能写 0。
ICO_SIZES = [16, 24, 32, 48, 64, 128, 256]

# ICNS 的类型码 -> 像素尺寸。ic11/ic12/ic13/ic14 是 Retina 变体，
# 和 icp4/ic07/ic08 尺寸重复是正常的：macOS 按 @1x/@2x 分别取。
ICNS_TYPES = [
    (b"icp4", 16),
    (b"icp5", 32),
    (b"ic11", 32),
    (b"ic12", 64),
    (b"ic07", 128),
    (b"ic13", 256),
    (b"ic08", 256),
    (b"ic14", 512),
    (b"ic09", 512),
    (b"ic10", 1024),
]


def load(png_dir: Path, size: int) -> bytes:
    path = png_dir / f"onvifsim-{size}.png"
    if not path.is_file():
        sys.exit(f"缺少 {path}，先跑 tools/make-icons.sh")
    return path.read_bytes()


def write_ico(png_dir: Path, out: Path) -> None:
    images = [(size, load(png_dir, size)) for size in ICO_SIZES]
    header = struct.pack("<HHH", 0, 1, len(images))
    offset = len(header) + 16 * len(images)
    entries, blobs = [], []
    for size, data in images:
        entries.append(struct.pack(
            "<BBBBHHII",
            size if size < 256 else 0,   # 0 表示 256
            size if size < 256 else 0,
            0,                            # 调色板颜色数：真彩色写 0
            0,                            # 保留
            1,                            # 色彩平面
            32,                           # 位深
            len(data),
            offset))
        blobs.append(data)
        offset += len(data)
    out.write_bytes(header + b"".join(entries) + b"".join(blobs))
    print(f"-- ico: {out} ({len(ICO_SIZES)} 个尺寸)")


def write_icns(png_dir: Path, out: Path) -> None:
    chunks = []
    for ostype, size in ICNS_TYPES:
        data = load(png_dir, size)
        chunks.append(ostype + struct.pack(">I", len(data) + 8) + data)
    body = b"".join(chunks)
    out.write_bytes(b"icns" + struct.pack(">I", len(body) + 8) + body)
    print(f"-- icns: {out} ({len(ICNS_TYPES)} 个条目)")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("png_dir", type=Path)
    parser.add_argument("--ico", type=Path)
    parser.add_argument("--icns", type=Path)
    args = parser.parse_args()
    if args.ico:
        write_ico(args.png_dir, args.ico)
    if args.icns:
        write_icns(args.png_dir, args.icns)


if __name__ == "__main__":
    main()
