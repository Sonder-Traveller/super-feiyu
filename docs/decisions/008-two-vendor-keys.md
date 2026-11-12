# ADR-008：必须两个厂商 key（DeepSeek 无 embedding）

**状态**：已定
**日期**：2026-11-12

---

## 背景

用户的要求是"全都用 API，本地只做转发和存储"。
直觉上，这意味着"一把 DeepSeek key 搞定所有"。

## 问题

**DeepSeek 没有 embeddings 接口。**

> "DeepSeek does not expose an embeddings endpoint."
> — gomodel deepseek provider 源码

社区问答也确认："deepseek 没有 embedding 服务，
只能找提供了 embedding 服务的模型服务商。"

这不是"暂时没开放"，而是**从未提供**。

而长期记忆需要向量检索，向量必须有 embedding 模型。

## 决策

**必须引入第二家厂商。** 选**硅基流动**，因为一个账号同时解决两项：

| 能力 | 模型 | 价格 |
|---|---|---|
| 向量 | `BAAI/bge-m3` | 免费 |
| 语音转文字 | `FunAudioLLM/SenseVoiceSmall` | 免费 |

语音合成用 **Edge-TTS**（微软），无需任何账号。

## 最终的能力-厂商映射

| 能力 | 厂商 | key |
|---|---|---|
| 对话 + 看图 | DeepSeek | `DEEPSEEK_API_KEY` |
| 向量 | 硅基流动 | `SILICONFLOW_API_KEY` |
| 语音识别 | 硅基流动 | 同上（复用） |
| 语音合成 | Edge-TTS | **无需 key** |

**结论：两把 key，不是三把。**

## 免费的真实门槛

| 项 | 门槛 |
|---|---|
| bge-m3 | **必须实名认证**。未实名仅 10 RPM，重建索引必 429 |
| SenseVoiceSmall | **必须实名认证** |
| Edge-TTS | 无，但是非官方接口，随时可能失效 |

实名是硬门槛：不实名也能跑，但记忆功能会在索引重建时卡死，
**而且这个卡死是间歇性的，很难归因**。

## 后果

### 架构影响

- `IEmbeddingModel` 与 `IChatModel` 必须是独立接口（不能合并）
- 配置里必须支持**不同 base_url + 不同 key**
- 熔断需按厂商独立计数（硅基流动挂了不影响对话）

### 成本影响

- 对话 30–45 元/月（唯一花钱项）
- 向量 0 元
- ASR 0 元
- TTS 0 元

**全家桶的边际成本几乎为零**，钱只花在对话 token 上。

## 风险与备选

| 风险 | 备选 |
|---|---|
| 硅基流动免费额度调整 | 阿里 text-embedding（有免费额度）、Gemini embedding |
| Edge-TTS 失效 | 豆包语音（约 1 元/分钟）、硅基流动 TTS |
| 实名限制 | 无替代，必须实名 |

## 被否决的替代方案

| 方案 | 否决理由 |
|---|---|
| 本地跑 embedding 模型 | bge-m3 约 1.2 GB，1G 机器放不下 |
| 不用向量，纯关键词检索 | 中文效果差，召回质量不够 |
| 只用 DeepSeek | **不可能**，接口不存在 |
