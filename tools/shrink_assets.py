#!/usr/bin/env python3
"""把已入库的图片压到可接受体积。

QQ 富媒体直传上限 5MB（base64 之后），也就是原始字节约 3.7MB。
目标定 700KB 以内，留足余量，也避免仓库臃肿。

策略：长边先保持 1024；若仍超限，降到 768；再不行降到 640。
PNG 保留透明通道（表情包有透明背景）。
"""
from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image

TARGET_KB = 700


def shrink(p: Path, target_kb: int) -> tuple[int, int]:
    """反复缩放直到体积达标。返回 (最终字节数, 最终长边)。"""
    im = Image.open(p)
    if getattr(im, "is_animated", False):
        im.seek(0)
    im = im.convert("RGBA")

    for side in (1024, 896, 768, 640, 512):
        w, h = im.size
        cur = im
        if max(w, h) > side:
            scale = side / max(w, h)
            cur = im.resize((int(w * scale), int(h * scale)), Image.LANCZOS)
        cur.save(p, "PNG", optimize=True)
        size = p.stat().st_size
        if size <= target_kb * 1024:
            return size, max(cur.size)
    return p.stat().st_size, max(im.size)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default="data/memory/portraits")
    ap.add_argument("--target-kb", type=int, default=TARGET_KB)
    args = ap.parse_args()

    d = Path(args.dir)
    total_before = 0
    total_after = 0
    changed = 0

    for p in sorted(d.glob("*.png")):
        before = p.stat().st_size
        total_before += before
        if before <= args.target_kb * 1024:
            total_after += before
            continue
        after, side = shrink(p, args.target_kb)
        total_after += after
        changed += 1
        print(f"  {p.name:40} {before // 1024:5} KB → {after // 1024:4} KB  (长边 {side})")

    print(f"\n压缩 {changed} 张")
    print(f"总计 {total_before // 1024 // 1024} MB → {total_after // 1024 // 1024} MB")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
