// tests/probe_capability.cpp
// 真实调用测试：不走 mock，直接打各家真实端点。
// 目的是验证「我们自己的实现」而不是「curl 能不能通」——
// 各家字段名的坑（serper 的 link、brave 的嵌套、
// 火山的 base64 分片）只有走真实解析才暴露得出来。
//
// 用环境变量提供凭据，没有就跳过该组。
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

import fish.core;
import fish.log;
import fish.config;
import fish.capability;

namespace {

int g_pass = 0;
int g_fail = 0;
int g_skip = 0;

void check(bool ok, const std::string& name, const std::string& detail = "") {
  if (ok) {
    ++g_pass;
    std::printf("[PASS] %s\n", name.c_str());
  } else {
    ++g_fail;
    std::printf("[FAIL] %s %s\n", name.c_str(), detail.c_str());
  }
}

void skip(const std::string& name, const std::string& why) {
  ++g_skip;
  std::printf("[SKIP] %s (%s)\n", name.c_str(), why.c_str());
}

const char* env(const char* k) {
  const char* v = std::getenv(k);
  return (v != nullptr && *v != '\0') ? v : nullptr;
}

// ── 向量：真调 SiliconFlow ──
void test_embedding_siliconflow() {
  const char* key = env("SILICONFLOW_API_KEY");
  if (key == nullptr) {
    skip("embedding/siliconflow", "SILICONFLOW_API_KEY 未设置");
    return;
  }
  fish::capability::EmbeddingConfig cfg;
  cfg.provider = "siliconflow";
  cfg.base_url = "https://api.siliconflow.cn/v1";
  cfg.api_key = key;
  cfg.model = "BAAI/bge-m3";
  cfg.dimension = 1024;

  auto m = fish::capability::make_embedding(cfg);
  if (!m.ok()) {
    check(false, "embedding/siliconflow 构造", m.error.detail);
    return;
  }
  auto v = (*m.value)->embed("今天天气不错");
  if (!v.ok()) {
    check(false, "embedding/siliconflow 调用", v.error.detail);
    return;
  }
  check(v.value->size() == 1024, "embedding/siliconflow 维度=1024",
        "got " + std::to_string(v.value->size()));
  // 同一句话两次结果应该几乎一致。
  // 上游有微小浮点抖动（实测 bge-m3 第 3 次就变了，量级 ~1e-4），
  // 所以阈值放宽到「余弦相似度 > 0.9999」而不是逐位相等。
  auto v2 = (*m.value)->embed("今天天气不错");
  bool same = false;
  if (v2.ok() && v2.value->size() == v.value->size()) {
    double dot = 0, na = 0, nb = 0;
    for (std::size_t i = 0; i < v.value->size(); ++i) {
      dot += (*v.value)[i] * (*v2.value)[i];
      na += (*v.value)[i] * (*v.value)[i];
      nb += (*v2.value)[i] * (*v2.value)[i];
    }
    const double cos = (na > 0 && nb > 0) ? dot / std::sqrt(na * nb) : 0.0;
    same = cos > 0.9999;
    std::printf("       余弦相似度: %.8f\n", cos);
  }
  check(same, "embedding/siliconflow 确定性（余弦>0.9999）");

  // 不同文本的向量应该不同——否则等于没算
  auto v3 = (*m.value)->embed("量子力学的基本假设");
  bool different = false;
  if (v3.ok() && v3.value->size() == v.value->size()) {
    double dot = 0, na = 0, nb = 0;
    for (std::size_t i = 0; i < v.value->size(); ++i) {
      dot += (*v.value)[i] * (*v3.value)[i];
      na += (*v.value)[i] * (*v.value)[i];
      nb += (*v3.value)[i] * (*v3.value)[i];
    }
    const double cos = (na > 0 && nb > 0) ? dot / std::sqrt(na * nb) : 1.0;
    different = cos < 0.99;
    std::printf("       无关文本余弦: %.8f\n", cos);
  }
  check(different, "embedding/siliconflow 区分不同文本");
}

// ── ASR：真调 SiliconFlow SenseVoice ──
void test_stt_siliconflow(const std::string& audio_name) {
  const char* key = env("SILICONFLOW_API_KEY");
  if (key == nullptr || audio_name.empty()) {
    skip("stt/siliconflow", "缺 key 或音频");
    return;
  }
  fish::capability::SttConfig cfg;
  cfg.provider = "siliconflow";
  cfg.base_url = "https://api.siliconflow.cn/v1";
  cfg.api_key = key;
  cfg.model = "FunAudioLLM/SenseVoiceSmall";
  cfg.language = "auto";

  auto s = fish::capability::make_stt(cfg);
  if (!s.ok()) {
    check(false, "stt/siliconflow 构造", s.error.detail);
    return;
  }
  auto t = (*s.value)->transcribe("http://127.0.0.1:18999/" + audio_name);
  if (!t.ok()) {
    check(false, "stt/siliconflow 调用", t.error.detail);
    return;
  }
  check(!t.value->empty(), "stt/siliconflow 返回非空文本", "got: " + *t.value);
  std::printf("       转写结果: %s\n", t.value->c_str());
}

// ── 搜索：真调（有 key 的）──
void test_search(const char* provider, const char* key_env, const char* base,
                 const char* engine) {
  const std::string name = std::string("search/") + provider;
  const char* key = env(key_env);
  if (key == nullptr) {
    skip(name, std::string(key_env) + " 未设置");
    return;
  }
  fish::capability::SearchConfig cfg;
  cfg.provider = provider;
  cfg.base_url = base;
  cfg.api_key = key;
  cfg.api_engine = engine != nullptr ? engine : "";
  cfg.max_results = 3;
  cfg.timeout_ms = 20000;

  auto s = fish::capability::make_search(cfg);
  if (!s.ok()) {
    check(false, name + " 构造", s.error.detail);
    return;
  }
  auto hits = (*s.value)->search("DeepSeek", 3);
  if (!hits.ok()) {
    check(false, name + " 调用", hits.error.detail);
    return;
  }
  check(!hits.value->empty(), name + " 有结果",
        "got " + std::to_string(hits.value->size()));
  if (!hits.value->empty()) {
    const auto& h = hits.value->front();
    std::printf("       首条: title=%s url=%s\n", h.title.c_str(), h.url.c_str());
    // 解析对了才有这些字段——字段名映射错的话这里会是空的
    check(!h.title.empty() || !h.snippet.empty(), name + " 标题/摘要非空");
  }
}

// ── TTS：真调 edge（无 key，本机）──
void test_tts_edge() {
  fish::capability::TtsConfig cfg;
  cfg.provider = "edge";
  cfg.voice = "zh-CN-XiaoyiNeural";
  cfg.format = "mp3";

  auto t = fish::capability::make_tts(cfg);
  if (!t.ok()) {
    check(false, "tts/edge 构造", t.error.detail);
    return;
  }
  auto path = (*t.value)->synthesize("你好，这是一次测试。");
  if (!path.ok()) {
    check(false, "tts/edge 合成", path.error.detail);
    return;
  }
  std::ifstream f(*path.value, std::ios::binary | std::ios::ate);
  const auto size = f ? static_cast<long long>(f.tellg()) : 0;
  check(size > 1000, "tts/edge 产出音频", "bytes=" + std::to_string(size));
  std::printf("       落盘: %s (%lld bytes)\n", path.value->c_str(), size);
  std::remove(path.value->c_str());
}

// ── TTS：火山（有凭据才测）──
void test_tts_volcengine() {
  const char* app_id = env("VOLCENGINE_TTS_APP_ID");
  const char* token = env("VOLCENGINE_TTS_TOKEN");
  if (app_id == nullptr || token == nullptr) {
    skip("tts/volcengine", "缺 VOLCENGINE_TTS_APP_ID / VOLCENGINE_TTS_TOKEN");
    return;
  }
  fish::capability::TtsConfig cfg;
  cfg.provider = "volcengine";
  cfg.app_id = app_id;
  cfg.api_key = token;
  cfg.voice = "zh_female_shuangkuaisisi_moon_bigtts";
  cfg.cluster = "volcano_tts";
  cfg.format = "mp3";

  auto t = fish::capability::make_tts(cfg);
  if (!t.ok()) {
    check(false, "tts/volcengine 构造", t.error.detail);
    return;
  }
  auto path = (*t.value)->synthesize("火山引擎语音合成测试。");
  if (!path.ok()) {
    check(false, "tts/volcengine 合成", path.error.detail);
    return;
  }
  std::ifstream f(*path.value, std::ios::binary | std::ios::ate);
  const auto size = f ? static_cast<long long>(f.tellg()) : 0;
  check(size > 1000, "tts/volcengine 产出音频", "bytes=" + std::to_string(size));
  std::remove(path.value->c_str());
}

}  // namespace

