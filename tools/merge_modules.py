#!/usr/bin/env python3
"""把「接口 .cppm + 实现 .cpp」合并成单文件模块（clang C++20）。

为什么要合并：
clang 不允许模块实现单元 import 自己，而实现体又需要看到接口里的声明。
把实现放进 `module :private;` 片段是标准做法，也避免两套命名空间拼接。

产物：src/fish.<mod>.cppm  (接口 + private 实现)
      src/fish.<mod>.cpp   删除
"""

import re
import sys
from pathlib import Path

# 模块 -> (接口文件, 实现文件列表, 命名空间)
MODULES = {
    "fish.core": ("src/fish.core.cppm", [], "fish::core"),
    "fish.log": ("src/fish.log.cppm", ["src/log.cpp"], "fish::log"),
    "fish.config": ("src/fish.config.cppm", ["src/config.cpp"], "fish::config"),
    "fish.memory": (
        "src/fish.memory.cppm",
        ["src/working_memory.cpp", "src/notes_store.cpp", "src/index_store.cpp",
         "src/consolidator.cpp", "src/memory_system.cpp"],
        "fish::memory",
    ),
    "fish.capability": (
        "src/fish.capability.cppm",
        ["src/http_client.cpp", "src/providers.cpp"],
        "fish::capability",
    ),
    "fish.transport": (
        "src/fish.transport.cppm",
        ["src/local_channel.cpp", "src/qq_official.cpp", "src/qq_onebot.cpp",
         "src/wechat_ilink.cpp"],
        "fish::transport",
    ),
    "fish.orchestrator": (
        "src/fish.orchestrator.cppm",
        ["src/pipeline.cpp", "src/reply_budget.cpp", "src/layered_persona.cpp"],
        "fish::orchestrator",
    ),
}

INC_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]\s*$')


def split_impl(path: Path):
    """返回 (std_incs, other_incs, body_lines)，剔除 fish/ 头与 module 声明。"""
    text = path.read_text(encoding="utf-8")
    std_incs, other_incs, body = [], [], []
    skip_module_decl = True

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
        # 去掉老的 module/import 行
        if skip_module_decl and (line.startswith("module;") or line.startswith("export module")
                                 or line.startswith("import ") or line.strip() == ""):
            continue
        skip_module_decl = False
        body.append(line)
    return std_incs, other_incs, body


def main(root: Path) -> None:
    for module, (iface, impls, ns) in MODULES.items():
        if not impls:
            print(f"{module}: interface-only, kept")
            continue

        iface_path = root / iface
        iface_text = iface_path.read_text(encoding="utf-8")

        # 从接口里抽出现有的全局片段 include，与实现合并去重
        all_std, all_other = [], []
        iface_imports = []
        iface_body = []
        for line in iface_text.splitlines():
            m = INC_RE.match(line)
            if m:
                h = m.group(1)
                if h.startswith("fish/"):
                    continue
                if h.startswith(("nlohmann/", "curl/", "sqlite")) or "/" in h:
                    all_other.append(h)
                else:
                    all_std.append(h)
                continue
            if line.startswith("module;") or line.startswith("export module"):
                continue
            if line.startswith("import "):
                iface_imports.append(line.strip())   # 保留 import
                continue
            iface_body.append(line)

        impl_bodies = []
        for rel in impls:
            p = root / rel
            if not p.exists():
                continue
            s, o, b = split_impl(p)
            all_std += s
            all_other += o
            impl_bodies.append("\n".join(b).strip())

        out = ["module;"]
        out += [f"#include <{h}>" for h in sorted(set(all_std))]
        out += [f"#include <{h}>" for h in sorted(set(all_other))]
        out += ["", f"export module {module};", ""]
        out += iface_imports
        out += [""]
        out += [ln for ln in iface_body]
        out += ["", "// ── 实现（private 片段，不导出）────────────────────", "module :private;", ""]
        out += ["\n\n".join(impl_bodies)]

        iface_path.write_text("\n".join(out) + "\n", encoding="utf-8")
        for rel in impls:
            p = root / rel
            if p.exists():
                p.unlink()
        print(f"{module}: merged {len(impls)} impl file(s)")


if __name__ == "__main__":
    main(Path(sys.argv[1]))
