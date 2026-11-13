#!/usr/bin/env python3
"""SILK → WAV 转换器。

QQ 的语音消息是腾讯自有 SILK 格式，STT 服务（SiliconFlow 等）不支持，
必须先转成 PCM/WAV。

决策序列（按优先级，第一个成功的就用）：
  1. ffmpeg —— 最快最稳，但需要系统安装（apt install ffmpeg）
  2. pilk   —— 纯 Python，pip 可装，无需 sudo，速度稍慢
  3. 失败   —— 明确报错，让上层降级为「[语音消息转写失败]」

为什么这个顺序：
  ffmpeg 是成熟 C 实现，对各种 SILK 变体兼容性最好；
  pilk 是纯 Python 兜底，保证没有 sudo 权限时也能工作。

用法:
    python3 tools/silk2wav.py input.silk output.wav
    python3 tools/silk2wav.py input.silk output.wav --rate 16000
    python3 tools/silk2wav.py --check          # 只检查可用后端

退出码: 0 成功；1 失败
"""
from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

# 采样率选择说明：
#   STT 模型通常按 16kHz 训练，采样率过高无益且文件更大
DEFAULT_RATE = 16000


def has_ffmpeg() -> bool:
    return shutil.which("ffmpeg") is not None


def has_pilk() -> bool:
    try:
        import pilk  # noqa: F401
        return True
    except ImportError:
        return False


def convert_ffmpeg(src: Path, dst: Path, rate: int) -> bool:
    """用 ffmpeg 转。ffmpeg 原生支持 SILK 容器。"""
    cmd = [
        "ffmpeg", "-y", "-loglevel", "error",
        "-i", str(src),
        "-ar", str(rate), "-ac", "1",
        str(dst),
    ]
    try:
        r = subprocess.run(cmd, capture_output=True, timeout=60)
        return r.returncode == 0 and dst.exists() and dst.stat().st_size > 0
    except Exception:
        return False


def convert_pilk(src: Path, dst: Path, rate: int) -> bool:
    """用 pilk（纯 Python）解码 SILK。

    pilk 提供多种 API，优先用最稳的 silk_to_wav（一步到位），
    再退回 decode + 手动包 WAV 头。
    """
    try:
        import pilk
    except ImportError:
        return False

    # 首选：silk_to_wav（内部已处理采样率与 WAV 头）
    if hasattr(pilk, "silk_to_wav"):
        try:
            pilk.silk_to_wav(str(src), str(dst), pcm_rate=rate)
            if dst.exists() and dst.stat().st_size > 44:
                return True
        except TypeError:
            try:
                pilk.silk_to_wav(str(src), str(dst))
                if dst.exists() and dst.stat().st_size > 44:
                    return True
            except Exception as e:
                print(f"[!] silk_to_wav 失败: {e}", file=sys.stderr)
        except Exception as e:
            print(f"[!] silk_to_wav 失败: {e}", file=sys.stderr)

    # 退回：decode 成裸 PCM 再包 WAV
    tmp_pcm = dst.with_suffix(".pcm")
    try:
        try:
            pilk.decode(str(src), str(tmp_pcm), pcm_rate=rate)
        except TypeError:
            pilk.decode(str(src), str(tmp_pcm), rate)
    except Exception as e:
        print(f"[!] pilk decode 失败: {e}", file=sys.stderr)
        return False

    if not tmp_pcm.exists() or tmp_pcm.stat().st_size == 0:
        return False

    ok = wrap_wav(tmp_pcm, dst, rate)
    tmp_pcm.unlink(missing_ok=True)
    return ok


def wrap_wav(pcm: Path, wav: Path, rate: int) -> bool:
    """给裸 PCM(s16le 单声道) 加上 WAV 头。"""
    import struct

    data = pcm.read_bytes()
    if not data:
        return False

    channels = 1
    bits = 16
    byte_rate = rate * channels * bits // 8
    block_align = channels * bits // 8
    data_size = len(data)

    with wav.open("wb") as f:
        f.write(b"RIFF")
        f.write(struct.pack("<I", 36 + data_size))
        f.write(b"WAVE")
        f.write(b"fmt ")
        f.write(struct.pack("<I", 16))           # fmt chunk size
        f.write(struct.pack("<H", 1))            # PCM
        f.write(struct.pack("<H", channels))
        f.write(struct.pack("<I", rate))
        f.write(struct.pack("<I", byte_rate))
        f.write(struct.pack("<H", block_align))
        f.write(struct.pack("<H", bits))
        f.write(b"data")
        f.write(struct.pack("<I", data_size))
        f.write(data)
    return wav.exists() and wav.stat().st_size > 44


def main() -> int:
    ap = argparse.ArgumentParser(description="SILK → WAV")
    ap.add_argument("src", type=Path, nargs="?")
    ap.add_argument("dst", type=Path, nargs="?")
    ap.add_argument("--rate", type=int, default=DEFAULT_RATE, help="采样率，默认 16000")
    ap.add_argument("--check", action="store_true", help="只检查可用后端")
    args = ap.parse_args()

    # --check：报告后端可用性，供运维排查
    if args.check:
        ff = has_ffmpeg()
        pk = has_pilk()
        print(f"ffmpeg: {'可用' if ff else '缺失'}")
        print(f"pilk:   {'可用' if pk else '缺失'}")
        if ff or pk:
            print("SILK 转换可用")
            return 0
        print("SILK 转换不可用（需要 ffmpeg 或 pip install pilk）")
        return 1

    if args.src is None or args.dst is None:
        ap.error("需要 src 和 dst 参数（或用 --check）")

    if not args.src.exists():
        print(f"[x] 源文件不存在: {args.src}", file=sys.stderr)
        return 1

    head = args.src.read_bytes()[:10]
    is_silk = head.startswith(b"#!SILK") or (
        len(head) > 1 and head[0] == 0x02 and b"#!SILK" in head
    )
    if not is_silk:
        # 不是 SILK，直接复制（可能已经是 wav/mp3）
        args.dst.write_bytes(args.src.read_bytes())
        print(f"[i] 非 SILK 格式，直接复制到 {args.dst}")
        return 0

    # 决策序列：ffmpeg → pilk
    if has_ffmpeg() and convert_ffmpeg(args.src, args.dst, args.rate):
        print(f"[✓] ffmpeg 转换成功: {args.dst}")
        return 0

    if convert_pilk(args.src, args.dst, args.rate):
        print(f"[✓] pilk 转换成功: {args.dst}")
        return 0

    print("[x] 转换失败：既没有可用的 ffmpeg，pilk 也解不了", file=sys.stderr)
    print("    装其一即可：apt install ffmpeg 或 pip install pilk", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
