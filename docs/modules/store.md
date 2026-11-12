# store 模块设计

> 职责：记忆的持久化与检索。**只落盘，不推理。**
> 向量由 capability 算好传进来，store 不感知模型。

---

## 1. 定位

```
capability::IEmbeddingModel  →  向量
                                  ↓
orchestrator  →  store::append / recall / recent
                                  ↓
                            SQLite + 向量检索
```

**边界**：store 不知道"记忆"语义，只知道"存一批带向量的记录，按相似度取几条"。

---

## 2. 数据模型

### 2.1 表结构

```sql
-- 对话记录（原始流水）
CREATE TABLE messages (
  id           INTEGER PRIMARY KEY AUTOINCREMENT,
  conversation_id TEXT NOT NULL,
  sender_id    TEXT NOT NULL,
  role         TEXT NOT NULL,          -- user | assistant
  text         TEXT NOT NULL,
  msg_hash     TEXT NOT NULL,          -- 用于去重
  created_at   INTEGER NOT NULL        -- unix ms
);
CREATE INDEX idx_conv_time ON messages(conversation_id, created_at DESC);

-- 向量（与 messages 一对多，一条消息可有多段向量）
CREATE TABLE embeddings (
  message_id   INTEGER NOT NULL REFERENCES messages(id) ON DELETE CASCADE,
  chunk_index  INTEGER NOT NULL,
  vector       BLOB NOT NULL,          -- float32 小端数组
  dim          INTEGER NOT NULL,
  PRIMARY KEY (message_id, chunk_index)
);

-- 长期记忆条目（从对话中提炼，独立于原始流水）
CREATE TABLE memories (
  id           INTEGER PRIMARY KEY AUTOINCREMENT,
  conversation_id TEXT NOT NULL,
  subject      TEXT,                   -- 关于谁
  content      TEXT NOT NULL,
  confidence   REAL NOT NULL,          -- 0~1
  created_at   INTEGER NOT NULL,
  last_used_at INTEGER
);
```

### 2.2 为什么分三张表

| 表 | 用途 | 生命周期 |
|---|---|---|
| `messages` | 原始流水 | 短期，按容量淘汰 |
| `embeddings` | 语义检索 | 跟随 messages 级联删除 |
| `memories` | 提炼后的长期事实 | 长期，带置信度衰减 |

**关键区别**：`messages` 是"说过什么"，`memories` 是"记住了什么"。
混在一起会导致上下文窗口被原始流水撑爆。

---

## 3. 向量检索方案

### 3.1 为什么不用 FAISS

| 方案 | 内存 | 依赖 | 适用规模 |
|---|---|---|---|
| FAISS | 索引常驻内存 | C++ 库，体积大 | 百万级 |
| **暴力扫描 + SQLite** | 按需加载 | 无 | **万级以内** |
| sqlite-vec 扩展 | 低 | 一个扩展 | 十万级 |

**判断**：单条鱼的个人对话量，一年也就几万条消息。
1024 维 × 4 字节 × 1 万条 = **40 MB**，一次暴力扫描在 2 核上约 10 ms。
**完全够用，不需要引入 FAISS。**

> 若未来超过 10 万条，再考虑 sqlite-vec 扩展，接口不变。

### 3.2 检索实现

```cpp
// 伪代码
auto rows = db.query(
  "SELECT e.vector, e.dim, m.text FROM embeddings e "
  "JOIN messages m ON m.id = e.message_id "
  "WHERE m.conversation_id = ? ORDER BY m.created_at DESC LIMIT 5000",
  conv_id);

// 暴力余弦相似度
std::partial_sort(rows, top_k, by_cosine(query_vec));
```

**两阶段**：
1. 先用 `created_at DESC LIMIT N` 缩小候选（最近的更相关）
2. 再在候选中算相似度

避免全表扫描导致延迟随数据量线性增长。

### 3.3 向量序列化

```cpp
// float32 小端，紧凑存储
std::string serialize(const std::vector<float>& v) {
  std::string out(v.size() * 4, '\0');
  std::memcpy(out.data(), v.data(), out.size());
  return out;
}
```

**注意字节序**：x86 和 ARM 都是小端，但显式约定避免将来踩坑。

---

## 4. 容量控制（1G 机器的关键）

### 4.1 三层限制

