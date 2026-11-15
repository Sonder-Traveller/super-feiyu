# 待解决问题清单

记录于实现过程中，最后统一汇报。

## 语音链路

### P1: 缺少 ffmpeg（阻塞）
- **现象**：`which ffmpeg` 无输出，SILK→WAV 转换无法进行
- **影响**：QQ 语音消息是 SILK 格式，SiliconFlow STT 只接受 wav/mp3，没有 ffmpeg 就无法转写
- **临时方案**：降级为「[语音消息，未启用转写]」
- **待定**：安装 ffmpeg，或改用纯 Python 的 silk 解码库

### P2: 缺少 edge-tts（阻塞）
- **现象**：`which edge-tts` 无输出，TTS 无法合成
- **影响**：`provider = "edge"` 时所有语音回复会失败
- **临时方案**：降级为纯文本回复
- **待定**：`pip install edge-tts`，或改用 SiliconFlow TTS（需付费）

### P3: SiliconFlow STT 接口返回 500
- **现象**：用 png 测试返回 HTTP 500（预期，因为不是音频）
- **影响**：无法确认接口本身是否可用
- **待定**：需要真实音频样本测试

### P4: 群聊语音策略未验证
- **配置**：`group_voice = false`（群里不发语音）
- **风险**：QQ 官方群聊语音受配额限制，可能被拒
- **待定**：实测群聊发语音

## 上游能力

### P5: 探测只覆盖 assistant 图片
- **现状**：只探测 `assistant_image` 和 `requires_stream`
- **未覆盖**：tools/function calling、多图、视频、文件
- **待定**：后续按需扩展

### P6: 探测依赖真实请求
- **成本**：每次探测消耗一次模型调用（约 100 tokens）
- **频率**：30 天一次，可接受
- **风险**：探测失败时沿用旧结果，可能长期过时
- **待定**：加探测失败计数，连续失败时告警

## 待验证

- [ ] 语音端到端（需要真实语音消息）
- [x] 工具调用是否被上游支持 —— 实测支持，需 tool_choice: "auto"
- [ ] 人格调教效果
- [ ] 安全防护有效性

---

# 本轮实现记录

## 已完成

### 1. 上游能力自动探测（替代配置开关）
- `fish.probe` 模块，探测 `assistant_image` / `requires_stream`
- 结果写 `data/probe/capabilities.json`，默认 30 天 TTL
- 探测失败不覆盖上次成功结果
- **P9 已修**：探测前查缓存新鲜度 + 二次读盘，
  多实例同时启动最多 1-2 次重复探测
- **实测**：正确判定 WorkBuddy `assistant_image=false`

### 2. 语音接入
- **TTS 实测可用**：edge-tts 生成 mp3，QQ 直接接受（不需要 SILK 转换）
- **STT 实测可用**：SiliconFlow SenseVoiceSmall，mp3 → 准确转写
- **SILK 转换**：`tools/silk2wav.py`，决策序列 ffmpeg → pilk
  - `--check` 报告后端可用性
  - **P1 已解决**：ffmpeg 缺失时 pilk 兜底，实测转换成功
- 按文件头嗅探音频格式（mp3/wav/silk/ogg/flac/amr）

