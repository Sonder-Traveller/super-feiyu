#!/usr/bin/env python3
"""super-feiyu 控制面 —— 跨平台 GUI。

桌面：原生窗口
Android/iOS：打包成 APP（flet build apk）

设计要点：
  - 连接地址由用户自己填完整 URL，不预置任何默认主机/端口
  - 宽屏（>=800px）左列表 + 右编辑；窄屏单栏切换
  - 网络请求在后台线程跑，不阻塞 UI
  - token 存本地，不硬编码
"""
from __future__ import annotations

import json
import os
import threading
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

import flet as ft

# 只保留环境变量覆盖能力，不设任何默认地址。
# 没填 URL 就用不了——这是刻意的：控制面地址因部署而异，
# 预置一个默认值只会让人误以为连上了。
ENV_URL = os.environ.get("FEIYU_URL", "")


def _data_dir() -> Path:
    """找一个可写的本地目录存 token 和配置。移动端 Path.home() 可能不可写。"""
    for cand in (
        Path.home() / ".super-feiyu",
        Path(os.environ.get("FLET_APP_STORAGE_DATA", "/tmp")) / "super-feiyu",
        Path("/tmp") / "super-feiyu",
    ):
        try:
            cand.mkdir(parents=True, exist_ok=True)
            probe = cand / ".w"
            probe.write_text("x", encoding="utf-8")
            probe.unlink()
            return cand
        except Exception:
            continue
    return Path("/tmp")


DATA_DIR = _data_dir()
TOKEN_FILE = DATA_DIR / "token"
CONFIG_FILE = DATA_DIR / "config.json"


def normalize_url(raw: str) -> str:
    """把用户填的地址规范成 base URL。

    接受这些写法：
        http://host:port
        host:port             → 补 http://
        host                  → 补 http://，用默认端口
        https://host

    末尾多余的 / 和路径会被去掉（API 路径由代码拼）。
    """
    s = (raw or "").strip()
    if not s:
        raise ValueError("请填写控制面地址")
    if "://" not in s:
        s = "http://" + s
    parsed = urllib.parse.urlsplit(s)
    if not parsed.hostname:
        raise ValueError("地址格式不对，应形如 http://主机:端口")
    if parsed.scheme not in ("http", "https"):
        raise ValueError("只支持 http:// 或 https://")
    netloc = parsed.hostname
    if parsed.port:
        netloc = f"{netloc}:{parsed.port}"
    return f"{parsed.scheme}://{netloc}"


class ApiError(Exception):
    pass


class Api:
    def __init__(self, base: str, token: str) -> None:
        self.base = base.rstrip("/")
        self.token = token

    def _req(self, method: str, path: str, body: dict | None = None) -> dict:
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(
            self.base + path,
            data=data,
            method=method,
            headers={
                "Authorization": f"Bearer {self.token}",
                "Content-Type": "application/json",
            },
        )
        try:
            with urllib.request.urlopen(req, timeout=12) as resp:
                raw = resp.read().decode("utf-8", "replace")
        except urllib.error.HTTPError as e:
            detail = ""
            try:
                detail = e.read().decode("utf-8", "replace")[:160]
            except Exception:
                pass
            raise ApiError(f"HTTP {e.code} {detail}") from e
        except urllib.error.URLError:
            raise ApiError(f"连不上 {self.base}") from None
        except Exception as e:
            raise ApiError(str(e)) from e
        try:
            return json.loads(raw) if raw.strip() else {}
        except json.JSONDecodeError:
            raise ApiError("响应不是 JSON") from None

    def health(self) -> dict:
        return self._req("GET", "/healthz")

    def get_default(self) -> str:
        return self._req("GET", "/v1/persona/default").get("persona", "")

    def set_default(self, text: str) -> None:
        self._req("PUT", "/v1/persona/default", {"persona": text})

    def get_session(self, conv_id: str) -> str:
        q = urllib.parse.quote(conv_id, safe="")
        return self._req("GET", f"/v1/persona/session?conv_id={q}").get("persona", "")

    def set_session(self, conv_id: str, text: str) -> None:
        q = urllib.parse.quote(conv_id, safe="")
        self._req("PUT", f"/v1/persona/session?conv_id={q}", {"persona": text})

    def clear_session(self, conv_id: str) -> None:
        q = urllib.parse.quote(conv_id, safe="")
        self._req("DELETE", f"/v1/persona/session?conv_id={q}")


# ── 本地存储 ────────────────────────────────────────────