| 层 | 限制 | 淘汰策略 |
|---|---|---|
| 单会话消息数 | 默认 2000 条 | 超出删最旧 |
| 全库总大小 | 默认 200 MB | 按会话 LRU 淘汰 |
| 向量维度 | 固定 1024 | — |

### 4.2 淘汰实现

定时任务（每小时）执行：

```
1. 每个会话保留最近 N 条 messages
2. 删除孤儿 embeddings（CASCADE 自动处理）
3. 若库文件 > 上限，按最久未活跃的会话整体淘汰
4. VACUUM（每周一次，避免频繁整理）
```

**为什么要 VACUUM**：SQLite 删除后文件不缩小，长期运行会吃掉磁盘。
但 VACUUM 会锁库且耗时，不能频繁做。

### 4.3 WAL 模式

```sql
PRAGMA journal_mode = WAL;
PRAGMA synchronous = NORMAL;
PRAGMA cache_size = -8000;   -- 8 MB 页缓存
PRAGMA busy_timeout = 5000;
```

WAL 让读写不互斥（工作线程写、召回线程读）。
`cache_size` 限制在 8 MB，避免 SQLite 自己吃掉大量内存。

---

## 5. 并发

### 5.1 连接策略

| 方案 | 问题 |
|---|---|
| 单连接 + 全局锁 | 简单，但读写互斥 |
| 每线程一连接 | WAL 下可并发，但连接数不可控 |
| **连接池（读 2 + 写 1）** | 平衡 |

**采用连接池**：写走专用连接（避免写冲突），读用 2 个连接。

### 5.2 写冲突

同会话已由 orchestrator 串行化，理论上不会并发写。
但不同会话可能同时写，需要 `busy_timeout` 兜底。

---

## 6. 接口契约

```cpp
class IMemoryStore {
 public:
  struct Record {
    std::string conversation_id, sender_id, role, text;
    std::vector<float> embedding;
    TimePoint created_at;
  };

  virtual Error append(const Record&) = 0;                        // 追加一条
  virtual Result<std::vector<Record>> recall(
      const std::string& conv_id,
      const std::vector<float>& query_vec, int top_k) = 0;        // 语义召回
  virtual Result<std::vector<Record>> recent(
      const std::string& conv_id, int n) = 0;                     // 最近 n 条
};
```

**`recall` 与 `recent` 分开**：
- `recent` 用于短期上下文（保证连贯）
- `recall` 用于长期记忆（按相关性）

编排层两者都用，`recent` 是必需，`recall` 是增强。

---

## 7. 现有实现与演进

### 7.1 当前：InMemoryStore

已实现（环形缓冲 + 余弦召回），刻意极简：

- 用途：**先把编排逻辑跑通**
- 局限：重启丢失、无持久化
- 价值：验证接口设计是否合理

### 7.2 下一步：SqliteStore

按本文档实现，接口不变，替换实现即可。

**迁移路径**：
1. 实现 `SqliteStore`，保持 `IMemoryStore` 接口
2. 配置里切换 `store.type = "sqlite"`
3. 保留 `InMemoryStore` 用于测试

---

## 8. 待定事项

| 事项 | 选项 | 倾向 |
|---|---|---|
| SQLite 绑定 | sqlite3 直接调用 / sqlite_orm | **直接调用**（少一层抽象） |
| 向量检索 | 暴力扫描 / sqlite-vec | 先暴力，量大再换 |
| 记忆提炼 | 定时批量 / 触发式 | 定时（每小时） |
| 记忆衰减 | 时间半衰期 / 不衰减 | 半衰期（更真实） |
| 多段向量 | 长消息切块 | 需要（超 8192 token 时） |
| 备份 | 定时拷 db 文件 / SQLite Backup API | Backup API（一致性） |

---

## 9. 测试要点

| 用例 | 断言 |
|---|---|
| append 后 recent 能取到 | 条数与顺序正确 |
| 容量上限 | 超出后最旧的被淘汰 |
| 余弦相似度排序 | 构造已知相似度的向量，断言顺序 |
| 空向量不参与召回 | embedding 为空时被跳过 |
| 会话隔离 | A 会话的记录不会出现在 B 的召回里 |
| 并发写不损坏 | 多线程写入后校验完整性 |
| 时间戳时区 | 存储用 unix ms，无时区歧义 |

**边界**：`top_k > 记录数`、`n = 0`、空会话 id 都要覆盖。
