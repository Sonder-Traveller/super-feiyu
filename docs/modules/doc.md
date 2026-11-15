# doc 模块设计（文档解析层）

> 职责：把对方发来的 **PDF / Word / PPT / 表格 / 图片 / 电子书**解析成模型能读的内容。
> 独立成层，不并进 `capability`。

---

## 1. 为什么单独一层

文档不是「又一种模型能力」，它的失败模式和成本模型都不一样：

| 维度 | 模型能力（capability） | 文档解析（doc） |
|---|---|---|
| 失败原因 | 上游 5xx / 鉴权 / 限流 | 页数超限、加密、解析超时、格式不支持 |
| 成本单位 | token | **按页计费**（图片按张） |
| 耗时 | 秒级 | 几十秒到几分钟 |
| 是否有状态 | 无 | **有**：`file_id` 跨轮引用 |
| 失败影响 | 这轮回答降级 | 这轮回答仍可发，只是没有正文 |

把它塞进 `capability` 会让「一个统一的 `make_xxx(cfg)` 工厂」这个模式
被迫为文档开特例——文档不是「调一次 API 就返回结果」，而是
「上传一次，之后每轮只传一个句柄」。

---

## 2. 接口

```cpp
namespace fish::doc {

enum class Mode : std::uint8_t { kText, kLayout };
//   kText   —— 只取文字。文字为主的文档（合同、论文）用它，便宜。
//   kLayout —— 文字 + 图。带图表的报告、幻灯片用它，按图计费。

struct Config {
  std::string provider{"none"};   // none | deepseek
  std::string base_url{"https://api.deepseek.com"};
  std::string api_key;
  int timeout_ms{180000};         // 解析慢，给足时间
  std::size_t max_bytes{100u << 20};
};

// 上传后的文档句柄。id 由厂商给，后续轮次只回传它。
struct UploadedDoc {
  std::string file_id;
  std::string filename;
  std::size_t bytes{0};
  Mode mode{Mode::kText};
};

class IDocumentParser {
 public:
  virtual ~IDocumentParser() = default;

  // 上传 + 解析，返回厂商侧的 file_id。
  virtual Result<UploadedDoc> upload(const std::string& path,
                                     const std::string& filename,
                                     Mode mode) = 0;

  // 直接取纯文本（txt/md/csv，或不需要跨轮保留的小文档）。
  virtual Result<std::string> extract_text(const std::string& path,
                                           const std::string& filename) = 0;

  virtual bool available() const noexcept = 0;
};

// 按配置造一个解析器。provider=none 时返回 nullptr（调用方据此降级）。
std::unique_ptr<IDocumentParser> make_parser(const Config& cfg);

Config load_config(const fish::config::Config& c);

// 该文件是否走「解析」，还是当纯文本直读。
bool needs_parsing(const std::string& filename);

}  // namespace fish::doc
```

**没有异常**（见 [ADR-004](../decisions/004-no-exceptions.md)）：
所有跨模块调用返回 `Result<T>`。

---

## 3. Provider

| provider | 说明 |
|---|---|
| `none` | 不启用。收到文件时模型只看到「收到一个文件」 |
| `deepseek` | DeepSeek Files API（`POST /files`） |

> 文档层的 provider 清单与 `capability` 的几个清单**各自独立**，
> 不要合并成一张表——两者的可选集合本来就不同。

### 3.1 DeepSeek Files API

端点 `POST {base_url}/files`，multipart 上传。

- **解析在上传时完成**，成功后才返回 `file_id`
- multipart 字段**有序**：`purpose` 必须排在 `file` 前面，否则上游 400
- `purpose` 取值：
  - 需要解析 → `text_recognition`（只取文字）或 `rich_layout_recognition`（文字 + 图）
  - 纯文本 → `user_data`（原样带上去，不解析、不计费）
- `file_id` 后续轮次只传 id，不必每轮重复塞全文

---

## 4. 分流：解析 还是 直读

`needs_parsing(filename)` 按扩展名判断：

| 走解析 | 走直读 |
|---|---|
| `pdf` `doc` `docx` `ppt` `pptx` `xls` `xlsx` `epub` `mobi` | `txt` `md` `csv` `json` 代码文件等纯文本 |
| `png` `jpg` `jpeg` `webp` `gif` `bmp` `tiff` `tif` | — |

规则：

1. **纯文本本地直读**，不为几 KB 的文字走一趟上传（省时间也省上传流量）
2. **有扩展名但既不需解析、也不在文本白名单里**（`.exe` / `.zip` / `.db`）
   → 明确拒绝，不能当文本直读（否则就是往上下文里灌二进制）
3. **扩展名缺失或为 `bin`** → 明确拒绝，提示「格式不支持解析」
4. 表格与电子书上游会转 Markdown，也算「需要解析」

