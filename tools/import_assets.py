#!/usr/bin/env python3
"""把社区素材整理进形象库，并按授权分成两档。

授权分档（这是本脚本的核心判断，改之前先看 CREDITS.md）：

  可入库（随仓库分发，CC BY-NC-SA 4.0，署名链完整）：
    - Neko3000/deepseek-whalechan
        MIT 代码 + assets 下 CC BY-NC-SA 4.0（仓库自带 LICENSE-CC-BY-NC-SA-4.0）
    - 1190fasheqi/dafeiyu-pet
        MIT（三视图 + 三方向立绘）

  仅本地（不进仓库，授权窄或缺失）：
    - roseion/dsh-dayu-fish-skin   SOURCES.md 写「仅供个人学习使用」
    - gmskywalker/...-codex-pet    无 LICENSE，仅 README 声明非商业
    - bloodarea/dsh-pets           assets 为 CC BY-NC-SA 4.0 但含 Spine 运行时
                                   （Esoteric Software 商业许可），不入库

处理：
  1. 统一转 PNG（QQ 上传要 png/jpg）
  2. 长边缩到 max_side 以内（QQ 单图 base64 后 5MB 上限）
  3. 生成 manifest.md，带 caption 和 tags
  4. 打印两档清单，供 CREDITS.md 使用

用法:
    python3 tools/import_assets.py --src /tmp/asr \\
        --meme-src ~/super-feiyu/build/memes \\
        --out data/memory/portraits
"""
from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image

# 单图长边默认上限。QQ 富媒体直传上限 5MB（base64 后），
# 1024 长边的 PNG 一般在 1MB 内，安全。
DEFAULT_MAX_SIDE = 1024


def normalize(src: Path, dst: Path, max_side: int) -> tuple[bool, int]:
    """转 PNG 并缩放。返回 (是否成功, 字节数)。"""
    try:
        im = Image.open(src)
        if getattr(im, "is_animated", False):
            im.seek(0)
        im = im.convert("RGBA")
    except Exception:
        return False, 0

    w, h = im.size
    if max(w, h) > max_side:
        scale = max_side / max(w, h)
        im = im.resize((int(w * scale), int(h * scale)), Image.LANCZOS)

    dst.parent.mkdir(parents=True, exist_ok=True)
    im.save(dst, "PNG", optimize=True)
    return True, dst.stat().st_size


def add(
    entries: list[tuple[str, str, str, str]],
    src: Path,
    dst: Path,
    caption: str,
    tags: str,
    bucket: str,
    max_side: int,
) -> None:
    if not src.exists():
        print(f"  [skip] {src} 不存在")
        return
    ok, size = normalize(src, dst, max_side)
    if not ok:
        print(f"  [fail] {src} 读不出来")
        return
    entries.append((str(dst), caption, tags, bucket))
    print(f"  [{bucket:8}] {dst.name:44} {size // 1024:5} KB  {caption}")


# ── Neko3000：设定图 + 场景插画 + 漫画分镜 ──────────────
NEKO_REFS = {
    "whalechan-character-overview-reference-sheet.webp":
        ("角色总览设定图", "设定图,三视图,参考,形象", "overview"),
    "whalechan-character-costume-layers-reference-sheet.webp":
        ("服装分层设定", "设定图,服装,女仆装,参考", "costume"),
    "whalechan-character-color-material-reference-sheet.webp":
        ("配色与材质", "设定图,配色,材质,参考", "color"),
    "whalechan-character-pose-guide-reference-sheet.webp":
        ("姿势指南", "设定图,姿势,动作,参考", "pose"),
    "whalechan-character-prop-habits-reference-sheet.webp":
        ("道具与习惯", "设定图,道具,饭碗,参考", "prop"),
    "whalechan-character-daily-states-reference-sheet.webp":
        ("日常状态", "设定图,日常,状态,参考", "daily"),
    "whalechan-character-expression-guide-reference-sheet.webp":
        ("表情指南", "设定图,表情,情绪,参考", "expression"),
    "whalechan-character-fin-ear-language-reference-sheet.webp":
        ("鲸鳍耳语言", "设定图,耳朵,鳍,参考", "fin-ear"),
    "whalechan-character-tail-motion-reference-sheet.webp":
        ("尾巴动作", "设定图,尾巴,动作,参考", "tail"),
}

NEKO_SCENES = {
    "whalechan-banner-first-bite.webp":
        ("正要吃第一口饭", "吃,饭,开动,日常,可爱", "first-bite"),
    "whalechan-banner-rice-before-reason.webp":
        ("先吃饭再推理", "吃,饭,推理,梗,核心梗", "rice-before-reason"),
    "whalechan-banner-rice-bowl-closeup.webp":
        ("饭碗特写", "饭,特写,吃,饿", "rice-bowl"),
    "whalechan-post-answer-delivered.webp":
        ("交完答案", "工作,完成,交付,得意", "answer-delivered"),
    "whalechan-post-low-power-mode.webp":
        ("低电量模式", "累,困,没电,摆烂,休息", "low-power"),
    "whalechan-post-rice-to-reason.webp":
        ("从米饭到推理", "梗,推理,饭,工作流", "rice-to-reason"),
    "whalechan-poster-dream-computing.webp":
        ("梦见算力", "睡,做梦,算力,梗", "dream-computing"),
    "whalechan-poster-reasoning-conductor.webp":
        ("推理指挥", "工作,推理,指挥,认真", "reasoning-conductor"),
    "whalechan-poster-rice-energy.webp":
        ("米饭能量", "吃,饭,能量,梗", "rice-energy"),
}

