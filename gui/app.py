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
    new_conv = ft.TextField(
        label="会话 ID", hint_text="群聊 g:群号 / 私聊 openid",
        dense=True, expand=True, text_size=13, border_color=BORDER,
        on_submit=lambda e: _add_session(),
    )

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

    def _add_session() -> None:
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

        def item(key: str, is_default: bool = False) -> ft.Control:
            selected = state["editing"] == key
            row = ft.Container(
                content=ft.Row([
                    ft.Icon(ft.Icons.HOME if is_default else ft.Icons.CHAT_BUBBLE_OUTLINE,
                            size=15,
                            color=BRAND if selected else TEXT_SUB),
                    ft.Text("全局默认" if is_default else key, size=12.5, expand=True,
                            max_lines=1, overflow=ft.TextOverflow.ELLIPSIS,
                            color=TEXT_MAIN if selected else "#4A4A55"),
                ], spacing=8),
                padding=ft.Padding.symmetric(horizontal=10, vertical=9),
                border_radius=8,
                gradient=(ft.LinearGradient(
                    begin=ft.Alignment(-1, 0), end=ft.Alignment(1, 0),
                    colors=[BRAND_TINT, "#B8F5F2FF"],
                ) if selected else None),
                on_click=lambda e, k=key: _select(k),
                ink=True,
            )
            if is_default:
                return row
            return ft.Row([
                ft.Container(content=row, expand=True),
                ft.IconButton(ft.Icons.CLOSE, icon_size=13, icon_color=TEXT_HINT,
                              tooltip="从列表移除（不影响服务端）",
                              on_click=lambda e, k=key: _remove_session(k)),
            ], spacing=0)

        session_list.controls.append(item("default", is_default=True))
        for s in state["sessions"]:
            session_list.controls.append(item(s))
        page.update()

    def persona_view() -> ft.Control:
        return ft.Column([
            section_title("人格", "persona"),
            ft.Container(height=6),
            ft.Row([
                ft.Container(width=240, content=ft.Column([
                    session_list,
                    ft.Row([new_conv,
                            ft.IconButton(ft.Icons.ADD, icon_size=18, icon_color=BRAND,
                                          tooltip="添加会话", on_click=lambda e: _add_session())],
                           spacing=4),
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
            return api.put_config(payload)

        def done(err, result):
            if err:
                _toast(f"保存失败：{err}", ok=False)
                return
            restart = result.get("restart_required") if result else False
            _toast("渠道配置已保存" + ("，需重启服务生效" if restart else ""))
            _load_config_silent()

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
        _toast(f"QQ 绑定成功（AppID {app_id}），已自动启用", ok=True)
        _load_config_silent()

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
        _toast("微信登录成功，已自动启用", ok=True)
        _load_config_silent()

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
    model_fields: dict[str, ft.TextField] = {}

    def _mf(name: str, label: str, password: bool = False, hint: str = "") -> ft.TextField:
        f = field(label, "", password=password, hint=hint)
        model_fields[name] = f
        return f

    chat_provider = _mf("chat.provider", "Provider")
    chat_url = _mf("chat.base_url", "Base URL")
    chat_model = _mf("chat.model", "模型")
    chat_effort = _mf("chat.reasoning_effort", "思考强度", hint="low/medium/high/max")
    chat_key = _mf("chat.api_key", "API Key", password=True)

    emb_provider = _mf("embedding.provider", "Provider")
    emb_url = _mf("embedding.base_url", "Base URL")
    emb_model = _mf("embedding.model", "模型")
    emb_key = _mf("embedding.api_key", "API Key", password=True)

    stt_provider = _mf("stt.provider", "Provider")
    stt_url = _mf("stt.base_url", "Base URL")
    stt_model = _mf("stt.model", "模型")
    stt_key = _mf("stt.api_key", "API Key", password=True)

    tts_provider = _mf("tts.provider", "Provider")
    tts_voice = _mf("tts.voice", "音色")

    search_provider = _mf("search.provider", "Provider",
                          hint="deepseek/tavily/generic/none")
    search_url = _mf("search.base_url", "Base URL")
    search_key = _mf("search.api_key", "API Key", password=True)

    def _fill_models_from_config() -> None:
        c = state["config"]
        if not c:
            return
        cap = c.get("capability", {})
        vals = {
            "chat.provider": cap.get("chat", {}).get("provider", ""),
            "chat.base_url": cap.get("chat", {}).get("base_url", ""),
            "chat.model": cap.get("chat", {}).get("model", ""),
            "chat.reasoning_effort": cap.get("chat", {}).get("reasoning_effort", ""),
            "chat.api_key": "",
            "embedding.provider": cap.get("embedding", {}).get("provider", ""),
            "embedding.base_url": cap.get("embedding", {}).get("base_url", ""),
            "embedding.model": cap.get("embedding", {}).get("model", ""),
            "embedding.api_key": "",
            "stt.provider": cap.get("stt", {}).get("provider", ""),
            "stt.base_url": cap.get("stt", {}).get("base_url", ""),
            "stt.model": cap.get("stt", {}).get("model", ""),
            "stt.api_key": "",
            "tts.provider": cap.get("tts", {}).get("provider", ""),
            "tts.voice": cap.get("tts", {}).get("voice", ""),
            "search.provider": cap.get("search", {}).get("provider", ""),
            "search.base_url": cap.get("search", {}).get("base_url", ""),
            "search.api_key": "",
        }
        for name, f in model_fields.items():
            f.value = vals.get(name, "")
        chat_key.hint_text = "已设置，留空不动" if cap.get("chat", {}).get("api_key_set") else "未设置"
        emb_key.hint_text = "已设置，留空不动" if cap.get("embedding", {}).get("api_key_set") else "未设置"
        stt_key.hint_text = "已设置，留空不动" if cap.get("stt", {}).get("api_key_set") else "未设置"
        search_key.hint_text = "已设置，留空不动" if cap.get("search", {}).get("api_key_set") else "未设置"
        page.update()

    def _save_models() -> None:
        api = state["api"]
        if api is None:
            _toast("还没连接", ok=False)
            return

        def cap(section: str, key: str) -> str:
            f = model_fields[f"{section}.{key}"]
            return (f.value or "").strip()

        payload = {"capability": {
            "chat": {"provider": cap("chat", "provider"),
                     "base_url": cap("chat", "base_url"),
                     "model": cap("chat", "model"),
                     "reasoning_effort": cap("chat", "reasoning_effort")},
            "embedding": {"provider": cap("embedding", "provider"),
                          "base_url": cap("embedding", "base_url"),
                          "model": cap("embedding", "model")},
            "stt": {"provider": cap("stt", "provider"),
                    "base_url": cap("stt", "base_url"),
                    "model": cap("stt", "model")},
            "tts": {"provider": cap("tts", "provider"),
                    "voice": cap("tts", "voice")},
            "search": {"provider": cap("search", "provider"),
                       "base_url": cap("search", "base_url")},
        }}
        # 密钥只在用户真的填了新值时才发；留空一律不发（= 不修改）
        for sec, keyf in (("chat", "api_key"), ("embedding", "api_key"),
                          ("stt", "api_key"), ("search", "api_key")):
            v = cap(sec, keyf)
            if v:
                payload["capability"][sec][keyf] = v

        def do():
            return api.put_config(payload)

        def done(err, result):
            if err:
                _toast(f"保存失败：{err}", ok=False)
                return
            restart = result.get("restart_required") if result else False
            _toast("模型配置已保存" + ("，需重启服务生效" if restart else ""))
            _load_config_silent()

        _run_async(do, done)

    def models_view() -> ft.Control:
        return ft.Column([
            section_title("模型", "capability"),
            ft.Container(height=6),
            glass(ft.Column([
                section_title("对话（chat）", ""),
                ft.Row([chat_provider, chat_model], spacing=10),
                ft.Row([chat_url], spacing=10),
                ft.Row([chat_effort, chat_key], spacing=10),
            ], spacing=10)),
            ft.Container(height=14),
            glass(ft.Column([
                section_title("向量（embedding）", ""),
                ft.Row([emb_provider, emb_model], spacing=10),
                ft.Row([emb_url, emb_key], spacing=10),
            ], spacing=10)),
            ft.Container(height=14),
            glass(ft.Column([
                section_title("语音识别（stt）", ""),
                ft.Row([stt_provider, stt_model], spacing=10),
                ft.Row([stt_url, stt_key], spacing=10),
            ], spacing=10)),
            ft.Container(height=14),
            glass(ft.Column([
                section_title("语音合成（tts）", ""),
                ft.Row([tts_provider, tts_voice], spacing=10),
            ], spacing=10)),
            ft.Container(height=14),
            glass(ft.Column([
                section_title("搜索（search）", ""),
                ft.Row([search_provider], spacing=10),
                ft.Row([search_url, search_key], spacing=10),
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