int main(int argc, char** argv) {
  fish::log::Logger::instance().set_level(fish::log::LogLevel::kWarn);
  fish::capability::HttpClient::global_init();

  const std::string audio = argc > 1 ? argv[1] : "";

  test_embedding_siliconflow();
  test_stt_siliconflow(audio);
  test_tts_edge();
  test_tts_volcengine();

  test_search("tavily", "TAVILY_API_KEY", "https://api.tavily.com", "");
  test_search("bocha", "BOCHA_API_KEY", "https://api.bochaai.com", "");
  test_search("serper", "SERPER_API_KEY", "https://google.serper.dev", "");
  test_search("brave", "BRAVE_API_KEY", "https://api.search.brave.com", "");
  test_search("exa", "EXA_API_KEY", "https://api.exa.ai", "");
  test_search("jina", "JINA_API_KEY", "https://s.jina.ai", "");
  test_search("zhipu", "ZHIPU_API_KEY", "https://open.bigmodel.cn", "search_std");
  test_search("google", "GOOGLE_SEARCH_KEY", "https://www.googleapis.com",
              env("GOOGLE_SEARCH_CX"));
  test_search("deepseek", "DEEPSEEK_API_KEY", "https://api.deepseek.com", "");
  test_search("searxng", "SEARXNG_URL", "http://localhost:8080", "");

  fish::capability::HttpClient::global_cleanup();
  std::printf("\n%d passed, %d failed, %d skipped\n", g_pass, g_fail, g_skip);
  return g_fail == 0 ? 0 : 1;
}
