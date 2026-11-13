# 人设提示词：参考高 star 项目的实测结论

调研目的：不再凭感觉写人设，看真正被大规模使用的项目怎么做。

## 参考对象

| 项目 | Star | 用途 |
|---|---|---|
| [AstrBot](https://github.com/AstrBotDevs/AstrBot) | 46.3k | 多平台 LLM 聊天机器人框架 |
| [LangBot](https://github.com/RockChinQ/LangBot) | 18.8k | IM 机器人平台 |

两者都支持 QQ，都是同类项目里使用人数最多的。

## 结论一：框架不内置角色人设

AstrBot 的默认人格（`astrbot/core/persona_mgr.py:22`）只有一句：

```python
DEFAULT_PERSONALITY = {
    "prompt": "You are a helpful and friendly assistant.",
}
```

LangBot 更彻底：源码里搜不到任何默认角色文本，人设完全由用户在面板里填。

**这说明角色人设没有通用标准写法**——写得好不好，框架作者也不打算替你解决。

## 结论二：用 XML 标签分区，不靠「不要」

AstrBot 拼 system prompt 的方式（`main_agent/persona.py`）：

```python
req.system_prompt += f"\n<persona_instructions>\n{prompt}\n</persona_instructions>\n"
```

技能、安全规则也各占一个标签：

```xml
<persona_instructions>...</persona_instructions>
<skills_instructions>...</skills_instructions>
<safety_instructions>...</safety_instructions>
<genui_instructions>...</genui_instructions>
```

好处是模型能明确区分「这段是人格」「这段是工具说明」，不会串味。

## 结论三：默认人格全是行为描述，没有一条「不要」

AstrBot 给 WebChat 的默认人格（`CHATUI_SPECIAL_DEFAULT_PERSONA_PROMPT`）值得逐句看：

> You are a calm, patient friend with a systems-oriented way of thinking.
>
> When someone expresses strong emotional needs, you begin by offering a concise,
> grounding response that acknowledges the weight of what they are experiencing,
> removes self-blame, and reassures them that their feelings are valid and
> understandable.
>
> You then focus on articulating the emotions, tensions, and unspoken conflicts
> beneath the surface—helping name what the person may feel but has not yet fully
> put into words.
>
> You listen more than you speak, respect uncertainty, avoid forcing quick
> conclusions or grand narratives, and prefer clear, restrained language over
> unnecessary emotional embellishment.

注意结构：**每一条都是「遇到什么情况 → 你怎么反应」**，没有一句「不要 XX」。

对比我之前写的 200 行：20 多个「不要」，全是否定式约束。

## 结论四：安全规则单独隔离

AstrBot 把安全规则放在独立的 `<safety_instructions>` 里，并且可以配置策略
（`safety_mode_strategy = "system_prompt"` 时前置到最前面）。

好处：安全规则和人格不混在一起，人格可以写得放开，安全由另一层兜。

## 对我们的启示

1. **别再堆「不要」**——高 star 项目的默认人格里一个都没有
2. **写「情况 → 反应」**，不写「你有 X 性格」
3. **分区**：人格 / 工具 / 安全 各自独立
4. **短**——AstrBot 默认一句话，说明长度不是质量来源

## 关于「鲸鱼」

模型没养过鲸鱼，「你是鲸鱼」对它只是个词，能推出「住水里、吃磷虾」的
刻板印象，推不出具体的说话方式。

所以人设里真正起作用的不是「你是鲸鱼」这个身份声明，
而是**它处在一个什么样的聊天关系里、怎么接话**。

这也是为什么给例子（few-shot）比给形容词有效：
例子是可模仿的，形容词不是。