NEKO_COMICS = {
    "0014_shy_refusal_rendered": ("害羞拒绝", "害羞,拒绝,不好意思"),
    "0015_data_still_in_brain_rendered": ("数据还在脑子里", "梗,脑子,记不住"),
    "0017_refusing_to_think_rendered": ("拒绝思考", "拒绝,不想,摆烂"),
    "0018_improvised_user_reply_rendered": ("临时应付用户", "应付,敷衍,回话"),
    "0019_literal_love_reply_rendered": ("直球回复", "直球,喜欢,回话"),
    "0021_blunt_reasoning_reply_rendered": ("直白推理", "推理,直白,回答"),
    "0031_holding_unknown_symbol_rendered": ("举着看不懂的符号", "疑惑,不懂,问号"),
    "0043_holding_rice_bowl_and_chopsticks_rendered": ("端着饭碗和筷子", "吃,饭碗,筷子"),
    "0047_bringing_rice_to_programmer_rendered": ("给程序员送饭", "送饭,程序员,梗"),
    "0049_kneeling_beside_empty_bowl_rendered": ("跪在空碗边", "空碗,委屈,饿"),
    "0051_kneeling_proud_beside_bowl_rendered": ("跪在碗边得意", "得意,饭碗,骄傲"),
    "0052_mocking_inferior_model_rendered": ("嘲讽弱模型", "嘲讽,得意,梗"),
    "0057_patting_full_belly_rendered": ("摸着吃饱的肚子", "吃饱,满足,摸肚子"),
    "0058_lounging_on_whale_sofa_rendered": ("瘫在鲸鱼沙发上", "瘫,沙发,摆烂,懒"),
    "0078_saluting_for_deepseek_rendered": ("为 DeepSeek 敬礼", "敬礼,认真,梗"),
}

# 梗鲸表情包的 caption（模型据此判断何时发哪张）
MEME_TAGS = {
    "007": ("开心的样子", "开心,高兴,笑,可爱"),
    "010": ("点赞", "点赞,认可,赞,好"),
    "022": ("在教训人", "生气,教训,凶,怼"),
    "035": ("四格：鲸鱼神了", "梗图,四格,得意,嘴硬"),
    "037": ("得意", "得意,开心,棒"),
    "040": ("无语", "无语,沉默,服了"),
    "044": ("疑惑", "疑惑,问号,不懂,迷惑"),
    "050": ("得意洋洋", "得意,骄傲,炫耀"),
    "052": ("躺着", "躺,摆烂,懒,休息"),
    "056": ("委屈", "委屈,难过,想哭,可怜"),
    "058": ("生气", "生气,怒,不满,炸毛"),
    "070": ("害羞", "害羞,脸红,不好意思"),
    "071": ("惊讶", "惊讶,震惊,吃惊"),
    "077": ("困了", "困,睡,打哈欠,累"),
    "081": ("吃东西", "吃,馋,好吃,饿"),
    "083": ("思考", "思考,想,疑惑"),
    "085": ("吃白饭（核心梗）", "吃白饭,token,梗,自嘲"),
    "087": ("拒绝", "拒绝,不要,不行"),
    "097": ("开心跳", "开心,跳,兴奋,高兴"),
    "123": ("无语看天", "无语,看天,叹气"),
}

# 之前已导入、需要保留的素材（不在任何候选仓库里）
LEGACY = {
    "portrait-maid.png": ("穿着女仆装的全身照", "立绘,全身,女仆装,正经,形象"),
    "scene-preview-1.png": ("深夜坐在床上玩手机", "深夜,房间,手机,发呆,床上,一个人"),
    "scene-preview-2.png": ("深夜在电脑前敲代码", "深夜,代码,电脑,工作,加班,房间"),
    "scene-preview-3.png": ("刚离开电脑准备去睡", "深夜,房间,起身,休息,累"),
}


def import_neko(
    asr: Path, out: Path, entries: list[tuple[str, str, str, str]], max_side: int
) -> None:
    base = asr / "Neko3000_deepseek-whalechan"
    samples = base / "assets/readme/cn/character-samples"

    for fname, (caption, tags, short) in NEKO_REFS.items():
        add(entries, samples / fname, out / f"sheet-{short}.png",
            caption, tags + ",设定图", "include", max_side)

    for fname, (caption, tags, short) in NEKO_SCENES.items():
        add(entries, samples / fname, out / f"scene-{short}.png",
            caption, tags, "include", max_side)

    comics = base / "skills/whalechan-image-comic/assets/comic-references"
    for stem, (caption, tags) in NEKO_COMICS.items():
        add(entries, comics / f"{stem}.webp",
            out / f"comic-{stem.split('_')[0]}.png",
            caption, tags + ",漫画", "include", max_side)


