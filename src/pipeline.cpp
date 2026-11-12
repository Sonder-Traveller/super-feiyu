// src/pipeline.cpp
#include "fish/orchestrator/pipeline.hpp"

#include <utility>

namespace fish::orchestrator {

using core::BlockKind;
using core::ContentBlock;
using core::Error;
using core::ErrorCode;
using core::Result;

ReplyPolicy::Decision ReplyPolicy::evaluate(const core::InboundMessage& msg,
                                            const core::ChannelCapability& cap) const {
  // 私聊：一律回。
  if (msg.kind == core::ChatKind::kPrivate) {
    if (!cap.supports_private) return {false, "private_unsupported"};
    return {true, "private"};
  }

  // 群聊：只有被 @ 才回，这是"被动对话"的核心红线。
  if (!cap.supports_group) return {false, "group_unsupported"};
  if (!msg.mentioned_me) return {false, "group_not_mentioned"};
  return {true, "group_mentioned"};
}

Pipeline::Pipeline(Deps deps) : deps_(std::move(deps)) {}

Result<std::vector<ContentBlock>> Pipeline::normalize_input(
    const core::InboundMessage& msg) {
  std::vector<ContentBlock> out;
  out.reserve(msg.blocks.size());

  for (const auto& block : msg.blocks) {
    if (block.kind == BlockKind::kText) {
      out.push_back(block);
      continue;
    }
    if (block.kind == BlockKind::kImage) {
      // 图片原样透传，由模型侧多模态能力处理（deepseek-flash 原生支持）。
      out.push_back(block);
      continue;
    }
    if (block.kind == BlockKind::kVoice) {
      if (deps_.stt == nullptr) {
        // 没挂 STT 就降级成占位文本，绝不静默丢消息。
        ContentBlock note;
        note.kind = BlockKind::kText;
        note.text = "[语音消息，未启用转写]";
        out.push_back(std::move(note));
        continue;
      }
      auto stt = deps_.stt->transcribe(block.media_url);
      if (!stt.ok()) {
        ContentBlock note;
        note.kind = BlockKind::kText;
        note.text = "[语音消息转写失败]";
        out.push_back(std::move(note));
        continue;
      }
      ContentBlock text;
      text.kind = BlockKind::kText;
      text.text = std::move(*stt.value);
      out.push_back(std::move(text));
    }
  }

  if (out.empty()) {
    return Result<std::vector<ContentBlock>>::fail(ErrorCode::kBadRequest,
                                                   "message_has_no_usable_block");
  }
  return Result<std::vector<ContentBlock>>::ok(std::move(out));
}

Result<std::vector<ContentBlock>> Pipeline::build_history(const core::InboundMessage& msg,
                                                          const std::string& query) {
  std::vector<ContentBlock> history;

  if (deps_.memory == nullptr) return Result<std::vector<ContentBlock>>::ok({});

  // 语义召回：只有在挂了 embedding 时才有意义。
  std::vector<core::IMemoryStore::Record> recalled;
  if (deps_.embedding != nullptr) {
    auto vec = deps_.embedding->embed(query);
    if (vec.ok()) {
      auto r = deps_.memory->recall(msg.conversation_id, *vec.value, 5);
      if (r.ok()) recalled = std::move(*r.value);
    }
  }

  // 短期窗口：无条件拉最近若干条，保证上下文连贯。
  auto recent = deps_.memory->recent(msg.conversation_id, 8);
  if (recent.ok()) {
    for (const auto& rec : *recent.value) {
      ContentBlock block;
      block.kind = BlockKind::kText;
      block.text = rec.role + ": " + rec.text;
      history.push_back(std::move(block));
    }
  }

  // 召回结果单列一段，便于模型区分"长期记忆"与"最近对话"。
  if (!recalled.empty()) {
    std::string joined = "【长期记忆】\n";
    for (const auto& rec : recalled) {
      joined += "- " + rec.text + "\n";
    }
    ContentBlock block;
    block.kind = BlockKind::kText;
    block.text = std::move(joined);
    history.push_back(std::move(block));
  }

  return Result<std::vector<ContentBlock>>::ok(std::move(history));
}

Result<int> Pipeline::handle(core::IChannel& channel, const core::InboundMessage& msg) {
  const auto cap = channel.capability();
  const auto decision = policy_.evaluate(msg, cap);
  if (!decision.should_reply) {
    return Result<int>::ok(0);  // 策略决定不参与，直接返回
  }

  if (deps_.budget != nullptr && deps_.budget->remaining(msg.message_id) <= 0) {
    return Result<int>::fail(ErrorCode::kBudgetExhausted, "reply_budget_exhausted");
  }

  auto input = normalize_input(msg);
  if (!input.ok()) return Result<int>::fail(input.error.code, input.error.detail);

  // 用纯文本部分作为召回查询
  std::string query;
  for (const auto& b : *input.value) {
    if (b.kind == BlockKind::kText) query += b.text;
  }

  auto history = build_history(msg, query);
  if (!history.ok()) return Result<int>::fail(history.error.code, history.error.detail);

  core::IChatModel::Request req;
  req.system_prompt = deps_.persona->stable_prefix();
  req.user_blocks = std::move(*input.value);
  req.history = std::move(*history.value);

  auto resp = deps_.chat->complete(req);
  if (!resp.ok()) return Result<int>::fail(resp.error.code, resp.error.detail);

  // 落盘：用户消息与助手回复都要记，否则记忆不完整。
  if (deps_.memory != nullptr) {
    core::IMemoryStore::Record user_rec;
    user_rec.conversation_id = msg.conversation_id;
    user_rec.sender_id = msg.sender_id;
    user_rec.role = "user";
    user_rec.text = query;
    user_rec.created_at = core::Clock::now();
    deps_.memory->append(user_rec);

    core::IMemoryStore::Record bot_rec;
    bot_rec.conversation_id = msg.conversation_id;
    bot_rec.sender_id = "bot";
    bot_rec.role = "assistant";
    bot_rec.text = resp.value->text;
    bot_rec.created_at = core::Clock::now();
    deps_.memory->append(bot_rec);
  }

  core::OutboundMessage out;
  out.conversation_id = msg.conversation_id;
  out.reply_to_message_id = msg.message_id;  // 被动回复凭证
  out.context_token = msg.context_token;

  ContentBlock text_block;
  text_block.kind = BlockKind::kText;
  text_block.text = resp.value->text;
  out.blocks.push_back(std::move(text_block));

  // 语音只在通道支持且挂了 TTS 时附加，且不占用额外语义——
  // 注意：QQ 官方把语音和文本算作两条回复，这里交给预算层裁决。
  if (deps_.tts != nullptr && cap.supports_voice_out) {
    auto audio = deps_.tts->synthesize(resp.value->text);
    if (audio.ok()) {
      ContentBlock voice;
      voice.kind = BlockKind::kVoice;
      voice.media_url = std::move(*audio.value);
      voice.mime = "audio/mpeg";
      out.blocks.push_back(std::move(voice));
    }
  }

  // 按通道预算裁剪：超出部分直接丢弃，避免整条发送失败。
  int allow = cap.max_replies_per_message;
  if (static_cast<int>(out.blocks.size()) > allow) {
    out.blocks.resize(static_cast<std::size_t>(allow));
  }

  int sent = 0;
  for (int i = 0; i < static_cast<int>(out.blocks.size()); ++i) {
    if (deps_.budget != nullptr && !deps_.budget->consume(msg.message_id)) break;

    core::OutboundMessage one;
    one.conversation_id = out.conversation_id;
    one.reply_to_message_id = out.reply_to_message_id;
    one.context_token = out.context_token;
    one.blocks.push_back(out.blocks[static_cast<std::size_t>(i)]);

    auto err = channel.send(one, i + 1);
    if (!err.ok()) {
      return Result<int>::fail(err.code, err.detail);
    }
    ++sent;
  }

  return Result<int>::ok(sent);
}

}  // namespace fish::orchestrator
