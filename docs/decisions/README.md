# ADR 索引

关键架构决策记录。每条 ADR 回答：**为什么这样选，放弃了什么。**

| 编号 | 决策 | 状态 |
|---|---|---|
| [001](001-no-napcat.md) | 不用 NapCat，直连官方 Bot API | 已定 |
| [002](002-zero-local-inference.md) | 本机零推理，全部远程 API | 已定 |
| [003](003-prefix-cache-first.md) | 前缀缓存优先，强制人格分层 | 已定 |
| [004](004-no-exceptions.md) | 不用异常，统一 Result | 已定 |
| [005](005-blocking-io-threads.md) | 阻塞 IO + 固定线程池，不用异步 | 已定 |
| [006](006-striped-locks.md) | 会话串行化用条带锁，不用每会话队列 | 已定 |
| [007](007-brute-force-vector.md) | 向量检索暴力扫描，不用 FAISS | 已定 |
| [008](008-two-vendor-keys.md) | 必须两个厂商 key（DeepSeek 无 embedding） | 已定 |