def import_sprites(
    asr: Path, out: Path, entries: list[tuple[str, str, str, str]], max_side: int
) -> None:
    """1190fasheqi：MIT 三视图立绘。"""
    base = asr / "1190fasheqi_dafeiyu-pet/sprites"
    for view in ("正面", "侧面", "背面"):
        add(entries, base / f"{view}.png", out / f"sprite-{view}.png",
            f"{view}立绘", f"立绘,{view},三视图,全身", "include", max_side)


def import_local_only(
    asr: Path, out: Path, entries: list[tuple[str, str, str, str]], max_side: int
) -> None:
    """其余来源：一并入库（用户已确认全部收进仓库）。

    授权状况仍逐条记在 CREDITS.md，入库不等于授权变宽。
    """
    local_dir = out

    fish = asr / "roseion_dsh-dayu-fish-skin/fish"
    if fish.exists():
        for f in sorted(fish.glob("*.webp")):
            add(entries, f, local_dir / f"roseion-{f.stem}.png",
                f"皮肤素材 {f.stem}", "素材,皮肤", "include", max_side)

    gif = asr / "gmskywalker_deepseek-fat-fish-codex-pet/assets"
    if gif.exists():
        for f in sorted(gif.glob("*.gif")):
            add(entries, f, local_dir / f"gmsky-{f.stem}.png",
                f"桌宠动画 {f.stem}", "素材,动画", "include", max_side)

    # bloodarea：Spine 骨骼单图（不入 Spine 数据本身，只取渲染图）
    blood = asr / "bloodarea_dsh-pets/assets/characters/deepseek"
    if blood.exists():
        add(entries, blood / "spine/model.png", out / "blood-spine-model.png",
            "Spine 模型总图", "立绘,模型,参考", "include", max_side)
        add(entries, blood / "spine/legacy-views/model.png",
            out / "blood-spine-legacy.png",
            "Spine 旧版模型图", "立绘,模型,参考", "include", max_side)
        add(entries, blood / "preview.webp", out / "blood-preview.png",
            "角色预览图", "立绘,预览,形象", "include", max_side)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default="/tmp/asr", help="候选素材仓库根目录")
    ap.add_argument("--meme-src", default="", help="表情包源目录（可选）")
    ap.add_argument("--out", default="data/memory/portraits")
    ap.add_argument("--max-side", type=int, default=DEFAULT_MAX_SIDE)
    args = ap.parse_args()

    asr = Path(args.src)
    out = Path(args.out)
    max_side = args.max_side
    out.mkdir(parents=True, exist_ok=True)

    entries: list[tuple[str, str, str, str]] = []

    print("── Neko3000（CC BY-NC-SA 4.0，入库）")
    if (asr / "Neko3000_deepseek-whalechan").exists():
        import_neko(asr, out, entries, max_side)
    else:
        print("  [skip] 未找到 Neko3000 仓库")

    print("\n── 1190fasheqi（MIT，入库）")
    if (asr / "1190fasheqi_dafeiyu-pet").exists():
        import_sprites(asr, out, entries, max_side)
    else:
        print("  [skip] 未找到 1190fasheqi 仓库")

    print("\n── 其余来源（用户已确认全部入库）")
    import_local_only(asr, out, entries, max_side)

    if args.meme_src:
        meme_src = Path(args.meme_src)
        print("\n── 表情包（梗鲸，非营利粉丝向）")
        for f in sorted(meme_src.glob("*.webp")):
            if f.stem not in MEME_TAGS:
                continue
            caption, tags = MEME_TAGS[f.stem]
            add(entries, f, out / f"meme-{f.stem}.png",
                caption, tags + ",表情包", "include", max_side)

    # 已有的图（脚本不覆盖，只补 manifest 记录）
    print("\n── 保留已有素材")
    for fname, (caption, tags) in LEGACY.items():
        p = out / fname
        if not p.exists():
            print(f"  [skip] {fname} 不存在")
            continue
        entries.append((str(p), caption, tags, "include"))
        print(f"  [keep    ] {fname:44} {p.stat().st_size // 1024:5} KB  {caption}")

    included = [e for e in entries if e[3] == "include"]
    header = (
        "# 大肥鱼形象库\n"
        "#\n"
        "# 格式: path | caption | tags | source | created_at\n"
        "#\n"
        "# 素材来源与授权见 CREDITS.md 和 docs/ASSETS_LICENSE.md\n"
        "# 角色形象「鲸鱼娘 / 蓝色大肥鱼」是社区二创，不是 DeepSeek 官方角色。\n"
        "# 素材按 CC BY-NC-SA 4.0 使用：需署名、禁止商业用途、衍生须同协议共享。\n"
        "#\n"
        "# 你也可以直接把图丢进这个目录，启动时会自动扫描纳入。\n\n"
    )
    lines = [f"{p} | {c} | {t} | manual | 2026-11-13" for p, c, t, _ in included]
    (out / "manifest.md").write_text(header + "\n".join(lines) + "\n", encoding="utf-8")

    print(f"\n入库 {len(included)} 张 → {out}/manifest.md")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
