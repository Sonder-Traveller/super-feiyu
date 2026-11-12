// include/fish/core/interfaces.hpp
// 五个职责边界的抽象契约。依赖方向永远指向 core，core 不反向依赖实现。
// 换通道、换模型厂商、换存储后端，只换实现，编排层零改动。
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "fish/core/types.hpp"

namespace fish::core {

// ── 职责 1：通道（传输） ──────────────────────────────────
// 只负责把平台协议翻译成 InboundMessage，把 OutboundMessage 翻译回平台协议。
// 不允许在这里做任何业务判断（比如"要不要回复"）。
class IChannel {
 public:
  virtual ~IChannel() = default;

  [[nodiscard]] virtual Channel id() const noexcept = 0;
  [[nodiscard]] virtual ChannelCapability capability() const noexcept = 0;

  // 阻塞式拿下一条消息；返回空表示当前无消息（由调用方决定轮询节奏）。
  virtual Result<InboundMessage> poll() = 0;

  // 发送。attempt_no 从 1 开始，用于被动回复的 msg_seq 递增。
  virtual Error send(const OutboundMessage& msg, int attempt_no) = 0;
};

// ── 职责 2：推理能力（全部远程 API） ─────────────────────
// 本机不做任何推理，这里只有网络调用。
class IChatModel {
 public:
  virtual ~IChatModel() = default;

  struct Request {
    std::string system_prompt;               // 必须稳定且置前，命中前缀缓存
    std::vector<ContentBlock> user_blocks;   // 本轮用户输入（可含图片）
    std::vector<ContentBlock> history;       // 已渲染成扁平块的历史
    int max_tokens{512};
    double temperature{1.0};
  };

  struct Response {
    std::string text;
    int prompt_tokens{0};
    int cached_tokens{0};  // 缓存命中数，用于成本观测
    int output_tokens{0};
  };

  virtual Result<Response> complete(const Request& req) = 0;
};

class IEmbeddingModel {
 public:
  virtual ~IEmbeddingModel() = default;
  virtual Result<std::vector<float>> embed(const std::string& text) = 0;
  [[nodiscard]] virtual int dimension() const noexcept = 0;
};

class ISpeechToText {
 public:
  virtual ~ISpeechToText() = default;
  virtual Result<std::string> transcribe(const std::string& media_url) = 0;
};

class ITextToSpeech {
 public:
  virtual ~ITextToSpeech() = default;
  // 返回本地文件路径或可直接发送的地址
  virtual Result<std::string> synthesize(const std::string& text) = 0;
};

// 网页搜索。被动场景用得少，默认 provider = none 时不构造。
class IWebSearch {
 public:
  struct Hit {
    std::string title;
    std::string url;
    std::string snippet;
    double score{0.0};
  };

  virtual ~IWebSearch() = default;
  virtual Result<std::vector<Hit>> search(const std::string& query, int max_results) = 0;
};

// ── 职责 3：存储（只落盘，不推理） ───────────────────────
class IMemoryStore {
 public:
  virtual ~IMemoryStore() = default;

  struct Record {
    std::string conversation_id;
    std::string sender_id;
    std::string role;  // "user" | "assistant"
    std::string text;
    std::vector<float> embedding;
    TimePoint created_at{};
  };

  // 追加一条对话记录。持久化由实现决定（SQLite/JSONL 皆可）。
  virtual Error append(const Record& rec) = 0;

  // 按语义召回 top_k 条相关历史。embedding 为查询向量。
  virtual Result<std::vector<Record>> recall(const std::string& conversation_id,
                                             const std::vector<float>& query_embedding,
                                             int top_k) = 0;

  // 拉取最近 n 条（用于短期上下文窗口）。
  virtual Result<std::vector<Record>> recent(const std::string& conversation_id, int n) = 0;
};

// ── 职责 4：人格 ─────────────────────────────────────────
// 只产出稳定前缀 + 动态尾部的分层提示词，不碰网络。
class IPersona {
 public:
  virtual ~IPersona() = default;

  // 稳定前缀：必须逐字节固定，否则前缀缓存失效、成本翻 50 倍。
  [[nodiscard]] virtual std::string stable_prefix() const = 0;

  // 动态尾部：本轮才变化的内容（时间、召回记忆、微约束）。
  [[nodiscard]] virtual std::string dynamic_suffix(const InboundMessage& msg,
                                                   const std::string& recalled_memory) const = 0;
};

// ── 职责 5：回复预算 ─────────────────────────────────────
// 被动通道对"一条入站能回几条"有硬限制，集中管理避免各通道重复实现。
class IReplyBudget {
 public:
  virtual ~IReplyBudget() = default;
  [[nodiscard]] virtual int remaining(const std::string& message_id) const = 0;
  virtual bool consume(const std::string& message_id) = 0;
};

}  // namespace fish::core