def load_config() -> dict:
    try:
        if CONFIG_FILE.exists():
            return json.loads(CONFIG_FILE.read_text(encoding="utf-8"))
    except Exception:
        pass
    return {}


def save_config(cfg: dict) -> None:
    try:
        CONFIG_FILE.write_text(json.dumps(cfg, ensure_ascii=False, indent=2), "utf-8")
    except Exception:
        pass


def load_token() -> str:
    try:
        if TOKEN_FILE.exists():
            return TOKEN_FILE.read_text(encoding="utf-8").strip()
    except Exception:
        pass
    return ""


def save_token(token: str) -> None:
    try:
        TOKEN_FILE.write_text(token, encoding="utf-8")
        os.chmod(TOKEN_FILE, 0o600)
    except Exception:
        pass


# ── 界面 ────────────────────────────────────────────────


def main(page: ft.Page) -> None:
    cfg = load_config()
    page.title = "super-feiyu 控制面"
    page.theme_mode = ft.ThemeMode.DARK
    page.padding = 0
    page.theme = ft.Theme(color_scheme_seed=ft.Colors.BLUE)

    state = {
        "url": cfg.get("url", ENV_URL),
        "token": load_token(),
        "api": None,
        "sessions": cfg.get("sessions", []),
        "editing": "default",
        "dirty": False,
        "connected": False,
    }

    # ── 控件 ──
    status_dot = ft.Container(width=10, height=10, border_radius=5,
                              bgcolor=ft.Colors.GREY_600)
    status_text = ft.Text("未连接", size=12, color=ft.Colors.GREY_400)

    editor = ft.TextField(
        multiline=True,
        min_lines=10,
        max_lines=40,
        expand=True,
        text_size=14,
        hint_text="在这里写人格设定。留空表示没有设定。",
        on_change=lambda e: _mark_dirty(),
    )

    title_text = ft.Text("全局默认人格", size=18, weight=ft.FontWeight.BOLD)
    subtitle_text = ft.Text("所有会话默认使用这份设定", size=12,
                            color=ft.Colors.GREY_400)

    session_list = ft.Column(spacing=2, scroll=ft.ScrollMode.AUTO, expand=True)

    save_btn = ft.FilledButton("保存", icon=ft.Icons.SAVE, on_click=lambda e: _save())
    revert_btn = ft.OutlinedButton("撤销", on_click=lambda e: _load_current())
    clear_btn = ft.TextButton("清除覆盖", on_click=lambda e: _clear())

    snack = ft.SnackBar(content=ft.Text(""))
    busy = ft.ProgressRing(width=16, height=16, stroke_width=2, visible=False)

    # ── 工具函数 ──

    def _toast(msg: str, ok: bool = True) -> None:
        snack.content = ft.Text(msg, color=ft.Colors.WHITE)
        snack.bgcolor = ft.Colors.GREEN_700 if ok else ft.Colors.RED_700
        page.show_dialog(snack)

    def _mark_dirty() -> None:
        state["dirty"] = True
        title_text.value = _title_for(state["editing"]) + " •"
        page.update()

    def _title_for(key: str) -> str:
        return "全局默认人格" if key == "default" else "会话人格"

    def _subtitle_for(key: str) -> str:
        if key == "default":
            return "所有会话默认使用这份设定"
        return f"{key}　—　只影响这个会话"

    def _run_async(fn, on_done=None) -> None:
        busy.visible = True
        page.update()

        def worker():
            err = None
            try:
                fn()
            except (ApiError, ValueError) as e:
                err = str(e)
            except Exception as e:
                err = str(e)
            busy.visible = False
            try:
                page.update()
            except Exception:
                pass
            if on_done:
                try:
                    on_done(err)
                except Exception:
                    pass

        threading.Thread(target=worker, daemon=True).start()

    def _refresh_status() -> None:
        def do():
            state["api"].health()

        def done(err):
            if err:
                status_dot.bgcolor = ft.Colors.RED_500
                status_text.value = err[:60]
                state["connected"] = False
            else:
                status_dot.bgcolor = ft.Colors.GREEN_500
                status_text.value = f"已连接 {state['api'].base}"
                state["connected"] = True
            page.update()

        _run_async(do, done)

    def _load_current() -> None:
        api = state["api"]
        if api is None:
            _toast("还没连接", ok=False)
            return
        key = state["editing"]

        def do():
            text = api.get_default() if key == "default" else api.get_session(key)
            editor.value = text
            state["dirty"] = False
            title_text.value = _title_for(key)
            subtitle_text.value = _subtitle_for(key)
            clear_btn.visible = key != "default"
            page.update()

        def done(err):
            if err:
                _toast(f"读取失败：{err}", ok=False)

        _run_async(do, done)

    def _save() -> None:
        api = state["api"]
        if api is None:
            _toast("还没连接", ok=False)
            return
        key = state["editing"]
        text = editor.value or ""

        def do():
            if key == "default":
                api.set_default(text)
            else:
                api.set_session(key, text)
            state["dirty"] = False
            title_text.value = _title_for(key)
            page.update()

        def done(err):
            if err:
                _toast(f"保存失败：{err}", ok=False)
            else:
                _toast("已保存，下一条消息生效")

        _run_async(do, done)

    def _clear() -> None:
        api = state["api"]
        if api is None or state["editing"] == "default":
            return

        def do():
            api.clear_session(state["editing"])

        def done(err):
            if err:
                _toast(f"清除失败：{err}", ok=False)
            else:
                _toast("已清除，回到全局默认")
                _load_current()

        _run_async(do, done)

    def _select(key: str) -> None:
        if state["dirty"]:
            _toast("有未保存的修改，先保存或撤销", ok=False)
            return
        state["editing"] = key
        _load_current()
        _rebuild_session_list()
        if (page.width or 1000) < 800:
            show_editor()

    def _persist() -> None:
        save_config({
            "url": state["url"],
            "sessions": state["sessions"],
        })

    def _add_session(e=None) -> None:
        value = (new_conv.value or "").strip()
        if not value:
            return
        if value not in state["sessions"]:
            state["sessions"].append(value)
            _persist()
        new_conv.value = ""
        _rebuild_session_list()
        _select(value)

    def _remove_session(conv_id: str) -> None:
        if conv_id in state["sessions"]:
            state["sessions"].remove(conv_id)
            _persist()
        if state["editing"] == conv_id:
            state["editing"] = "default"
            _load_current()
        _rebuild_session_list()

    def _rebuild_session_list() -> None:
        session_list.controls.clear()

        def item(key: str, label: str, is_default: bool = False) -> ft.Control:
            selected = state["editing"] == key
            row = ft.Container(
                content=ft.Row(
                    [
                        ft.Icon(
                            ft.Icons.HOME if is_default else ft.Icons.CHAT_BUBBLE_OUTLINE,
                            size=16,
                            color=ft.Colors.BLUE_300 if selected else ft.Colors.GREY_500,
                        ),
                        ft.Text(
                            label,
                            size=13,
                            expand=True,
                            overflow=ft.TextOverflow.ELLIPSIS,
                            color=ft.Colors.WHITE if selected else ft.Colors.GREY_300,
                        ),
                    ],
                    spacing=8,
                ),
                padding=ft.Padding.symmetric(horizontal=10, vertical=10),
                border_radius=6,
                bgcolor=ft.Colors.BLUE_900 if selected else None,
                on_click=lambda e, k=key: _select(k),
                ink=True,
            )
            if is_default:
                return row
            return ft.Row(
                [
                    ft.Container(content=row, expand=True),
                    ft.IconButton(
                        ft.Icons.CLOSE,
                        icon_size=14,
                        tooltip="从列表移除（不影响服务端）",
                        on_click=lambda e, k=key: _remove_session(k),
                    ),
                ],
                spacing=0,
            )

        session_list.controls.append(item("default", "全局默认", is_default=True))
        if state["sessions"]:
            session_list.controls.append(
                ft.Container(
                    content=ft.Text("会话覆盖", size=11, color=ft.Colors.GREY_500),
                    padding=ft.Padding.only(left=10, top=12, bottom=4),
                )
            )
            for s in state["sessions"]:
                session_list.controls.append(item(s, s))
        page.update()

    def _connect(e=None) -> None:
        raw = (url_field.value or "").strip()
        token = (token_field.value or "").strip()

        try:
            base = normalize_url(raw)
        except ValueError as ex:
            _toast(str(ex), ok=False)
            return
        if not token:
            _toast("token 不能为空", ok=False)
            return

        state["url"] = base
        state["token"] = token
        state["api"] = Api(base, token)
        url_field.value = base
        save_token(token)
        _persist()

        def do():
            state["api"].health()

        def done(err):
            if err:
                status_dot.bgcolor = ft.Colors.RED_500
                status_text.value = err[:60]
                state["connected"] = False
                page.update()
                _toast(f"连接失败：{err}", ok=False)
                return
            status_dot.bgcolor = ft.Colors.GREEN_500
            status_text.value = f"已连接 {base}"
            state["connected"] = True
            page.update()
            _load_current()
            _rebuild_session_list()

        _run_async(do, done)

    # ── 布局 ──

    url_field = ft.TextField(
        label="控制面地址",
        value=state["url"],
        hint_text="http://主机:端口",
        dense=True,
        expand=True,
        text_size=13,
    )
    token_field = ft.TextField(
        label="Token",
        value=state["token"],
        dense=True,
        password=True,
        can_reveal_password=True,
        expand=True,
        text_size=13,
    )
    new_conv = ft.TextField(
        label="会话 ID",
        hint_text="群聊 g:群号 / 私聊 openid",
        dense=True,
        expand=True,
        text_size=13,
        on_submit=_add_session,
    )

    connect_btn = ft.FilledButton("连接", icon=ft.Icons.LINK,
                                  on_click=lambda e: _connect())

    settings_panel = ft.Container(
        content=ft.Column(
            [
                ft.Row([url_field], spacing=8),
                ft.Row([token_field], spacing=8),
                ft.Row([busy, connect_btn], alignment=ft.MainAxisAlignment.END,
                       spacing=10),
            ],
            spacing=8,
        ),
        padding=12,
        bgcolor=ft.Colors.SURFACE_CONTAINER_HIGHEST,
        border_radius=8,
    )

    sidebar = ft.Container(
        content=ft.Column(
            [
                ft.Row(
                    [
                        ft.Icon(ft.Icons.WATER, color=ft.Colors.BLUE_300, size=20),
                        ft.Text("大肥鱼", size=16, weight=ft.FontWeight.BOLD),
                    ],
                    spacing=6,
                ),
                ft.Divider(height=1),
                ft.Container(content=session_list, expand=True,
                             padding=ft.Padding.only(top=6)),
                ft.Divider(height=1),
                ft.Row(
                    [
                        new_conv,
                        ft.IconButton(ft.Icons.ADD, icon_size=18,
                                      tooltip="添加会话", on_click=_add_session),
                    ],
                    spacing=4,
                ),
            ],
            spacing=8,
        ),
        padding=12,
        width=260,
        bgcolor=ft.Colors.SURFACE_CONTAINER_LOW,
    )

    editor_panel = ft.Container(
        content=ft.Column(
            [
                ft.Row(
                    [
                        ft.Column([title_text, subtitle_text], spacing=2, expand=True),
                        clear_btn,
                    ],
                    alignment=ft.MainAxisAlignment.SPACE_BETWEEN,
                ),
                editor,
                ft.Row([revert_btn, save_btn],
                       alignment=ft.MainAxisAlignment.END, spacing=8),
            ],
            spacing=10,
            expand=True,
        ),
        padding=16,
        expand=True,
    )

    status_bar = ft.Container(
        content=ft.Row(
            [status_dot, status_text, ft.Container(expand=True),
             ft.Text("super-feiyu control", size=11, color=ft.Colors.GREY_600)],
            spacing=8,
        ),
        padding=ft.Padding.symmetric(horizontal=12, vertical=6),
        bgcolor=ft.Colors.SURFACE_CONTAINER_LOWEST,
    )

    body = ft.Container(expand=True)

    def show_list() -> None:
        body.content = ft.Column([sidebar], spacing=0, expand=True,
                                 scroll=ft.ScrollMode.AUTO)
        page.update()

    def show_editor() -> None:
        back = ft.IconButton(ft.Icons.ARROW_BACK, on_click=lambda _: show_list())
        body.content = ft.Column(
            [ft.Row([back, ft.Text("返回", size=13)]), editor_panel],
            expand=True,
        )
        page.update()

    def _layout(e=None) -> None:
        w = page.width or 1000
        if w >= 800:
            body.content = ft.Row(
                [sidebar, ft.VerticalDivider(width=1), editor_panel],
                expand=True,
                spacing=0,
            )
        else:
            show_editor()
        page.update()

    page.on_resize = _layout
    page.add(
        ft.Column(
            [
                ft.Container(content=settings_panel,
                             padding=ft.Padding.only(left=12, right=12, top=12)),
                body,
                status_bar,
            ],
            spacing=8,
            expand=True,
        )
    )

    _rebuild_session_list()
    _layout()

    # 有完整配置才自动连；否则等用户填
    if state["url"] and state["token"]:
        _connect()


if __name__ == "__main__":
    ft.run(main)
