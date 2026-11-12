// include/fish/store/memory_store.hpp
#pragma once

#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "fish/core/interfaces.hpp"

namespace fish::store {

// 极简记忆实现：进程内环形缓冲 + 余弦相似度召回。
// 刻意不用 SQLite/FAISS —— 1G 机器上先把正确性跑通，再谈持久化。
// 接口已按 IMemoryStore 抽象好，将来换 SQLite 实现不影响编排层。
class InMemoryStore final : public core::IMemoryStore {
 public:
  explicit InMemoryStore(std::size_t per_conversation_capacity = 500);

  core::Error append(const Record& rec) override;
  core::Result<std::vector<Record>> recall(const std::string& conversation_id,
                                           const std::vector<float>& query_embedding,
                                           int top_k) override;
  core::Result<std::vector<Record>> recent(const std::string& conversation_id, int n) override;

 private:
  static float cosine(const std::vector<float>& a, const std::vector<float>& b);

  std::size_t capacity_;
  mutable std::mutex mu_;
  std::unordered_map<std::string, std::deque<Record>> data_;
};

}  // namespace fish::store
