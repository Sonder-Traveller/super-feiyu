#!/usr/bin/env python3
"""GUI 数据流回归：模拟 GUI 读配置 → 填表 → PUT 回写的完整链路。

重点验证 ${VAR} 占位符在往返中不被杀死。
"""
import importlib.util
import sys

spec = importlib.util.spec_from_file_location("app", "gui/app.py")
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)

token = open("/home/assistant/super-feiyu/.api_token").read().strip()
api = mod.Api("http://127.0.0.1:8765", token)

ok = fail = 0


def check(name, cond, extra=""):
    global ok, fail
    if cond:
        ok += 1
        print(f"  [PASS] {name}")
    else:
        fail += 1
        print(f"  [FAIL] {name} {extra}")


# 1. 读配置：占位符应解析成真值，密钥应被遮蔽
c = api.get_config()
q = c["channel"]["qq_official"]
check("app_id_resolved", q["app_id"] == "1906397285", q["app_id"])
check("secret_masked", "app_secret" not in q)

# 2. GUI 保存渠道：app_secret 发 __keep__（不动），app_id 发解析值
r = api.put_config({"channel": {"qq_official": {
    "enabled": True, "app_id": q["app_id"], "app_secret": "__keep__"}}})
check("keep_secret_ok", r.get("ok") is True and r.get("changed") == [], str(r))

# 3. config.toml 里占位符必须还活着
raw = open("/home/assistant/super-feiyu/config.toml").read()
check("app_id_placeholder_alive", 'app_id = "${QQ_APP_ID}"' in raw)
check("secret_placeholder_alive", 'app_secret = "${QQ_APP_SECRET}"' in raw)

# 4. 启停切换（GUI 开关）也不能杀占位符
api.put_config({"channel": {"qq_official": {"enabled": False}}})
api.put_config({"channel": {"qq_official": {"enabled": True}}})
raw2 = open("/home/assistant/super-feiyu/config.toml").read()
check("toggle_keeps_placeholder",
      "${QQ_APP_SECRET}" in raw2 and "${QQ_APP_ID}" in raw2)

# 5. 真改一个非占位符字段应该生效
r = api.put_config({"capability": {"tts": {"voice": "zh-CN-XiaoyiNeural"}}})
check("real_change_ok", r.get("ok") is True)

# 6. 模型页：key 留空（不发字段）不应清掉已设置的 key
c2 = api.get_config()
check("chat_key_still_set", c2["capability"]["chat"]["api_key_set"] is True)

print(f"\n== {ok} passed, {fail} failed ==")
sys.exit(1 if fail else 0)