### 3. 人格调教
参考 [百度智能云](https://cloud.baidu.com/doc/RTC/s/jmhcsb8te)、
[Character Card V3](https://github.com/davidgibbons/sillytavern-skills/blob/f1a45bf6029a3ecbfb75b518a51fb594e3090340/skills/character-card-v3-generator/references/spec-v3-field-map.md)
等实践，按「身份 / 性格 / 说话风格 / 关系 / 底线 / 照片 / 角色区分」分节重构。
说话风格按五维定义（语气/词汇/句式/篇幅/弹性），不堆口癖。

### 4. 本地图片储存
- `PortraitStore::scan_directory()`：直接丢图进目录就能用，caption 用文件名
- 媒体缓存 `fish.media`：入站/出站图片落本地，按内容哈希去重

### 5. 工具调用
- **上游实测支持 function calling**（需 `tool_choice: "auto"`）
- `fish.tool` 模块，工具循环轮数由 `[tool] max_rounds` 控制；
  `0` = 不限制，由模型自己决定何时停（另有 64 轮硬兜底防上游死循环）
- 工具结果用 user 角色承载（上游对 tool 角色支持不稳定）

### 6. 安全防护
- `fish.guard`：输入检查（注入检测/长度）、输出清洗（脱敏/去链接/截断）、频率限制
- 注入检测覆盖 7 类模式，宁可漏一些也不误伤正常对话

### 7. 发言人标识与消息时间
- `Speaker`：id / 昵称 / 群名片 / 群内角色
- `conversation_title`：群名
- `at_ms`：用事件 ISO8601 时间戳，缺失才退回本地接收时间
- 提示词里转成「3 分钟前」这类相对描述，模型能理解"昨天""刚才"

### 8. 网络热梗学习
- `fish.meme`：独立存储区 `data/memes/memes.tsv`，与记忆分开
- **列清单，不做匹配**：不带 `index` 时把全部条目原样交给模型自己挑；
  程序不做子串匹配、不猜「用户问的是哪个梗」
- `lookup_meme` 工具：清单里没有的词，由模型用 `learn` 点名后
  才联网查一次，结果写回本地；下次直接出现在清单里

### 9. web 工具接口化
- 四种 provider：`deepseek` / `tavily` / `generic` / `none`
- **DeepSeekSearch/Fetch**：官方 `/v1/tools/search`、`/v1/tools/fetch`
- **GenericSearch/Fetch**：接任意 HTTP 端点，宽容解析多种响应形态
- GenericFetch 自己做 SSRF 防护（内网/环回/链路本地/CGNAT/IPv6 全拒）

## 新增问题

### P7: tools 声明时不加 tool_choice 会超时
- **现象**：只传 `tools` 不传 `tool_choice`，上游 90s 超时
- **原因**：上游可能要求显式指定 tool_choice
- **处理**：代码里固定传 `tool_choice: "auto"`

### P8: 工具结果的角色承载
- **现象**：上游对 OpenAI 标准的 `role: "tool"` 支持不稳定
- **处理**：改用 `role: "user"` + 明确标注「以下是工具调用的真实结果」
- **风险**：可能被模型误认为是用户说的话

### P9: 多实例重复探测 —— 已修复
- 探测前查缓存新鲜度，新鲜则零网络请求
- 探测前二次读盘：可能别的实例刚探完
- 日志可见 `cache_hit age_days=0`

### P10: 注入检测可能误伤
- **现象**：正则匹配「忽略之前的指令」等模式
- **风险**：正常对话里说「忽略上面那个错误」可能被误判
- **缓解**：模式限定在「指令/规则/设定/提示/要求」等词，不含「错误」
- **已验证**：「忽略上面那个小错误，我们继续」能正常通过

### P11: 群聊语音 —— 延后待办
- **配置**：`group_voice = false`（群里不发语音）
- **原因**：QQ 官方群聊语音可能受配额限制，需实测
- **状态**：按用户要求延后

### P12: web_search / web_fetch 未实测（缺 key）
- **现状**：已按 [DeepSeek 官方文档](https://api-docs.deepseek.com/zh-cn/guides/search/)
  实现 `DeepSeekSearch` / `DeepSeekFetch`
- **阻塞**：`.env.local` 里没有 `DEEPSEEK_API_KEY`
- **计费**：每 1000 次成功请求 5 元（失败/空结果不计费）
- **限速**：Search 100 QPS / Fetch 25 QPS（账号级）
- **待办**：拿到 key 后实测

### P13: search provider 选择
- **已接口化**：`deepseek` / `tavily` / `generic` / `none`
- **generic** 可接任意 HTTP 端点（自建服务、聚合 API、本地桥接层）
- **现状**：`capability.search.api_key` 未配时 web 工具不注册，
  `tools_ready` 只有本地那几个（记忆 / 图片 / 语音 / 热梗）
- **待办**：用户决定接哪个后端

### P14: 探测只覆盖 assistant 图片
- **现状**：只探测 `assistant_image` 和 `requires_stream`
- **未覆盖**：tools/function calling、多图、视频、文件
- **待定**：后续按需扩展

### P15: 人格效果未实测
- **现状**：persona.md 已按业界实践重构
- **待验证**：实际对话中是否自然、是否还有身份混淆
- **风险**：结构化分节可能让模型变得「模板化」

### P16: 安全防护未实战验证
- **现状**：单元测试覆盖 40+ 断言
- **未验证**：真实攻击场景下的效果
- **已知局限**：正则匹配容易绕过（换写法、用拼音、用英文同义词）

---

# 待办队列（按优先级）

## P17: 中文语音多音字（延后）

**问题**：edge-tts 不支持 SSML 和拼音标注，只能喂纯文本。
多音字读错无法在 TTS 层解决。

**已验证的限制**：
```
edge-tts --help 只有 -t/--text、-v/--voice、--rate、--volume、--pitch
没有 SSML 入口，也没有拼音/注音参数
```

**可能方案**（待评估）：
1. **文本预处理替换**：把易错词换成同音字或加停顿
   - 例：「重庆」→「重庆（chóng qìng）」不行，TTS 会念括号
   - 例：「银行」→「银航」读对但字形错，只对 TTS 有效
   - 缺点：维护词表，且替换后字幕会错
2. **换 TTS 引擎**：Azure / 火山 / 讯飞支持 SSML 的 `<phoneme>`
   - 需要 API key 和费用
   - `ITextToSpeech` 接口已支持换实现，只需加一个 provider
3. **接受现状**：只在语音回复里避开易错词
   - 由模型自己规避（persona 里加提示）
   - 零成本，但不彻底

**建议**：先做方案 3（persona 提示模型避免），
有需求再上方案 2（换支持 SSML 的引擎）。

## P18: 图片库需要真实素材（延后）

**现状**：`data/memory/portraits/` 是空目录。

**原因**：第三方设定图授权不清晰，不能打包。
详见 `docs/ASSETS_LICENSE.md`。

**三条路**：
1. 用户自己放图（默认，零风险）
2. 接生图模型（`capability.image.provider` 已支持）
3. 引入社区素材（需核实授权 + 署名 + 不打包进 git）

## P19: 多音字词表（如果做方案 1）

已知易错词，供参考：

| 词 | 正确读音 | 常见错误 |
|---|---|---|
| 银行 | yín háng | yín xíng |
| 重庆 | chóng qìng | zhòng qìng |
| 长大 | zhǎng dà | cháng dà |
| 音乐 | yīn yuè | yīn lè |
| 归还 | guī huán | guī hái |
| 觉得 | jué de | jiào de |
| 了解 | liǎo jiě | le jiě |
| 行长 | háng zhǎng | xíng zhǎng |
