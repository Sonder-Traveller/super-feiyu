// src/memory_store.cpp
#include "fish/store/memory_store.hpp"

#include <algorithm>
#include <cmath>

namespace fish::store {

using core::Error;
using core::ErrorCode;
using core::Result;

InMemoryStore::InMemoryStore(std::size_t per_conversation_capacity)
    : capacity_(per_conversation_capacity == 0 ? 1 : per_conversation_capacity) {}

Error InMemoryStore::append(const Record& rec) {
  if (rec.conversation_id.empty()) {
    return Error::make(ErrorCode::kBadRequest, "empty_conversation_id");
  }
  std::lock_guard<std::mutex> lock(mu_);
  auto& dq = data_[rec.conversation_id];
  dq.push_back(rec);
  while (dq.size() > capacity_) dq.pop_front();
  return Error::none();
}

Result<std::vector<InMemoryStore::Record>> InMemoryStore::recent(
    const std::string& conversation_id, int n) {
  if (n <= 0) return Result<std::vector<Record>>::ok({});

  std::lock_guard<std::mutex> lock(mu_);
  auto it = data_.find(conversation_id);
  if (it == data_.end()) return Result<std::vector<Record>>::ok({});

  const auto& dq = it->second;
  const std::size_t take = std::min<std::size_t>(dq.size(), static_cast<std::size_t>(n));
  std::vector<Record> out(dq.end() - static_cast<std::ptrdiff_t>(take), dq.end());
  return Result<std::vector<Record>>::ok(std::move(out));
}

float InMemoryStore::cosine(const std::vector<float>& a, const std::vector<float>& b) {
  if (a.empty() || b.empty() || a.size() != b.size()) return 0.0F;
  double dot = 0.0;
  double na = 0.0;
  double nb = 0.0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    dot += static_cast<double>(a[i]) * static_cast<double>(b[i]);
    na += static_cast<double>(a[i]) * static_cast<double>(a[i]);
    nb += static_cast<double>(b[i]) * static_cast<double>(b[i]);
  }
  if (na <= 0.0 || nb <= 0.0) return 0.0F;
  return static_cast<float>(dot / (std::sqrt(na) * std::sqrt(nb)));
}

Result<std::vector<InMemoryStore::Record>> InMemoryStore::recall(
    const std::string& conversation_id, const std::vector<float>& query_embedding,
    int top_k) {
  if (top_k <= 0 || query_embedding.empty()) {
    return Result<std::vector<Record>>::ok({});
  }

  std::lock_guard<std::mutex> lock(mu_);
  auto it = data_.find(conversation_id);
  if (it == data_.end()) return Result<std::vector<Record>>::ok({});

  std::vector<std::pair<float, const Record*>> scored;
  scored.reserve(it->second.size());
  for (const auto& rec : it->second) {
    if (rec.embedding.empty()) continue;  // 没有向量的记录不参与语义召回
    scored.emplace_back(cosine(query_embedding, rec.embedding), &rec);
  }

  const std::size_t k =
      std::min<std::size_t>(scored.size(), static_cast<std::size_t>(top_k));
  std::partial_sort(scored.begin(), scored.begin() + static_cast<std::ptrdiff_t>(k),
                    scored.end(),
                    [](const auto& lhs, const auto& rhs) { return lhs.first > rhs.first; });

  std::vector<Record> out;
  out.reserve(k);
  for (std::size_t i = 0; i < k; ++i) {
    if (scored[i].first <= 0.0F) continue;  // 负相关直接丢弃
    out.push_back(*scored[i].second);
  }
  return Result<std::vector<Record>>::ok(std::move(out));
}

}  // namespace fish::store
