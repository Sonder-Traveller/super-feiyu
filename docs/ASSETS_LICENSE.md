# 形象素材：来源与授权

> 图片不是自己画的，也不是随便能用的。这份文档记录来源和授权状态。
> 逐图署名见 [CREDITS.md](../CREDITS.md)。

## 一、当前状态

`data/memory/portraits/` 有 **129 张**图片，随仓库分发。

> **收进仓库不等于授权变宽。** 全部素材禁止商业使用；
> 各图仍受原作者许可约束，详见下表与 [CREDITS.md](../CREDITS.md)。

| 来源 | 数量 | 授权 | 商用 |
|---|---|---|---|
| [Neko3000/deepseek-whalechan](https://github.com/Neko3000/deepseek-whalechan) | 33 | CC BY-NC-SA 4.0 | ❌ |
| [梗鲸 GengJing 表情包](https://github.com/the-beating-light-of-the-nail/deepseek-chan-meme-pack) | 20 | 非营利粉丝向可随意使用 | ❌ |
| [roseion/dsh-dayu-fish-skin](https://github.com/roseion/dsh-dayu-fish-skin) | 62 | 代码 MIT；素材「仅供个人学习使用」 | ❌ |
| 早期导入（壁纸 + 女仆立绘） | 4 | MIT / CC BY-NC-SA 4.0 | ❌ |
| [1190fasheqi/dafeiyu-pet](https://github.com/1190fasheqi/dafeiyu-pet) | 3 | MIT | ⚠️ 见下 |
| [gmskywalker/...-codex-pet](https://github.com/gmskywalker/deepseek-fat-fish-codex-pet) | 4 | **无 LICENSE**，仅声明非商业 | ❌ |
| [bloodarea/dsh-pets](https://github.com/bloodarea/dsh-pets) | 3 | 角色资产 CC BY-NC-SA 4.0 | ❌ |

## 二、各来源要点

### 1. Neko3000/deepseek-whalechan（33 张）

- 授权：**代码 MIT**；`assets/` 下角色美术 **CC BY-NC-SA 4.0**
  仓库自带 `LICENSE-CC-BY-NC-SA-4.0` 全文，声明清晰
- 内容：9 张设定图 + 9 张场景插画 + 15 张漫画分镜
- 修改：`.webp` → PNG、长边缩至 ≤1024、体积压至 ≤700KB

### 2. 梗鲸 GengJing 表情包（20 张）

- 作者声明「**非营利粉丝向玩梗可以随意使用**」，角色形象版权归原作者
- 修改：`.webp` → PNG

### 3. roseion/dsh-dayu-fish-skin（62 张）

- 代码 MIT；但 `SOURCES.md` 明确写：

  > 本皮肤全部美术素材来自互联网公开项目，**未自行创作**。
  > **仅供个人学习使用**，请尊重原作者权利。

- ⚠️ 这是全部来源里**最窄**的一档。商用前必须联系原作者。

### 4. 早期导入（4 张）

| 文件 | 来源 | 授权 |
|---|---|---|
| `portrait-maid.png` | 萌娘百科「DeepSeek娘」条目，作者 ZipZipPipe | CC BY-NC-SA 4.0 |
| `scene-preview-{1,2,3}.png` | [dsh-whale-girl-wallpaper](https://github.com/QWEQ-CELL-DEL/dsh-whale-girl-wallpaper) | MIT |

### 5. 1190fasheqi/dafeiyu-pet（3 张）

- 授权：**MIT**（三视图立绘）
- ⚠️ MIT 覆盖的是**仓库内容**，但角色形象本身的著作权仍在原作者，
  商用仍需谨慎

### 6. gmskywalker（4 张）

- **仓库没有 LICENSE 文件**，README 仅声明「非官方、非商业同人…仅用于个人桌面定制与技术学习」
- ⚠️ 授权链不完整，**最保守的做法是商用前移除**

### 7. bloodarea/dsh-pets（3 张）

- `assets/characters/` 为 CC BY-NC-SA 4.0
- 但 Spine **运行时**受 Esoteric Software 商业许可约束
- 处理：**只取渲染后的 PNG 单图，未收录 Spine 骨骼数据与运行时**，
  因此不涉及 Esoteric 许可

### 8. 原型「溟月」

- 作者：B 站 UP 主「上善无形」
- 授权：**CC BY-NC-SA 4.0**（署名 / 非商业 / 相同方式共享）
- 来源：[百度百科·鲸鱼娘](https://baike.baidu.com/item/%E9%B2%B8%E9%B1%BC%E5%A8%98/69589494)

## 三、DeepSeek 官方 logo

- 是 DeepSeek 的商标
- 不能当作自己项目的素材随意使用
- 来源：[Brandfetch](https://brandfetch.com/deepseek.com)

## 四、用户自己加图

`PortraitStore::scan_directory()` 会自动纳入丢进目录的图片，
caption 先用文件名，可手改 `manifest.md` 补充。

### manifest.md 格式

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

## 五、重新导入素材

```bash
# 1. 拉候选仓库到 /tmp/asr
bash tools/fetch_assets.sh

# 2. 导入（全部进 data/memory/portraits/）
python3 tools/import_assets.py --src /tmp/asr \
    --meme-src build/memes --out data/memory/portraits

# 3. 压体积（QQ base64 后 5MB 上限）
python3 tools/shrink_assets.py --dir data/memory/portraits

# 4. 校验 manifest 与文件是否一致
bash tools/diff_manifest.sh

# 5. 校验 CREDITS.md 与实际文件是否对得上
bash tools/check_credits.sh
```

## 六、参考链接

- [CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/)
- [Neko3000/deepseek-whalechan](https://github.com/Neko3000/deepseek-whalechan)
- [1190fasheqi/dafeiyu-pet](https://github.com/1190fasheqi/dafeiyu-pet)
- [roseion/dsh-dayu-fish-skin](https://github.com/roseion/dsh-dayu-fish-skin)
- [梗鲸 GengJing](https://aigengtu.com/)
- [百度百科·鲸鱼娘](https://baike.baidu.com/item/%E9%B2%B8%E9%B1%BC%E5%A8%98/69589494)
- [Brandfetch DeepSeek](https://brandfetch.com/deepseek.com)（官方商标，不可随意使用）
