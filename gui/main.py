#!/usr/bin/env python3
"""super-feiyu 控制面 —— 跨平台 GUI（玻璃质感版）。

桌面：原生窗口；Android/iOS：打包成 APP（flet build apk）。

设计来源：参照 DS-API（DeepSeek 官方风格）提取的视觉语言——
品牌靛蓝 #4D6BFE、玻璃卡片（半透明白 + 1px 浅描边 + 大而软的投影）、
大圆角（卡片 14 / 输入区 12）、渐变只用在选中态和主按钮。

功能：
  1. 连接   —— 服务器 URL + token，运行时状态
  2. 人格   —— 全局默认 + 会话覆盖
  3. 渠道   —— QQ 官方（扫码绑定）/ QQ OneBot / 微信（扫码登录）
  4. 模型   —— chat / embedding / stt / tts / search 的 key 与参数

约定：
  - 地址由用户自己填，不预置默认值
  - 密钥在 GUI 里只显示「已设置」，留空 = 不修改，绝不回显明文
  - 所有网络请求在后台线程，不阻塞 UI
"""
from __future__ import annotations

import json
import os
import threading
import urllib.error
import urllib.parse
import urllib.request
import webbrowser
from pathlib import Path

import flet as ft

# ── 视觉 token（提取自 DS-API theme.dart）────────────────
BRAND = "#4D6BFE"        # 品牌靛蓝
BRAND_DEEP = "#3D5AF0"
BRAND_SOFT = "#7189FF"
VIOLET = "#A57BFF"
BRAND_TINT = "#EDF1FF"
INPUT_BG = "#F5F5F5"
TEXT_MAIN = "#1A1A1A"
TEXT_SUB = "#9A9A9A"
TEXT_HINT = "#B0B0B0"
BORDER = "#E6E6E6"
PANEL_BG = "#F7F7F8"
DANGER = "#E5484D"
OK_GREEN = "#22A06B"
PAGE_BG = "#FAFBFF"

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
    """http://h:p / h:p / h / https://h 都收；末尾 / 去掉，路径由代码拼。"""
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

    def _req(self, method: str, path: str, body: dict | None = None,
             timeout: int = 15) -> dict:
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
            with urllib.request.urlopen(req, timeout=timeout) as resp:
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

    # 探活与状态
    def health(self) -> dict:
        return self._req("GET", "/healthz")

    def status(self) -> dict:
        return self._req("GET", "/v1/status")

    # 配置
    def get_config(self) -> dict:
        return self._req("GET", "/v1/config")

    def put_config(self, payload: dict) -> dict:
        return self._req("PUT", "/v1/config", payload, timeout=20)

    def restart(self) -> dict:
        """触发服务优雅重启（systemd 拉起新进程）。保存关键配置后调用。"""
        return self._req("POST", "/v1/restart", {}, timeout=10)

    # 渠道
    def qq_test(self, app_id: str = "", app_secret: str = "") -> dict:
        body = {}
        if app_id:
            body["app_id"] = app_id
        if app_secret:
            body["app_secret"] = app_secret
        return self._req("POST", "/v1/channel/qq/test", body, timeout=25)

    def wechat_test(self) -> dict:
        return self._req("POST", "/v1/channel/wechat/test", {}, timeout=25)

    def qq_qr_create(self) -> dict:
        return self._req("POST", "/v1/channel/qq/qr", {"source": "super-feiyu"},
                         timeout=25)

    def qq_qr_status(self, task_id: str) -> dict:
        q = urllib.parse.quote(task_id, safe="")
        return self._req("GET", f"/v1/channel/qq/qr/status?task_id={q}", timeout=20)

    def wechat_qr_create(self) -> dict:
        return self._req("POST", "/v1/channel/wechat/qr", {}, timeout=25)

    def wechat_qr_status(self, qrcode: str) -> dict:
        q = urllib.parse.quote(qrcode, safe="")
        return self._req("GET", f"/v1/channel/wechat/qr/status?qrcode={q}", timeout=45)

    # WorkBuddy 链接登录：拿授权链接 + 轮询结果 + 桥接层测试
    def workbuddy_login(self) -> dict:
        return self._req("POST", "/v1/capability/workbuddy/login", {}, timeout=35)

    def workbuddy_login_status(self, login_id: str) -> dict:
        q = urllib.parse.quote(login_id, safe="")
        return self._req("GET",
                         f"/v1/capability/workbuddy/login/status?login_id={q}",
                         timeout=45)

    def workbuddy_test(self) -> dict:
        return self._req("POST", "/v1/capability/workbuddy/test", {}, timeout=15)

    # 人格
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

    def persona_list(self) -> dict:
        return self._req("GET", "/v1/persona/list")

    def conversations(self) -> dict:
        return self._req("GET", "/v1/conversations", timeout=15)


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


# ── 界面组件 ────────────────────────────────────────────


def glass(content: ft.Control, padding: int = 16) -> ft.Container:
    """玻璃卡片：半透明白 + 1px 浅描边 + 大而软的投影。"""
    return ft.Container(
        content=content,
        padding=padding,
        bgcolor="#B8FFFFFF",
        border=ft.Border.all(1, "#DBFFFFFF"),
        border_radius=14,
        shadow=ft.BoxShadow(
            blur_radius=34,
            spread_radius=1,
            color="#141F2687",
            offset=ft.Offset(0, 14),
        ),
    )


def section_title(text: str, eyebrow: str = "") -> ft.Column:
    """eyebrow 小字 + 标题，DS-API 空态的排版习惯。"""
    items = []
    if eyebrow:
        items.append(ft.Text(eyebrow.upper(), size=11, color="#8B95B5", weight=ft.FontWeight.W_600))
    items.append(ft.Text(text, size=18, weight=ft.FontWeight.BOLD, color=TEXT_MAIN))
    return ft.Column(items, spacing=2)


def field(label: str, value: str = "", password: bool = False,
          hint: str = "", expand: bool = True, multiline: bool = False,
          on_change=None) -> ft.TextField:
    common = dict(
        label=label,
        value=value,
        hint_text=hint,
        dense=True,
        expand=expand,
        text_size=13,
        border_color=BORDER,
        focused_border_color=BRAND,
        on_change=on_change,
    )
    if multiline:
        common.update(min_lines=3, max_lines=6)
    if password:
        common.update(password=True, can_reveal_password=True)
    return ft.TextField(**common)


