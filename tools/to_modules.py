#!/usr/bin/env python3
"""把 .cpp 实现文件转换为 C++20 模块实现单元（最终版）。

关键规则（前两版踩过的坑）：
1. 模块实现单元必须 **import 自己**，否则看不到本模块导出区的声明
2. `import` 不会隐式转导出：下游要用到的类型必须用 `export import`
3. 标准库头必须进全局模块片段

第三个规则在接口文件（.cppm）里处理，这里负责前两个，以及
把实现体里的旧命名空间改写成模块自己的命名空间。
"""

import re
import sys
from pathlib import Path

# 文件名 -> (模块名, 命名空间, 需要 import 的模块)
IMPL = {
    "log.cpp": ("fish.log", "fish::log", ["fish.core"]),
    "config.cpp": ("fish.config", "fish::config", ["fish.core", "fish.log"]),
    "reply_budget.cpp": ("fish.orchestrator", "fish::orchestrator",
                         ["fish.core", "fish.memory"]),
    "layered_persona.cpp": ("fish.orchestrator", "fish::orchestrator",
                            ["fish.core", "fish.memory"]),
    "memory_store.cpp": ("fish.memory", "fish::memory", ["fish.core"]),
    "working_memory.cpp": ("fish.memory", "fish::memory", ["fish.core"]),
    "notes_store.cpp": ("fish.memory", "fish::memory", ["fish.core", "fish.log"]),
    "index_store.cpp": ("fish.memory", "fish::memory", ["fish.core", "fish.log"]),
    "consolidator.cpp": ("fish.memory", "fish::memory", ["fish.core", "fish.log"]),
    "memory_system.cpp": ("fish.memory", "fish::memory", ["fish.core", "fish.log"]),
    "http_client.cpp": ("fish.capability", "fish::capability", ["fish.core", "fish.log"]),
    "providers.cpp": ("fish.capability", "fish::capability", ["fish.core", "fish.log"]),
    "local_channel.cpp": ("fish.transport", "fish::transport", ["fish.core"]),
    "qq_official.cpp": ("fish.transport", "fish::transport",
                        ["fish.core", "fish.log", "fish.capability"]),
    "qq_onebot.cpp": ("fish.transport", "fish::transport",
                      ["fish.core", "fish.log", "fish.capability"]),
    "wechat_ilink.cpp": ("fish.transport", "fish::transport",
                         ["fish.core", "fish.log", "fish.capability"]),
    "pipeline.cpp": ("fish.orchestrator", "fish::orchestrator",
                     ["fish.core", "fish.log", "fish.memory"]),
}

# 旧命名空间 -> 所属模块的命名空间
OLD_NS = {
    "fish.log": "fish::core",       # log.cpp 原来写在 fish::core 里
    "fish.memory": "fish::store",   # memory_store.cpp 原来在 fish::store
}

INC_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]\s*$')


def convert(path: Path) -> None:
    name = path.name
    if name not in IMPL:
        print(f"skip {name}")
        return
    module, ns, imports = IMPL[name]
    imports = [i for i in imports if i != module]

    text = path.read_text(encoding="utf-8")
    std_incs, other_incs, body = [], [], []
    seen_body = False

    for line in text.splitlines():
        m = INC_RE.match(line)
        if m:
            header = m.group(1)
            if header.startswith("fish/"):
                continue
            if header.startswith(("nlohmann/", "curl/", "sqlite")) or "/" in header:
                other_incs.append(header)
            else:
                std_incs.append(header)
            continue
        if not line.strip() and not seen_body:
            continue
        seen_body = True
        body.append(line)

    # 旧命名空间改写
    old = OLD_NS.get(module)
    if old is not None and name not in ("log.cpp",):
        body = [ln.replace(f"namespace {old}", f"namespace {ns}")
                .replace(f"}}  // namespace {old}", f"}}  // namespace {ns}")
                for ln in body]

    out = ["module;"]
    out += [f"#include <{i}>" for i in sorted(set(std_incs))]
    out += [f"#include <{i}>" for i in sorted(set(other_incs))]
    out += ["", f"export module {module};", ""]
    out += [f"import {module};"]          # ① 导入自己
    out += [f"import {i};" for i in imports]
    out += [""] + body

    path.write_text("\n".join(out) + "\n", encoding="utf-8")
    print(f"converted {name} -> {module}")


if __name__ == "__main__":
    root = Path(sys.argv[1]) / "src"
    for f in sorted(root.glob("*.cpp")):
        convert(f)
