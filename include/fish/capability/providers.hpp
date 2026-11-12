// include/fish/capability/providers.hpp
// 多 Provider 统一构造：配置里 provider 写谁，就构造谁。
// 上层只认接口，不认厂商。
#pragma once

#include <memory>
#include <string>

#include "fish/core/config.hpp"
#include "fish/core/interfaces.hpp"
#include "fish/core/types.hpp"

namespace fish::capability {

// 各类能力的 provider 名白名单，用于启动校验
const char* chat_provider_list();      // "deepseek,openai,anthropic,siliconflow,ollama"
const char* embedding_provider_list(); // "siliconflow,openai,ollama"
const char* stt_provider_list();       // "siliconflow,groq,openai,local"
const char* tts_provider_list();       // "edge,openai,siliconflow"
const char* search_provider_list();    // "none,deepseek,tavily,bocha"

struct ChatConfig {
  std::string provider{"deepseek"};
  std::string base_url;
  std::string api_key;
  std::string model;
  int max_tokens{512};
  double temperature{1.0};
  int timeout_ms{30000};
};

struct EmbeddingConfig {
  std::string provider{"siliconflow"};
  std::string base_url;
  std::string api_key;
  std::string model;
  int dimension{1024};
  int timeout_ms{20000};
};

struct SttConfig {
  std::string provider{"siliconflow"};
  std::string base_url;
  std::string api_key;
  std::string model;
  std::string language{"auto"};
  int timeout_ms{30000};
};

struct TtsConfig {
  std::string provider{"edge"};
  std::string base_url;
  std::string api_key;
  std::string voice{"zh-CN-XiaoyiNeural"};
  std::string format{"mp3"};
  int timeout_ms{20000};
};

struct SearchConfig {
  std::string provider{"none"};
  std::string base_url;
  std::string api_key;
  int max_results{5};
  int timeout_ms{15000};
};

// 工厂：provider 未知时返回 kUnsupported，并在 detail 里列出支持的列表
core::Result<std::unique_ptr<core::IChatModel>> make_chat(const ChatConfig& cfg);
core::Result<std::unique_ptr<core::IEmbeddingModel>> make_embedding(const EmbeddingConfig& cfg);
core::Result<std::unique_ptr<core::ISpeechToText>> make_stt(const SttConfig& cfg);
core::Result<std::unique_ptr<core::ITextToSpeech>> make_tts(const TtsConfig& cfg);
core::Result<std::unique_ptr<core::IWebSearch>> make_search(const SearchConfig& cfg);

// 从 Config 读取各段配置
ChatConfig load_chat_config(const core::Config& c);
EmbeddingConfig load_embedding_config(const core::Config& c);
SttConfig load_stt_config(const core::Config& c);
TtsConfig load_tts_config(const core::Config& c);
SearchConfig load_search_config(const core::Config& c);

}  // namespace fish::capability
