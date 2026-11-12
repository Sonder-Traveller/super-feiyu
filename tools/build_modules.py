#!/usr/bin/env python3
"""把 .cpp 实现合并进模块接口文件，生成单文件模块（clang C++20）。

设计要点（都是踩过坑总结出来的）：
1. 模块实现放 `module :private;` 片段，避免"实现单元 import 自己"的非法写法
2. 模块不能导出宏 → 日志用函数 fish::log::log_line(...)
3. import 不会隐式转导出，下游要用的类型必须出现在 export namespace 里
4. 命名空间要与模块名对齐（fish::memory 等）
"""

import re
import sys
from pathlib import Path

# 模块 -> (接口, 实现列表)
MODULES = [
    ("fish.core", "src/fish.core.cppm", []),
    ("fish.log", "src/fish.log.cppm", []),          # 手写，不合并
    ("fish.config", "src/fish.config.cppm", ["src/config.cpp"]),
    ("fish.memory", "src/fish.memory.cppm",
     ["src/working_memory.cpp", "src/notes_store.cpp", "src/index_store.cpp",
      "src/consolidator.cpp", "src/memory_system.cpp"]),
    ("fish.capability", "src/fish.capability.cppm",
     ["src/http_client.cpp", "src/providers.cpp"]),
    ("fish.transport", "src/fish.transport.cppm",
     ["src/local_channel.cpp", "src/qq_official.cpp", "src/qq_onebot.cpp",
      "src/wechat_ilink.cpp"]),
    ("fish.orchestrator", "src/fish.orchestrator.cppm",
     ["src/pipeline.cpp", "src/reply_budget.cpp", "src/layered_persona.cpp"]),
]

# 实现体里旧命名空间 -> 目标命名空间
NS_MAP = {
    "fish.core": "fish.config",       # config.cpp 原来写在 fish::core
    "fish.store": "fish.memory",      # memory_store.cpp
    "fish.persona": "fish.orchestrator",
}

INC_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]\s*$')
LOG_MACRO_RE = re.compile(r'\b(LOG_DEBUG|LOG_INFO|LOG_WARN|LOG_ERROR)\(')
LV = {"LOG_DEBUG": "kDebug", "LOG_INFO": "kInfo", "LOG_WARN": "kWarn", "LOG_ERROR": "kError"}


def rewrite_impl(body: str, module_ns: str) -> str:
    """统一实现体的命名空间与日志调用。"""
    # 命名空间
    for old, new in NS_MAP.items():
        body = body.replace(f"namespace {old}", f"namespace {new}")
        body = body.replace(f"}}  // namespace {old}", f"}}  // namespace {new}")
    # 日志宏 → 函数
    for mac, lv in LV.items():
        body = body.replace(f"{mac}(",
                            f"::fish::log::log_line(::fish::log::LogLevel::{lv}, ")
    return body


def split_file(path: Path):
    text = path.read_text(encoding="utf-8")
    std_incs, other_incs, body = [], [], []
    for line in text.splitlines():
        m = INC_RE.match(line)
        if m:
            h = m.group(1)
            if h.startswith("fish/"):
                continue
            if h.startswith(("nlohmann/", "curl/", "sqlite")) or "/" in h:
                other_incs.append(h)
            else:
                std_incs.append(h)
            continue
        if line.startswith(("module;", "export module", "import ", "module :private")):
            continue
        body.append(line)
    return std_incs, other_incs, "\n".join(body).strip()


def main(root: Path) -> None:
    for module, iface_rel, impls in MODULES:
        if not impls:
            print(f"{module}: interface-only")
            continue

        iface_path = root / iface_rel
        if not iface_path.exists():
            print(f"{module}: interface missing, skipped")
            continue

        std_all, other_all, iface_imports, iface_body = [], [], [], []
        for line in iface_path.read_text(encoding="utf-8").splitlines():
            m = INC_RE.match(line)
            if m:
                h = m.group(1)
                if h.startswith("fish/"):
                    continue
                if h.startswith(("nlohmann/", "curl/", "sqlite")) or "/" in h:
                    other_all.append(h)
                else:
                    std_all.append(h)
                continue
            if line.startswith(("module;", "export module", "module :private")):
                continue
            if line.startswith("import "):
                iface_imports.append(line.strip())
                continue
            iface_body.append(line)

        module_ns = "fish::" + module.split(".")[-1]
        impl_bodies = []
        for rel in impls:
            p = root / rel
            if not p.exists():
                continue
            s, o, b = split_file(p)
            std_all += s
            other_all += o
            impl_bodies.append(rewrite_impl(b, module_ns))

        out = ["module;"]
        out += [f"#include <{h}>" for h in sorted(set(std_all))]
        out += [f"#include <{h}>" for h in sorted(set(other_all))]
        out += ["", f"export module {module};", ""]
        out += iface_imports
        out += [""] + iface_body
        out += ["", "// ═══ 实现（private 片段，不导出）═══════════════════",
                "module :private;", ""]
        out += ["\n\n".join(impl_bodies)]

        iface_path.write_text("\n".join(out) + "\n", encoding="utf-8")
        for rel in impls:
            p = root / rel
            if p.exists():
                p.unlink()
        print(f"{module}: merged {len(impls)} impl(s)")


if __name__ == "__main__":
    main(Path(sys.argv[1]))
