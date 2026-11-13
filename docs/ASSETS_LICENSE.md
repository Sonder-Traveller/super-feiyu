# 形象素材：来源与授权

> 图片不是自己画的，也不是随便能用的。这份文档记录来源和授权状态。

## 一、当前状态：**未放入任何第三方图片**

`data/memory/portraits/` 目前只有 `manifest.md`，没有图片文件。

原因：第三方设定图的授权不清晰，直接打包进仓库有侵权风险。

## 二、已调研的素材来源

### 1. TreapGoGo/deepseek-whale-girl

| 文件 | 内容 | 授权 |
|---|---|---|
| `characters/dafeiyu/reference-sheets/dafeiyu-chibi-character-sheet.png` | Q 版设定图（三视图 + 细节） | ⚠️ **不在 MIT 范围内** |
| `characters/dafeiyu/reference-sheets/dafeiyu-mature-character-sheet.png` | 成熟版设定图 | ⚠️ 同上 |
| `characters/brand/whale-girl-logo-master.png` | 角色 logo | ⚠️ 同上 |
| `characters/brand/whale-girl-logo-dynamic-master.png` | 动态 logo | ⚠️ 同上 |

**README 原文**：

> 本仓库网站源代码按 MIT License 授权。网站原创说明文字与生成提示词按
> CC BY-SA 4.0 共享。
>
> **角色设定图、社区投稿及外部项目素材不包含在上述代码许可中**，
> 分别遵循其作者或来源的授权说明；在使用图片前，请先核实对应许可。
> **收录不代表本站拥有相关作品版权，也不代表获得再次使用授权。**

### 2. 原型「溟月」

- 作者：B 站 UP 主「上善无形」
- 授权：**CC BY-NC-SA 4.0**（署名 / 非商业 / 相同方式共享）
- 来源：[百度百科·鲸鱼娘](https://baike.baidu.com/item/%E9%B2%B8%E9%B1%BC%E5%A8%98/69589494)

**这意味着**：
- 可以用（非商业）
- 必须署名
- 衍生作品必须同样以 CC BY-NC-SA 4.0 共享

### 3. DeepSeek 官方 logo

- 是 DeepSeek 的商标
- 不能当作自己项目的素材随意使用
- 来源：[Brandfetch](https://brandfetch.com/deepseek.com)

## 三、当前采用的方案

### 方案 A：用户自己放图（默认）

`data/memory/portraits/` 是空目录 + 一份说明。
用户自己准备图片放进去，`PortraitStore::scan_directory()` 会自动纳入。

**优点**：零侵权风险，用户自负其责。
**缺点**：开箱没有图。

### 方案 B：用户配置图床 URL

`manifest.md` 里可以直接写 http(s) URL，指向用户自己有权使用的图。

### 方案 C：接生图模型（待办）

`capability.image.provider` 已支持 `siliconflow` / `openai` / `bridge`。
接上之后由模型按需生成，不涉及第三方素材授权。

## 四、如果要引入社区素材，需要做

1. **核实每一张图的作者**，不能只看仓库
2. **确认授权**：CC BY-NC-SA 4.0 需要署名 + 非商业 + 同协议共享
3. **保留署名文件**（`CREDITS.md`）
4. **不要打包进 git**（用 `.gitignore` 排除，让用户自己下载）
5. 如果是商业用途，**必须换素材**

## 五、manifest.md 的正确写法

```markdown
# 大肥鱼形象库
# 格式: path | caption | tags | source | created_at

# 本地文件
data/memory/portraits/eating.png | 在吃泡面 | 吃,泡面,夜宵 | manual | 2026-11-13

# 远程 URL（用户自己有权使用的图）
https://example.com/my-whale.png | 我自己的图 | 自定义 | manual | 2026-11-13
```

`source` 字段建议填：
- `manual` —— 用户手动放的
- `generated` —— 生图模型生成的
- `uploaded` —— 用户上传的

**不要填 `official`**，因为鲸鱼娘不是官方角色。

## 六、参考链接

- [TreapGoGo/deepseek-whale-girl](https://github.com/TreapGoGo/deepseek-whale-girl)（素材库，授权需逐个核实）
- [百度百科·鲸鱼娘](https://baike.baidu.com/item/%E9%B2%B8%E9%B1%BC%E5%A8%98/69589494)（含授权说明）
- [CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/)
- [Brandfetch DeepSeek](https://brandfetch.com/deepseek.com)（官方商标，不可随意使用）
