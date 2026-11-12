# ADR-007：向量检索暴力扫描，不用 FAISS

**状态**：已定

---

## 决策

用 **SQLite 存向量 + 进程内暴力余弦扫描**，不引入 FAISS 或向量数据库。

## 数据规模论证

单条鱼的个人使用场景：

| 项 | 估算 |
|---|---|
| 每天消息 | 100–200 条 |
| 一年累计 | 约 5 万条 |
| 参与向量的条数 | 约 1 万（长篇才 embed） |
| 向量维度 | 1024 |
| 单向量大小 | 4 KB（float32） |
| **向量总大小** | **约 40 MB** |

## 性能论证

暴力扫描 1 万条 × 1024 维点积：

- 约 1000 万次乘加
- 2 核 CPU 单核约 10^9 FMA/s
- **耗时约 10 ms**

**完全可接受。** 用户等 3 秒都不在乎，10 ms 无所谓。

## 两阶段优化

避免延迟随数据量线性增长：

```sql
-- 阶段 1：只取最近 5000 条候选
SELECT vector FROM embeddings e
JOIN messages m ON m.id = e.message_id
WHERE m.conversation_id = ?
ORDER BY m.created_at DESC LIMIT 5000;
```

```cpp
// 阶段 2：候选中暴力算相似度，取 top_k
std::partial_sort(candidates, top_k, by_cosine(query_vec));
```

`partial_sort` 是 O(n log k)，比全排序快得多。

## 被否决的方案

| 方案 | 否决理由 |
|---|---|
| FAISS | C++ 库体积大（数 MB），索引常驻内存，为百万级设计 |
| sqlite-vec 扩展 | 可行，但引入外部扩展；当前规模用不上 |
| 专用向量库（Qdrant 等） | 需要独立进程，内存翻倍 |
| 不存向量，纯关键词 | 中文召回质量差 |

## 升级路径

若数据量超过 **10 万条**（约 10 年），改用 sqlite-vec：

- 接口 `IMemoryStore` 不变
- 只换实现
- 预估耗时仍在 100 ms 内

**当前不做，但接口预留。**

## 序列化约定

```cpp
// float32 小端，紧凑存储，无分隔符
std::string serialize(const std::vector<float>& v) {
  std::string out(v.size() * 4, '\0');
  std::memcpy(out.data(), v.data(), out.size());
  return out;
}
```

显式约定小端，避免将来在 ARM 上踩坑（虽然当前都是小端）。

## 内存控制

向量**不常驻内存**：

- 存 BLOB 在 SQLite
- 检索时按需加载候选集
- 查询完释放

这样数据库再大，进程 RSS 也不会线性增长。
