#!/usr/bin/env python3
"""分析表情包图片，自动分类并生成 manifest。

分类逻辑：
  - 有大面积文字（顶部/底部条带）→ 表情包，不能当"照片"用
  - 纯图无文字 → 形象照，可以说"这是我"
  - 白色/透明背景 → 立绘/头像
  - 有场景背景 → 场景照

不依赖 OCR（太慢且不准），用简单的视觉启发式：
  1. 检测顶部/底部是否有明显的横条文字区域（高对比度密集小块）
  2. 检测背景是否纯白
  3. 检测是否四格（有明显的十字分割线）

用法:
    python3 tools/classify_memes.py --dir build/memes
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

from PIL import Image


def edge_density(img: Image.Image, box: tuple[int, int, int, int]) -> float:
    """计算区域内的高频边缘密度，用来判断是不是文字区。"""
    region = img.crop(box).convert("L")
    if region.width < 4 or region.height < 4:
        return 0.0
    px = region.load()
    w, h = region.size
    edges = 0
    total = 0
    step = 2
    for y in range(1, h - 1, step):
        for x in range(1, w - 1, step):
            v = px[x, y]
            # 与右、下邻居比较
            if abs(v - px[x + 1, y]) > 60 or abs(v - px[x, y + 1]) > 60:
                edges += 1
            total += 1
    return edges / total if total else 0.0


def is_mostly_white(img: Image.Image) -> bool:
    """背景是不是纯白/浅色（立绘特征）。"""
    rgb = img.convert("RGB")
    w, h = rgb.size
    # 采样四角和边中点
    pts = [(2, 2), (w - 3, 2), (2, h - 3), (w - 3, h - 3),
           (w // 2, 2), (w // 2, h - 3), (2, h // 2), (w - 3, h // 2)]
    light = 0
    for x, y in pts:
        r, g, b = rgb.getpixel((x, y))
        if r > 225 and g > 225 and b > 225:
            light += 1
    return light >= 6


def has_text_band(img: Image.Image) -> tuple[bool, str]:
    """检测顶部/底部是否有文字条带。返回 (是否有, 位置)。"""
    w, h = img.size
    # 顶部 22%、底部 22%
    top = (0, 0, w, int(h * 0.22))
    bottom = (0, int(h * 0.78), w, h)

    top_d = edge_density(img, top)
    bottom_d = edge_density(img, bottom)
    mid_d = edge_density(img, (0, int(h * 0.35), w, int(h * 0.65)))

    # 文字区边缘密度显著高于中部
    threshold = 0.18
    if top_d > threshold and top_d > mid_d * 1.5:
        return True, "top"
    if bottom_d > threshold and bottom_d > mid_d * 1.5:
        return True, "bottom"
    return False, ""


def is_grid(img: Image.Image) -> bool:
    """检测是否四格漫画（有明显十字分割）。"""
    w, h = img.size
    rgb = img.convert("RGB")

    def line_score(coords):
        light = 0
        for x, y in coords:
            r, g, b = rgb.getpixel((x, y))
            if r > 240 and g > 240 and b > 240:
                light += 1
        return light / len(coords) if coords else 0

    # 中线
    vline = [(w // 2, y) for y in range(0, h, 4)]
    hline = [(x, h // 2) for x in range(0, w, 4)]
    return line_score(vline) > 0.85 and line_score(hline) > 0.85


def classify(path: Path) -> dict:
    img = Image.open(path)
    w, h = img.size

    has_text, text_pos = has_text_band(img)
    white_bg = is_mostly_white(img)
    grid = is_grid(img)

    info = {
        "file": path.name,
        "width": w,
        "height": h,
        "has_text": has_text,
        "text_pos": text_pos,
        "white_bg": white_bg,
        "is_grid": grid,
    }

    # 分类
    if grid:
        info["category"] = "meme_grid"       # 四格梗图，只能当表情包
        info["usable_as_photo"] = False
    elif has_text:
        info["category"] = "meme_text"       # 带文字的表情包
        info["usable_as_photo"] = False
    elif white_bg:
        info["category"] = "portrait_plain"  # 纯色背景立绘，可以当"照片"
        info["usable_as_photo"] = True
    else:
        info["category"] = "scene"           # 有场景的图
        info["usable_as_photo"] = True

    return info


def main() -> int:
    ap = argparse.ArgumentParser(description="分析表情包图片")
    ap.add_argument("--dir", default="build/memes")
    ap.add_argument("--out", default="build/memes/analysis.json")
    args = ap.parse_args()

    d = Path(args.dir)
    if not d.exists():
        print(f"[x] 目录不存在: {d}")
        return 1

    results = []
    for f in sorted(d.glob("*.webp")):
        try:
            info = classify(f)
            results.append(info)
            mark = "照片" if info["usable_as_photo"] else "表情"
            print(f"  {info['file']:12s} {info['category']:15s} [{mark}]")
        except Exception as e:
            print(f"  {info['file']:12s} 分析失败: {e}")

    Path(args.out).write_text(
        json.dumps(results, ensure_ascii=False, indent=2), encoding="utf-8"
    )

    usable = [r for r in results if r["usable_as_photo"]]
    print(f"\n共 {len(results)} 张，其中 {len(usable)} 张可当照片")
    print(f"分析结果写入 {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
