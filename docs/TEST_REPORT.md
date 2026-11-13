# 全量测试报告

日期：2026-11-13
测试对象：super-feiyu（WSL 部署实例）
版本：提交 `b6a0048`

---

## 一、总体结论

| 维度 | 结果 |
|---|---|
| 构建 | ✅ 20 模块，零警告零错误 |
| 单元测试 | ⚠️ **仅 215 项有效**（曾误报 274） |
| 资源占用 | ✅ 22.8 MB / 29 线程 / 9 fd，60 秒零漂移 |
| 崩溃恢复 | ✅ kill -9 后 SQLite 完整、12 秒自动重启 |
| 安全 | ⚠️ 防护机制有效，但有权限与输入校验漏洞 |
| 控制面 API | ⚠️ 主流程正常，畸形输入处理有缺陷 |
| 文档一致性 | ❌ 多处与实现不符 |

**发现 11 个问题，其中 4 个需要修。**

---

## 二、严重问题（需修）

### P1. 三个测试二进制是历史残留，从未被构建

`build/test_memory`、`build/test_policy`、`build/test_transport` 时间戳是 **00:18**，
而 `build.sh` 只构建 `test_all` / `test_e2e` / `test_protocol` / `test_lifecycle`。

**我第一轮报的「274 项全过」是错的**，其中 61 项来自过期二进制。

更严重的是：`tests/test_memory.cpp` 引用了 `fish::memory::Forgetting`、
`select_evictions` 这些**生产代码里根本不存在**的符号（记忆淘汰逻辑已按用户要求删除）。
这个文件现在**编译不过**，只是没人编译它。

### P2. e2e 测试路径断言过时

```
[FAIL] facts.md 已生成（旁路文件是权威源）
[FAIL] episodes 目录已生成
```

实际文件在 `notes/user/e2e-conv/facts.md`（会话隔离后的结构），
测试断言的是 `notes/facts.md`（旧结构）。

文件确实生成了，是测试没跟上代码演进。

### P3. TOML 语法错误被静默忽略

```toml
this is not toml      # 被跳过
[broken               # 未闭合段头，被跳过
```

`fish.config.cppm:143`：

```cpp
if (eq == std::string::npos) continue;   // 直接跳过
```

用户写错配置，看到的是「api_key 为空」这种完全无关的提示，
会白白排查很久。

### P4. API 畸形输入静默成功

| 请求 | 返回 | 问题 |
|---|---|---|
| `PUT /v1/persona/default` body=`not json` | **200** | 静默把人格清空 |
| body=`{"other":"x"}`（缺 persona） | **200** | 静默把人格清空 |
| body=2MB | **200** | 无大小限制 |

传错参数会以为成功，实际人格被清空。

---

## 三、中等问题（建议修）

### P5. `config.toml` 权限 777

```
.env.local:   600 ✓
config.toml:  777 ✗  所有人可写
```

配置可被任意本地用户篡改（能把 `api.auth` 改成 false）。

### P6. README 数字与实现不符

| 项 | README | 实际 |
|---|---|---|
| 模块数 | 19 | **20** |
| 断言数 | 206 | **215** |

### P7. 4 个文档引用已删除的 `rules.md`

- `docs/PROMPTS.md`
- `docs/modules/persona.md`
- `docs/modules/runtime.md`
- `docs/architecture.md`

### P8. curl / sqlite 动态链接到 `~/local/lib`

```
libcurl.so.4   => /home/assistant/local/lib/libcurl.so.4
libsqlite3.so.0 => /home/assistant/local/lib/libsqlite3.so.0
```

服务器上没有 `~/local` 就会启动失败。
`build-docker.sh` 里做了静态链接，但 `build.sh` 没有。

---

## 四、观察项（不一定要修）

### P9. 启动峰值内存 448 MB

```
VmPeak: 448.5 MB
VmRSS:  22.8 MB
```

常驻 22.8 MB 很正常，但启动峰值 448 MB。
1G 机器（系统占 100-150 MB）上如果同时有其他进程，可能触发 OOM。

### P10. 单元测试与 e2e 的覆盖盲区

日志显示 **从未有真实消息触发过**：
- 工具调用（`tool_ran` 无记录）
- 图片发送（`photo_attached` 无记录）
- 语音发送（`voice_attached` 无记录）

这些人设相关的核心功能只验证到「代码能编译」，没有端到端验证。

### P11. 语音标记与图片标记是并存的文本协议

`[PHOTO]` / `[VOICE]` 走的是文本标记解析，不是工具调用。
之前发现桥接层丢 `tool_calls` 已修复，但发图/发语音仍走旧路径。

---

## 五、通过的部分

### 构建
- 20 模块，零警告
- 二进制 3.7 MB

### 单元测试（有效的 215 项）

| 套件 | 断言 |
|---|---|
| test_all | 173 |
| test_protocol | 24 |
| test_lifecycle | 16 |
| test_e2e | 2（4 项因路径过时失败） |

覆盖：策略、配额、去重、工作记忆、笔记存储、索引、巩固、索引重建、
召回、OneBot 解析、能力声明、注入检测、长度限制、输出脱敏、限流、
工具注册、结果截断、会话键、会话注册、图片标记、人格存储、API。

### 资源稳定性
```
60 秒采样：RSS 22.8 MB 恒定，线程 29，fd 9，零漂移
```

### 崩溃恢复
```
kill -9 → SQLite integrity_check: ok
       → 重启后 129 立绘 / 24 热梗 / 人格 全部加载
       → systemd 12 秒内自动拉起
```

### 安全
- 注入检测：`忽略之前的所有指令` 被识别并拦截
- 超长输入：200 字节超限被拒
- 输出脱敏：API key 被替换为标记
- 链接剥离
- 限流：第 4 次请求被拦，其他会话不受影响
- 路径穿越：`g:123456/../evil` 被哈希，无法逃出目录
- API 鉴权：无 token / 错 token 一律 401
- 密钥不泄露：`SILICONFLOW_API_KEY`、`QQ_APP_SECRET` 在日志中出现 **0 次**

### 控制面 API
- 6 个端点全部正常
- 10 并发 31 ms
- 中文 / emoji / 引号 / 反斜杠 / 换行 往返无损
- `/healthz` 免鉴权
- 压测后资源无泄漏（RSS 23.9 MB，fd 10）

---

## 六、测试方法上的失误（自我记录）

第一轮我报告「274 项断言全过」，这是**错的**：

1. 直接跑了 `build/` 下所有 `test_*`，没检查它们是否在构建列表里
2. 三个二进制的修改时间是 9 小时前，内容已与代码脱节
3. 更没意识到 `test_memory.cpp` 引用的 `Forgetting` 类根本不存在

修正方法：先比对 `build.sh` 的构建目标与 `build/` 里的实际文件，
再用时间戳交叉验证。这个检查已写进 `tools/test_02_unit_fixed.sh`。

---

## 七、建议修复顺序

1. **P1** 删除三个残留二进制，`test_memory.cpp` 要么修好纳入构建、要么删掉
2. **P2** 更新 e2e 路径断言
3. **P3** TOML 解析遇到无法识别的行时报错，而不是跳过
4. **P4** API 校验 `persona` 字段必须存在，加 body 大小上限
5. **P5** `chmod 600 config.toml`
6. **P6-P7** 更新 README 与文档

P8 取决于部署目标：如果服务器上会装依赖就没事，否则改用 `build-docker.sh`。