> 文件名必须**带扩展名**。上游按扩展名选解析器，
> 名字里没扩展名就会把 PDF 当纯文本直读。
> 所以调用方要先用媒体 URL 的扩展名补齐文件名。

---

## 5. 大小限制

`max_bytes` 一直存在但曾经**从来没被执行过**：
用户发来一个 500MB 的压缩包（或误传视频）时，程序会老实把它整个读进内存再上传，
直接把进程撑爆。入口统一挡掉。

- 入口统一检查：`check_size(path, max_bytes)`
- `max_bytes == 0` = **不限制**（与 `guard` 的口径一致）
- 超限返回 `doc_too_large:<实际字节>>上限字节`，并记一条 warn 日志
- 量不出文件大小（文件不存在等）时**不误报超限**，交给后续 open 报错

---

## 6. 编排层的接入

`orchestrator` 拿到入站文件块后的处理顺序：

```
文件块到达
   │
   ├─ 1. 补齐扩展名（URL 里有就用，没有就拒绝）
   │
   ├─ 2. 不需解析且不在文本白名单 → 拒绝，记「格式不支持解析」
   │
   ├─ 3. 落到本地副本
   │      平台给的是临时签名 URL，过期即死链，
   │      而解析要花几秒到几十秒，必须有自己的副本
   │      （走 fish.media 的内容哈希缓存）
   │
   ├─ 4. 不需解析 → extract_text() → 拼成一段文本进本轮上下文
   │
   └─ 5. 需要解析 → upload(Mode::kLayout) → 拿 file_id
            │
            ├─ 成功 → 内容块（kFile，media_url = file_id）
            │        + 提示块「[对方发来了文件：xxx]」
            │
            └─ 失败 → 文本块「[收到文件：xxx，解析失败：<detail>]」
                     + warn 日志（doc_parse_failed）
```

**失败不阻断对话**：解析失败时这一轮仍然发得出去，
只是上下文里没有正文。这是全局降级原则在文档层的落点。

### 6.1 两种模式

| 模式 | 何时用 | 代价 |
|---|---|---|
| `file_id` 引用 | 大文档、需要跨轮保留 | 按页计费，正文留在厂商侧 |
| 取文本拼进本轮 | 纯文本、小文件 | 占用本轮上下文，但不计解析费 |

`deps_.doc_use_file_id` 控制用哪种，默认走 `file_id` 引用。

---

## 7. 配置

```toml
[capability.document]
provider = "none"                              # none | deepseek
base_url = "https://api.deepseek.com"
api_key = "${DEEPSEEK_API_KEY}"
timeout_ms = 180000
max_bytes = 104857600                         # 0 = 不限制
```

| 键 | 默认 | 说明 |
|---|---|---|
| `provider` | `none` | `none` / `deepseek` |
| `base_url` | `https://api.deepseek.com` | 空 = 用这个默认 |
| `api_key` | 空 | **空 = 不启用**（`make_parser` 返回 nullptr） |
| `timeout_ms` | `180000` | 解析慢，给足时间 |
| `max_bytes` | `104857600`（100 MB） | 单文件上限，0 = 不限制 |

---

## 8. 降级矩阵

| 失败点 | 行为 | 用户感知 |
|---|---|---|
| `provider=none` 或 key 为空 | 不注册解析器，文件只留提示 | 知道收到了文件，读不到内容 |
| 文件超 `max_bytes` | 拒绝，记 warn | 提示「文件太大」 |
| 下载失败（临时 URL 过期） | 记 warn，继续 | 提示「无法下载」 |
| 上传 / 解析失败 | 记 warn，用占位文本继续 | 提示「解析失败」 |
| 解析超时 | 同上 | 提示「解析失败」 |

**任何一种失败都不让对话整体失败**——这是全局原则。

---

## 9. 依赖

```
fish.doc  →  fish.core     领域类型 + Result
          →  fish.log      结构化日志
          →  fish.config   配置读取
          →  fish.capability   HttpClient（复用 HTTP 基础设施）
```

`fish.doc` 依赖 `fish.capability` 的 **HTTP 客户端**，
不依赖它的**能力工厂**——所以模块顺序上 `doc` 紧跟 `capability`，
且在 `probe` / `media` / `image` 之前。

---

## 10. 测试要点

| 用例 | 断言 |
|---|---|
| `needs_parsing` 扩展名分流 | PDF/Office/图片 → true；txt/md/csv → false |
| 未知扩展名 | 不进解析也不进直读 |
| 超 `max_bytes` | 返回 `doc_too_large:*`，不进上传 |
| `max_bytes = 0` | 不拦 |
| 纯文本直读 | 不发 HTTP 请求 |
| multipart 字段顺序 | `purpose` 在 `file` 之前 |
| `provider=none` | `make_parser` 返回 nullptr |
| 空 api_key | `make_parser` 返回 nullptr |
| 未知 provider | 返回 nullptr + warn 日志 |

**均 Mock HTTP，不打真实 API。**
