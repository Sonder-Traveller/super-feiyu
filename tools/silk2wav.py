#!/usr/bin/env python3
"""SILK → WAV 转换器。

QQ 的语音消息是腾讯自有 SILK 格式，STT 服务（SiliconFlow 等）不支持，
必须先转成 PCM/WAV。优先用 ffmpeg（快），没有则用纯 Python 的 pilk。

用法:
    python3 tools/silk2wav.py input.silk output.wav
    python3 tools/silk2wav.py input.silk output.wav --rate 16000

退出码: 0 成功；1 失败
"""
from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path


def has_ffmpeg() -> bool:
    return shutil.which("ffmpeg") is not None


def convert_ffmpeg(src: Path, dst: Path, rate: int) -> bool:
    """用 ffmpeg 转。ffmpeg 原生支持 SILK。"""
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
    ap.add_argument("src", type=Path)
    ap.add_argument("dst", type=Path)
    ap.add_argument("--rate", type=int, default=16000, help="采样率，默认 16000")
    args = ap.parse_args()

    if not args.src.exists():
        print(f"[x] 源文件不存在: {args.src}", file=sys.stderr)
        return 1

    head = args.src.read_bytes()[:10]
    is_silk = head.startswith(b"#!SILK") or (len(head) > 1 and head[0] == 0x02 and b"#!SILK" in head)
    if not is_silk:
        # 不是 SILK，直接复制（可能已经是 wav/mp3）
        args.dst.write_bytes(args.src.read_bytes())
        print(f"[i] 非 SILK 格式，直接复制到 {args.dst}")
        return 0

    if has_ffmpeg() and convert_ffmpeg(args.src, args.dst, args.rate):
        print(f"[✓] ffmpeg 转换成功: {args.dst}")
        return 0

    if convert_pilk(args.src, args.dst, args.rate):
        print(f"[✓] pilk 转换成功: {args.dst}")
        return 0

    print("[x] 转换失败：既没有可用的 ffmpeg，pilk 也解不了", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