def main(page: ft.Page) -> None:
    cfg = load_config()
    page.title = "super-feiyu 控制面"
    page.theme_mode = ft.ThemeMode.LIGHT
    page.padding = 0
    page.bgcolor = PAGE_BG
    page.theme = ft.Theme(
        color_scheme_seed=BRAND,
        font_family="sans-serif",
    )

    state = {
        "url": cfg.get("url", ENV_URL),
        "token": load_token(),
        "api": None,
        "sessions": cfg.get("sessions", []),
        "editing": "default",
        "dirty": False,
        "connected": False,
        "config": None,          # GET /v1/config 的原始 dict
        "qr_polling": False,     # 扫码轮询开关
        "conv_req_id": 0,        # 会话列表请求序号（丢弃过期回调）
    }

    # ═══ 顶部：品牌 + 状态 ═══════════════════════════════
    status_dot = ft.Container(width=10, height=10, border_radius=5,
                              bgcolor=ft.Colors.GREY_400)
    status_text = ft.Text("未连接", size=12, color=TEXT_SUB)

    def _toast(msg: str, ok: bool = True) -> None:
        snack.content = ft.Text(msg, color=ft.Colors.WHITE, size=13)
        snack.bgcolor = OK_GREEN if ok else DANGER
        page.show_dialog(snack)

    snack = ft.SnackBar(content=ft.Text(""), duration=2500)

    busy = ft.ProgressRing(width=16, height=16, stroke_width=2, visible=False)

    def _run_async(fn, on_done=None) -> None:
        busy.visible = True
        page.update()

        def worker():
            err = None
            result = None
            try:
                result = fn()
            except Exception as e:
                err = str(e)
            busy.visible = False
            try:
                page.update()
            except Exception:
                pass
            if on_done:
                try:
                    on_done(err, result)
                except Exception:
                    pass

        threading.Thread(target=worker, daemon=True).start()

    # ═══ 页 1：连接 ═══════════════════════════════════════
    url_field = ft.TextField(
        label="控制面地址", value=state["url"], hint_text="http://主机:端口",
        dense=True, text_size=13, border_color=BORDER, focused_border_color=BRAND,
    )
    token_field = ft.TextField(
        label="Token", value=state["token"], dense=True, password=True,
        can_reveal_password=True, text_size=13,
        border_color=BORDER, focused_border_color=BRAND,
    )
    connect_btn = ft.FilledButton(
        "连接", icon=ft.Icons.LINK, height=40,
        style=ft.ButtonStyle(shape=ft.RoundedRectangleBorder(radius=12)),
        on_click=lambda e: _connect(),
    )
    status_card = ft.Column([], spacing=6)

    def _render_status(info: dict) -> None:
        rows = []
        up = info.get("uptime_s", 0)
        h, m = up // 3600, (up % 3600) // 60
        rows.append(ft.Row([
            ft.Text("运行时长", size=12, color=TEXT_SUB),
            ft.Container(expand=True),
            ft.Text(f"{h}小时{m}分", size=13, color=TEXT_MAIN, weight=ft.FontWeight.W_600),
        ]))
        chs = info.get("channels", [])
        for ch in chs:
            ok = ch.get("connected", False)
            rows.append(ft.Row([
                ft.Container(width=8, height=8, border_radius=4,
                             bgcolor=OK_GREEN if ok else DANGER),
                ft.Text(ch.get("name", "?"), size=12, color=TEXT_MAIN),
                ft.Container(expand=True),
                ft.Text(f"收 {ch.get('received', 0)} · 错 {ch.get('errors', 0)}",
                        size=12, color=TEXT_SUB),
            ]))
        if not chs:
            rows.append(ft.Text("（服务端未启用通道或版本较旧）", size=12, color=TEXT_HINT))
        status_card.controls = rows
        page.update()

    def _refresh_runtime() -> None:
        def do():
            return state["api"].status()

        def done(err, result):
            if err:
                status_card.controls = [ft.Text(f"状态读取失败：{err}", size=12,
                                                color=TEXT_HINT)]
                page.update()
                return
            _render_status(result)

        _run_async(do, done)

    def connect_view() -> ft.Control:
        return ft.Column([
            section_title("连接", "server"),
            ft.Container(height=6),
            glass(ft.Column([
                url_field, token_field,
                ft.Row([busy, ft.Container(expand=True), connect_btn]),
            ], spacing=10)),
            ft.Container(height=14),
            section_title("运行状态", "runtime"),
            ft.Container(height=6),
            glass(status_card, padding=14),
            ft.Container(height=14),
            ft.Text("super-feiyu control · GPL-3.0", size=11, color=TEXT_HINT),
        ], spacing=0, scroll=ft.ScrollMode.AUTO, expand=True)

    # ═══ 页 2：人格 ═══════════════════════════════════════
    editor = ft.TextField(
        multiline=True, min_lines=12, max_lines=40, expand=True, text_size=14,
        border_color=BORDER, focused_border_color=BRAND,
        hint_text="在这里写人格设定。留空表示没有设定。",
        on_change=lambda e: _mark_dirty(),
    )
    persona_title = ft.Text("全局默认人格", size=16, weight=ft.FontWeight.BOLD,
                            color=TEXT_MAIN)
    persona_sub = ft.Text("所有会话默认使用这份设定", size=12, color=TEXT_SUB)
    session_list = ft.Column(spacing=2, scroll=ft.ScrollMode.AUTO)

    def _mark_dirty() -> None:
        state["dirty"] = True
        persona_title.value = _title_for(state["editing"]) + " •"
        page.update()

    def _title_for(key: str) -> str:
        return "全局默认人格" if key == "default" else "会话人格"

    def _load_current() -> None:
        api = state["api"]
        if api is None:
            return
        key = state["editing"]

        def do():
            return api.get_default() if key == "default" else api.get_session(key)

        def done(err, text):
            if err:
                _toast(f"读取失败：{err}", ok=False)
                return
            editor.value = text
            state["dirty"] = False
            persona_title.value = _title_for(key)
            persona_sub.value = ("所有会话默认使用这份设定" if key == "default"
                                 else f"{key} — 只影响这个会话")
            page.update()

        _run_async(do, done)

    def _save_persona() -> None:
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
            if key != "default" and key not in state["sessions"]:
                state["sessions"].append(key)
                _persist()

        def done(err, _):
            if err:
                _toast(f"保存失败：{err}", ok=False)
            else:
                state["dirty"] = False
                persona_title.value = _title_for(key)
                _rebuild_session_list()
                _toast("已保存，下一条消息生效")

        _run_async(do, done)

    def _clear_override() -> None:
        api = state["api"]
        if api is None or state["editing"] == "default":
            return

        def do():
            api.clear_session(state["editing"])

        def done(err, _):
            if err:
                _toast(f"清除失败：{err}", ok=False)
            else:
                editor.value = ""
                _toast("已清除，回到全局默认")

        _run_async(do, done)

    def _select(key: str) -> None:
        if state["dirty"]:
            _toast("有未保存的修改，先保存或撤销", ok=False)
            return
        state["editing"] = key
        _load_current()
        _rebuild_session_list()

    def _persist() -> None:
        save_config({"url": state["url"], "sessions": state["sessions"]})

    def _rebuild_session_list() -> None:
        """人格页左侧列表：bot/群卡片（头像 + 名称 + 唯一标识小字）。

        数据来自服务端会话登记表（/v1/conversations）——机器人见过的
        每个会话都在里面，不需要用户手工输会话 ID。点卡片直接进入
        该会话的人格编辑。服务端拉不到时退回本地保存的会话列表。
        """
        # 请求序号：这个函数在切页、保存人格后都会调，异步回调回来的
        # 先后顺序没有保证。两个回调都往 controls 里 append 就会叠出
        # 重复卡片，所以回调里比对序号，过期的直接丢弃。
        state["conv_req_id"] = (state.get("conv_req_id") or 0) + 1
        req_id = state["conv_req_id"]

        session_list.controls.clear()

        def avatar(label: str) -> ft.Control:
            # 头像：名字首字符画在圆里。取不到字符就用对话气泡图标。
            ch = (label or "").strip()[:1]
            if not ch:
                return ft.Container(
                    width=34, height=34, border_radius=17,
                    bgcolor=BRAND_TINT,
                    content=ft.Icon(ft.Icons.CHAT_BUBBLE_OUTLINE, size=16, color=BRAND),
                )
            return ft.Container(
                width=34, height=34, border_radius=17,
                gradient=ft.LinearGradient(
                    begin=ft.Alignment(-1, -1), end=ft.Alignment(1, 1),
                    colors=[BRAND, "#7C6FF0"],
                ),
                alignment=ft.Alignment(0, 0),
                content=ft.Text(ch, size=15, weight=ft.FontWeight.W_600,
                                color="#FFFFFF"),
            )

        def card(title: str, ident: str, key: str, icon: bool = False) -> ft.Control:
            selected = state["editing"] == key
            name_line = title if title else (ident or key)
            id_line = "" if icon else (ident or key)
            return ft.Container(
                content=ft.Row([
                    avatar(name_line if not icon else ""),
                    ft.Column([
                        ft.Text(("全局默认" if icon else name_line),
                                size=13, weight=ft.FontWeight.W_500,
                                max_lines=1, overflow=ft.TextOverflow.ELLIPSIS,
                                color=TEXT_MAIN if selected else "#3A3A45"),
                        ft.Text(("所有会话的兜底设定" if icon else id_line),
                                size=11, color=TEXT_HINT,
                                max_lines=1, overflow=ft.TextOverflow.ELLIPSIS),
                    ], spacing=1, expand=True),
                    ft.Icon(ft.Icons.CHEVRON_RIGHT, size=14, color=TEXT_HINT),
                ], spacing=10),
                padding=ft.Padding.symmetric(horizontal=10, vertical=8),
                border_radius=10,
                border=ft.Border.all(1, BRAND if selected else BORDER),
                gradient=(ft.LinearGradient(
                    begin=ft.Alignment(-1, 0), end=ft.Alignment(1, 0),
                    colors=[BRAND_TINT, "#B8F5F2FF"],
                ) if selected else None),
                on_click=lambda e, k=key: _select(k),
                ink=True,
            )

        session_list.controls.append(card("", "", "default", icon=True))
        api = state["api"]
        if api is None:
            page.update()
            return

        def do():
            return api.conversations()

        def done(err, result):
            if state.get("conv_req_id") != req_id:
                return   # 已有更新的请求，丢弃这次的结果
            if err or not result:
                # 服务端拉不到 → 退回本地列表，至少能用
                for s in state["sessions"]:
                    session_list.controls.append(card(s, s, s))
                page.update()
                return
            convs = result.get("conversations", []) or []
            for c in convs:
                key = c.get("key", "")
                if not key:
                    continue
                title = c.get("title", "") or key
                # 小字：唯一标识；群聊带渠道前缀更清楚
                ident = key
                ch = c.get("channel", "")
                if ch:
                    ident = f"{ch} · {key}"
                session_list.controls.append(card(title, ident, key))
            page.update()

        _run_async(do, done)

    def persona_view() -> ft.Control:
        return ft.Column([
            section_title("人格", "persona"),
            ft.Container(height=6),
            ft.Row([
                ft.Container(width=250, content=ft.Column([
                    session_list,
                ], spacing=8),
                            visible=(page.width or 1000) >= 700),
                ft.Container(width=12, visible=(page.width or 1000) >= 700),
                ft.Column([
                    ft.Row([
                        ft.Column([persona_title, persona_sub], spacing=2, expand=True),
                        ft.TextButton("清除覆盖", icon_color=DANGER,
                                      on_click=lambda e: _clear_override()),
                    ]),
                    editor,
                    ft.Row([
                        ft.OutlinedButton("撤销", on_click=lambda e: _load_current(),
                                          style=ft.ButtonStyle(
                                              shape=ft.RoundedRectangleBorder(radius=12))),
                        ft.FilledButton("保存", icon=ft.Icons.SAVE,
                                        on_click=lambda e: _save_persona(),
                                        style=ft.ButtonStyle(
                                            shape=ft.RoundedRectangleBorder(radius=12),
                                            bgcolor=BRAND)),
                    ], alignment=ft.MainAxisAlignment.END, spacing=10),
                ], spacing=8, expand=True),
            ], spacing=0, expand=True,
               scroll=ft.ScrollMode.AUTO if (page.width or 1000) < 700 else None),
        ], spacing=0, expand=True)

    # ═══ 页 3：渠道 ═══════════════════════════════════════
    # QQ 官方
    qq_enabled = ft.Switch(label="启用", label_position=ft.LabelPosition.LEFT, scale=0.85)
    qq_appid = field("AppID", "", hint="扫码绑定后自动填入", expand=True)
    qq_secret = field("AppSecret", "", password=True, hint="已设置则留空不动")
    qq_test_result = ft.Text("", size=12, color=TEXT_SUB)

    # QQ OneBot
    ob_enabled = ft.Switch(label="启用", label_position=ft.LabelPosition.LEFT, scale=0.85)
    ob_ws = field("WS 地址", "ws://127.0.0.1:3001")
    ob_http = field("HTTP 地址", "http://127.0.0.1:3000")
    ob_self = field("机器人 QQ 号", "", hint="用于判断是否被 @")
    ob_token = field("Access Token", "", password=True, hint="留空不修改")

    # 微信
    wx_enabled = ft.Switch(label="启用", label_position=ft.LabelPosition.LEFT, scale=0.85)
    wx_token = field("Bot Token", "", password=True, hint="扫码登录后自动填入")
    wx_test_result = ft.Text("", size=12, color=TEXT_SUB)

    def _fill_from_config() -> None:
        c = state["config"]
        if not c:
            return
        q = c.get("channel", {}).get("qq_official", {})
        qq_enabled.value = bool(q.get("enabled"))
        qq_appid.value = q.get("app_id", "") or ""
        qq_secret.value = ""
        qq_secret.hint_text = ("已设置，留空不动" if q.get("app_secret_set")
                               else "未设置；扫码绑定后自动填入")
        ob = c.get("channel", {}).get("qq_onebot", {})
        ob_enabled.value = bool(ob.get("enabled"))
        ob_ws.value = ob.get("ws_url", "") or "ws://127.0.0.1:3001"
        ob_http.value = ob.get("http_url", "") or ""
        ob_self.value = ob.get("self_id", "") or ""
        ob_token.hint_text = "已设置，留空不动" if ob.get("access_token_set") else "未设置"
        w = c.get("channel", {}).get("wechat", {})
        wx_enabled.value = bool(w.get("enabled"))
        wx_token.value = ""
        wx_token.hint_text = "已设置，留空不动" if w.get("bot_token_set") else "未设置；扫码登录后自动填入"
        page.update()

    def _save_channels() -> None:
        api = state["api"]
        if api is None:
            _toast("还没连接", ok=False)
            return
        payload = {"channel": {
            "qq_official": {"enabled": bool(qq_enabled.value),
                            "app_id": (qq_appid.value or "").strip()},
            "qq_onebot": {"enabled": bool(ob_enabled.value),
                          "ws_url": (ob_ws.value or "").strip(),
                          "http_url": (ob_http.value or "").strip(),
                          "self_id": (ob_self.value or "").strip()},
            "wechat": {"enabled": bool(wx_enabled.value)},
        }}
        if (qq_secret.value or "").strip():
            payload["channel"]["qq_official"]["app_secret"] = qq_secret.value.strip()
        if (ob_token.value or "").strip():
            payload["channel"]["qq_onebot"]["access_token"] = ob_token.value.strip()
        if (wx_token.value or "").strip():
            payload["channel"]["wechat"]["bot_token"] = wx_token.value.strip()

        def do():
            r = api.put_config(payload)
            # 渠道凭据/开关必须重启进程才生效；保存后直接触发重启
            api.restart()
            return r

        def done(err, result):
            if err:
                _toast(f"保存失败：{err}", ok=False)
                return
            _toast("已保存，服务正在重启（约 5 秒）…")
            def later():
                import time
                time.sleep(5)
                _connect_silent()
            threading.Thread(target=later, daemon=True).start()

        _run_async(do, done)

    def _qq_test() -> None:
        api = state["api"]
        if api is None:
            _toast("还没连接", ok=False)
            return
        qq_test_result.value = "测试中…"
        qq_test_result.color = TEXT_SUB
        page.update()

        def do():
            return api.qq_test((qq_appid.value or "").strip(),
                               (qq_secret.value or "").strip())

        def done(err, result):
            if err:
                qq_test_result.value = f"✗ {err}"
                qq_test_result.color = DANGER
            elif result and result.get("ok"):
                qq_test_result.value = (f"✓ {result.get('bot_name', '')} "
                                        f"(QQ {result.get('bot_uin', '?')})")
                qq_test_result.color = OK_GREEN
            else:
                qq_test_result.value = f"✗ {result.get('detail', '未知错误') if result else '未知错误'}"
                qq_test_result.color = DANGER
            page.update()

        _run_async(do, done)

    def _wx_test() -> None:
        api = state["api"]
        if api is None:
            _toast("还没连接", ok=False)
            return
        wx_test_result.value = "测试中…"
        wx_test_result.color = ft.Colors.GREY_500
        page.update()

        def do():
            return api.wechat_test()

        def done(err, result):
            if err:
                wx_test_result.value = f"✗ {err}"
                wx_test_result.color = DANGER
            elif result and result.get("ok"):
                wx_test_result.value = "✓ token 有效"
                wx_test_result.color = OK_GREEN
            else:
                wx_test_result.value = (f"✗ {result.get('detail', '未知错误') if result else '未知错误'}")
                wx_test_result.color = DANGER
            page.update()

        _run_async(do, done)

    # ── 扫码对话框 ──
    def _qr_dialog(title: str) -> tuple[ft.AlertDialog, ft.Image, ft.Text]:
        qr_img = ft.Image(src="", width=260, height=260, fit=ft.BoxFit.CONTAIN)
        qr_tip = ft.Text("正在生成二维码…", size=13, color=TEXT_SUB)
        dlg = ft.AlertDialog(
            title=ft.Text(title, size=16, weight=ft.FontWeight.BOLD),
            content=ft.Column([qr_img, qr_tip], spacing=12, tight=True,
                              horizontal_alignment=ft.CrossAxisAlignment.CENTER),
            actions=[ft.TextButton("取消", on_click=lambda e: _close_qr())],
        )
        return dlg, qr_img, qr_tip

    def _close_qr() -> None:
        state["qr_polling"] = False
        try:
            page.close_dialog()
        except Exception:
            pass

    def _poll_qr(get_status, on_completed) -> None:
        state["qr_polling"] = True

        def loop():
            while state["qr_polling"]:
                try:
                    r = get_status()
                except Exception:
                    threading.Event().wait(2.0)
                    continue
                st = r.get("status", "")
                if st in ("completed", "confirmed"):
                    state["qr_polling"] = False
                    try:
                        page.close_dialog()
                    except Exception:
                        pass
                    try:
                        on_completed(r)
                        page.update()
                    except Exception:
                        pass
                    return
                if st in ("expired", "error"):
                    state["qr_polling"] = False
                    qr_tip_global.value = ("二维码过期，请重新扫码"
                                           if st == "expired"
                                           else f"出错：{r.get('detail', '')}")
                    qr_img_global.src = ""
                    page.update()
                    return
                threading.Event().wait(2.0)

        threading.Thread(target=loop, daemon=True).start()

    qr_img_global = ft.Image(src="", width=260, height=260,
                             fit=ft.BoxFit.CONTAIN)
    qr_tip_global = ft.Text("", size=13, color=TEXT_SUB)

    def _qq_bind() -> None:
        api = state["api"]
        if api is None:
            _toast("还没连接", ok=False)
            return
        qr_img_global.src = ""
        qr_tip_global.value = "正在生成二维码…"
        dlg = ft.AlertDialog(
            title=ft.Text("QQ 扫码绑定", size=16, weight=ft.FontWeight.BOLD),
            content=ft.Column([
                qr_img_global, qr_tip_global,
                ft.Text("用手机 QQ 扫码，绑定后 AppID/Secret 自动写入服务端",
                        size=11, color=TEXT_HINT, text_align=ft.TextAlign.CENTER),
            ], spacing=12, tight=True, horizontal_alignment=ft.CrossAxisAlignment.CENTER),
            actions=[ft.TextButton("取消", on_click=lambda e: _close_qr())],
        )
        page.show_dialog(dlg)

        def do():
            return api.qq_qr_create()

        def done(err, result):
            if err or not result:
                qr_tip_global.value = f"生成失败：{err or '未知错误'}"
                page.update()
                return
            task_id = result.get("task_id", "")
            qr_img_global.src = "data:image/png;base64," + result.get("qr_png_b64", "")
            qr_tip_global.value = "请用手机 QQ 扫描"
            page.update()
            _poll_qr(lambda: api.qq_qr_status(task_id), _qq_bind_done)

        _run_async(do, done)

    def _qq_bind_done(r: dict) -> None:
        app_id = r.get("app_id", "")
        _toast(f"QQ 绑定成功（AppID {app_id}），服务重启中…", ok=True)

        def later():
            import time
            time.sleep(4)
            api = state["api"]
            if api is not None:
                try:
                    api.restart()
                except Exception:
                    pass
            time.sleep(5)
            _connect_silent()
        threading.Thread(target=later, daemon=True).start()

    def _wx_login() -> None:
        api = state["api"]
        if api is None:
            _toast("还没连接", ok=False)
            return
        qr_img_global.src = ""
        qr_tip_global.value = "正在生成二维码…"
        dlg = ft.AlertDialog(
            title=ft.Text("微信扫码登录", size=16, weight=ft.FontWeight.BOLD),
            content=ft.Column([
                qr_img_global, qr_tip_global,
                ft.Text("用微信扫码，登录后 token 自动写入服务端",
                        size=11, color=TEXT_HINT, text_align=ft.TextAlign.CENTER),
            ], spacing=12, tight=True, horizontal_alignment=ft.CrossAxisAlignment.CENTER),
            actions=[ft.TextButton("取消", on_click=lambda e: _close_qr())],
        )
        page.show_dialog(dlg)

        def do():
            return api.wechat_qr_create()

        def done(err, result):
            if err or not result:
                qr_tip_global.value = f"生成失败：{err or '未知错误'}"
                page.update()
                return
            qrcode = result.get("qrcode", "")
            qr_img_global.src = "data:image/png;base64," + result.get("qr_png_b64", "")
            qr_tip_global.value = "请用微信扫描"
            page.update()
            _poll_qr(lambda: api.wechat_qr_status(qrcode), _wx_login_done)

        _run_async(do, done)

    def _wx_login_done(r: dict) -> None:
        _toast("微信登录成功，服务重启中…", ok=True)

        def later():
            import time
            time.sleep(4)
            api = state["api"]
            if api is not None:
                try:
                    api.restart()
                except Exception:
                    pass
            time.sleep(5)
            _connect_silent()
        threading.Thread(target=later, daemon=True).start()

    def channels_view() -> ft.Control:
        btn_style = ft.ButtonStyle(shape=ft.RoundedRectangleBorder(radius=12))
        return ft.Column([
            section_title("渠道", "channels"),
            ft.Container(height=6),
            glass(ft.Column([
                ft.Row([section_title("QQ 官方机器人", ""), qq_enabled],
                       alignment=ft.MainAxisAlignment.SPACE_BETWEEN),
                ft.Row([qq_appid, qq_secret], spacing=10),
                ft.Row([
                    ft.FilledButton("扫码绑定", icon=ft.Icons.QR_CODE_2,
                                    on_click=lambda e: _qq_bind(), style=btn_style),
                    ft.OutlinedButton("测试连接", on_click=lambda e: _qq_test(),
                                      style=btn_style),
                ], spacing=10),
                qq_test_result,
            ], spacing=10)),
            ft.Container(height=14),
            glass(ft.Column([
                ft.Row([section_title("QQ 小号（OneBot）", ""), ob_enabled],
                       alignment=ft.MainAxisAlignment.SPACE_BETWEEN),
                ft.Row([ob_ws, ob_http], spacing=10),
                ft.Row([ob_self, ob_token], spacing=10),
            ], spacing=10)),
            ft.Container(height=14),
            glass(ft.Column([
                ft.Row([section_title("微信", ""), wx_enabled],
                       alignment=ft.MainAxisAlignment.SPACE_BETWEEN),
                ft.Row([wx_token], spacing=10),
                ft.Row([
                    ft.FilledButton("扫码登录", icon=ft.Icons.QR_CODE_2,
                                    on_click=lambda e: _wx_login(), style=btn_style),
                    ft.OutlinedButton("测试连接", on_click=lambda e: _wx_test(),
                                      style=btn_style),
                ], spacing=10),
                wx_test_result,
            ], spacing=10)),
            ft.Container(height=14),
            ft.Row([
                ft.Container(expand=True),
                ft.FilledButton("保存渠道配置", icon=ft.Icons.SAVE, style=btn_style,
                                on_click=lambda e: _save_channels()),
            ]),
        ], spacing=0, scroll=ft.ScrollMode.AUTO, expand=True)

    # ═══ 页 4：模型 ═══════════════════════════════════════
    # 设计：提供方用下拉选择，选谁就只出现谁需要的字段。
    # base_url/model 由后端预设表决定，用户不填不看不见。
    # 底层模型锁定 deepseek-flash，页面上只读展示。
    chat_presets: list[dict] = []      # 从 GET /v1/config 的 presets 拿
    model_locked = "deepseek-flash"

    # ── 对话：提供方下拉 ──
    chat_provider_dd = ft.Dropdown(
        label="提供方", dense=True, expand=True, text_size=13,
        border_color=BORDER, focused_border_color=BRAND,
        options=[], hint_text="选择对话模型从哪里来",
        on_select=lambda e: _chat_provider_changed(),
    )
    chat_key = field("API Key", "", password=True)
    chat_key_hint = ft.Text("", size=11.5, color=TEXT_SUB)
    # ── WorkBuddy 链接登录 ──
    # 不用填 Key：点「登录」拿一个授权链接，在浏览器/手机里完成登录，
    # 服务端轮询到凭据后自动落盘。与 dsh-connect-workbuddy 同一套 OAuth。
    wb_login_status_text = ft.Text("", size=12, color=TEXT_SUB)

    def _wb_login_done(r: dict) -> None:
        nick = r.get("nickname", "")
        _toast(f"WorkBuddy 登录成功（{nick or '账号'}）", ok=True)
        wb_login_status_text.value = f"已登录：{nick or r.get('uid', '')}"
        page.update()

    def _wb_poll(login_id: str) -> None:
        # 轮询条件就是 state["qr_polling"]：点「取消」时 _close_qr() 把它置
        # False，循环下一轮就退出。原来写的是 while True，取消后线程还要
        # 一直跑到 done 或服务端超时（最长 10 分钟），白白占着连接。
        state["qr_polling"] = True

        def loop():
            api = state["api"]
            while state["qr_polling"]:
                if api is None:
                    return
                try:
                    r = api.workbuddy_login_status(login_id)
                except Exception:
                    threading.Event().wait(3.0)
                    continue
                if r.get("done"):
                    state["qr_polling"] = False
                    try:
                        page.close_dialog()
                    except Exception:
                        pass
                    if r.get("success"):
                        _wb_login_done(r)
                    else:
                        qr_tip_global.value = f"登录失败：{r.get('error', '未知')}"
                        qr_img_global.src = ""
                        page.update()
                    return
                threading.Event().wait(3.0)

        threading.Thread(target=loop, daemon=True).start()

    def _wb_login() -> None:
        api = state["api"]
        if api is None:
            _toast("还没连接", ok=False)
            return
        qr_img_global.src = ""
        qr_tip_global.value = "正在获取授权链接…"
        dlg = ft.AlertDialog(
            title=ft.Text("WorkBuddy 登录", size=16, weight=ft.FontWeight.BOLD),
            content=ft.Column([
                qr_img_global, qr_tip_global,
                ft.Text("手机扫码，或在电脑上打开链接完成登录。\n"
                        "登录后凭据自动写入服务端，无需填 Key。",
                        size=11, color=TEXT_HINT, text_align=ft.TextAlign.CENTER),
            ], spacing=12, tight=True, horizontal_alignment=ft.CrossAxisAlignment.CENTER),
            actions=[ft.TextButton("取消", on_click=lambda e: _close_qr())],
        )
        page.show_dialog(dlg)

        def do():
            return api.workbuddy_login()

        def done(err, result):
            if err or not result or not result.get("ok"):
                qr_tip_global.value = f"获取失败：{err or (result or {}).get('detail', '未知错误')}"
                page.update()
                return
            auth_url = result.get("auth_url", "")
            qr_img_global.src = ("data:image/png;base64," +
                                 result.get("qr_png_base64", ""))
            qr_tip_global.value = ""
            qr_link_btn.value = auth_url
            qr_link_btn.visible = True
            page.update()
            _wb_poll(result.get("login_id", ""))

        _run_async(do, done)

    qr_link_btn = ft.TextButton(
        "在浏览器打开授权页",
        icon=ft.Icons.OPEN_IN_BROWSER,
        visible=False,
        on_click=lambda e: webbrowser.open(qr_link_btn.value or ""),
    )

    wb_login_btn = ft.FilledButton(
        "登录 WorkBuddy", icon=ft.Icons.LINK,
        on_click=lambda e: _wb_login(),
        style=ft.ButtonStyle(shape=ft.RoundedRectangleBorder(radius=12)),
    )
    wb_test_btn = ft.OutlinedButton(
        "测试桥接", icon=ft.Icons.NETWORK_CHECK,
        on_click=lambda e: _wb_test(),
        style=ft.ButtonStyle(shape=ft.RoundedRectangleBorder(radius=12)),
    )

    def _wb_test() -> None:
        api = state["api"]
        if api is None:
            _toast("还没连接", ok=False)
            return
        wb_login_status_text.value = "测试中…"
        page.update()

        def do():
            return api.workbuddy_test()

        def done(err, result):
            if err or not result:
                wb_login_status_text.value = f"测试失败：{err or '未知错误'}"
            elif result.get("ok"):
                models = result.get("models", [])
                wb_login_status_text.value = (
                    f"桥接正常，可用模型：{', '.join(models) or '（空）'}"
                    + ("，已登录" if result.get("logged_in") else "，未登录"))
            else:
                wb_login_status_text.value = f"桥接异常：{result.get('detail', '')}"
            page.update()

        _run_async(do, done)

    chat_workbuddy_row = ft.Column([], spacing=8)
    chat_lock_note = ft.Text("", size=12, color=TEXT_HINT)

    # ── 思考强度：固定 high（产品决定），只读展示 ──
    chat_effort_note = ft.Text("思考强度：high（锁定，不可调）", size=12,
                               color=TEXT_HINT)

    # ── 服务端联网搜索（DeepSeek 官方）───────────────────
    #
    # 开关本身对应 capability.chat.deepseek_web_tools（后端默认 true）。
    # 注意：**开关开着 ≠ 生效**。服务端联网工具只有 DeepSeek 官方端点
    # 支持；选了 workbuddy 桥接层时端点不是官方，本项无效，搜索走本地
    # web_search/web_fetch。所以这里在非官方提供方时把开关置灰，并说明
    # 原因——不然用户会以为开关生效了。
    chat_web_tools_switch = ft.Switch(
        label="服务端联网搜索（DeepSeek 官方）",
        label_position=ft.LabelPosition.LEFT,
        value=True,
        scale=0.85,
    )
    chat_web_tools_note = ft.Text(
        "开启后搜索在 DeepSeek 服务端执行，支持边思考边搜、引用原文；按次计费。"
        "仅 DeepSeek 官方提供方有效。",
        size=11.5, color=TEXT_SUB,
    )
    # 非官方提供方时的额外提示（说明「当前实际走的是本地搜索」）
    chat_web_tools_state = ft.Text("", size=11.5, color=TEXT_HINT)

    # ── 其他能力：也简化成下拉 ──
    # 选项由 GET /v1/config 的 capability.providers 驱动——
    # 后端加一家厂商，这里自动多一项，前端不硬编码。
    #
    # base_url 不在界面上出现（用户不该记域名），但保存时必须一起发：
    # 后端拿到 provider 变化后会按 capability.defaults 改写 base_url，
    # 这里显式带上可以让「用户自己改过的地址」优先于默认表。
    # 记录每个能力「用户刚选中的 provider」，保存时据此查 defaults。
    state["cap_base_url"] = {}
    # 每个能力「用户当前选中的 provider」，用来判断这次保存是不是
    # 换了提供方（换了就要清掉旧提供方留下的 key 绑定）。
    state["cap_provider"] = {}
    # 每个能力当前选的 provider → 它的 key 输入框。切提供方时要把
    # 输入框清空并提示：旧 key 是旧提供方的，留着会拿错 key 去请求。
    _cap_key_fields: dict[str, object] = {}

    def _on_cap_provider(cap_name: str, dd) -> None:
        defaults = ((state.get("config") or {}).get("capability", {})
                    .get("defaults", {}) or {})
        url = (defaults.get(cap_name, {}) or {}).get(dd.value or "", "")
        if url:
            state["cap_base_url"][cap_name] = url
        else:
            state["cap_base_url"].pop(cap_name, None)

        # 换提供方 → 清空该能力的 key 输入框。
        #
        # 为什么必须清：config.toml 里 capability.stt.api_key 写的是
        # ${SILICONFLOW_API_KEY}，切到 deepseek 后这个 ${VAR} 展开的仍是
        # 硅基流动那把真 key。后端已经在「provider 变化且没带新 key」时
        # 把该能力的 api_key 字段清空了，但 GUI 这边的输入框里若还留着
        # 用户先前抄进去的旧 key，保存时会被原样发回去 → 又绑回旧提供方。
        old = state["cap_provider"].get(cap_name)
        new = dd.value or ""
        if old and old != new:
            key_field = _cap_key_fields.get(cap_name)
            if key_field is not None:
                key_field.value = ""
                key_field.hint_text = "已切换提供方，如需单独填 Key 请重新输入"
        state["cap_provider"][cap_name] = new
        page.update()

    def _mk_provider_dd(label: str, key: str, cap_name: str) -> ft.Dropdown:
        return ft.Dropdown(
            label=label, dense=True, expand=True, text_size=13,
            border_color=BORDER, options=[],
            hint_text="加载中…", key=key,
            on_select=lambda e: _on_cap_provider(cap_name, e.control),
        )

    emb_provider_dd = _mk_provider_dd("向量提供方", "emb_provider", "embedding")
    emb_key = field("API Key", "", password=True)

    stt_provider_dd = _mk_provider_dd("语音识别提供方", "stt_provider", "stt")
    stt_key = field("API Key", "", password=True)

    tts_provider_dd = _mk_provider_dd("语音合成提供方", "tts_provider", "tts")
    tts_key = field("API Key", "", password=True)
    # 音色下拉：只有 deepseek 提供方才有两个可选音色（官方 voice_id）。
    # 其它提供方的音色是各自的长名字（edge 的 zh-CN-Xxx、火山的 ID），
    # 手填进 config.toml 即可，不做下拉。
    tts_voice_dd = ft.Dropdown(
        label="音色", dense=True, text_size=13, border_color=BORDER,
        options=[ft.dropdown.Option("stella", "stella · 海星（女声）"),
                 ft.dropdown.Option("tide", "tide · 暗潮（男声）")],
        visible=False,
    )

    def _sync_tts_voice_dd(p: str, current_voice: str = "") -> None:
        """按 TTS 提供方显隐音色下拉（仅 deepseek 显示）。"""
        tts_voice_dd.visible = (p == "deepseek")
        if p == "deepseek":
            # 现有配置里的 voice 是 stella/tide 就带上；空或别的厂商音色 → 默认 stella
            tts_voice_dd.value = current_voice if current_voice in ("stella", "tide") else "stella"

    def _on_tts_provider(e) -> None:
        # 先按新 provider 同步音色下拉，再走通用的 key 清绑逻辑
        _sync_tts_voice_dd(e.control.value or "")
        _on_cap_provider("tts", e.control)

    tts_provider_dd.on_select = _on_tts_provider

    search_provider_dd = _mk_provider_dd("搜索提供方", "search_provider", "search")
    search_key = field("API Key", "", password=True)

    doc_provider_dd = _mk_provider_dd("文档解析提供方", "doc_provider", "document")
    doc_key = field("API Key", "", password=True)

    # key 输入框 ↔ 能力的对应表：切提供方时要按能力找到对应输入框清空。
    _cap_key_fields.update({
        "embedding": emb_key,
        "stt": stt_key,
        "tts": tts_key,
        "search": search_key,
        "document": doc_key,
    })

    # provider 字符串 → 下拉显示。chat 当前是 openai（桥接）时映射回 workbuddy
    CHAT_PROVIDER_ALIAS = {"openai": "workbuddy", "workbuddy": "workbuddy",
                           "deepseek": "deepseek", "siliconflow": "siliconflow"}

    # 服务端联网工具只有 DeepSeek 官方端点支持。
    #
    # 判据和后端一致（fish::capability::is_deepseek_endpoint）：看的是
    # **base_url**（含 api.deepseek.com / deepseek），不是 provider 名字。
    # 后端 GET /v1/config 的 capability.chat.presets 里带 base_url，
    # 这里直接用；拿不到预设时退回「provider == deepseek」。
    def _chat_provider_is_official(p: str) -> bool:
        base = ""
        for preset in chat_presets:
            if preset.get("provider") == p:
                base = (preset.get("base_url") or "").lower()
                break
        if not base:
            return p == "deepseek"
        return "api.deepseek.com" in base or "deepseek" in base

    def _sync_web_tools_state(p: str) -> None:
        """按当前提供方更新服务端联网搜索开关的可用性与提示。"""
        official = _chat_provider_is_official(p)
        chat_web_tools_switch.disabled = not official
        if official:
            chat_web_tools_state.value = ""
        else:
            chat_web_tools_state.value = (
                "仅 DeepSeek 官方提供方支持：当前提供方下本项不生效，"
                "搜索走本地 web_search/web_fetch。"
            )

    def _chat_provider_changed() -> None:
        """切换提供方：只显示该提供方需要的输入。"""
        p = chat_provider_dd.value or ""
        # workbuddy：无需 key，显示扫码提示
        is_wb = (p == "workbuddy")
        chat_key.visible = not is_wb
        chat_workbuddy_row.visible = is_wb
        hint = ""
        for preset in chat_presets:
            if preset.get("provider") == p:
                hint = preset.get("api_key_hint", "")
                break
        chat_key_hint.value = hint

        # 服务端联网搜索开关：非官方提供方时置灰并说明原因
        _sync_web_tools_state(p)

        # 换提供方 → 清空 key 输入框。
        #
        # chat 的 key 在 config.toml 里是单值，切提供方后它就是旧提供方的
        # 那把（workbuddy 的 "bridge-local" 之类），拿去请求新提供方必然
        # 401。清掉输入框并提示用户重新填。
        #
        # 后端**不会**自动清 config.toml 里的 chat.api_key：chat 的 key 是
        # 直读的（不走共享密钥），一清空 make_chat 就失败，机器人会静默
        # 退回 EchoChat 桩（只复读不报错），比 401 更难查。所以由用户显式
        # 重新填。
        old = state.get("chat_provider") or ""
        if old and old != p:
            chat_key.value = ""
            chat_key.hint_text = "已切换提供方，如需单独填 Key 请重新输入"
        state["chat_provider"] = p
        page.update()

    # 把后端暴露的 provider 清单灌进下拉。加一家厂商只改后端，
    # 这里自动多一项——前端不再硬编码任何厂商名。
    def _apply_provider_list(cap: dict, key: str, dd) -> None:
        spec = (cap.get("providers") or {}).get(key) or {}
        raw = spec.get("values") or ""
        labels = spec.get("labels") or {}
        vals = [v for v in str(raw).split(",") if v]
        if not vals:
            return
        dd.options = [ft.dropdown.Option(v, labels.get(v, v)) for v in vals]
        dd.hint_text = "选择提供方"

    def _fill_models_from_config() -> None:
        c = state["config"]
        if not c:
            return
        # 配置重新拉取后，之前记下的「用户切过哪个 provider」就过期了
        state["cap_base_url"] = {}
        cap = c.get("capability", {})
        chat = cap.get("chat", {})

        # 预设列表从服务端拿（含 api_key_hint）
        nonlocal chat_presets, model_locked
        chat_presets = chat.get("presets", []) or []
        model_locked = chat.get("model_locked", "deepseek-flash")
        if chat_provider_dd.options:
            pass  # options 只在首次填
        else:
            labels = {"workbuddy": "WorkBuddy（扫码登录，本机桥接）",
                      "deepseek": "DeepSeek 官方（填 API Key）",
                      "siliconflow": "硅基流动（填 API Key）"}
            chat_provider_dd.options = [
                ft.dropdown.Option(p.get("provider", ""),
                                   labels.get(p.get("provider", ""),
                                              p.get("provider", "")))
                for p in chat_presets
            ]

        # 当前 provider → 下拉值（openai 是桥接的 dialect 名，映射回 workbuddy）
        cur = CHAT_PROVIDER_ALIAS.get(chat.get("provider", ""), "")
        chat_provider_dd.value = cur or None
        # 记下当前选中的 provider，作为「用户切下拉时是否换了提供方」的基准
        state["chat_provider"] = cur
        chat_lock_note.value = f"底层模型：{model_locked}（锁定，不可更换）"
        chat_effort_note.value = "思考强度：high（锁定，不可调）"

        # 服务端联网搜索开关的初值。
        # 后端 GET /v1/config 已在 capability.chat.deepseek_web_tools 回显
        # 原始开关；deepseek_web_tools_active 是「实际是否生效」的判据结果
        # （还要端点是 DeepSeek 官方）。
        chat_web_tools_switch.value = bool(chat.get("deepseek_web_tools", True))
        _sync_web_tools_state(cur)
        # 后端说的「实际是否生效」优先于前端自己的推断：它是运行时用的
        # 那一份判据（capability::server_web_tools_active）。
        if chat.get("deepseek_web_tools_supported") is False:
            chat_web_tools_state.value = (
                "仅 DeepSeek 官方提供方支持：当前提供方下本项不生效，"
                "搜索走本地 web_search/web_fetch。"
            )

        # workbuddy 特殊：显示桥接状态 + 登录入口
        wb = chat.get("base_url", "")
        is_wb = (cur == "workbuddy")
        chat_key.visible = not is_wb
        chat_workbuddy_row.controls = [
            ft.Text(f"桥接地址：{wb}", size=12, color=TEXT_SUB),
            ft.Row([wb_login_btn, wb_test_btn], spacing=10),
            wb_login_status_text,
            qr_link_btn,
        ]
        chat_workbuddy_row.visible = is_wb
        hint = ""
        for preset in chat_presets:
            if preset.get("provider") == cur:
                hint = preset.get("api_key_hint", "")
        chat_key_hint.value = hint

        # 其余能力
        _apply_provider_list(cap, "embedding", emb_provider_dd)
        _apply_provider_list(cap, "stt", stt_provider_dd)
        _apply_provider_list(cap, "tts", tts_provider_dd)
        _apply_provider_list(cap, "search", search_provider_dd)
        _apply_provider_list(cap, "document", doc_provider_dd)

        emb = cap.get("embedding", {})
        emb_provider_dd.value = emb.get("provider") or None
        emb_key.value = ""
        emb_key.visible = not emb.get("key_ready", emb.get("api_key_set", False))
        stt = cap.get("stt", {})
        stt_provider_dd.value = stt.get("provider") or None
        stt_key.value = ""
        stt_key.visible = not stt.get("key_ready", stt.get("api_key_set", False))
        tts = cap.get("tts", {})
        tts_provider_dd.value = tts.get("provider") or None
        tts_key.value = ""
        tts_key.visible = not tts.get("key_ready", tts.get("api_key_set", False))
        # 音色下拉：deepseek 时显示并带上 config 里的现值；其它提供方隐藏
        _sync_tts_voice_dd(tts.get("provider") or "", tts.get("voice") or "")
        s = cap.get("search", {})
        search_provider_dd.value = s.get("provider") or None
        search_key.value = ""
        search_key.visible = not s.get("key_ready", s.get("api_key_set", False))
        doc = cap.get("document", {})
        doc_provider_dd.value = doc.get("provider") or None
        doc_key.value = ""
        doc_key.visible = not doc.get("key_ready", doc.get("api_key_set", False))

        # 记录「当前每个能力选中的 provider」作为下次比较的基准：
        # 用户切下拉时若和这里记的不同，就说明换了提供方 → 清 key 绑定。
        state["cap_provider"] = {
            "embedding": emb.get("provider") or "",
            "stt": stt.get("provider") or "",
            "tts": tts.get("provider") or "",
            "search": s.get("provider") or "",
            "document": doc.get("provider") or "",
        }

        chat_key.hint_text = ("已设置，留空不动" if chat.get("api_key_set")
                              else "未设置")
        def key_hint(sec: dict) -> str:
            if sec.get("api_key_set"):
                return "已设置，留空不动"
            if sec.get("key_ready"):
                return "已复用同一提供方在别处填过的密钥"
            return "未设置"
        emb_key.hint_text = key_hint(emb)
        stt_key.hint_text = key_hint(stt)
        tts_key.hint_text = key_hint(tts)
        search_key.hint_text = key_hint(s)
        doc_key.hint_text = key_hint(doc)
        page.update()

    def _save_models() -> None:
        api = state["api"]
        if api is None:
            _toast("还没连接", ok=False)
            return

        payload: dict = {"capability": {}}
        chat_payload: dict = {}
        p = chat_provider_dd.value
        if p:
            # provider 发预设名，后端展开成 dialect+base_url+model
            chat_payload["provider"] = p
        if (chat_key.value or "").strip():
            chat_payload["api_key"] = chat_key.value.strip()
        # 服务端联网搜索开关：每次都发（和 provider 一样是「当前状态」，
        # 不是「用户这次改了什么」）。后端按 kBool 写回 config.toml。
        #
        # 非官方提供方时开关是置灰的，值仍然照发——它记录的是用户的
        # 偏好，等用户切到 DeepSeek 官方时立刻生效（默认值本来就是 true，
        # 用户在官方端点下关掉之后换到桥接再换回来，这个「关」应当保留）。
        chat_payload["deepseek_web_tools"] = bool(chat_web_tools_switch.value)
        if chat_payload:
            payload["capability"]["chat"] = chat_payload

        # 五个非 chat 能力：provider + （用户这次切过才带的）base_url + key。
        # base_url 只在用户真的动过下拉时才发（state["cap_base_url"] 只由
        # on_select 写入），这样「用户手改过 config.toml 的地址」不会被
        # 一次无意义的保存覆盖回默认值。
        #
        # provider **每次都发**（只要下拉有值）：后端靠「请求里的 provider
        # 和 config.toml 里现值不同」判断用户换了提供方，进而把该能力
        # 绑在旧提供方上的 api_key 解绑。不发的话后端看不到变化，
        # ${SILICONFLOW_API_KEY} 就还绑着 → 切到 deepseek 后拿错 key → 401。
        #
        # api_key 只在用户这次真的填了才发：留空 = 不动后端的 key。
        # 切过提供方时 key 输入框已被清空（_on_cap_provider），
        # 这时「不发 api_key」+「后端解绑」= 落到共享密钥逻辑，正确。
        def cap_payload(name: str, dd, key_field) -> None:
            sec: dict = {}
            if dd.value:
                sec["provider"] = dd.value
            url = state.get("cap_base_url", {}).get(name, "")
            if url:
                sec["base_url"] = url
            if (key_field.value or "").strip():
                sec["api_key"] = key_field.value.strip()
            if sec:
                payload["capability"][name] = sec

        cap_payload("embedding", emb_provider_dd, emb_key)
        cap_payload("stt", stt_provider_dd, stt_key)
        cap_payload("tts", tts_provider_dd, tts_key)
        # deepseek 提供方时把选中的音色一起保存（写进 capability.tts.voice）
        if (tts_provider_dd.value == "deepseek" and tts_voice_dd.visible):
            payload["capability"]["tts"]["voice"] = tts_voice_dd.value or "stella"
        cap_payload("search", search_provider_dd, search_key)
        cap_payload("document", doc_provider_dd, doc_key)

        if not payload["capability"]:
            _toast("没有改动", ok=True)
            return

        def do():
            r = api.put_config(payload)
            # 配置类改动都需要重启进程才生效；直接触发重启
            api.restart()
            return r

        def done(err, result):
            if err:
                _toast(f"保存失败：{err}", ok=False)
                return
            # 已经写进配置了，别让下一次保存又把同一个 base_url 发一遍
            state.get("cap_base_url", {}).clear()
            # 保存后把「当前 provider」更新成实际生效的值：否则下次再切
            # 同一个下拉会被误判成「没换提供方」，key 输入框不清空。
            state["chat_provider"] = chat_provider_dd.value or ""
            for cap_name, dd in (("embedding", emb_provider_dd),
                                 ("stt", stt_provider_dd),
                                 ("tts", tts_provider_dd),
                                 ("search", search_provider_dd),
                                 ("document", doc_provider_dd)):
                state["cap_provider"][cap_name] = dd.value or ""
            _toast("已保存，服务正在重启（约 5 秒）…")
            # 等几秒后重新连接并刷新
            def later():
                import time
                time.sleep(5)
                _connect_silent()
            threading.Thread(target=later, daemon=True).start()

        _run_async(do, done)

    def models_view() -> ft.Control:
        return ft.Column([
            section_title("模型", "capability"),
            ft.Container(height=6),
            glass(ft.Column([
                section_title("对话提供方", ""),
                chat_provider_dd,
                chat_key,
                chat_key_hint,
                # 服务端联网搜索：开关 + 说明 + 「当前是否生效」的提示
                chat_web_tools_switch,
                chat_web_tools_note,
                chat_web_tools_state,
                chat_workbuddy_row,
                chat_lock_note,
                chat_effort_note,
            ], spacing=10)),
            ft.Container(height=14),
            glass(ft.Column([
                section_title("向量（记忆检索用）", ""),
                emb_provider_dd,
                emb_key,
            ], spacing=10)),
            ft.Container(height=14),
            glass(ft.Column([
                section_title("语音识别", ""),
                stt_provider_dd,
                stt_key,
            ], spacing=10)),
            ft.Container(height=14),
            glass(ft.Column([
                section_title("语音合成", ""),
                tts_provider_dd,
                tts_voice_dd,
                tts_key,
            ], spacing=10)),
            ft.Container(height=14),
            glass(ft.Column([
                section_title("网络搜索", ""),
                search_provider_dd,
                search_key,
            ], spacing=10)),
            ft.Container(height=14),
            glass(ft.Column([
                section_title("文档解析", "对方发来 PDF / Word / PPT 时自动解析"),
                doc_provider_dd,
                doc_key,
            ], spacing=10)),
            ft.Container(height=14),
            ft.Row([
                ft.Container(expand=True),
                ft.FilledButton("保存模型配置", icon=ft.Icons.SAVE,
                                style=ft.ButtonStyle(shape=ft.RoundedRectangleBorder(radius=12)),
                                on_click=lambda e: _save_models()),
            ]),
        ], spacing=0, scroll=ft.ScrollMode.AUTO, expand=True)

    # ═══ 配置加载 ═════════════════════════════════════════

    def _load_config_silent() -> None:
        api = state["api"]
        if api is None:
            return

        def do():
            return api.get_config()

        def done(err, result):
            if err:
                return
            state["config"] = result
            _fill_from_config()
            _fill_models_from_config()

        _run_async(do, done)

    # ═══ 连接 ═════════════════════════════════════════════

    def _connect_silent() -> None:
        """重启服务后的静默重连：不弹 toast，成功就刷新数据。"""
        base = state.get("url", "")
        token = state.get("token", "")
        if not base or not token:
            return
        state["api"] = Api(base, token)

        def do():
            state["api"].health()

        def done(err, _):
            if err:
                status_dot.bgcolor = DANGER
                status_text.value = "重启中…"
                page.update()
                return
            status_dot.bgcolor = OK_GREEN
            status_text.value = f"已连接 {base}"
            state["connected"] = True
            page.update()
            _load_config_silent()
            _refresh_runtime()

        _run_async(do, done)

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

        def done(err, _):
            if err:
                status_dot.bgcolor = DANGER
                status_text.value = err[:50]
                state["connected"] = False
                page.update()
                _toast(f"连接失败：{err}", ok=False)
                return
            status_dot.bgcolor = OK_GREEN
            status_text.value = f"已连接 {base}"
            state["connected"] = True
            page.update()
            _load_config_silent()
            _refresh_runtime()
            if state["editing"] == "default":
                _load_current()
            _rebuild_session_list()

        _run_async(do, done)

    # ═══ 导航与布局 ═════════════════════════════════════════
    views = [connect_view, persona_view, channels_view, models_view]
    nav_titles = ["连接", "人格", "渠道", "模型"]
    nav_icons = [ft.Icons.LINK, ft.Icons.PERSON_OUTLINE_OUTLINED,
                 ft.Icons.FORUM_OUTLINED, ft.Icons.SMART_TOY_OUTLINED]
    current = {"idx": 0}

    body = ft.Container(expand=True)

    def _rebuild_nav() -> None:
        w = page.width or 1000
        idx = current["idx"]
        header = ft.Container(
            content=ft.Row([
                ft.Row([
                    ft.Container(
                        content=ft.Icon(ft.Icons.WATER, color=ft.Colors.WHITE, size=18),
                        width=34, height=34, border_radius=10,
                        gradient=ft.LinearGradient(
                            begin=ft.Alignment(-1, -1), end=ft.Alignment(1, 1),
                            colors=[BRAND_SOFT, BRAND_DEEP]),
                    ),
                    ft.Column([
                        ft.Text("大肥鱼", size=15, weight=ft.FontWeight.BOLD,
                                color=TEXT_MAIN),
                        ft.Text("控制面", size=10, color=TEXT_SUB),
                    ], spacing=0),
                ], spacing=8),
                ft.Container(expand=True),
                status_dot, status_text,
            ], spacing=6),
            padding=ft.Padding.symmetric(horizontal=16, vertical=10),
        )

        content = views[idx]()
        if w >= 800:
            rail = ft.NavigationRail(
                selected_index=idx,
                destinations=[
                    ft.NavigationRailDestination(
                        icon=nav_icons[i], selected_icon=nav_icons[i],
                        label=nav_titles[i])
                    for i in range(4)
                ],
                on_change=lambda e: _switch(int(e.control.selected_index)),
                label_type=ft.NavigationRailLabelType.ALL,
                min_width=64,
                # 不给 height 的话 Flet 会抛 unbounded height 异常
                height=None,
                expand=True,
            )
            nav = ft.Container(
                content=rail,
                bgcolor="#66FFFFFF", width=80,
                expand=True,
            )
            body.content = ft.Row([nav, ft.VerticalDivider(width=1, color=BORDER),
                                   ft.Container(ft.Column([header, content],
                                                           spacing=4, expand=True),
                                                padding=ft.Padding.only(right=16),
                                                expand=True)],
                                  expand=True, spacing=0)
        else:
            bar = ft.NavigationBar(
                selected_index=idx,
                destinations=[
                    ft.NavigationBarDestination(icon=nav_icons[i],
                                             selected_icon=nav_icons[i],
                                             label=nav_titles[i])
                    for i in range(4)
                ],
                on_change=lambda e: _switch(int(e.control.selected_index)),
                height=60,
            )
            body.content = ft.Column([
                ft.Column([header, ft.Container(content, expand=True)], expand=True),
                bar,
            ], spacing=0, expand=True)
        page.update()

    def _switch(idx: int) -> None:
        current["idx"] = idx
        _rebuild_nav()
        if idx == 1 and state["api"] is not None:
            _rebuild_session_list()

    def _layout(e=None) -> None:
        _rebuild_nav()

    page.on_resize = _layout
    page.add(
        ft.Column([
            body,
        ], expand=True)
    )

    _rebuild_session_list()
    _layout()

    if state["url"] and state["token"]:
        _connect()


if __name__ == "__main__":
    import argparse

    ap = argparse.ArgumentParser(description="super-feiyu 控制面")
    ap.add_argument("--web", action="store_true", help="以浏览器模式启动（不弹桌面窗口）")
    ap.add_argument("--web-port", type=int, default=8550)
    ap.add_argument("--web-host", default="127.0.0.1")
    # 渲染器：auto/canvaskit/skwasm。无 GPU 的远程环境里 canvaskit
    # 可能一直转圈，auto 让 Flet 自己挑。
    ap.add_argument("--web-renderer", default="auto",
                    choices=["auto", "canvaskit", "skwasm"])
    args = ap.parse_args()

    if args.web:
        ft.run(main, view=ft.AppView.WEB_BROWSER, port=args.web_port,
               host=args.web_host, web_renderer=args.web_renderer)
    else:
        ft.run(main)
