// src/fish.capability.cppm
// 远程能力：HTTP 基础设施 + 多 provider 统一接口。
// 本机零推理，此处全是网络调用。
module;
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

export module fish.capability;

import fish.core;
import fish.log;
import fish.config;

export namespace fish::capability {

struct HttpRequest {
  std::string method{"POST"};
  // UPLOAD 方法时真正发的 HTTP verb（CDN 上传是 POST）。
  std::string custom_verb;
  std::string url;
  std::map<std::string, std::string> headers;
  std::string body;
  int timeout_ms{30000};
  int max_retries{2};
  bool want_binary{false};

  // ── multipart/form-data ──
  // 有序：OCR / 文件解析要求 purpose 字段排在 file 之前，
  // 用 map 会按 key 排序把顺序打乱。
  std::vector<std::pair<std::string, std::string>> form_fields;
  struct FormFile {
    std::string name;   // 表单字段名，如 "file"
    std::string path;   // 本地文件路径
    std::string mime;   // 空 = 按扩展名猜
    std::string filename;  // 空 = 用路径的文件名
  };
  std::vector<FormFile> form_files;

  // DNS 钉住表：每项形如 "host:port:1.2.3.4" 或 "host:port:[::1]"。
  // 非空时转成 CURLOPT_RESOLVE 传给 libcurl，让本次连接只连这里
  // 列出的地址。用于 SSRF 防护：先自己解析并校验 IP，再把校验过的
  // 结果钉住，避免「校验时解析到公网、连接时又解析到内网」的 rebinding。
  std::vector<std::string> resolve_entries;
};

// URL 查询参数转义（空格/+/斜杠/等号等）。微信 CDN 上传的
// encrypted_query_param 里有 '=' 和 '/'，必须转义。
inline std::string url_encode(const std::string& s) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size() * 3);
  for (unsigned char c : s) {
    const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                      (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                      c == '~';
    if (safe) {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 0x0F];
    }
  }
  return out;
}

struct HttpResult {
  int status{0};
  std::string body;
  std::string bytes;
  std::map<std::string, std::string> headers;
};

// 按 provider 独立熔断：一家挂掉不拖累另一家
class CircuitBreaker {
 public:
  explicit CircuitBreaker(std::string name, int threshold = 5,
                          std::chrono::seconds cooldown = std::chrono::seconds(30));
  [[nodiscard]] bool allow() const;
  void on_success();
  void on_failure();
  [[nodiscard]] bool is_open() const;

 private:
  std::string name_;
  int threshold_;
  std::chrono::seconds cooldown_;
  mutable std::mutex mu_;
  int failures_{0};
  fish::core::TimePoint open_until_{};
};

class HttpClient {
 public:
  static HttpClient& instance();
  static void global_init();
  static void global_cleanup();

  fish::core::Result<HttpResult> perform(const HttpRequest& req,
                                         CircuitBreaker* breaker = nullptr);
  fish::core::Result<HttpResult> post_json(
      const std::string& url, const std::map<std::string, std::string>& headers,
      const std::string& json_body, int timeout_ms = 30000,
      CircuitBreaker* breaker = nullptr);
  fish::core::Result<std::string> download(const std::string& url,
                                           std::size_t max_bytes = 16u << 20,
                                           const std::vector<std::string>& resolve_entries = {});

  // multipart 便捷入口。files 里 path 为空表示不传文件。
  fish::core::Result<HttpResult> post_form(
      const std::string& url, const std::map<std::string, std::string>& headers,
      const std::vector<std::pair<std::string, std::string>>& fields,
      const std::vector<HttpRequest::FormFile>& files = {},
      int timeout_ms = 30000, CircuitBreaker* breaker = nullptr);

 private:
  HttpClient() = default;
};

// ── Provider 配置 ───────────────────────────────────────
struct ChatConfig {
  std::string provider{"deepseek"};
  std::string base_url{"https://api.deepseek.com"};
  std::string api_key;
  std::string model{"deepseek-flash"};
  // 0 / 负数 = 不发送该字段，让上游用自己的默认值。
  // 不主动设上限：模型该输出多长、多随机，交给上游和提示词决定。
  int max_tokens{0};
  double temperature{-1.0};
  // 思考强度：low / medium / high / xhigh / max。
  // **产品要求锁死 high**：控制面 PUT 已拒绝修改这个字段，
  // load_chat_config 的默认值也写死 "high"；配置里手改成别的合法
  // 档位仍会被发送（配置是用户手可编辑的），改成非法值则由发送侧
  // 退回 "high"。空字符串 = 不发该字段，用上游默认档（仅测试用）。
  std::string reasoning_effort;
  int timeout_ms{30000};

  // DeepSeek 服务端联网工具（deepseek_web_search / deepseek_read_url）。
  //
  // 开启后由 DeepSeek 服务端执行搜索与抓取，客户端不执行任何工具；
  // 配合 reasoning_effort=high 时 deepseek_web_search 是「深度思考搜索」。
  // 默认开（default_deepseek_web_tools()）：服务端工具按次计费，但它是
  // 产品默认路径（边思考边搜 + 引用原文）。
  // 只对 DeepSeek 官方端点生效（其他 provider 声明这两个 type 会 400）；
  // 端点不是官方时，本地 web_search/web_fetch 照常注册，搜索能力不受影响。
  bool deepseek_web_tools{true};

  // 上游是否支持「assistant 角色消息里带图片」。
  //
  // 这个值**不由配置文件决定**，而是由 fish.probe 自动探测后注入：
  //   - 启动时读缓存（默认 30 天有效），过期则重探
  //   - 上游行为会静默变更（如 WorkBuddy 会在 HTTP 200 下丢弃
  //     assistant 图片），靠人工配置必然过时
  // 见 fish.probe::Capabilities::assistant_image。
  bool supports_assistant_image{false};
};

struct EmbeddingConfig {
  std::string provider{"siliconflow"};
  std::string base_url{"https://api.siliconflow.cn/v1"};
  std::string api_key;
  std::string model{"BAAI/bge-m3"};
  int dimension{1024};
  // 空 = 按 provider 用默认路径（多数是 /embeddings）
  std::string path;
  // 请求体里输出维度字段的名字："" = 不发。
  // 默认空：多数模型的维度是固定的，多传这个字段反而 400
  // （实测 SiliconFlow 的 bge-m3 收到 dimensions 直接报 20015）。
  // 明确支持 Matryoshka 截断的（OpenAI text-embedding-3、Jina、
  // Voyage、Mistral）再按需写 dimensions / output_dimension。
  std::string dimensions_field;
  // 额外固定字段，如 Voyage 的 input_type、Gemini 的 taskType
  std::string extra_json;
  int timeout_ms{20000};
};

struct SttConfig {
  std::string provider{"siliconflow"};
  std::string base_url{"https://api.siliconflow.cn/v1"};
  std::string api_key;
  std::string model{"FunAudioLLM/SenseVoiceSmall"};
  std::string language{"auto"};
  std::string path;              // 空 = 按 provider 用默认路径
  // 需要转成 16k 单声道 WAV 的厂商（百度 / Azure / DeepSeek 等只收特定格式）
  bool need_wav16k{false};
  std::size_t max_upload_bytes{0};   // 0 = 不限（上游各自有限制）
  int timeout_ms{30000};
};

struct TtsConfig {
  std::string provider{"edge"};
  std::string base_url;
  std::string api_key;
  std::string voice{"zh-CN-XiaoyiNeural"};
  std::string format{"mp3"};
  std::string model;            // HTTP 类厂商用（如 tts-1 / CosyVoice2 / 火山 voice_type 走 voice）
  std::string path;             // 空 = 按 provider 用默认路径
  // 火山引擎：appid + access_token 是两套凭据，且 Must 放在 body 里的 app 段。
  std::string app_id;
  std::string cluster;          // 火山 cluster，如 volcano_tts
  // 语速/音量等厂商特有参数的直通 JSON（空 = 不发）
  std::string extra_json;
  int timeout_ms{20000};
};

// 网络搜索。provider 只是「用哪家的协议去问」，不代表只能接这家的服务：
// 自建/聚合端点用 generic。
//
// 各家差异收敛在三处：鉴权 header、请求体、响应里 title/url/snippet 的路径。
// 把这三处做成表，就不必为每家写一个类。
struct SearchConfig {
  std::string provider{"none"};
  std::string base_url;
  std::string path;        // 空 = 按 provider 用默认路径
  std::string api_key;
  std::string api_engine;  // 需要额外参数的厂商（Google cx / 智谱 search_engine）
  int max_results{5};
  int timeout_ms{15000};

  // search：是否要求返回网页正文（need_content）
  bool search_need_content{false};
  // fetch：给模型看的正文上限
  std::size_t fetch_max_chars{6000};

  // generic 抓取（我们直连目标站点，需要 SSRF 防护）
  bool allow_private_hosts{false};
  std::size_t fetch_max_bytes{2u << 20};
};

// 生图。与 TTS 同构：换厂商只改 provider/base_url/model。
struct ImageConfig {
  std::string provider{"none"};        // none | siliconflow | openai | bridge
  std::string base_url;
  std::string api_key;
  std::string model;
  std::string default_size{"1024x1024"};
  int timeout_ms{120000};              // 生图慢，给足时间
};

const char* chat_provider_list();
const char* embedding_provider_list();
const char* stt_provider_list();
const char* tts_provider_list();
const char* search_provider_list();
const char* image_provider_list();

// 共享密钥解析（导出给 fish.doc 使用）。
//
// 见实现处的详细说明。current_section 形如 "capability.stt"，
// provider 形如 "deepseek"。
std::string resolve_provider_key_for(const fish::config::Config& c,
                                     const std::string& current_section,
                                     const std::string& provider);

// 思考强度的锁定值（产品要求 high，不可调）。
//
// 三处要用同一个值，所以只定义一份：
//   1) 控制面 GET /v1/config 回显它（GUI 显示成只读）；
//   2) 控制面 PUT 拒绝修改时返回 effort_locked:<这个值>；
//   3) 运行时 load_chat_config 的兜底默认值。
// 各写各的 "high" 迟早会跑偏（曾经就出现控制面回显空串、运行时发 high）。
const char* locked_reasoning_effort();

// 端点是不是 DeepSeek 官方（服务端联网工具的唯一端点判据）。
//
// 判断依据必须是 base_url 而不是 provider：控制面在用户选
// 「DeepSeek 官方」时会把 provider 写成接入方式 "openai"（见
// fish.control.cppm 的 kChatDialect + ProviderPreset），所以
// provider 字段几乎永远是 openai，靠它判断会把开关彻底变成死开关。
[[nodiscard]] bool is_deepseek_endpoint(const std::string& provider,
                                         const std::string& base_url);

// deepseek_web_tools 的出厂默认值（true）。
//
// 单独定义一份而不是各处硬编码 true：控制面 GET 回显、运行时
// load_chat_config 的默认值必须同源，否则界面显示「关」而运行时
// 已经按「开」在跑（历史上 reasoning_effort 就出过这种错）。
[[nodiscard]] bool default_deepseek_web_tools();

// 服务端联网工具是否真正生效：既要看开关，也要看端点是不是 DeepSeek 官方。
// 两者缺一不可——开关开着但端点不是官方时，本地 web_search/web_fetch 必须照常注册。
//
// 这个函数是**唯一的判据来源**，capability（是否注入服务端工具）与
// main.cpp（是否注册本地搜索/抓取工具）都用它。两边各判各的会出
// 「搜索能力凭空消失」：一边不注入服务端工具、另一边又注销了本地工具，
// 而且没有任何报错。
//
// 注意：这里只收基本类型，**不要**在 export 接口里出现 fish::config::Config
// 这类外部模块的具体类型——clang 21 序列化 pcm 时会因为跨模块的具体类型
// 崩溃（崩在 ASTWriter::VisitUnresolvedLookupExpr，位置随机）。
// 需要读 config 的调用方请用下面的三参数版本，自己取好值传进来。
[[nodiscard]] bool server_web_tools_active_for(const std::string& provider,
                                               const std::string& base_url,
                                               bool enabled);

fish::core::Result<std::unique_ptr<fish::core::IChatModel>> make_chat(const ChatConfig& cfg);
fish::core::Result<std::unique_ptr<fish::core::IEmbeddingModel>> make_embedding(
    const EmbeddingConfig& cfg);
fish::core::Result<std::unique_ptr<fish::core::ISpeechToText>> make_stt(const SttConfig& cfg);
fish::core::Result<std::unique_ptr<fish::core::ITextToSpeech>> make_tts(const TtsConfig& cfg);
fish::core::Result<std::unique_ptr<fish::core::IWebSearch>> make_search(
    const SearchConfig& cfg);
fish::core::Result<std::unique_ptr<fish::core::IWebFetch>> make_fetch(
    const SearchConfig& cfg);
fish::core::Result<std::unique_ptr<fish::core::IImageModel>> make_image(const ImageConfig& cfg);

ChatConfig load_chat_config(const fish::config::Config& c);
EmbeddingConfig load_embedding_config(const fish::config::Config& c);
SttConfig load_stt_config(const fish::config::Config& c);
TtsConfig load_tts_config(const fish::config::Config& c);
SearchConfig load_search_config(const fish::config::Config& c);
ImageConfig load_image_config(const fish::config::Config& c);

}  // namespace fish::capability

// ═══ 实现 ═══════════════════════════════════════════════
module :private;

namespace fish::capability {

using fish::core::ErrorCode;
using fish::core::Result;
using json = nlohmann::json;

// 提供方清单。只留两类：
//   1. DeepSeek 官方（直出 deepseek-flash）
//   2. 确实托管 DeepSeek 模型的云：硅基流动、火山方舟、阿里百炼、百度千帆
//
// 底层模型锁死 deepseek-flash，所以「能选哪家」等价于「哪家真能出
// DeepSeek」——列一家不托管 DeepSeek 的厂商，用户选了也只会拿到
// 别的模型，等于绕过了锁定，所以干脆不列。
const char* chat_provider_list() {
  return "deepseek,workbuddy";
}

// 思考强度锁定值。定义在这里（而不是各调用点各写一个 "high"）：
// 控制面 GET 回显、PUT 拒绝、运行时默认值三处共用同一个来源。
const char* locked_reasoning_effort() {
  return "high";
}

// 端点是不是 DeepSeek 官方。
//
// 这两个服务端工具只有 DeepSeek 官方支持，声明给别家会 400。
//
// 判断依据必须是 base_url 而不是 provider：控制面在用户选
// 「DeepSeek 官方」时会把 provider 写成接入方式 "openai"（见
// fish.control.cppm 的 kChatDialect + ProviderPreset），所以
// provider 字段几乎永远是 openai，靠它判断会把开关彻底变成死开关。
bool is_deepseek_endpoint(const std::string& provider, const std::string& base_url) {
  if (provider == "deepseek") return true;
  std::string base = base_url;
  for (auto& ch : base) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return base.find("api.deepseek.com") != std::string::npos ||
         base.find("deepseek") != std::string::npos;
}

// 出厂默认值：开。
//
// 服务端联网工具是产品默认路径（边思考边搜 + 引用原文），默认关的话
// 绝大多数用户根本不会发现它存在。
bool default_deepseek_web_tools() { return true; }

// 三参数版本：判据的唯一实现。调用方自己从配置取值传进来。
//
// = 开关打开 且 端点是 DeepSeek 官方。
//
// 两个条件缺一不可。开关开着但端点不是官方（例如当前 WorkBuddy 桥接
// 部署）时，本地 web_search/web_fetch 必须照常注册——否则两个搜索工具
// 同时消失，搜索能力凭空消失，而且没有任何报错。
//
// 这里没有再判协议方言：make_chat 只接受 deepseek/workbuddy/openai
// 三家，全是 OpenAI 兼容协议（dialect_of 一律返回 kOpenAI），
// 方言条件恒成立，所以没必要在这里重复一遍。
bool server_web_tools_active_for(const std::string& provider, const std::string& base_url,
                                 bool enabled) {
  if (!enabled) return false;
  return is_deepseek_endpoint(provider, base_url);
}
// 向量：这几家都托管 DeepSeek，且各有向量接口。
// DeepSeek 官方没有向量端点，所以不在列表里。
// workbuddy 是本机桥接层：桥后面接谁就是谁，只要桥能出向量就可用。
const char* embedding_provider_list() {
  return "ark,dashscope,qianfan,siliconflow,workbuddy";
}
const char* stt_provider_list() {
  return "dashscope,deepseek,siliconflow,volcengine";
}
const char* tts_provider_list() {
  return "dashscope,deepseek,edge,siliconflow,volcengine";
}
// 搜索：这里必须与 ProviderSearch 里已实现的分支、以及 make_search 的
// 白名单保持一致。三者不一致的后果是「配置里写了 provider 就启动失败」
// 或者「分支写好了但永远走不到」——两者都踩过，所以清单只留一份：
// search_provider_list() 生成字符串，make_search 按同一字符串 split 校验。
const char* search_provider_list() {
  return "bocha,brave,deepseek,exa,generic,google,jina,none,searxng,serper,tavily,zhipu";
}
const char* image_provider_list() { return "bridge,openai,siliconflow"; }

namespace {

enum class Dialect { kOpenAI, kAnthropic };

Dialect dialect_of(const std::string& provider) {
  return provider == "anthropic" ? Dialect::kAnthropic : Dialect::kOpenAI;
}

std::size_t write_cb(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  const std::size_t total = size * nmemb;
  if (out->size() + total > (32u << 20)) return 0;  // 32MB 上限
  out->append(ptr, total);
  return total;
}

std::size_t header_cb(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
  auto* map = static_cast<std::map<std::string, std::string>*>(userdata);
  const std::size_t total = size * nmemb;
  std::string line(ptr, total);
  const auto colon = line.find(':');
  if (colon != std::string::npos) {
    std::string k = line.substr(0, colon);
    std::string v = line.substr(colon + 1);
    auto trim = [](std::string& s) {
      while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) {
        s.pop_back();
      }
      std::size_t i = 0;
      while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
      if (i > 0) s.erase(0, i);
    };
    trim(k);
    trim(v);
    (*map)[k] = v;
  }
  return total;
}

bool is_retryable(int status) { return status == 429 || (status >= 500 && status < 600); }

// 取字符串字段，缺失或类型不对都返回空串。
//
// 为什么不直接用 json::value("k","")：那个版本在字段存在但类型不是
// string（例如 null）时会抛 type_error，服务端工具解析里 null 是常态
// （web_fetch_tool_result 的 title 明确可能为 null）。
std::string str_or(const json& obj, const char* key, std::string def = {}) {
  if (!obj.is_object() || !obj.contains(key)) return def;
  const auto& v = obj[key];
  if (v.is_string()) return v.get<std::string>();
  if (v.is_null()) return def;
  return def;
}

// assistant 发图在「上游不支持」时的降级归属标签。
//
// 用结构化 XML 风格而非自然语言：
//   - 不会被模型误读成用户说的话
//   - 与正文明显区分，便于模型解析
//   - 换回支持 assistant 图的上游时，这段不会出现
constexpr const char* kAssistantImageTag =
    "<assistant_attachment role=\"assistant\" type=\"image\">"
    "这条消息的图片是 assistant 发送的，不是 user 发的。"
    "描述图片内容时，请明确这是 assistant 自己发过的图。"
    "</assistant_attachment>";

// 本地文件 → data URI（base64）。
// 模型只接受 http(s) URL 或 data URI，不认本地路径。
// 读不到或超过上限时返回空串，调用方跳过该块。
std::string local_file_to_data_uri(const std::string& path,
                                   const std::string& mime_hint = "") {
  constexpr std::size_t kMaxBytes = 8u << 20;   // 单图 8MB 上限

  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) return {};
  const auto size = in.tellg();
  if (size <= 0 || static_cast<std::size_t>(size) > kMaxBytes) return {};
  in.seekg(0);
  std::string bytes(static_cast<std::size_t>(size), '\0');
  in.read(bytes.data(), size);

  // mime 优先级：调用方给的 hint > 扩展名 > image/png 兜底。
  //
  // 反过来（扩展名覆盖 hint）会出错：图片按内容真实类型存成 .jpg
  // 之外的名字、或缓存里 png 被存成 .jpg 时，标错 mime 会让上游
  // 用错解码器。hint 是上游/通道告诉我们的真实类型，比文件名可信。
  const bool hint_usable =
      !mime_hint.empty() && mime_hint != "application/octet-stream";
  std::string mime = hint_usable ? mime_hint : std::string();
  if (!hint_usable) {
    const auto dot = path.find_last_of('.');
    const std::string ext =
        (dot == std::string::npos) ? "" : path.substr(dot);
    if (ext == ".jpg" || ext == ".jpeg") {
      mime = "image/jpeg";
    } else if (ext == ".png") {
      mime = "image/png";
    } else if (ext == ".gif") {
      mime = "image/gif";
    } else if (ext == ".webp") {
      mime = "image/webp";
    } else {
      mime = "image/png";
    }
  }

  // 极简 base64（注意 unsigned char 转换，避免符号扩展）
  static constexpr char kTable[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string b64;
  b64.reserve(((bytes.size() + 2) / 3) * 4);
  const auto at = [&bytes](std::size_t i) -> unsigned {
    return static_cast<unsigned>(static_cast<unsigned char>(bytes[i]));
  };
  std::size_t i = 0;
  while (i + 2 < bytes.size()) {
    const unsigned n = (at(i) << 16) | (at(i + 1) << 8) | at(i + 2);
    b64 += kTable[(n >> 18) & 0x3F];
    b64 += kTable[(n >> 12) & 0x3F];
    b64 += kTable[(n >> 6) & 0x3F];
    b64 += kTable[n & 0x3F];
    i += 3;
  }
  const std::size_t rest = bytes.size() - i;
  if (rest == 1) {
    const unsigned n = at(i) << 16;
    b64 += kTable[(n >> 18) & 0x3F];
    b64 += kTable[(n >> 12) & 0x3F];
    b64 += "==";
  } else if (rest == 2) {
    const unsigned n = (at(i) << 16) | (at(i + 1) << 8);
    b64 += kTable[(n >> 18) & 0x3F];
    b64 += kTable[(n >> 12) & 0x3F];
    b64 += kTable[(n >> 6) & 0x3F];
    b64 += '=';
  }
  return "data:" + mime + ";base64," + b64;
}

json to_blocks(const std::vector<fish::core::ContentBlock>& blocks) {
  json arr = json::array();
  for (const auto& b : blocks) {
    if (b.kind == fish::core::BlockKind::kText) {
      arr.push_back({{"type", "text"}, {"text", b.text}});
    } else if (b.kind == fish::core::BlockKind::kImage) {
      arr.push_back({{"type", "image_url"}, {"image_url", {{"url", b.media_url}}}});
    } else if (b.kind == fish::core::BlockKind::kFile) {
      // media_url 里存的是厂商返回的 file_id（Orchestrator 上传后填的）。
      // 文档内容由厂商侧保留，这里只发 id。
      if (!b.media_url.empty()) {
        arr.push_back({{"type", "file"}, {"file_id", b.media_url}});
      }
    }
  }
  return arr;
}

void extract_usage(const json& usage, fish::core::IChatModel::Response* out) {
  if (!usage.is_object()) return;
  auto pick = [&](std::initializer_list<const char*> keys) -> int {
    for (const char* k : keys) {
      if (usage.contains(k) && usage[k].is_number()) return usage[k].get<int>();
    }
    return 0;
  };
  out->prompt_tokens = pick({"prompt_tokens", "input_tokens"});
  out->output_tokens = pick({"completion_tokens", "output_tokens"});

  int cached = pick({"prompt_cache_hit_tokens"});
  if (cached == 0 && usage.contains("prompt_tokens_details") &&
      usage["prompt_tokens_details"].is_object()) {
    const auto& d = usage["prompt_tokens_details"];
    if (d.contains("cached_tokens") && d["cached_tokens"].is_number()) {
      cached = d["cached_tokens"].get<int>();
    }
  }
  out->cached_tokens = cached;
}

// ── 文本生成 ────────────────────────────────────────────
class HttpChat final : public fish::core::IChatModel {
 public:
  HttpChat(ChatConfig cfg, Dialect d, CircuitBreaker* b)
      : cfg_(std::move(cfg)), dial_(d), breaker_(b) {}

  Result<Response> complete(const Request& req) override {
    json payload;
    payload["model"] = cfg_.model;
    // max_tokens / temperature：配置里没写就不发，让上游用默认值。
    // 早期无条件发 512，等于替上游设了一个它没要求的输出上限。
    const int want_max = req.max_tokens > 0 ? req.max_tokens : cfg_.max_tokens;
    if (want_max > 0) payload["max_tokens"] = want_max;
    const double want_temp =
        req.temperature >= 0 ? req.temperature : cfg_.temperature;
    if (want_temp >= 0) payload["temperature"] = want_temp;
    // 思考强度：产品要求锁死 high。
    // 控制面 PUT 已拒绝修改这个字段，load_chat_config 的默认值也是 "high"，
    // 这里再兜一层：配置被手改成非法档位时退回 high，不发一个上游不认的值。
    // 空字符串 = 不发（上游默认档），保留给测试用。
    if (!cfg_.reasoning_effort.empty()) {
      static const std::set<std::string> kEfforts{"low", "medium", "high", "xhigh",
                                                  "max"};
      const bool valid = kEfforts.count(cfg_.reasoning_effort) > 0;
      if (!valid) {
        ::fish::log::log_line(::fish::log::LogLevel::kWarn, "capability",
                              "effort_invalid")
            .field("got", cfg_.reasoning_effort)
            .field("fallback", "high");
      }
      payload["reasoning_effort"] = valid ? cfg_.reasoning_effort : "high";
    }

    json msgs = json::array();
    if (dial_ == Dialect::kOpenAI) {
      // system 单独置前，保证前缀缓存命中。
      // 人格为空时不发这条消息：部分上游对空 content 会 400。
      if (!req.system_prompt.empty()) {
        msgs.push_back({{"role", "system"}, {"content", req.system_prompt}});
      }
    } else if (!req.system_prompt.empty()) {
      payload["system"] = req.system_prompt;
    }
    // 历史消息按「相邻同角色合并」的方式组装。
    //
    // 关于 assistant 的图片：
    //   上游支持（supports_assistant_image=true）时按正常语义走，
    //   assistant 消息里直接带 image_url。
    //   不支持时（如 WorkBuddy /v2/chat/completions 会静默丢弃），
    //   仅在该图片位置插入一条独立的 user 消息，内容只有归属标签 + 图。
    //   文字消息仍然走原本的 role，不做任何改变。
    //
    // 关于服务端联网工具：
    //   assistant 消息必须**原样带回 server_tool_calls**，否则上一轮
    //   deepseek_read_url 读过的网页正文（signature）不在上下文里。
    //   req.assistant_server_tools_json 按「assistant 消息出现的顺序」
    //   一一对应第 N 条 assistant 消息。
    std::string cur_role;
    json cur_parts = json::array();
    bool cur_has_parts = false;
    // 当前 assistant 分组对应的 server_tool_calls（JSON 数组文本）
    std::string cur_stc;
    // assistant 文本块的序号：与 req.assistant_server_tools_json 对齐
    std::size_t assistant_text_ord = 0;

    auto flush_history = [&]() {
      if (!cur_has_parts) return;
      json m;
      m["role"] = cur_role;
      // 只有一个 text 块时用纯字符串，兼容性最好
      if (cur_parts.size() == 1 && cur_parts[0].value("type", "") == "text") {
        m["content"] = cur_parts[0].value("text", "");
      } else {
        m["content"] = cur_parts;
      }
      // 服务端联网工具的调用记录：原样回传（格式与响应里的一致）。
      //
      // 只在 OpenAI 兼容协议下带这个字段：它是 DeepSeek 的扩展，
      // Anthropic 协议的消息体里没有这个键，多塞会被上游拒。
      // （实际上只有 DeepSeek 会填这个字段，但显式判一下更稳。）
      if (cur_role == "assistant" && !cur_stc.empty() &&
          dial_ == Dialect::kOpenAI) {
        try {
          m["server_tool_calls"] = json::parse(cur_stc);
        } catch (const std::exception& e) {
          ::fish::log::log_line(::fish::log::LogLevel::kWarn, "capability",
                                "server_tool_calls_parse")
              .field("detail", e.what());
        }
      }
      msgs.push_back(std::move(m));
      cur_parts = json::array();
      cur_has_parts = false;
      cur_role.clear();
      cur_stc.clear();
    };

    // 本地路径 → data URI；失败返回空串
    auto to_url = [](const std::string& raw, const std::string& mime) -> std::string {
      if (raw.empty()) return {};
      if (raw.rfind("http://", 0) == 0 || raw.rfind("https://", 0) == 0 ||
          raw.rfind("data:", 0) == 0) {
        return raw;
      }
      return local_file_to_data_uri(raw, mime);
    };

    for (const auto& h : req.history) {
      const std::string role = (h.role == "assistant") ? "assistant" : "user";

      if (h.kind == fish::core::BlockKind::kImage) {
        const std::string url = to_url(h.media_url, h.mime);
        if (url.empty()) continue;   // 读不到就跳过，不要发死路径

        if (role == "assistant" && !cfg_.supports_assistant_image) {
          // 只在图片这一处做降级：先收尾当前消息，再插一条
          // 只含「归属标签 + 图」的 user 消息，然后清空合并状态，
          // 让后续文字重新按正常 role 走。
          flush_history();
          json parts = json::array();
          parts.push_back({{"type", "text"}, {"text", std::string(kAssistantImageTag)}});
          parts.push_back({{"type", "image_url"}, {"image_url", {{"url", url}}}});
          msgs.push_back({{"role", "user"}, {"content", parts}});
          continue;
        }

        if (role != cur_role) {
          flush_history();
          cur_role = role;
        }
        cur_parts.push_back({{"type", "image_url"}, {"image_url", {{"url", url}}}});
        cur_has_parts = true;
        continue;
      }

      if (h.kind == fish::core::BlockKind::kFile) {
        // 历史里的 file_id 同样要原样回传，否则后续轮次读不到文档。
        if (h.media_url.empty()) continue;
        if (role != cur_role) {
          flush_history();
          cur_role = role;
        }
        cur_parts.push_back({{"type", "file"}, {"file_id", h.media_url}});
        cur_has_parts = true;
        continue;
      }

      if (h.kind == fish::core::BlockKind::kText) {
        // 服务端联网工具：按 assistant 文本块的出现顺序取对应的原始 JSON。
        // req.assistant_server_tools_json 的第 N 项对应第 N 条 assistant
        // 消息（编排层按同一顺序收集），空串表示那条消息没有服务端工具调用。
        //
        // 注意序号在**空文本时也要推进**：编排层每条 assistant 记忆消息
        // 都会产生一个 text 块（正文可能为空），序号必须与之对齐，
        // 否则后面所有消息的 server_tool_calls 都会错位。
        std::string stc;
        if (role == "assistant") {
          if (assistant_text_ord < req.assistant_server_tools_json.size()) {
            stc = req.assistant_server_tools_json[assistant_text_ord];
          }
          ++assistant_text_ord;
        }
        if (h.text.empty()) continue;   // 空文本块不进上下文（原行为）
        if (role != cur_role) {
          flush_history();
          cur_role = role;
          cur_stc = stc;
        } else if (role == "assistant" && cur_stc.empty()) {
          cur_stc = stc;
        }
        cur_parts.push_back({{"type", "text"}, {"text", h.text}});
        cur_has_parts = true;
      }
    }
    flush_history();

    msgs.push_back({{"role", "user"}, {"content", to_blocks(req.user_blocks)}});
    payload["messages"] = std::move(msgs);

    // 工具声明：只在调用方提供了 tools 时加，避免无谓的 token 消耗
    if (!req.tools_json.empty()) {
      try {
        payload["tools"] = json::parse(req.tools_json);
        payload["tool_choice"] = "auto";
      } catch (const std::exception& e) {
        ::fish::log::log_line(::fish::log::LogLevel::kWarn, "capability", "tools_parse")
            .field("detail", e.what());
      }
    }

    // ── DeepSeek 服务端联网工具 ──
    //
    // 官方文档（guides/web_tools.md）：
    //   - tools 里声明 {"type":"deepseek_web_search"} / {"type":"deepseek_read_url"}
    //   - 搜索与抓取都在服务端执行，客户端不需要执行任何工具
    //   - 与 reasoning_effort=high 同时使用时 deepseek_web_search 变成
    //     「深度思考搜索」（多步推理中自主搜索/开网页/页内查找，约 20 次调用）
    //   - tool_choice 不能指定这两个工具（400）
    //
    // 限制与保守处理：
    //   - 官方明确「声明了其他服务端工具」会 400。普通 function 工具与
    //     服务端工具混用是否被拒没有明确文档，所以这里**不混**：
    //     开启服务端联网工具时，本地 tools（web_search / web_fetch /
    //     send_image / send_voice）整体不发送。谁该关掉本地 web_search
    //     由调用方（orchestrator）决定，capability 只按开关办事。
    //   - 声明这些工具时 temperature / top_p / frequency_penalty /
    //     presence_penalty / stop / seed / n / parallel_tool_calls 会被
    //     服务端忽略，所以这里干脆不发，避免"看起来生效了"。
    const bool web_tools =
        cfg_.deepseek_web_tools && dial_ == Dialect::kOpenAI &&
        is_deepseek_endpoint(cfg_.provider, cfg_.base_url);
    if (web_tools) {
      json tools = json::array();
      tools.push_back({{"type", "deepseek_web_search"}});
      tools.push_back({{"type", "deepseek_read_url"}});
      payload["tools"] = std::move(tools);
      // tool_choice 只能 auto（指定这两个工具会 400）
      payload["tool_choice"] = "auto";
      // 服务端会忽略的参数，发了只会让人误以为生效
      payload.erase("temperature");
      payload.erase("top_p");
      payload.erase("frequency_penalty");
      payload.erase("presence_penalty");
      payload.erase("stop");
      payload.erase("seed");
      payload.erase("n");
      payload.erase("parallel_tool_calls");
      ::fish::log::log_line(::fish::log::LogLevel::kInfo, "capability",
                            "deepseek_web_tools_on")
          .field("effort", cfg_.reasoning_effort)
          // 深度思考搜索：effort=high 时服务端自动进入该模式
          .field("deep_search", cfg_.reasoning_effort == "high")
          .field("local_tools_dropped", !req.tools_json.empty());
    }

    std::map<std::string, std::string> headers;
    if (dial_ == Dialect::kOpenAI) {
      headers["Authorization"] = "Bearer " + cfg_.api_key;
    } else {
      headers["x-api-key"] = cfg_.api_key;
      headers["anthropic-version"] = "2023-06-01";
    }

    const std::string url =
        cfg_.base_url + (dial_ == Dialect::kOpenAI ? "/chat/completions" : "/v1/messages");

    auto resp =
        HttpClient::instance().post_json(url, headers, payload.dump(), cfg_.timeout_ms, breaker_);
    if (!resp.ok()) return Result<Response>::fail(resp.error.code, resp.error.detail);

    Response out;
    try {
      const auto body = json::parse(resp.value->body);
      if (dial_ == Dialect::kOpenAI) {
        if (!body.contains("choices") || body["choices"].empty()) {
          return Result<Response>::fail(ErrorCode::kBadRequest, "no_choices");
        }
        // content 可能是 null（思考模式/工具调用时常见），json::value()
        // 会抛 type_error 把整个响应判成 parse 失败，所以走容错的 str_or。
        const json msg = (body["choices"][0].contains("message") &&
                          body["choices"][0]["message"].is_object())
                             ? body["choices"][0]["message"]
                             : json::object();
        out.text = str_or(msg, "content");
        extract_usage(body.value("usage", json::object()), &out);

        // 解析工具调用：上游可能返回 tool_calls 数组
        if (msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
          for (const auto& tc : msg["tool_calls"]) {
            if (!tc.is_object()) continue;
            const auto fn = tc.value("function", json::object());
            IChatModel::ToolCall call;
            call.id = tc.value("id", "");
            call.name = fn.value("name", "");
            // arguments 可能是字符串，也可能是对象
            if (fn.contains("arguments")) {
              if (fn["arguments"].is_string()) {
                call.arguments = fn["arguments"].get<std::string>();
              } else {
                call.arguments = fn["arguments"].dump();
              }
            }
            if (!call.name.empty()) out.tool_calls.push_back(std::move(call));
          }
          if (!out.tool_calls.empty()) {
            ::fish::log::log_line(::fish::log::LogLevel::kInfo, "capability", "tool_calls")
                .field("count", static_cast<long long>(out.tool_calls.size()))
                .field("first", out.tool_calls[0].name);
          }
        }

        // 服务端联网工具（deepseek_web_search / deepseek_read_url）：
        // 调用记录在扩展字段 message.server_tool_calls，引用在
        // message.annotations（type=url_citation）。客户端不执行任何工具，
        // 这里把原始 JSON 存进 Response（供编排层写回历史、多轮回传），
        // 同时记日志、汇总引用 URL。
        const int server_calls = log_server_tool_calls(msg);
        if (server_calls > 0) {
          if (msg.contains("server_tool_calls")) {
            out.server_tool_calls_json = msg["server_tool_calls"].dump();
          }
          if (msg.contains("annotations")) {
            out.annotations_json = msg["annotations"].dump();
          }
          const auto cites = collect_citations(msg);
          ::fish::log::log_line(::fish::log::LogLevel::kInfo, "capability",
                                "server_tool_summary")
              .field("calls", server_calls)
              .field("citations", static_cast<long long>(cites.size()))
              .field("first_citation", cites.empty() ? "" : cites.front());
        }
      } else {
        if (!body.contains("content") || !body["content"].is_array()) {
          return Result<Response>::fail(ErrorCode::kBadRequest, "no_content");
        }
        for (const auto& blk : body["content"]) {
          if (blk.value("type", "") == "text") out.text += blk.value("text", "");
        }
        extract_usage(body.value("usage", json::object()), &out);
      }
    } catch (const std::exception& e) {
      return Result<Response>::fail(ErrorCode::kBadRequest,
                                    std::string("parse:") + e.what());
    }

    // 空文本只有在「也没有工具调用」时才算失败。
    //
    // OpenAI 协议里，模型决定调工具时 content 通常就是空的、
    // finish_reason 为 tool_calls，这是完全正常的响应。
    // 早期只看 text.empty() 就判 kUpstreamUnavailable，导致
    // 「模型想调工具」被当成上游故障，日志里同时出现
    //   tool_calls count=1
    //   chat_degraded detail=empty_completion
    // 用户看到的就是「我卡了一下」，工具永远不生效。
    if (out.text.empty() && out.tool_calls.empty()) {
      return Result<Response>::fail(ErrorCode::kUpstreamUnavailable, "empty_completion");
    }

    ::fish::log::log_line(::fish::log::LogLevel::kInfo, "capability", "chat_ok")
        .field("provider", cfg_.provider)
        .field("prompt", out.prompt_tokens)
        .field("cached", out.cached_tokens)
        .field("output", out.output_tokens);

    return Result<Response>::ok(std::move(out));
  }

 private:
  // ── 服务端联网工具：解析与观测 ──
  //
  // Chat Completions 协议里的形态（见 guides/web_tools.md）：
  //   message.server_tool_calls[] = {index, reasoning_content, calls[]}
  //   calls[] = {index, id, type, kind, action, status, sources/page, signature}
  //     kind   : search | deep_search | read_link | image_search | deep_image_search
  //     action : {type:"search", queries[]} | {type:"open_page", url}
  //              | {type:"find_in_page", url, pattern} | {type:"read_link", url}
  //
  // 完整的「多轮回传」分三步，三步都已就位：
  //     1. fish.core.cppm 的 Response 已有 server_tool_calls_json /
  //        annotations_json，本模块在响应解析时填入；
  //     2. orchestrator 把这两个字段写进本轮记忆的 assistant 消息的
  //        **独立字段**（fish.memory::Message::server_tool_calls_json /
  //        annotations_json），正文保持干净（不再往正文里塞 \x01 标记）；
  //     3. history 组装支持 assistant 消息带 server_tool_calls——
  //        见上面 flush_history：req.assistant_server_tools_json 按
  //        assistant 消息顺序一一对应，原样写进消息的 "server_tool_calls"。
  //
  // 注意：signature 是加密串，只在结构化字段里传递，绝不拼进正文
  // （拼正文既不能还原网页，还会污染上下文、多烧 token）。
  //
  // 返回解析到的调用条数（0 = 本次响应没有服务端工具调用）。
  static int log_server_tool_calls(const json& msg) {
    if (!msg.contains("server_tool_calls") || !msg["server_tool_calls"].is_array()) {
      return 0;
    }
    int total = 0;
    for (const auto& step : msg["server_tool_calls"]) {
      if (!step.is_object()) continue;
      if (!step.contains("calls") || !step["calls"].is_array()) continue;
      for (const auto& call : step["calls"]) {
        if (!call.is_object()) continue;
        ++total;
        const std::string kind = str_or(call, "kind");
        const std::string type = str_or(call, "type");
        const std::string status = str_or(call, "status");
        const json action =
            (call.contains("action") && call["action"].is_object())
                ? call["action"]
                : json::object();
        const std::string action_type = str_or(action, "type");

        auto line =
            ::fish::log::log_line(::fish::log::LogLevel::kInfo, "capability",
                                  "server_tool")
                .field("kind", kind)
                .field("action", action_type)
                .field("tool", type)
                .field("status", status);

        if (action_type == "search") {
          // 一次调用可以带多个查询词；只记前 3 个，日志不宜过长
          std::string queries;
          int n = 0;
          if (action.contains("queries") && action["queries"].is_array()) {
            for (const auto& q : action["queries"]) {
              if (!q.is_string()) continue;
              if (n >= 3) {
                queries += " …";
                break;
              }
              if (n > 0) queries += " | ";
              queries += q.get<std::string>();
              ++n;
            }
          }
          line.field("queries", queries)
              .field("query_count",
                     static_cast<long long>(
                         action.contains("queries") && action["queries"].is_array()
                             ? action["queries"].size()
                             : 0));
        } else {
          line.field("url", str_or(action, "url"));
          if (action_type == "find_in_page") {
            line.field("pattern", str_or(action, "pattern"));
          }
        }
        // 失败调用带 error.code（url_not_accessible / url_not_allowed / unavailable）
        if (call.contains("error") && call["error"].is_object()) {
          line.field("error", str_or(call["error"], "code"));
        }
        if (call.contains("sources") && call["sources"].is_array()) {
          line.field("sources", static_cast<long long>(call["sources"].size()));
        }
      }
    }
    return total;
  }

  // 收集 message.annotations 里 type=url_citation 的 URL（去重，保序）。
  // 目前只用于日志；要让用户看到来源，需要在 fish.core.cppm 的 Response
  // 增加 citations 字段并由通道渲染。
  static std::vector<std::string> collect_citations(const json& msg) {
    std::vector<std::string> urls;
    if (!msg.contains("annotations") || !msg["annotations"].is_array()) {
      return urls;
    }
    for (const auto& a : msg["annotations"]) {
      if (!a.is_object()) continue;
      if (str_or(a, "type") != "url_citation") continue;
      const json cite = (a.contains("url_citation") && a["url_citation"].is_object())
                            ? a["url_citation"]
                            : json::object();
      const std::string url = str_or(cite, "url");
      if (url.empty()) continue;
      if (std::find(urls.begin(), urls.end(), url) != urls.end()) continue;
      urls.push_back(url);
    }
    return urls;
  }

  ChatConfig cfg_;
  Dialect dial_;
  CircuitBreaker* breaker_;
};

// ── 向量 ────────────────────────────────────────────────
//
// 绝大多数厂商是 OpenAI 兼容格式，差异只有三处：
//   1. 鉴权位置（Bearer / x-goog-api-key / query key）
//   2. 输出维度字段名（dimensions / output_dimension / 无）
//   3. 响应里向量的路径
// 把这三处做成配置，就不必为每家写一个类。
//
// 响应路径的三种形态：
//   A. data[*].embedding        —— OpenAI 兼容（大多数）
//   B. embeddings[*]            —— Ollama /api/embed（二维数组）
//   C. embedding 或 .values     —— 单条：Ollama 旧接口 / Gemini embedContent
//   D. embeddings.float         —— Cohere v2（对象里的二维数组）
class HttpEmbedding final : public fish::core::IEmbeddingModel {
 public:
  HttpEmbedding(EmbeddingConfig cfg, CircuitBreaker* b)
      : cfg_(std::move(cfg)), breaker_(b) {}

  int dimension() const noexcept override { return cfg_.dimension; }

  Result<std::vector<float>> embed(const std::string& text) override {
    json payload{{"model", cfg_.model}, {"input", text}};
    if (!cfg_.dimensions_field.empty() && cfg_.dimension > 0) {
      payload[cfg_.dimensions_field] = cfg_.dimension;
    }
    if (!cfg_.extra_json.empty()) {
      try {
        const auto extra = json::parse(cfg_.extra_json);
        if (extra.is_object()) {
          for (const auto& [k, v] : extra.items()) payload[k] = v;
        }
      } catch (const std::exception&) {
        // 配置写错不该让整条链路失败
      }
    }

    std::map<std::string, std::string> headers{{"Content-Type", "application/json"}};
    if (!cfg_.api_key.empty()) {
      if (cfg_.provider == "gemini") {
        headers["x-goog-api-key"] = cfg_.api_key;
      } else {
        headers["Authorization"] = "Bearer " + cfg_.api_key;
      }
    }

    std::string url = cfg_.base_url + (cfg_.path.empty() ? "/embeddings" : cfg_.path);
    // Google CSE 式的 query 参数鉴权（这里给自建/特殊端点留口子）
    if (cfg_.provider == "gemini-key-in-query" && !cfg_.api_key.empty()) {
      url += (url.find('?') == std::string::npos ? "?" : "&") + std::string("key=") +
             cfg_.api_key;
    }

    auto resp = HttpClient::instance().post_json(url, headers, payload.dump(),
                                                 cfg_.timeout_ms, breaker_);
    if (!resp.ok()) return Result<std::vector<float>>::fail(resp.error.code, resp.error.detail);

    try {
      const auto body = json::parse(resp.value->body);
      std::vector<float> out = extract_vector(body);
      // 有的端点会 200 + 空向量（如 DashScope 兼容模式请求 sparse），
      // 必须当失败，否则会往索引里写一个零向量。
      if (out.empty()) {
        return Result<std::vector<float>>::fail(ErrorCode::kBadRequest, "embedding_empty");
      }
      return Result<std::vector<float>>::ok(std::move(out));
    } catch (const std::exception& e) {
      return Result<std::vector<float>>::fail(ErrorCode::kBadRequest,
                                              std::string("parse:") + e.what());
    }
  }

 private:
  // 依次尝试四种已知形态，取第一个非空向量。
  static std::vector<float> extract_vector(const json& body) {
    auto to_vec = [](const json& arr) {
      std::vector<float> v;
      if (!arr.is_array()) return v;
      v.reserve(arr.size());
      for (const auto& e : arr) {
        if (e.is_number()) v.push_back(e.get<float>());
      }
      return v;
    };

    // A. OpenAI 兼容
    if (body.contains("data") && body["data"].is_array() && !body["data"].empty()) {
      const auto& first = body["data"][0];
      if (first.is_object() && first.contains("embedding")) {
        auto v = to_vec(first["embedding"]);
        if (!v.empty()) return v;
      }
    }
    // B. Ollama /api/embed：embeddings 是二维数组
    if (body.contains("embeddings") && body["embeddings"].is_array() &&
        !body["embeddings"].empty()) {
      const auto& first = body["embeddings"][0];
      auto v = to_vec(first);
      if (!v.empty()) return v;
    }
    // C. 单条：Ollama 旧接口 {"embedding":[...]} / Gemini {"embedding":{"values":[...]}}
    if (body.contains("embedding")) {
      const auto& e = body["embedding"];
      if (e.is_array()) {
        auto v = to_vec(e);
        if (!v.empty()) return v;
      }
      if (e.is_object() && e.contains("values")) {
        auto v = to_vec(e["values"]);
        if (!v.empty()) return v;
      }
    }
    // D. Cohere v2：{"embeddings":{"float":[[...]]}}
    if (body.contains("embeddings") && body["embeddings"].is_object()) {
      const auto& e = body["embeddings"];
      if (e.contains("float") && e["float"].is_array() && !e["float"].empty()) {
        auto v = to_vec(e["float"][0]);
        if (!v.empty()) return v;
      }
    }
    return {};
  }

  EmbeddingConfig cfg_;
  CircuitBreaker* breaker_;
};

// 临时文件名的唯一后缀：steady_clock + 进程内计数器 + random_device。
//
// 为什么不用 std::rand()：未播种时序列固定，多进程/多次调用会撞同一个
// 文件名，两个请求互相覆盖对方的临时文件。这里 counter 保证同进程内
// 不重复，random_device 保证跨进程不重复。
std::string unique_tmp_suffix() {
  static std::atomic<unsigned long long> counter{0};
  const auto now = static_cast<unsigned long long>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  const auto n = counter.fetch_add(1);
  return std::to_string(now) + "_" + std::to_string(n) + "_" +
         std::to_string(static_cast<unsigned long long>(std::random_device{}()));
}

// ── 语音识别 ────────────────────────────────────────────
//
// 统一走 multipart 上传，不再经由 shell 调 curl——凭据出现在
// 命令行里会被同机任何用户从 ps 读到。
//
// 各家的差异收敛成三个配置项：path（端点路径）、model（模型名）、
// need_wav16k（是否强制转 16k 单声道 WAV）。语音格式五花八门
// （QQ 发 SILK、微信发 AMR/SILK），转码统一在这里做一次。
class HttpStt final : public fish::core::ISpeechToText {
 public:
  HttpStt(SttConfig cfg, CircuitBreaker* b) : cfg_(std::move(cfg)), breaker_(b) {}

  Result<std::string> transcribe(const std::string& media_url) override {
    auto audio = HttpClient::instance().download(media_url, 64u << 20);
    if (!audio.ok()) return Result<std::string>::fail(audio.error.code, audio.error.detail);

    // 扩展名必须与真实格式一致：多数厂商按扩展名/魔数选解码器，
    // 存成 .audio 会 400。按文件头嗅探，不依赖 URL 后缀。
    // 非 const：转码成 WAV 后要同步改成 .wav，否则下面按旧扩展名
    // 选 mime、拼上传文件名（如 audio.silk）——上游会 400 或解出噪声。
    std::string ext = sniff_audio_ext(audio.value->data());
    const std::string stem = "/tmp/fish_stt_" + unique_suffix();
    std::string tmp = stem + ext;
    {
      std::ofstream f(tmp, std::ios::binary);
      if (!f) return Result<std::string>::fail(ErrorCode::kInternal, "tmp_write");
      f.write(audio.value->data(), static_cast<std::streamsize>(audio.value->size()));
    }

    // 需要转 WAV 的两种情况：
    //   1. SILK 是腾讯自有格式，几乎没有厂商支持，必须先转
    //   2. 厂商只收 16k 单声道 WAV（百度 / Azure / DeepSeek）
    if (ext == ".silk" || cfg_.need_wav16k) {
      const std::string wav = stem + ".wav";
      if (!to_wav16k(tmp, wav)) {
        std::remove(tmp.c_str());
        return Result<std::string>::fail(
            ErrorCode::kUnsupported, "audio_transcode_failed（装 ffmpeg 或 pilk）");
      }
      std::remove(tmp.c_str());
      tmp = wav;
      // 内容已经是 WAV，扩展名跟着改：mime 和上传文件名都用它
      ext = ".wav";
    }

    // 上传大小按各家的硬限制判断，超了直接给明确错误，
    // 不要发上去等一个语焉不详的 413。
    if (cfg_.max_upload_bytes > 0) {
      std::ifstream f(tmp, std::ios::binary | std::ios::ate);
      const auto size = f ? static_cast<std::size_t>(f.tellg()) : 0;
      if (size > cfg_.max_upload_bytes) {
        std::remove(tmp.c_str());
        return Result<std::string>::fail(
            ErrorCode::kBadRequest,
            "stt_audio_too_large:" + std::to_string(size) + ">" +
                std::to_string(cfg_.max_upload_bytes));
      }
    }

    std::map<std::string, std::string> headers;
    if (!cfg_.api_key.empty()) headers["Authorization"] = "Bearer " + cfg_.api_key;

    std::vector<std::pair<std::string, std::string>> fields;
    // DeepSeek 官方 ASR 只收 file 一个字段（响应 {"text":...,"duration_ms":...}），
    // 多余的 model / language / response_format 有被 400 的风险，直接不发。
    // 其余厂商按 OpenAI /audio/transcriptions 风格带全。
    const bool deepseek_asr = cfg_.path.rfind("/v1/tools/asr", 0) == 0 ||
                              cfg_.base_url.find("api.deepseek.com") != std::string::npos;
    if (!deepseek_asr) {
      if (!cfg_.model.empty()) fields.emplace_back("model", cfg_.model);
      // language=auto 时多数厂商要么不认、要么当成非法值，直接不发
      if (!cfg_.language.empty() && cfg_.language != "auto") {
        fields.emplace_back("language", cfg_.language);
      }
      // response_format：能返回 JSON 的都要 JSON，纯文本的厂商会忽略
      fields.emplace_back("response_format", "json");
    }

    std::vector<HttpRequest::FormFile> files;
    std::string mime;
    if (ext == ".wav") mime = "audio/wav";
    else if (ext == ".mp3") mime = "audio/mpeg";
    else if (ext == ".ogg") mime = "audio/ogg";
    else if (ext == ".m4a") mime = "audio/mp4";
    else if (ext == ".flac") mime = "audio/flac";
    else if (ext == ".amr") mime = "audio/amr";
    files.push_back({"file", tmp, mime, "audio" + ext});

    auto resp = HttpClient::instance().post_form(stt_endpoint(), headers, fields, files,
                                                 cfg_.timeout_ms, breaker_);
    std::remove(tmp.c_str());
    if (!resp.ok()) return Result<std::string>::fail(resp.error.code, resp.error.detail);

    return parse_text(resp.value->body);
  }

 private:
  static std::string unique_suffix() { return unique_tmp_suffix(); }

  // 转成 16k 单声道 WAV。优先 ffmpeg（通吃），退回 tools/silk2wav.py
  // （纯 Python，只处理 SILK）。
  static bool to_wav16k(const std::string& in, const std::string& out) {
    const std::string ff = "ffmpeg -y -loglevel error -i '" + in +
                           "' -ar 16000 -ac 1 -c:a pcm_s16le '" + out + "'";
    if (std::system((ff + " >/dev/null 2>&1").c_str()) == 0) {
      std::ifstream f(out, std::ios::binary | std::ios::ate);
      return f && f.tellg() > 44;  // 比 WAV 头大才算有内容
    }
    // ffmpeg 不可用时，SILK 用纯 Python 方案
    if (in.size() >= 4) {
      std::ifstream probe(in, std::ios::binary);
      std::string head(4, '\0');
      probe.read(head.data(), 4);
      if (head.rfind("#!SI", 0) == 0) {
        const std::string py = "python3 tools/silk2wav.py '" + in + "' '" + out +
                               "' --rate 16000 >/dev/null 2>&1";
        if (std::system(py.c_str()) == 0) {
          std::ifstream f(out, std::ios::binary | std::ios::ate);
          return f && f.tellg() > 44;
        }
      }
    }
    return false;
  }

  // 各家响应形态不同，按「最可能」的顺序取第一个非空文本。
  static Result<std::string> parse_text(const std::string& raw) {
    if (raw.empty()) {
      return Result<std::string>::fail(ErrorCode::kUpstreamUnavailable, "stt_empty");
    }
    try {
      const auto body = json::parse(raw);
      // OpenAI / Groq / SiliconFlow / 火山：{"text": "..."}
      if (body.contains("text") && body["text"].is_string()) {
        return Result<std::string>::ok(body["text"].get<std::string>());
      }
      // 腾讯 / 阿里等包了一层：{"Response": {"Result": "..."}}
      for (const char* k : {"result", "Result", "transcription", "transcript"}) {
        if (body.contains(k) && body[k].is_string()) {
          return Result<std::string>::ok(body[k].get<std::string>());
        }
      }
      if (body.contains("output") && body["output"].is_object()) {
        const auto& out = body["output"];
        if (out.contains("text") && out["text"].is_string()) {
          return Result<std::string>::ok(out["text"].get<std::string>());
        }
      }
      if (body.contains("Response") && body["Response"].is_object()) {
        const auto& r = body["Response"];
        for (const char* k : {"Result", "result", "Text"}) {
          if (r.contains(k) && r[k].is_string()) {
            return Result<std::string>::ok(r[k].get<std::string>());
          }
        }
      }
      // 上游明确报错时把它说的话带出来，否则只能靠猜
      for (const char* k : {"error", "message", "Message"}) {
        if (body.contains(k)) {
          const auto& e = body[k];
          const std::string msg = e.is_string() ? e.get<std::string>()
                                   : e.is_object() && e.contains("message")
                                       ? e["message"].dump()
                                       : e.dump();
          return Result<std::string>::fail(ErrorCode::kBadRequest, "stt_upstream:" + msg);
        }
      }
      return Result<std::string>::fail(ErrorCode::kBadRequest, "stt_no_text");
    } catch (const std::exception&) {
      // 有的厂商（如启用纯文本格式）直接返回一行文字。
      // 但「不是 JSON」也可能是错误页：HTML 500 页、代理的
      // "Internal Server Error"。整段当转写结果返回，等于把错误信息
      // 念给用户。兜底前做最小 sanity check：短、不像 HTML、不是经典错误串。
      if (raw.find('{') != std::string::npos) {
        return Result<std::string>::fail(ErrorCode::kBadRequest, "stt_parse_failed");
      }
      const bool looks_html = raw.find("<html") != std::string::npos ||
                              raw.find("<!DOCTYPE") != std::string::npos ||
                              raw.find("<!doctype") != std::string::npos;
      const bool looks_error =
          raw.find("Internal Server Error") != std::string::npos ||
          raw.find("Internal Error") != std::string::npos ||
          raw.find("Bad Gateway") != std::string::npos ||
          raw.find("Service Unavailable") != std::string::npos;
      if (looks_html || looks_error || raw.size() > 200) {
        return Result<std::string>::fail(ErrorCode::kBadRequest, "stt_parse_failed");
      }
      return Result<std::string>::ok(raw);
    }
  }

  std::string stt_endpoint() const {
    if (!cfg_.path.empty()) return cfg_.base_url + cfg_.path;
    return cfg_.base_url + "/audio/transcriptions";
  }

  // 按文件头判断音频格式，返回带点的扩展名。
  // QQ 的语音常见 SILK（腾讯自有），也有 mp3/wav/ogg/amr。
  static std::string sniff_audio_ext(const std::string& data) {
    if (data.size() < 12) return ".bin";
    const auto* p = reinterpret_cast<const unsigned char*>(data.data());
    // SILK: 以 "#!SILK_V3" 或 0x02 "#!SILK" 开头
    if (data.rfind("#!SILK", 0) == 0 || (p[0] == 0x02 && data.rfind("#!SILK", 1) == 0)) {
      return ".silk";
    }
    // ID3 或 0xFF 0xFB/0xF3/0xF2 → mp3
    if (data.rfind("ID3", 0) == 0 ||
        (p[0] == 0xFF && (p[1] & 0xE0) == 0xE0)) {
      return ".mp3";
    }
    // RIFF....WAVE
    if (data.size() > 12 && data.rfind("RIFF", 0) == 0 && data.substr(8, 4) == "WAVE") {
      return ".wav";
    }
    // OggS
    if (data.rfind("OggS", 0) == 0) return ".ogg";
    // fLaC
    if (data.rfind("fLaC", 0) == 0) return ".flac";
    // AMR
    if (data.rfind("#!AMR", 0) == 0) return ".amr";
    // MP4/M4A: ....ftyp
    if (data.size() > 12 && data.substr(4, 4) == "ftyp") return ".m4a";
    return ".bin";
  }

  SttConfig cfg_;
  CircuitBreaker* breaker_;
};

// ── 语音合成 ────────────────────────────────────────────

// base64 解码（生图与 TTS 共用，避免重复实现）
static std::vector<unsigned char> b64_decode(const std::string& s) {
  static const auto table = [] {
    std::array<int, 256> t{};
    t.fill(-1);
    const char* chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i = 0; i < 64; ++i) t[static_cast<unsigned char>(chars[i])] = i;
    return t;
  }();

  std::vector<unsigned char> out;
  out.reserve(s.size() * 3 / 4);
  int val = 0;
  int bits = -8;
  for (const char c : s) {
    if (c == '=' || c == '\n' || c == '\r') continue;
    const int d = table[static_cast<unsigned char>(c)];
    if (d < 0) continue;
    val = (val << 6) + d;
    bits += 6;
    if (bits >= 0) {
      out.push_back(static_cast<unsigned char>((val >> bits) & 0xFF));
      bits -= 8;
    }
  }
  return out;
}

// 合成结果落盘。返回本地路径供通道上传。
static Result<std::string> write_audio(const std::string& bytes,
                                       const std::string& format) {
  if (bytes.empty()) {
    return Result<std::string>::fail(ErrorCode::kUpstreamUnavailable, "tts_empty");
  }
  static std::atomic<unsigned long long> counter{0};
  const auto n = counter.fetch_add(1);
  const std::string tmp = "/tmp/fish_tts_" +
                          std::to_string(static_cast<unsigned long long>(
                              std::chrono::steady_clock::now().time_since_epoch().count())) +
                          "_" + std::to_string(n) + "." + format;
  std::ofstream f(tmp, std::ios::binary);
  if (!f) return Result<std::string>::fail(ErrorCode::kInternal, "tts_write");
  f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  f.close();
  return Result<std::string>::ok(tmp);
}

// 把任意音频转成目标格式。QQ 官方语音上传收 mp3 最稳，
// 但 OpenAI 兼容厂商可能只吐 wav/pcm，火山可吐 mp3/ogg/wav 等多档。
static Result<std::string> ensure_format(const std::string& path,
                                         const std::string& want) {
  if (want.empty()) return Result<std::string>::ok(path);
  const auto dot = path.find_last_of('.');
  const std::string have = dot == std::string::npos ? "" : path.substr(dot + 1);
  if (have == want) return Result<std::string>::ok(path);

  const std::string out = path.substr(0, path.find_last_of('.')) + "." + want;
  const std::string cmd = "ffmpeg -y -loglevel error -i '" + path + "' '" + out +
                          "' >/dev/null 2>&1";
  if (std::system(cmd.c_str()) != 0) {
    return Result<std::string>::fail(ErrorCode::kUnsupported,
                                     "ffmpeg_convert_failed:" + have + "->" + want);
  }
  return Result<std::string>::ok(out);
}

class EdgeTts final : public fish::core::ITextToSpeech {
 public:
  explicit EdgeTts(TtsConfig cfg) : cfg_(std::move(cfg)) {}

  Result<std::string> synthesize(const std::string& text) override {
    static std::atomic<unsigned long long> counter{0};
    const std::string tmp = "/tmp/fish_tts_" +
                            std::to_string(static_cast<unsigned long long>(
                                std::chrono::steady_clock::now().time_since_epoch().count())) +
                            "_" + std::to_string(counter.fetch_add(1)) + "." + cfg_.format;
    std::string safe;
    for (const char c : text) {
      if (c == '\'') {
        safe += "'\\''";
      } else if (c != '\n' && c != '\r') {
        safe += c;
      }
    }
    // edge-tts 常被 pip 装到 ~/.local/bin，不在默认 PATH 里，
    // 所以优先用绝对路径，找不到再退回裸命令名。
    const std::string exe = resolve_edge_tts();
    if (exe.empty()) {
      return Result<std::string>::fail(
          ErrorCode::kUnsupported,
          "edge_tts_not_found（pip install edge-tts）");
    }
    const std::string cmd = "'" + exe + "' --voice '" + cfg_.voice + "' --text '" + safe +
                            "' --write-media '" + tmp + "' 2>/dev/null";
    if (std::system(cmd.c_str()) != 0) {
      return Result<std::string>::fail(ErrorCode::kUpstreamUnavailable, "edge_tts_failed");
    }
    std::ifstream f(tmp, std::ios::binary | std::ios::ate);
    if (!f || f.tellg() <= 0) {
      std::remove(tmp.c_str());
      return Result<std::string>::fail(ErrorCode::kUpstreamUnavailable, "edge_tts_empty");
    }
    return Result<std::string>::ok(tmp);
  }

 private:
  // 依次尝试：绝对路径 → PATH 里的 edge-tts
  static std::string resolve_edge_tts() {
    static const char* kCandidates[] = {
        "/usr/local/bin/edge-tts",
        "/usr/bin/edge-tts",
    };
    for (const char* p : kCandidates) {
      std::ifstream f(p);
      if (f.good()) return p;
    }
    // ~/.local/bin（pip --user 的默认位置）
    if (const char* home = std::getenv("HOME")) {
      const std::string p = std::string(home) + "/.local/bin/edge-tts";
      std::ifstream f(p);
      if (f.good()) return p;
    }
    // 退回裸命令名，让 shell 去 PATH 里找
    if (std::system("command -v edge-tts >/dev/null 2>&1") == 0) return "edge-tts";
    return {};
  }

  TtsConfig cfg_;
};

// OpenAI 兼容 /audio/speech：响应体直接是音频二进制。
// 覆盖 openai / siliconflow / deepseek / azure-openai 等。
class HttpTts final : public fish::core::ITextToSpeech {
 public:
  HttpTts(TtsConfig cfg, CircuitBreaker* b) : cfg_(std::move(cfg)), breaker_(b) {}

  Result<std::string> synthesize(const std::string& text) override {
    json payload{{"input", text}};
    if (!cfg_.model.empty()) payload["model"] = cfg_.model;
    if (!cfg_.voice.empty()) payload["voice"] = cfg_.voice;
    if (!cfg_.format.empty()) payload["response_format"] = cfg_.format;

    HttpRequest req;
    req.url = tts_endpoint();
    req.headers = {{"Content-Type", "application/json"}};
    if (!cfg_.api_key.empty()) {
      // Azure 用 api-key 头，其余用 Bearer
      if (cfg_.provider == "azure-openai") {
        req.headers["api-key"] = cfg_.api_key;
      } else {
        req.headers["Authorization"] = "Bearer " + cfg_.api_key;
      }
    }
    req.body = payload.dump();
    req.timeout_ms = cfg_.timeout_ms;
    req.max_retries = 1;
    req.want_binary = true;

    auto resp = HttpClient::instance().perform(req, breaker_);
    if (!resp.ok()) return Result<std::string>::fail(resp.error.code, resp.error.detail);
    return write_audio(resp.value->bytes, cfg_.format.empty() ? "mp3" : cfg_.format);
  }

 private:
  std::string tts_endpoint() const {
    if (!cfg_.path.empty()) return cfg_.base_url + cfg_.path;
    return cfg_.base_url + "/audio/speech";
  }

  TtsConfig cfg_;
  CircuitBreaker* breaker_;
};

// DeepSeek 官方语音合成（/v1/tools/tts）。
//
// 与 OpenAI 风格的 /audio/speech 完全不同——之前把 deepseek 走进 HttpTts
// 请求的是 https://api.deepseek.com/audio/speech，官方没有这个端点，
// 直接 404（实测复现）。差异点：
//   - 端点：POST {base}/v1/tools/tts
//   - 字段：text（不是 input）、voice_id（不是 voice，只有 stella/tide）
//   - 格式：wav / pcm，音频在响应体里**流式**返回（curl 层面仍是
//     一次完整 body，want_binary 收完即可）
//   - 语种自动识别，没有语种参数；不支持语速调节
//
// 计费按字符（汉字算 2），单次最多 10 000 计费字符 / 30 分钟音频。
// 文档：https://api-docs.deepseek.com/zh-cn/guides/tts
class DeepseekTts final : public fish::core::ITextToSpeech {
 public:
  DeepseekTts(TtsConfig cfg, CircuitBreaker* b) : cfg_(std::move(cfg)), breaker_(b) {}

  Result<std::string> synthesize(const std::string& text) override {
    if (text.empty()) {
      return Result<std::string>::fail(ErrorCode::kBadRequest, "tts_empty_text");
    }

    json payload;
    payload["text"] = text;
    // 音色：voice 字段就是 voice_id。空 → stella（官方默认）。
    // GUI 侧给两个可选项（stella 女声 / tide 男声），值直接透传。
    payload["voice_id"] = cfg_.voice.empty() ? "stella" : cfg_.voice;
    // 官方只出 wav / pcm。QQ 上传收 mp3 最稳，先拿 wav 再用 ffmpeg 转
    //（发出去之前由 ensure_format / 通道层处理）。
    const std::string fmt = cfg_.format.empty() ? "wav" : cfg_.format;
    payload["format"] = fmt == "pcm" ? "pcm" : "wav";
    // QQ 语音气泡里 Markdown 符号念出来很难听，统一开启过滤
    payload["filter_markdown"] = true;

    HttpRequest req;
    req.url = tts_endpoint();
    req.headers = {{"Content-Type", "application/json"}};
    if (!cfg_.api_key.empty()) {
      req.headers["Authorization"] = "Bearer " + cfg_.api_key;
    }
    req.body = payload.dump();
    req.timeout_ms = cfg_.timeout_ms > 0 ? cfg_.timeout_ms : 120000;
    req.max_retries = 1;
    req.want_binary = true;

    auto resp = HttpClient::instance().perform(req, breaker_);
    if (!resp.ok()) return Result<std::string>::fail(resp.error.code, resp.error.detail);
    // 流式断流（文档明确说提前结束的响应视为失败）：
    // wav 至少要 RIFF 头 + data 块，几百字节以下基本是坏的
    if (resp.value->bytes.size() < 256) {
      return Result<std::string>::fail(ErrorCode::kUpstreamUnavailable,
                                       "tts_stream_truncated:" +
                                           std::to_string(resp.value->bytes.size()) + "B");
    }
    return write_audio(resp.value->bytes, "wav");
  }

 private:
  std::string tts_endpoint() const {
    // base_url 为空 → 官方默认；配置里带不带 /v1 都能对上
    const std::string base =
        cfg_.base_url.empty() ? "https://api.deepseek.com" : cfg_.base_url;
    if (!cfg_.path.empty()) return base + cfg_.path;
    // base 以 /v1 结尾时避免拼出 /v1/v1
    if (base.size() >= 3 && base.compare(base.size() - 3, 3, "/v1") == 0) {
      return base + "/tools/tts";
    }
    return base + "/v1/tools/tts";
  }

  TtsConfig cfg_;
  CircuitBreaker* breaker_;
};

// 火山引擎语音合成（v1 /api/v1/tts）。
//
// 为什么用 v1 而不是 v3：v3（/api/v3/tts/unidirectional）本质是流式的，
// 音频以多段 base64 分片下发，没有「一次返回完整音频」的形态。
// v1 一次往返返回单个 JSON，data 字段即完整 base64，更适合非流式管线。
//
// 鉴权：Authorization: Bearer;<token>（分号，不是空格）。
// 且 appid / token 必须在 body 的 app 段里再带一份——只在头里带会 403。
class VolcengineTts final : public fish::core::ITextToSpeech {
 public:
  VolcengineTts(TtsConfig cfg, CircuitBreaker* b)
      : cfg_(std::move(cfg)), breaker_(b) {}

  Result<std::string> synthesize(const std::string& text) override {
    if (cfg_.app_id.empty()) {
      return Result<std::string>::fail(ErrorCode::kBadRequest,
                                       "volcengine_app_id_required");
    }
    // v1 的音频编码枚举：mp3 / ogg_opus / pcm / wav
    std::string enc = cfg_.format;
    if (enc == "ogg" || enc == "opus") enc = "ogg_opus";

    json payload = {
        {"app",
         {{"appid", cfg_.app_id},
          {"token", cfg_.api_key},
          {"cluster", cfg_.cluster.empty() ? "volcano_tts" : cfg_.cluster}}},
        {"user", {{"uid", "fish"}}},
        {"audio",
         {{"voice_type", cfg_.voice},
          {"encoding", enc},
          {"speed_ratio", 1.0},
          {"rate", 24000}}},
        {"request",
         {{"reqid", reqid()},
          {"text", text},
          {"operation", "query"}}},
    };
    // 允许配置直通覆盖 audio 段（音量/音调/情感等）
    if (!cfg_.extra_json.empty()) {
      try {
        const auto extra = json::parse(cfg_.extra_json);
        if (extra.contains("audio") && extra["audio"].is_object()) {
          for (const auto& [k, v] : extra["audio"].items()) payload["audio"][k] = v;
        }
      } catch (const std::exception&) {
        // 配置写错不该让整条链路失败，忽略
      }
    }

    HttpRequest req;
    req.url = cfg_.base_url.empty()
                  ? "https://openspeech.bytedance.com/api/v1/tts"
                  : cfg_.base_url + (cfg_.path.empty() ? "/api/v1/tts" : cfg_.path);
    req.headers = {{"Authorization", "Bearer;" + cfg_.api_key},
                   {"Content-Type", "application/json"}};
    req.body = payload.dump();
    req.timeout_ms = cfg_.timeout_ms;
    req.max_retries = 1;

    auto resp = HttpClient::instance().perform(req, breaker_);
    if (!resp.ok()) return Result<std::string>::fail(resp.error.code, resp.error.detail);

    try {
      const auto body = json::parse(resp.value->body);
      // 火山把业务错误也放在 HTTP 200 里，code 非 3000 即失败
      const int code = body.value("code", -1);
      if (code != 3000) {
        return Result<std::string>::fail(
            ErrorCode::kUpstreamUnavailable,
            "volcengine_tts:" + std::to_string(code) + ":" +
                body.value("message", ""));
      }
      if (!body.contains("data") || !body["data"].is_string()) {
        return Result<std::string>::fail(ErrorCode::kUpstreamUnavailable, "tts_no_data");
      }
      const auto audio = b64_decode(body["data"].get<std::string>());
      std::string bytes(reinterpret_cast<const char*>(audio.data()), audio.size());
      // ogg_opus 落地时用 .ogg 扩展名，便于后续按格式判断
      const std::string ext = enc == "ogg_opus" ? "ogg" : enc;
      return write_audio(bytes, ext);
    } catch (const std::exception& e) {
      return Result<std::string>::fail(ErrorCode::kBadRequest,
                                       std::string("tts_parse:") + e.what());
    }
  }

 private:
  static std::string reqid() {
    // 火山要求每次请求唯一；固定序列会导致上游去重
    static std::atomic<unsigned long long> counter{0};
    return "fish_" +
           std::to_string(static_cast<unsigned long long>(
               std::chrono::steady_clock::now().time_since_epoch().count())) +
           "_" + std::to_string(counter.fetch_add(1));
  }

  TtsConfig cfg_;
  CircuitBreaker* breaker_;
};

// ── 生图 ────────────────────────────────────────────────
// 走 OpenAI 兼容的 /images/generations（文生图）。
// 返回可能是 b64_json 或 url，两种都处理：
//   - b64_json → 落盘到 /tmp，返回本地路径（通道可直接上传）
//   - url      → 直接返回 URL
class HttpImage final : public fish::core::IImageModel {
 public:
  HttpImage(ImageConfig cfg, CircuitBreaker* b) : cfg_(std::move(cfg)), breaker_(b) {}

  Result<Response> generate(const Request& in) override {
    if (cfg_.provider == "none") {
      return Result<Response>::fail(ErrorCode::kUnsupported, "image_provider_none");
    }

    json payload{{"model", cfg_.model}, {"prompt", in.prompt}, {"n", 1}};
    const std::string size = in.size.empty() ? cfg_.default_size : in.size;
    if (!size.empty()) payload["size"] = size;
    if (!in.negative_prompt.empty()) payload["negative_prompt"] = in.negative_prompt;
    // 只要 base64，省一次额外下载；不支持的厂商会忽略该字段
    payload["response_format"] = "b64_json";

    HttpRequest req;
    req.url = cfg_.base_url + "/images/generations";
    req.headers = {{"Authorization", "Bearer " + cfg_.api_key},
                   {"Content-Type", "application/json"}};
    req.body = payload.dump();
    req.timeout_ms = cfg_.timeout_ms;
    req.max_retries = 1;   // 生图失败重试代价高，不自动重试

    auto resp = HttpClient::instance().perform(req, breaker_);
    if (!resp.ok()) return Result<Response>::fail(resp.error.code, resp.error.detail);

    Response out;
    out.model = cfg_.model;
    out.actual_size = size;

    try {
      const auto body = json::parse(resp.value->body);
      if (!body.contains("data") || !body["data"].is_array() || body["data"].empty()) {
        return Result<Response>::fail(ErrorCode::kBadRequest, "image_no_data");
      }
      const auto& item = body["data"][0];

      // 优先 b64_json
      if (item.contains("b64_json") && item["b64_json"].is_string()) {
        const std::string b64 = item["b64_json"].get<std::string>();
        auto bin = b64_decode(b64);
        if (bin.empty()) {
          return Result<Response>::fail(ErrorCode::kBadRequest, "image_b64_bad");
        }
        const std::string path = "/tmp/fish_img_" + unique_tmp_suffix() + ".png";
        std::ofstream f(path, std::ios::binary);
        if (!f) return Result<Response>::fail(ErrorCode::kInternal, "image_write");
        f.write(reinterpret_cast<const char*>(bin.data()),
                static_cast<std::streamsize>(bin.size()));
        out.image_url = path;
        out.usage_note = "b64 " + std::to_string(bin.size()) + "B";
        return Result<Response>::ok(std::move(out));
      }

      // 退而求其次：url
      if (item.contains("url") && item["url"].is_string()) {
        out.image_url = item["url"].get<std::string>();
        out.usage_note = "url";
        return Result<Response>::ok(std::move(out));
      }

      return Result<Response>::fail(ErrorCode::kBadRequest, "image_no_url");
    } catch (const std::exception& e) {
      return Result<Response>::fail(ErrorCode::kBadRequest,
                                    std::string("image_parse:") + e.what());
    }
  }

  private:
  ImageConfig cfg_;
  CircuitBreaker* breaker_;
};

// ── 搜索 ────────────────────────────────────────────────
class NullSearch final : public fish::core::IWebSearch {
 public:
  Result<std::vector<Hit>> search(const std::string&, int) override {
    return Result<std::vector<Hit>>::ok({});
  }
};

// 多厂商聚合搜索。
//
// 各家协议差异只有三处：鉴权头、请求体、响应里 title/url/snippet 的字段名。
// 把这三处做成数据表，加一家新厂商等于加一行，不用写一个新类。
//
// 注意几家容易踩的坑（都已按实测/官方文档处理）：
//   - Serper 的 URL 字段叫 link，不是 url；鉴权头是 X-API-KEY
//   - Brave 鉴权头是 X-Subscription-Token，web/search 响应在 web.results；
//     need_content 时改用 llm/context，响应在 grounding.generic，
//     且条目里是 snippets[] 数组而不是单个字符串（见 fields_for）
//   - 博查外层有 data.webPages.value 信封，标题字段是 name
//   - 智谱的 URL 字段也叫 link，且要求 search_engine / search_intent 必填
//   - 谷歌 CSE 的 key 走 query 参数，不在 header；num 最大 10
//   - SearXNG 实例必须在 settings.yml 里开 json format，否则 403
//   - Jina 的 /<query> 端点不接受条数参数，max_results 只能本地截断
class ProviderSearch final : public fish::core::IWebSearch {
 public:
  explicit ProviderSearch(SearchConfig cfg) : cfg_(std::move(cfg)) {}

  Result<std::vector<Hit>> search(const std::string& query, int max_results) override {
    const int want = max_results > 0 ? max_results : cfg_.max_results;

    if (cfg_.provider == "brave") return search_brave(query, want);
    if (cfg_.provider == "searxng") return search_searxng(query, want);
    if (cfg_.provider == "google") return search_google(query, want);
    if (cfg_.provider == "jina") return search_jina(query, want);
    return search_post_json(query, want);
  }

 private:
  // ── 响应字段名 ──
  // 大多数厂商字段名一致，只有个别不同，用候选列表兼容。
  struct Fields {
    std::vector<const char*> title;
    std::vector<const char*> url;
    std::vector<const char*> snippet;
    std::vector<const char*> array;  // 结果数组的路径（逐级 key）
  };

  Fields fields_for() const {
    if (cfg_.provider == "bocha") {
      return {{"name", "title"}, {"url"}, {"summary", "snippet"},
              {"data", "webPages", "value"}};
    }
    if (cfg_.provider == "serper") {
      return {{"title"}, {"link", "url"}, {"snippet"}, {"organic"}};
    }
    if (cfg_.provider == "zhipu") {
      return {{"title"}, {"link", "url"}, {"content", "snippet"}, {"search_result"}};
    }
    if (cfg_.provider == "exa") {
      // Exa 的 text 是正文全文，summary 是 LLM 摘要，description 才是短摘要
      return {{"title"}, {"url"}, {"summary", "text"}, {"results"}};
    }
    if (cfg_.provider == "jina") {
      return {{"title"}, {"url"}, {"content", "description"}, {"data"}};
    }
    if (cfg_.provider == "google") {
      return {{"title"}, {"link", "url"}, {"snippet"}, {"items"}};
    }
    if (cfg_.provider == "searxng") {
      return {{"title"}, {"url"}, {"content"}, {"results"}};
    }
    if (cfg_.provider == "brave") {
      // 两个端点的响应形态完全不同，必须分开映射：
      //   web/search  → web.results[]，摘要字段 description
      //   llm/context → grounding.generic[]，摘要字段 snippets（字符串数组）
      // 早期统一走兜底映射，need_content 时解析出 0 条。
      if (cfg_.search_need_content) {
        return {{"title"}, {"url"}, {"snippets"}, {"grounding", "generic"}};
      }
      return {{"title"}, {"url"}, {"description"}, {"web", "results"}};
    }
    // tavily 与 generic 兜底
    return {{"title", "name", "heading"},
            {"url", "link", "href"},
            {"summary", "content", "snippet", "description", "text"},
            {"results", "data", "items", "hits", "sources"}};
  }

  static std::string pick(const json& obj, const std::vector<const char*>& keys) {
    for (const char* k : keys) {
      if (!obj.contains(k)) continue;
      if (obj[k].is_string()) return obj[k].get<std::string>();
      // Brave 的 llm/context 把摘要放在 snippets[] 数组里，拼成一个串
      if (obj[k].is_array()) {
        std::string joined;
        for (const auto& item : obj[k]) {
          if (!item.is_string()) continue;
          if (!joined.empty()) joined += "\n";
          joined += item.get<std::string>();
        }
        if (!joined.empty()) return joined;
      }
    }
    return {};
  }

  // 沿路径逐级取数组
  static const json* dig(const json& body, const std::vector<const char*>& path) {
    const json* cur = &body;
    for (const char* k : path) {
      if (!cur->is_object() || !cur->contains(k)) return nullptr;
      cur = &(*cur)[k];
    }
    return cur->is_array() ? cur : nullptr;
  }

  std::vector<Hit> parse_hits(const std::string& raw, int max_results) const {
    std::vector<Hit> hits;
    const auto body = json::parse(raw);
    const Fields f = fields_for();

    const json* arr = dig(body, f.array);
    // 裸数组响应
    if (arr == nullptr && body.is_array()) arr = &body;
    // 博查同时存在「带信封」和「扁平」两种示例，两种都兜
    if (arr == nullptr) arr = dig(body, {"results"});
    if (arr == nullptr) {
      // 只给一段摘要的聚合 API
      if (body.contains("answer") && body["answer"].is_string()) {
        Hit h;
        h.title = "搜索摘要";
        h.snippet = body["answer"].get<std::string>();
        hits.push_back(std::move(h));
      }
      return hits;
    }

    for (const auto& r : *arr) {
      if (!r.is_object()) continue;
      Hit h;
      h.title = pick(r, f.title);
      h.url = pick(r, f.url);
      h.snippet = pick(r, f.snippet);
      if (r.contains("score") && r["score"].is_number()) {
        h.score = r["score"].get<double>();
      } else if (r.contains("rerank_score") && r["rerank_score"].is_number()) {
        h.score = r["rerank_score"].get<double>();
      }
      if (!h.title.empty() || !h.snippet.empty() || !h.url.empty()) {
        hits.push_back(std::move(h));
      }
    }
    if (max_results > 0 && static_cast<int>(hits.size()) > max_results) {
      hits.resize(static_cast<std::size_t>(max_results));
    }
    return hits;
  }

  // 收尾：发请求 → 解析。各家共用。
  Result<std::vector<Hit>> finish(const Result<HttpResult>& resp, int max_results) const {
    if (!resp.ok()) {
      return Result<std::vector<Hit>>::fail(resp.error.code,
                                            "search_" + resp.error.detail);
    }
    try {
      return Result<std::vector<Hit>>::ok(parse_hits(resp.value->body, max_results));
    } catch (const std::exception& e) {
      return Result<std::vector<Hit>>::fail(ErrorCode::kBadRequest,
                                            std::string("search_parse:") + e.what());
    }
  }

  std::string base() const {
    if (!cfg_.base_url.empty()) return cfg_.base_url;
    if (cfg_.provider == "tavily") return "https://api.tavily.com";
    if (cfg_.provider == "bocha") return "https://api.bochaai.com";
    if (cfg_.provider == "serper") return "https://google.serper.dev";
    if (cfg_.provider == "exa") return "https://api.exa.ai";
    if (cfg_.provider == "zhipu") return "https://open.bigmodel.cn";
    if (cfg_.provider == "jina") return "https://s.jina.ai";
    if (cfg_.provider == "brave") return "https://api.search.brave.com";
    if (cfg_.provider == "google") return "https://www.googleapis.com";
    return {};
  }

  // ── POST + JSON 体（tavily / bocha / serper / exa / zhipu / generic）──
  Result<std::vector<Hit>> search_post_json(const std::string& query,
                                            int max_results) const {
    std::string url = base();
    if (url.empty()) {
      return Result<std::vector<Hit>>::fail(ErrorCode::kBadRequest,
                                            "search_base_url_empty");
    }
    while (!url.empty() && url.back() == '/') url.pop_back();
    if (!cfg_.path.empty()) {
      url += cfg_.path;
    } else if (url.find("/search") == std::string::npos &&
               url.find("/web-search") == std::string::npos &&
               url.find("/web_search") == std::string::npos &&
               url.find("/v1/") == std::string::npos &&
               url.find("/api/") == std::string::npos) {
      url += "/search";
    }

    json payload;
    std::map<std::string, std::string> headers{{"Content-Type", "application/json"}};
    payload["query"] = query;

    if (cfg_.provider == "bocha") {
      payload["count"] = max_results;
      if (!cfg_.api_key.empty()) headers["Authorization"] = "Bearer " + cfg_.api_key;
    } else if (cfg_.provider == "serper") {
      payload["q"] = query;
      payload["num"] = max_results;
      if (!cfg_.api_key.empty()) headers["X-API-KEY"] = cfg_.api_key;
      payload.erase("query");
    } else if (cfg_.provider == "exa") {
      payload["query"] = query;
      payload["numResults"] = max_results;
      // 正文要显式开，否则只有 title/url/date
      if (cfg_.search_need_content) {
        payload["contents"] = {{"text", true}};
      }
      if (!cfg_.api_key.empty()) headers["x-api-key"] = cfg_.api_key;
    } else if (cfg_.provider == "zhipu") {
      payload["search_query"] = query;
      payload["search_engine"] =
          cfg_.api_engine.empty() ? "search_std" : cfg_.api_engine;
      payload["search_intent"] = false;
      payload["count"] = max_results;
      payload.erase("query");
      if (!cfg_.api_key.empty()) headers["Authorization"] = "Bearer " + cfg_.api_key;
    } else if (cfg_.provider == "tavily") {
      payload["query"] = query;
      payload["max_results"] = max_results;
      if (cfg_.search_need_content) payload["include_raw_content"] = true;
      if (!cfg_.api_key.empty()) {
        headers["Authorization"] = "Bearer " + cfg_.api_key;
      }
    } else {
      // generic：尽量宽容，把常见字段名都带上
      payload["max_results"] = max_results;
      if (!cfg_.api_key.empty()) {
        payload["api_key"] = cfg_.api_key;
        headers["Authorization"] = "Bearer " + cfg_.api_key;
      }
    }

    auto resp = HttpClient::instance().post_json(url, headers, payload.dump(),
                                                 cfg_.timeout_ms);
    return finish(resp, max_results);
  }

  // ── GET + query 参数（brave / searxng / google / jina）──
  std::string with_query(std::string url,
                         const std::vector<std::pair<std::string, std::string>>& params,
                         const std::string& extra_headers = {}) const {
    (void)extra_headers;
    url += url.find('?') == std::string::npos ? "?" : "&";
    bool first = true;
    for (const auto& [k, v] : params) {
      if (!first) url += "&";
      first = false;
      url += k + "=" + url_encode(v);
    }
    return url;
  }

  Result<std::vector<Hit>> search_brave(const std::string& query, int max_results) const {
    // Brave 的 count 上限是 20，超过会被拒
    const int count = std::min(max_results > 0 ? max_results : 10, 20);
    // 两个端点二选一，不要在 web/search 的 URL 上再拼一次 llm/context：
    // 那样第一个 URL 白拼一遍，而且 count 参数会漏进 llm/context（它不认）。
    std::string url;
    if (cfg_.search_need_content) {
      // llm/context 是真·正文抽取，web/search 只给摘要
      url = with_query(base() + "/res/v1/llm/context", {{"q", query}});
    } else {
      url = with_query(base() + "/res/v1/web/search",
                       {{"q", query}, {"count", std::to_string(count)}});
    }
    HttpRequest req;
    req.method = "GET";
    req.url = url;
    req.headers = {{"Accept", "application/json"}};
    if (!cfg_.api_key.empty()) req.headers["X-Subscription-Token"] = cfg_.api_key;
    req.timeout_ms = cfg_.timeout_ms;
    return finish(HttpClient::instance().perform(req), max_results);
  }

  Result<std::vector<Hit>> search_searxng(const std::string& query,
                                          int max_results) const {
    std::string url = base();
    if (url.empty()) {
      return Result<std::vector<Hit>>::fail(ErrorCode::kBadRequest,
                                            "search_base_url_empty");
    }
    while (!url.empty() && url.back() == '/') url.pop_back();
    url += cfg_.path.empty() ? "/search" : cfg_.path;
    url = with_query(url, {{"q", query},
                           {"format", "json"},
                           {"pageno", "1"},
                           {"language", "auto"}});
    (void)max_results;  // SearXNG 不分页大小，由实例配置决定
    HttpRequest req;
    req.method = "GET";
    req.url = url;
    req.headers = {{"Accept", "application/json"}};
    if (!cfg_.api_key.empty()) {
      req.headers["Authorization"] = "Bearer " + cfg_.api_key;
    }
    req.timeout_ms = cfg_.timeout_ms;
    return finish(HttpClient::instance().perform(req), max_results);
  }

  Result<std::vector<Hit>> search_google(const std::string& query,
                                         int max_results) const {
    if (cfg_.api_engine.empty()) {
      return Result<std::vector<Hit>>::fail(ErrorCode::kBadRequest,
                                            "google_cx_required");
    }
    // num 上限 10，超过直接报错
    const int num = std::min(max_results > 0 ? max_results : 10, 10);
    // key 走 query 参数，不是 header
    std::string url = with_query(base() + "/customsearch/v1",
                                 {{"key", cfg_.api_key},
                                  {"cx", cfg_.api_engine},
                                  {"q", query},
                                  {"num", std::to_string(num)}});
    HttpRequest req;
    req.method = "GET";
    req.url = url;
    req.headers = {{"Accept", "application/json"}};
    req.timeout_ms = cfg_.timeout_ms;
    return finish(HttpClient::instance().perform(req), max_results);
  }

  Result<std::vector<Hit>> search_jina(const std::string& query, int max_results) const {
    // Jina 默认返回正文全文，这是它最大的特点。
    // 注意：s.jina.ai/<query> 这个端点**不接受条数参数**（没有 num/count/limit），
    // 上游返回多少条就是多少条，所以 max_results 只能本地截断——见下面 finish 之后。
    std::string url = base();
    while (!url.empty() && url.back() == '/') url.pop_back();
    url = with_query(url + "/" + url_encode(query), {});
    HttpRequest req;
    req.method = "GET";
    req.url = url;
    req.headers = {{"Accept", "application/json"}};
    if (!cfg_.api_key.empty()) req.headers["Authorization"] = "Bearer " + cfg_.api_key;
    if (!cfg_.search_need_content) req.headers["X-Respond-With"] = "no-content";
    req.timeout_ms = cfg_.timeout_ms;
    auto resp = finish(HttpClient::instance().perform(req), max_results);
    if (!resp.ok() || max_results <= 0) return resp;
    // 本地截断：上游不认条数限制，这里兜住，避免把几十条全塞给模型
    auto& hits = *resp.value;
    if (static_cast<int>(hits.size()) > max_results) {
      hits.resize(static_cast<std::size_t>(max_results));
    }
    return resp;
  }

  SearchConfig cfg_;
};

// DeepSeek 官方 Search API。
// 端点：POST {base_url}/v1/tools/search
// 文档：https://api-docs.deepseek.com/zh-cn/guides/search/
//
// 特点：
//   - 原始 JSON API，不消耗模型轮次
//   - 每 1000 次成功请求 5 元（失败/空结果不计费）
//   - 返回结构化结果：title/url/snippet/summary/content/date/authority
//   - 支持搜索语法：site: / -site: / -词 / "精确短语" / lang: / after: / before:
class DeepSeekSearch final : public fish::core::IWebSearch {
 public:
  explicit DeepSeekSearch(SearchConfig cfg) : cfg_(std::move(cfg)) {}

  Result<std::vector<Hit>> search(const std::string& query, int max_results) override {
    // query 限制 1~128 字符，不能全空白
    std::string q = query;
    if (q.size() > 128) q.resize(128);
    if (q.find_first_not_of(" \t\r\n") == std::string::npos) {
      return Result<std::vector<Hit>>::fail(ErrorCode::kBadRequest, "search_empty_query");
    }

    int count = max_results > 0 ? max_results : cfg_.max_results;
    if (count < 1) count = 1;
    if (count > 50) count = 50;

    json payload{
        {"query", q},
        {"count", count},
        {"need_content", cfg_.search_need_content},
    };

    const std::string url = search_endpoint() + "/search";
    std::map<std::string, std::string> headers{
        {"Authorization", "Bearer " + cfg_.api_key},
    };

    auto resp = HttpClient::instance().post_json(url, headers, payload.dump(),
                                                 cfg_.timeout_ms);
    if (!resp.ok()) {
      // 402 余额不足 / 429 限流 / 5xx 可重试，已在 HttpClient 内处理退避
      return Result<std::vector<Hit>>::fail(resp.error.code,
                                            "search_" + resp.error.detail);
    }

    try {
      const auto body = json::parse(resp.value->body);
      std::vector<Hit> hits;
      for (const auto& r : body.value("results", json::array())) {
        if (!r.is_object()) continue;
        Hit h;
        h.title = r.value("title", "");
        h.url = r.value("url", "");
        // summary 是给模型读的长摘要（600-800 字符），snippet 是给前端看的短摘要
        // 优先用 summary，模型理解更完整
        h.snippet = r.value("summary", "");
        if (h.snippet.empty()) h.snippet = r.value("snippet", "");
        if (r.contains("rerank_score") && r["rerank_score"].is_number()) {
          h.score = r["rerank_score"].get<double>();
        }
        if (!h.title.empty() || !h.url.empty()) hits.push_back(std::move(h));
      }
      return Result<std::vector<Hit>>::ok(std::move(hits));
    } catch (const std::exception& e) {
      return Result<std::vector<Hit>>::fail(ErrorCode::kBadRequest,
                                            std::string("search_parse:") + e.what());
    }
  }

 private:
  // base_url 可能带或不带 /v1，统一归一化
  std::string search_endpoint() const {
    std::string base = cfg_.base_url.empty() ? "https://api.deepseek.com" : cfg_.base_url;
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (base.find("/v1/tools") != std::string::npos) return base;
    if (base.size() >= 3 && base.compare(base.size() - 3, 3, "/v1") == 0) {
      return base + "/tools";
    }
    return base + "/v1/tools";
  }

  SearchConfig cfg_;
};

// DeepSeek 官方 Fetch API。
// 端点：POST {base_url}/v1/tools/fetch
// 返回 {title, content}，content 是 Markdown 正文。
//
// 注意：fetch 由 DeepSeek 服务端执行，不存在 SSRF 风险
// （我们不发请求到目标站点，只发给 DeepSeek）。
// 403 表示被安全策略拒绝，404 表示抓不到可用内容。
class DeepSeekFetch final : public fish::core::IWebFetch {
 public:
  explicit DeepSeekFetch(SearchConfig cfg) : cfg_(std::move(cfg)) {}

  Result<Page> fetch(const std::string& url) override {
    // 基本校验：必须是含 host 的 HTTP/HTTPS URL，不能带用户名密码
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
      return Result<Page>::fail(ErrorCode::kBadRequest, "fetch_scheme_not_allowed");
    }
    if (url.size() > 8192) {
      return Result<Page>::fail(ErrorCode::kBadRequest, "fetch_url_too_long");
    }
    const auto scheme_end = url.find("://");
    const auto host_start = scheme_end + 3;
    if (host_start >= url.size() || url[host_start] == '/') {
      return Result<Page>::fail(ErrorCode::kBadRequest, "fetch_missing_host");
    }
    // 拒绝带 userinfo 的 URL（可能用于钓鱼/凭据泄露）
    const auto authority_end = url.find_first_of("/?#", host_start);
    const std::string authority =
        url.substr(host_start, authority_end == std::string::npos
                                   ? std::string::npos
                                   : authority_end - host_start);
    if (authority.find('@') != std::string::npos) {
      return Result<Page>::fail(ErrorCode::kBadRequest, "fetch_userinfo_not_allowed");
    }

    json payload{{"url", url}};
    const std::string endpoint = fetch_endpoint() + "/fetch";
    std::map<std::string, std::string> headers{
        {"Authorization", "Bearer " + cfg_.api_key},
    };

    auto resp = HttpClient::instance().post_json(endpoint, headers, payload.dump(),
                                                 cfg_.timeout_ms);
    if (!resp.ok()) {
      return Result<Page>::fail(resp.error.code, "fetch_" + resp.error.detail);
    }

    try {
      const auto body = json::parse(resp.value->body);
      Page page;
      page.url = url;
      page.title = body.value("title", "");
      page.content = body.value("content", "");
      page.content_type = "text/markdown";
      page.status = 200;
      page.bytes = page.content.size();

      if (page.content.empty()) {
        return Result<Page>::fail(ErrorCode::kUpstreamUnavailable, "fetch_empty_content");
      }

      // 截断：超长页面会撑爆上下文，按配置截到合理长度
      if (page.content.size() > cfg_.fetch_max_chars) {
        std::size_t cut = cfg_.fetch_max_chars;
        while (cut > 0 &&
               (static_cast<unsigned char>(page.content[cut]) & 0xC0) == 0x80) {
          --cut;
        }
        page.content.resize(cut);
        page.content += "\n\n…（内容过长已截断）";
      }
      return Result<Page>::ok(std::move(page));
    } catch (const std::exception& e) {
      return Result<Page>::fail(ErrorCode::kBadRequest,
                                std::string("fetch_parse:") + e.what());
    }
  }

 private:
  std::string fetch_endpoint() const {
    std::string base = cfg_.base_url.empty() ? "https://api.deepseek.com" : cfg_.base_url;
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (base.find("/v1/tools") != std::string::npos) return base;
    if (base.size() >= 3 && base.compare(base.size() - 3, 3, "/v1") == 0) {
      return base + "/tools";
    }
    return base + "/v1/tools";
  }

  SearchConfig cfg_;
};

// 通用 HTTP 搜索（provider = "generic"）。
//
// 用途：让用户接**任意** API 或桥接层——自建搜索服务、第三方聚合 API、
// 把某个搜索引擎包成 HTTP 的本地桥接。与语音/生图同构：
// provider 只是配置，换实现不改调用方。
//
// 协议（尽量宽容，能适配大多数实现）：
//   请求：POST {base_url}{path}，path 默认 /search
//         body: {"query","max_results"}（有 key 时附 api_key）
//         header: Authorization: Bearer <key>（有 key 时带）
//   响应：接受以下任一形态
//         1. {"results":[{title,url,content/snippet,score}]}   (Tavily 风格)
//         2. {"data":[{title,url,snippet}]}
//         3. {"items":[...]} / {"hits":[...]} / {"sources":[...]}
//         4. [...]（裸数组）
//         5. {"answer":"..."}（只给一段摘要的聚合 API）
//   字段名做多重兼容，任一命中即可。
class GenericSearch final : public fish::core::IWebSearch {
 public:
  explicit GenericSearch(SearchConfig cfg) : cfg_(std::move(cfg)) {}

  Result<std::vector<Hit>> search(const std::string& query, int max_results) override {
    if (cfg_.base_url.empty()) {
      return Result<std::vector<Hit>>::fail(ErrorCode::kBadRequest,
                                            "search_base_url_empty");
    }

    std::string url = cfg_.base_url;
    while (!url.empty() && url.back() == '/') url.pop_back();
    if (!cfg_.path.empty()) {
      url += cfg_.path;
    } else if (url.find("/search") == std::string::npos &&
               url.find("/v1/") == std::string::npos) {
      url += "/search";
    }

    json payload{{"query", query}, {"max_results", max_results}};
    if (!cfg_.api_key.empty()) payload["api_key"] = cfg_.api_key;

    std::map<std::string, std::string> headers;
    if (!cfg_.api_key.empty()) {
      headers["Authorization"] = "Bearer " + cfg_.api_key;
    }

    auto resp = HttpClient::instance().post_json(url, headers, payload.dump(),
                                                 cfg_.timeout_ms);
    if (!resp.ok()) {
      return Result<std::vector<Hit>>::fail(resp.error.code,
                                            "search_" + resp.error.detail);
    }

    try {
      const auto body = json::parse(resp.value->body);
      std::vector<Hit> hits;

      const json* arr = nullptr;
      if (body.is_array()) {
        arr = &body;
      } else {
        for (const char* key : {"results", "data", "items", "hits", "sources"}) {
          if (body.contains(key) && body[key].is_array()) {
            arr = &body[key];
            break;
          }
        }
      }

      if (arr != nullptr) {
        for (const auto& r : *arr) {
          if (!r.is_object()) continue;
          Hit h;
          for (const char* k : {"title", "name", "heading"}) {
            if (r.contains(k) && r[k].is_string()) {
              h.title = r[k].get<std::string>();
              break;
            }
          }
          for (const char* k : {"url", "link", "href"}) {
            if (r.contains(k) && r[k].is_string()) {
              h.url = r[k].get<std::string>();
              break;
            }
          }
          for (const char* k : {"summary", "content", "snippet", "description", "text"}) {
            if (r.contains(k) && r[k].is_string()) {
              h.snippet = r[k].get<std::string>();
              break;
            }
          }
          if (r.contains("score") && r["score"].is_number()) {
            h.score = r["score"].get<double>();
          } else if (r.contains("rerank_score") && r["rerank_score"].is_number()) {
            h.score = r["rerank_score"].get<double>();
          }
          if (!h.title.empty() || !h.snippet.empty() || !h.url.empty()) {
            hits.push_back(std::move(h));
          }
        }
      } else if (body.contains("answer") && body["answer"].is_string()) {
        Hit h;
        h.title = "搜索摘要";
        h.snippet = body["answer"].get<std::string>();
        hits.push_back(std::move(h));
      }

      if (max_results > 0 && static_cast<int>(hits.size()) > max_results) {
        hits.resize(static_cast<std::size_t>(max_results));
      }
      return Result<std::vector<Hit>>::ok(std::move(hits));
    } catch (const std::exception& e) {
      return Result<std::vector<Hit>>::fail(ErrorCode::kBadRequest,
                                            std::string("search_parse:") + e.what());
    }
  }

 private:
  SearchConfig cfg_;
};

// 通用 HTTP 抓取（provider = "generic"）。
//
// 与 DeepSeekFetch 的区别：这是**我们直连目标站点**，
// 所以必须自己做 SSRF 防护（拒绝内网/环回/链路本地地址），
// 因为 URL 来自模型输出，模型可能被提示注入诱导去读内部服务。
class GenericFetch final : public fish::core::IWebFetch {
 public:
  explicit GenericFetch(SearchConfig cfg) : cfg_(std::move(cfg)) {}

  Result<Page> fetch(const std::string& url) override {
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
      return Result<Page>::fail(ErrorCode::kBadRequest, "fetch_scheme_not_allowed");
    }

    if (!cfg_.allow_private_hosts) {
      const std::string host = extract_host(url);
      if (host.empty()) {
        return Result<Page>::fail(ErrorCode::kBadRequest, "fetch_bad_url");
      }
      if (is_private_host(host)) {
        ::fish::log::log_line(::fish::log::LogLevel::kWarn, "fetch", "blocked_private")
            .field("host", host);
        return Result<Page>::fail(ErrorCode::kBadRequest, "fetch_private_host_blocked");
      }
      // 只查主机名字面量不够：公网域名完全可以解析到 169.254.169.254
      // （云元数据）或 127.0.0.1。URL 来自模型输出，这是真实攻击面。
      // 所以连接前先自己解析，校验**所有**解析结果，再用 CURLOPT_RESOLVE
      // 把校验过的 IP 钉住——否则校验和连接各解析一次，中间的 DNS
      // rebinding 能绕开校验。
      std::vector<std::string> pins;
      if (auto e = resolve_and_pin(url, host, &pins); !e.ok()) {
        ::fish::log::log_line(::fish::log::LogLevel::kWarn, "fetch",
                              "blocked_private_resolved")
            .field("host", host)
            .field("detail", e.detail);
        return Result<Page>::fail(ErrorCode::kBadRequest, e.detail);
      }
      auto dl = HttpClient::instance().download(url, cfg_.fetch_max_bytes, pins);
      if (!dl.ok()) {
        return Result<Page>::fail(dl.error.code, "fetch_" + dl.error.detail);
      }
      return build_page(url, *dl.value);
    }

    auto dl = HttpClient::instance().download(url, cfg_.fetch_max_bytes);
    if (!dl.ok()) {
      return Result<Page>::fail(dl.error.code, "fetch_" + dl.error.detail);
    }
    return build_page(url, *dl.value);
  }

 private:
  // 把已下载的原始响应组装成 Page（剥 HTML / 截断）。两条路径共用。
  Result<Page> build_page(const std::string& url, const std::string& raw) const {
    Page page;
    page.url = url;
    page.bytes = raw.size();

    const bool looks_html = raw.find("<html") != std::string::npos ||
                            raw.find("<!DOCTYPE") != std::string::npos ||
                            raw.find("<body") != std::string::npos;
    if (looks_html) {
      page.content_type = "text/html";
      page.title = extract_title(raw);
      page.content = strip_html(raw);
    } else {
      page.content_type = "text/plain";
      page.content = raw;
    }

    if (page.content.size() > cfg_.fetch_max_chars) {
      std::size_t cut = cfg_.fetch_max_chars;
      while (cut > 0 &&
             (static_cast<unsigned char>(page.content[cut]) & 0xC0) == 0x80) {
        --cut;
      }
      page.content.resize(cut);
      page.content += "\n…（内容过长已截断）";
    }
    page.status = 200;
    return Result<Page>::ok(std::move(page));
  }

  // 解析 URL 的 host，校验所有 A/AAAA 结果都不是内网，再生成
  // CURLOPT_RESOLVE 需要的 "host:port:ip" 钉住表。
  //
  // 返回的 Error.detail 直接作为对外的失败原因（fetch_* 前缀）。
  static fish::core::Error resolve_and_pin(const std::string& url,
                                           const std::string& host,
                                           std::vector<std::string>* pins) {
    std::string bare = host;
    // IPv6 字面量在 URL 里带方括号，getaddrinfo 不认
    if (bare.size() >= 2 && bare.front() == '[' && bare.back() == ']') {
      bare = bare.substr(1, bare.size() - 2);
    }

    const std::string port = extract_port(url);

    // 已经是 IP 字面量：不存在「解析结果和连接目标不一致」的问题，
    // 上面的 is_private_host 已经判过，不需要再钉一次（也避免
    // 把 IPv6 字面量塞进 CURLOPT_RESOLVE 这种奇怪的用法）。
    {
      in_addr v4{};
      in6_addr v6{};
      if (::inet_pton(AF_INET, bare.c_str(), &v4) == 1 ||
          ::inet_pton(AF_INET6, bare.c_str(), &v6) == 1) {
        return fish::core::Error::none();
      }
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;      // IPv4 与 IPv6 都要查
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (::getaddrinfo(bare.c_str(), port.c_str(), &hints, &res) != 0 || res == nullptr) {
      if (res != nullptr) ::freeaddrinfo(res);
      return fish::core::Error::make(ErrorCode::kBadRequest, "fetch_dns_failed");
    }

    std::vector<std::string> addrs;
    fish::core::Error err = fish::core::Error::none();
    for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
      if (p->ai_addr == nullptr) continue;
      char buf[INET6_ADDRSTRLEN] = {0};
      const bool v6 = p->ai_family == AF_INET6;
      const void* src = v6
                            ? static_cast<const void*>(
                                  &reinterpret_cast<const sockaddr_in6*>(p->ai_addr)->sin6_addr)
                            : static_cast<const void*>(
                                  &reinterpret_cast<const sockaddr_in*>(p->ai_addr)->sin_addr);
      if (::inet_ntop(p->ai_family, src, buf, sizeof(buf)) == nullptr) continue;
      const std::string ip{buf};
      if (is_private_ip(ip)) {
        err = fish::core::Error::make(ErrorCode::kBadRequest,
                                      "fetch_private_host_blocked:" + ip);
        break;
      }
      if (std::find(addrs.begin(), addrs.end(), ip) == addrs.end()) addrs.push_back(ip);
    }
    ::freeaddrinfo(res);

    if (!err.ok()) return err;
    if (addrs.empty()) {
      return fish::core::Error::make(ErrorCode::kBadRequest, "fetch_dns_empty");
    }

    // CURLOPT_RESOLVE 的格式：host:port:address
    // IPv6 地址要带方括号，且 host 用原始形态（不含方括号）
    const std::string pin_host =
        (bare.find(':') != std::string::npos) ? bare : host;
    for (const auto& ip : addrs) {
      const bool v6 = ip.find(':') != std::string::npos;
      pins->push_back(pin_host + ":" + port + ":" + (v6 ? "[" + ip + "]" : ip));
    }
    return fish::core::Error::none();
  }

  // 从 URL 里取端口，缺省按 scheme 给 80/443。
  static std::string extract_port(const std::string& url) {
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return "80";
    const bool https = url.compare(0, scheme_end, "https") == 0;
    std::string rest = url.substr(scheme_end + 3);
    const auto slash = rest.find_first_of("/?#");
    if (slash != std::string::npos) rest = rest.substr(0, slash);
    const auto at = rest.find('@');
    if (at != std::string::npos) rest = rest.substr(at + 1);
    // IPv6 字面量 [::1]:8080 —— 冒号在方括号里，要跳过
    if (!rest.empty() && rest.front() == '[') {
      const auto close = rest.find(']');
      if (close != std::string::npos) {
        const auto colon = rest.find(':', close);
        if (colon != std::string::npos) return rest.substr(colon + 1);
        return https ? "443" : "80";
      }
    }
    const auto colon = rest.find(':');
    if (colon != std::string::npos) {
      const std::string p = rest.substr(colon + 1);
      if (!p.empty()) return p;
    }
    return https ? "443" : "80";
  }

  // 二进制解析出来的 IP 再做一次内网判定（IPv4 + IPv6 全覆盖）。
  // 与 is_private_host 的分工：那个查「主机名字面量」，这个查「解析结果」。
  static bool is_private_ip(const std::string& ip) {
    if (ip.find(':') != std::string::npos) {
      // IPv6
      in6_addr a{};
      if (::inet_pton(AF_INET6, ip.c_str(), &a) != 1) return true;  // 解析不了就当不可信
      const unsigned char* b = a.s6_addr;
      // ::1 环回
      static const unsigned char kLoop[16] = {0, 0, 0, 0, 0, 0, 0, 0,
                                              0, 0, 0, 0, 0, 0, 0, 1};
      if (std::memcmp(b, kLoop, 16) == 0) return true;
      // :: （未指定）
      bool all_zero = true;
      for (int i = 0; i < 16; ++i) {
        if (b[i] != 0) {
          all_zero = false;
          break;
        }
      }
      if (all_zero) return true;
      // fc00::/7 唯一本地地址
      if ((b[0] & 0xFE) == 0xFC) return true;
      // fe80::/10 链路本地
      if (b[0] == 0xFE && (b[1] & 0xC0) == 0x80) return true;
      // ff00::/8 组播
      if (b[0] == 0xFF) return true;
      // ::ffff:a.b.c.d 映射的 IPv4：取回后 4 字节按 IPv4 规则再判一次
      static const unsigned char kMap[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
      if (std::memcmp(b, kMap, 12) == 0) {
        return is_private_ipv4(b[12], b[13], b[14], b[15]);
      }
      return false;
    }
    // IPv4
    in_addr a{};
    if (::inet_pton(AF_INET, ip.c_str(), &a) != 1) return true;
    const unsigned char* b = reinterpret_cast<const unsigned char*>(&a.s_addr);
    return is_private_ipv4(b[0], b[1], b[2], b[3]);
  }

  static bool is_private_ipv4(int a, int b, int c, int d) {
    if (a == 10) return true;                          // 10/8
    if (a == 172 && b >= 16 && b <= 31) return true;   // 172.16/12
    if (a == 192 && b == 168) return true;             // 192.168/16
    if (a == 127) return true;                         // 环回
    if (a == 169 && b == 254) return true;             // 链路本地（云元数据）
    if (a == 0) return true;                           // 0/8
    if (a >= 224) return true;                         // 组播/保留
    if (a == 100 && b >= 64 && b <= 127) return true;  // CGNAT
    if (a == 192 && b == 0 && c == 0) return true;     // 192.0.0/24
    if (a == 198 && (b == 18 || b == 19)) return true; // 198.18/15 基准测试
    (void)c;
    (void)d;
    return false;
  }
  static std::string extract_host(const std::string& url) {
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return {};
    std::string rest = url.substr(scheme_end + 3);
    const auto slash = rest.find_first_of("/?#");
    if (slash != std::string::npos) rest = rest.substr(0, slash);
    const auto at = rest.find('@');
    if (at != std::string::npos) rest = rest.substr(at + 1);
    const auto colon = rest.find(':');
    if (colon != std::string::npos) rest = rest.substr(0, colon);
    return rest;
  }

  static bool is_private_host(const std::string& host) {
    if (host.empty()) return true;
    if (host == "localhost" || host == "localhost.localdomain") return true;

    int a = 0, b = 0, c = 0, d = 0;
    if (std::sscanf(host.c_str(), "%d.%d.%d.%d", &a, &b, &c, &d) == 4) {
      if (a == 10) return true;                         // 10/8
      if (a == 172 && b >= 16 && b <= 31) return true;  // 172.16/12
      if (a == 192 && b == 168) return true;            // 192.168/16
      if (a == 127) return true;                        // 环回
      if (a == 169 && b == 254) return true;            // 链路本地（云元数据）
      if (a == 0) return true;
      if (a >= 224) return true;                        // 组播/保留
      if (a == 100 && b >= 64 && b <= 127) return true; // CGNAT
      return false;
    }

    if (host.find(':') != std::string::npos) {
      if (host == "::1" || host == "[::1]") return true;
      if (host.rfind("fe80:", 0) == 0 || host.rfind("[fe80:", 0) == 0) return true;
      if (host.rfind("fc", 0) == 0 || host.rfind("fd", 0) == 0) return true;
      if (host.rfind("[fc", 0) == 0 || host.rfind("[fd", 0) == 0) return true;
      return false;
    }
    return false;
  }

  static std::string extract_title(const std::string& html) {
    const auto open = html.find("<title");
    if (open == std::string::npos) return {};
    const auto gt = html.find('>', open);
    if (gt == std::string::npos) return {};
    const auto close = html.find("</title>", gt);
    if (close == std::string::npos) return {};
    std::string t = html.substr(gt + 1, close - gt - 1);
    std::string out;
    bool last_space = false;
    for (const char ch : t) {
      const bool is_space = (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t');
      if (is_space) {
        if (!last_space && !out.empty()) out += ' ';
        last_space = true;
      } else {
        out += ch;
        last_space = false;
      }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
  }

  // 剥离 HTML 标签。不追求完美解析，够模型理解就行。
  static std::string strip_html(const std::string& html) {
    std::string out;
    out.reserve(html.size() / 2);
    bool in_tag = false;
    bool in_script = false;
    bool in_style = false;

    for (std::size_t i = 0; i < html.size(); ++i) {
      const char ch = html[i];
      if (!in_tag && ch == '<') {
        if (html.compare(i, 7, "<script") == 0) in_script = true;
        if (html.compare(i, 6, "<style") == 0) in_style = true;
        in_tag = true;
        continue;
      }
      if (in_tag) {
        if (ch == '>') {
          in_tag = false;
          if (i >= 8 && html.compare(i - 8, 9, "</script>") == 0) in_script = false;
          if (i >= 7 && html.compare(i - 7, 8, "</style>") == 0) in_style = false;
          out += ' ';
        }
        continue;
      }
      if (in_script || in_style) continue;
      out += ch;
    }

    std::string cleaned;
    cleaned.reserve(out.size());
    bool last_space = false;
    for (const char ch : out) {
      const bool is_space = (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n');
      if (is_space) {
        if (!last_space) cleaned += ' ';
        last_space = true;
      } else {
        cleaned += ch;
        last_space = false;
      }
    }
    return cleaned;
  }

  SearchConfig cfg_;
};

CircuitBreaker& breaker_for(const std::string& provider, const char* kind) {  static std::mutex mu;
  static std::map<std::string, std::unique_ptr<CircuitBreaker>> pool;
  std::lock_guard<std::mutex> lock(mu);
  const std::string key = provider + "/" + kind;
  auto it = pool.find(key);
  if (it == pool.end()) it = pool.emplace(key, std::make_unique<CircuitBreaker>(key)).first;
  return *it->second;
}

}  // namespace

// ── CircuitBreaker ──────────────────────────────────────
CircuitBreaker::CircuitBreaker(std::string name, int threshold, std::chrono::seconds cooldown)
    : name_(std::move(name)), threshold_(threshold), cooldown_(cooldown) {}

bool CircuitBreaker::is_open() const {
  std::lock_guard<std::mutex> lock(mu_);
  if (failures_ < threshold_) return false;
  return fish::core::Clock::now() < open_until_;
}

bool CircuitBreaker::allow() const { return !is_open(); }

void CircuitBreaker::on_success() {
  std::lock_guard<std::mutex> lock(mu_);
  failures_ = 0;
}

void CircuitBreaker::on_failure() {
  std::lock_guard<std::mutex> lock(mu_);
  ++failures_;
  if (failures_ >= threshold_) {
    open_until_ = fish::core::Clock::now() + cooldown_;
    ::fish::log::log_line(::fish::log::LogLevel::kWarn, "http", "circuit_open")
        .field("provider", name_)
        .field("failures", failures_);
  }
}

// ── HttpClient ──────────────────────────────────────────
HttpClient& HttpClient::instance() {
  static HttpClient c;
  return c;
}

void HttpClient::global_init() { curl_global_init(CURL_GLOBAL_DEFAULT); }
void HttpClient::global_cleanup() { curl_global_cleanup(); }

Result<HttpResult> HttpClient::perform(const HttpRequest& req, CircuitBreaker* breaker) {
  if (breaker != nullptr && !breaker->allow()) {
    return Result<HttpResult>::fail(ErrorCode::kUpstreamUnavailable, "circuit_open");
  }

  // multipart 由 curl_mime 组装；file 读取失败要在发请求前就报出来，
  // 不能发一个缺内容的表单让上游返回语焉不详的 400。
  const bool multipart = !req.form_fields.empty() || !req.form_files.empty();
  if (multipart) {
    for (const auto& f : req.form_files) {
      std::ifstream probe(f.path, std::ios::binary);
      if (!probe) {
        return Result<HttpResult>::fail(ErrorCode::kBadRequest,
                                        "form_file_unreadable:" + f.path);
      }
    }
  }

  for (int attempt = 0; attempt <= req.max_retries; ++attempt) {
    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
      return Result<HttpResult>::fail(ErrorCode::kInternal, "curl_init");
    }

    HttpResult result;
    curl_slist* headers = nullptr;
    for (const auto& [k, v] : req.headers) {
      headers = curl_slist_append(headers, (k + ": " + v).c_str());
    }

    std::string body_out;
    curl_easy_setopt(curl, CURLOPT_URL, req.url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body_out);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &result.headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, req.timeout_ms);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);

    // DNS 钉住：调用方（GenericFetch）已自己解析并校验过 IP，
    // 这里让 libcurl 只连那些地址，避免校验与连接各解析一次。
    // 残留缺口：3xx 跳到另一个主机时，新主机不在这张表里，仍走正常解析。
    // 要彻底堵住得自己处理重定向（逐个校验再重发），不在本次范围。
    curl_slist* resolve = nullptr;
    for (const auto& entry : req.resolve_entries) {
      resolve = curl_slist_append(resolve, entry.c_str());
    }
    if (resolve != nullptr) {
      curl_easy_setopt(curl, CURLOPT_RESOLVE, resolve);
    }

    curl_mime* mime = nullptr;
    if (multipart) {
      mime = curl_mime_init(curl);
      for (const auto& [k, v] : req.form_fields) {
        curl_mimepart* part = curl_mime_addpart(mime);
        curl_mime_name(part, k.c_str());
        curl_mime_data(part, v.c_str(), CURL_ZERO_TERMINATED);
      }
      for (const auto& f : req.form_files) {
        curl_mimepart* part = curl_mime_addpart(mime);
        curl_mime_name(part, f.name.c_str());
        // 文件名必须带：多数上游靠文件名/扩展名选解析器
        std::string base = f.filename;
        if (base.empty()) {
          const auto slash = f.path.find_last_of('/');
          base = slash == std::string::npos ? f.path : f.path.substr(slash + 1);
        }
        curl_mime_filename(part, base.c_str());
        if (!f.mime.empty()) curl_mime_type(part, f.mime.c_str());
        // 文件由 curl 流式读取，不把整份内容读进内存。
        // 返回值必须检查：curl 是在发送时才打开文件的，预检之后文件被删
        // （临时文件被清理、语音文件被另一线程 remove）会静默变成一个
        // 空字段，上游只回一个语焉不详的 400。这里直接判失败并说清是哪个文件。
        if (curl_mime_filedata(part, f.path.c_str()) != CURLE_OK) {
          ::fish::log::log_line(::fish::log::LogLevel::kError, "http",
                                "mime_filedata_failed")
              .field("path", f.path);
          if (mime != nullptr) curl_mime_free(mime);
          curl_slist_free_all(resolve);
          curl_slist_free_all(headers);
          curl_easy_cleanup(curl);
          return Result<HttpResult>::fail(ErrorCode::kBadRequest,
                                          "form_file_unreadable:" + f.path);
        }
      }
      curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    } else if (req.method == "POST") {
      curl_easy_setopt(curl, CURLOPT_POST, 1L);
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.data());
      curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(req.body.size()));
    } else if (req.method == "GET") {
      curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    } else if (req.method == "UPLOAD") {
      // 带二进制 body 的自定义方法/POST：微信 CDN 上传用。
      // CUSTOMREQUEST + POSTFIELDS 而不是 CURLOPT_UPLOAD——后者会强制
      // 使用上传回调流式读取，跟已有的 write_cb 回应路径冲突。
      curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST,
                       req.custom_verb.empty() ? "POST" : req.custom_verb.c_str());
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.data());
      curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(req.body.size()));
    } else {
      curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, req.method.c_str());
    }

    const CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (mime != nullptr) curl_mime_free(mime);
    curl_slist_free_all(resolve);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    result.status = static_cast<int>(status);
    if (req.want_binary) {
      result.bytes = std::move(body_out);
    } else {
      result.body = std::move(body_out);
    }

    if (code == CURLE_OK && result.status >= 200 && result.status < 300) {
      if (breaker != nullptr) breaker->on_success();
      return Result<HttpResult>::ok(std::move(result));
    }

    const bool transport_err = code != CURLE_OK;
    const bool retryable = transport_err || is_retryable(result.status);
    if (breaker != nullptr) breaker->on_failure();

    if (!retryable || attempt == req.max_retries) {
      ErrorCode ec = ErrorCode::kUpstreamUnavailable;
      if (result.status == 429) {
        ec = ErrorCode::kRateLimited;
      } else if (result.status == 401 || result.status == 403) {
        ec = ErrorCode::kUnauthorized;
      } else if (result.status >= 400 && result.status < 500) {
        ec = ErrorCode::kBadRequest;
      }
      ::fish::log::log_line(::fish::log::LogLevel::kWarn, "http", "request_failed")
          .field("url", req.url)
          .field("status", result.status)
          // 4xx 多半是请求体不对，必须把上游说的话打出来，否则只能靠猜
          .field("body", result.body.substr(0, 400));
      return Result<HttpResult>::fail(ec, "http_error");
    }

    int wait_ms = 500 * (attempt + 1) * (attempt + 1);
    if (result.status == 429) {
      auto it = result.headers.find("retry-after");
      if (it != result.headers.end()) {
        try {
          wait_ms = std::stoi(it->second) * 1000;
        } catch (...) {
        }
      }
      wait_ms = std::max(wait_ms, 2000);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
  }
  return Result<HttpResult>::fail(ErrorCode::kUpstreamUnavailable, "retries_exhausted");
}

Result<HttpResult> HttpClient::post_json(const std::string& url,
                                         const std::map<std::string, std::string>& headers,
                                         const std::string& json_body, int timeout_ms,
                                         CircuitBreaker* breaker) {
  HttpRequest req;
  req.url = url;
  req.headers = headers;
  req.headers["Content-Type"] = "application/json";
  req.body = json_body;
  req.timeout_ms = timeout_ms;
  return perform(req, breaker);
}

Result<std::string> HttpClient::download(const std::string& url, std::size_t max_bytes,
                                         const std::vector<std::string>& resolve_entries) {
  HttpRequest req;
  req.method = "GET";
  req.url = url;
  req.want_binary = true;
  req.max_retries = 1;
  req.resolve_entries = resolve_entries;
  auto r = perform(req);
  if (!r.ok()) return Result<std::string>::fail(r.error.code, r.error.detail);
  if (r.value->bytes.size() > max_bytes) {
    return Result<std::string>::fail(ErrorCode::kBadRequest, "too_large");
  }
  return Result<std::string>::ok(std::move(r.value->bytes));
}

Result<HttpResult> HttpClient::post_form(
    const std::string& url, const std::map<std::string, std::string>& headers,
    const std::vector<std::pair<std::string, std::string>>& fields,
    const std::vector<HttpRequest::FormFile>& files, int timeout_ms,
    CircuitBreaker* breaker) {
  HttpRequest req;
  req.url = url;
  req.headers = headers;
  req.form_fields = fields;
  req.form_files = files;
  req.timeout_ms = timeout_ms;
  // 上传失败重试代价高（大文件要重传），只对连接层重试一次
  req.max_retries = 1;
  return perform(req, breaker);
}

// ── 工厂 ────────────────────────────────────────────────
Result<std::unique_ptr<fish::core::IChatModel>> make_chat(const ChatConfig& cfg) {
  // "workbuddy" 是 "openai" 的别名：桥接层就是 OpenAI 兼容协议。
  // 控制面预设展开应该总是写 openai，但配置文件是用户手可编辑的，
  // 写了别名也要能跑，不能因为一个名字让服务起不来。
  std::string dialect_provider = cfg.provider;
  if (dialect_provider == "workbuddy") dialect_provider = "openai";
  static const std::set<std::string> kOk{"deepseek", "workbuddy", "openai"};
  if (!kOk.count(dialect_provider)) {
    return Result<std::unique_ptr<fish::core::IChatModel>>::fail(
        ErrorCode::kUnsupported,
        "unknown chat provider '" + cfg.provider + "'. supported: " + chat_provider_list());
  }
  if (cfg.api_key.empty()) {
    return Result<std::unique_ptr<fish::core::IChatModel>>::fail(ErrorCode::kUnauthorized,
                                                                 "chat api_key empty");
  }
  return Result<std::unique_ptr<fish::core::IChatModel>>::ok(std::make_unique<HttpChat>(
      cfg, dialect_of(dialect_provider), &breaker_for(cfg.provider, "chat")));
}

Result<std::unique_ptr<fish::core::IEmbeddingModel>> make_embedding(const EmbeddingConfig& cfg) {
  static const std::set<std::string> kOk{"siliconflow", "ark", "dashscope", "qianfan",
                                         "workbuddy"};
  if (!kOk.count(cfg.provider)) {
    return Result<std::unique_ptr<fish::core::IEmbeddingModel>>::fail(
        ErrorCode::kUnsupported, "unknown embedding provider '" + cfg.provider +
                                     "'. supported: " + embedding_provider_list());
  }
  if (cfg.api_key.empty() && cfg.provider != "workbuddy") {
    return Result<std::unique_ptr<fish::core::IEmbeddingModel>>::fail(
        ErrorCode::kUnauthorized, "embedding api_key empty");
  }
  if (cfg.provider == "workbuddy") {
    // 桥接层不校验 key；地址默认本机桥
    EmbeddingConfig d = cfg;
    if (d.base_url.empty()) d.base_url = "http://127.0.0.1:18091/v1";
    return Result<std::unique_ptr<fish::core::IEmbeddingModel>>::ok(
        std::make_unique<HttpEmbedding>(d, &breaker_for(cfg.provider, "embedding")));
  }
  return Result<std::unique_ptr<fish::core::IEmbeddingModel>>::ok(
      std::make_unique<HttpEmbedding>(cfg, &breaker_for(cfg.provider, "embedding")));
}

Result<std::unique_ptr<fish::core::ISpeechToText>> make_stt(const SttConfig& cfg) {
  static const std::set<std::string> kOk{"siliconflow", "volcengine", "dashscope", "deepseek"};
  if (!kOk.count(cfg.provider)) {
    return Result<std::unique_ptr<fish::core::ISpeechToText>>::fail(
        ErrorCode::kUnsupported, "unknown stt provider '" + cfg.provider +
                                     "'. supported: " + stt_provider_list());
  }
  if (cfg.api_key.empty()) {
    return Result<std::unique_ptr<fish::core::ISpeechToText>>::fail(
        ErrorCode::kUnauthorized, "stt api_key empty");
  }
  if (cfg.provider == "deepseek") {
    // 官方 ASR 的硬约束直接在这里套默认值，配置里不用记：
    // 端点是 /v1/tools/asr（不是 OpenAI 的 /audio/transcriptions）；
    // 只收 16bit PCM WAV / Ogg Opus；≤8MiB ≤120s。
    SttConfig d = cfg;
    if (d.base_url.empty()) d.base_url = "https://api.deepseek.com";
    if (d.path.empty()) d.path = "/v1/tools/asr";
    d.need_wav16k = true;
    if (d.max_upload_bytes == 0) d.max_upload_bytes = 8u << 20;
    return Result<std::unique_ptr<fish::core::ISpeechToText>>::ok(
        std::make_unique<HttpStt>(d, &breaker_for(cfg.provider, "stt")));
  }
  return Result<std::unique_ptr<fish::core::ISpeechToText>>::ok(
      std::make_unique<HttpStt>(cfg, &breaker_for(cfg.provider, "stt")));
}

Result<std::unique_ptr<fish::core::ITextToSpeech>> make_tts(const TtsConfig& cfg) {
  static const std::set<std::string> kOk{"edge", "volcengine", "siliconflow", "dashscope", "deepseek"};
  if (!kOk.count(cfg.provider)) {
    return Result<std::unique_ptr<fish::core::ITextToSpeech>>::fail(
        ErrorCode::kUnsupported, "unknown tts provider '" + cfg.provider +
                                     "'. supported: " + tts_provider_list());
  }
  if (cfg.provider == "edge") {
    return Result<std::unique_ptr<fish::core::ITextToSpeech>>::ok(
        std::make_unique<EdgeTts>(cfg));
  }
  if (cfg.provider == "volcengine") {
    return Result<std::unique_ptr<fish::core::ITextToSpeech>>::ok(
        std::make_unique<VolcengineTts>(cfg, &breaker_for(cfg.provider, "tts")));
  }
  if (cfg.provider == "deepseek") {
    return Result<std::unique_ptr<fish::core::ITextToSpeech>>::ok(
        std::make_unique<DeepseekTts>(cfg, &breaker_for(cfg.provider, "tts")));
  }
  return Result<std::unique_ptr<fish::core::ITextToSpeech>>::ok(
      std::make_unique<HttpTts>(cfg, &breaker_for(cfg.provider, "tts")));
}

Result<std::unique_ptr<fish::core::IWebSearch>> make_search(const SearchConfig& cfg) {
  // provider 只决定「用哪家的协议发请求」，不限制接什么后端。
  //
  // 白名单必须与 search_provider_list() 完全一致：直接 split 那份清单，
  // 不再手写第二份集合。手写的那份漏掉了 brave/searxng/google/jina/...，
  // 结果这些分支全是死代码，用户配了 provider = "brave" 直接启动失败。
  static const std::set<std::string> kOk = [] {
    std::set<std::string> s;
    std::stringstream ss(search_provider_list());
    std::string item;
    while (std::getline(ss, item, ',')) {
      if (!item.empty()) s.insert(item);
    }
    return s;
  }();
  if (!kOk.count(cfg.provider)) {
    return Result<std::unique_ptr<fish::core::IWebSearch>>::fail(
        ErrorCode::kUnsupported, "unknown search provider '" + cfg.provider +
                                     "'. supported: " + search_provider_list());
  }
  if (cfg.provider == "none") {
    return Result<std::unique_ptr<fish::core::IWebSearch>>::ok(std::make_unique<NullSearch>());
  }
  // DeepSeek 官方 Search 有独立端点，其余走聚合实现
  if (cfg.provider == "deepseek") {
    return Result<std::unique_ptr<fish::core::IWebSearch>>::ok(
        std::make_unique<DeepSeekSearch>(cfg));
  }
  return Result<std::unique_ptr<fish::core::IWebSearch>>::ok(
      std::make_unique<ProviderSearch>(cfg));
}

Result<std::unique_ptr<fish::core::IWebFetch>> make_fetch(const SearchConfig& cfg) {
  if (cfg.provider == "none") {
    return Result<std::unique_ptr<fish::core::IWebFetch>>::fail(
        ErrorCode::kUnsupported, "fetch_provider_none");
  }
  if (cfg.provider == "generic") {
    // 通用抓取：我们直连目标站点，自己做 SSRF 防护
    return Result<std::unique_ptr<fish::core::IWebFetch>>::ok(
        std::make_unique<GenericFetch>(cfg));
  }
  // deepseek / bocha / tavily 走 DeepSeek 官方 Fetch（服务端执行，无 SSRF 风险）
  return Result<std::unique_ptr<fish::core::IWebFetch>>::ok(
      std::make_unique<DeepSeekFetch>(cfg));
}

Result<std::unique_ptr<fish::core::IImageModel>> make_image(const ImageConfig& cfg) {
  static const std::set<std::string> kOk{"none", "siliconflow", "openai", "bridge"};
  if (!kOk.count(cfg.provider)) {
    return Result<std::unique_ptr<fish::core::IImageModel>>::fail(
        ErrorCode::kUnsupported, "unknown image provider '" + cfg.provider +
                                     "'. supported: " + image_provider_list());
  }
  if (cfg.provider == "none") {
    return Result<std::unique_ptr<fish::core::IImageModel>>::fail(
        ErrorCode::kUnsupported, "image_provider_none");
  }
  return Result<std::unique_ptr<fish::core::IImageModel>>::ok(
      std::make_unique<HttpImage>(cfg, &breaker_for(cfg.provider, "image")));
}

// ── 配置读取 ────────────────────────────────────────────

// 环境变量名 → 提供方的映射。
//
// 为什么要这张表：config.toml 里密钥通常写成 ${SILICONFLOW_API_KEY} 这种
// 占位符，Config::load 会把它**展开成真值**。展开之后就看不出这把 key
// 属于哪个厂商了，于是「自己的 api_key 非空」永远命中第一步，
// 用户在 GUI 把 stt.provider 从 siliconflow 切到 deepseek 之后，
// 运行时仍然拿硅基流动的 key 去请求 DeepSeek → 401。
//
// 约定：${VAR} 形式表示「该 key 只在 provider 等于 VAR 名对应的厂商时
// 有效」。解析时取出 VAR 名，映射出厂商；映射出的厂商 ≠ 当前 provider
// 时就跳过自己的 key，继续往下找共享 key。
//
// 用「子串匹配」而不是精确表：变量名由控制面按 section.key 生成
// （见 fish.control::set_secret），也可能是用户手写的，形态很多
// （SILICONFLOW_API_KEY / SILICONFLOW_KEY / ARK_API_KEY /
// VOLCENGINE_TTS_TOKEN ...）。子串匹配对这些都成立。
struct EnvVarProviderRule {
  const char* needle;    // 变量名里出现这个片段
  const char* provider;  // 对应的 provider id
};

// 顺序有意义：先匹配到的赢。
//
// "ARK" 归到 ark（火山方舟的官方叫法），"VOLC" 也归到 volcengine
// （火山引擎的 TTS/STT 用 VOLCENGINE_* 前缀）。两个厂商在配置里是
// 不同的 provider 值，但 VOLCENGINE_API_KEY 这种变量名同时含
// "VOLC" 和 "ARK" 吗？不含——"VOLCENGINE" 里没有 "ARK" 子串，
// 所以不会撞。真正会撞的是 "VOLCENGINE_ARK_KEY" 这种少见写法，
// 那时按先匹配到的 volcengine 算。
constexpr EnvVarProviderRule kEnvVarProviders[] = {
    {"SILICONFLOW", "siliconflow"},
    {"DEEPSEEK", "deepseek"},
    {"DASHSCOPE", "dashscope"},
    {"QIANFAN", "qianfan"},
    {"BAIDU", "qianfan"},
    {"VOLC", "volcengine"},
    {"ARK", "ark"},
    {"GEMINI", "gemini"},
    {"GOOGLE", "gemini"},
    {"JINA", "jina"},
    {"VOYAGE", "voyage"},
    {"MISTRAL", "mistral"},
    {"COHERE", "cohere"},
    {"ZHIPU", "zhipu"},
    {"HUNYUAN", "hunyuan"},
    {"TENCENT", "hunyuan"},
    {"GROQ", "groq"},
    {"OPENAI", "openai"},
};

// 从环境变量名反推它绑定的提供方。映射不到返回空串。
//
// 只用于判断「这把 key 是不是别的厂商的」；映射不到（变量名不含任何
// 已知厂商关键词）时返回空串 = 不做限制 = 按原来用自己的。
std::string provider_of_env_var(const std::string& var_name) {
  std::string upper = var_name;
  for (auto& c : upper) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  for (const auto& rule : kEnvVarProviders) {
    if (upper.find(rule.needle) != std::string::npos) return rule.provider;
  }
  return {};
}

// 某个能力 section 实际对应的厂商。
//
// 为什么不能直接读 provider 字段：capability.chat 的 provider 存的是
// **接入方式**（dialect），OpenAI 兼容端点一律写 "openai"（见
// fish.control 的 kChatDialect）。厂商要从 base_url 认：base_url 指向
// api.deepseek.com 就是 DeepSeek 官方。
//
// 这个区分很关键：「只在对话里填了 DeepSeek key，文档/搜索选 DeepSeek
// 官方」是文档解析静默不启用的根因 —— 按 provider 字面值比对永远匹配
// 不上 chat 那一段。
std::string section_vendor(const fish::config::Config& c,
                           const std::string& section) {
  const std::string p = c.get_or(section + ".provider", "");
  if (section == "capability.chat") {
    std::string base = c.get_or(section + ".base_url", "");
    for (auto& ch : base) {
      ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    if (base.find("deepseek") != std::string::npos) return "deepseek";
    if (p == "workbuddy") return "workbuddy";
    return p;
  }
  return p;
}

// 共享密钥解析：同一提供方在任一能力里填过密钥，其他能力选它时直接复用。
//
// 为什么必须在运行时做（而不是只在控制面 GUI 做）：
//   ControlService::shared_api_key() 只用来决定「要不要隐藏 key 输入框」，
//   运行时工厂不复用它。于是 STT 从 siliconflow 切到 deepseek 后，
//   load_stt_config 读到的还是 capability.stt.api_key（config.toml 里
//   绑死 ${SILICONFLOW_API_KEY}），拿硅基流动的 key 去请求 DeepSeek → 401。
//
// 解析顺序（与控制面一致）：
//   1. 自己 section 的 api_key
//      —— 但如果它是 ${VAR} 形式且 VAR 绑的是**别的**厂商，就跳过
//      （这是「切换 provider 后仍然失效」的根因，见上）
//   2. 其他能力 section 里 provider 相同且 api_key 非空的那个
//      —— 同样要检查 ${VAR} 绑定
//   3. DEEPSEEK_API_KEY 环境变量（仅 provider == "deepseek"）
//
// 注意：
//   - Config::get_or 已经处理了 ${VAR} 展开（见 fish.config.cppm），
//     所以这里读出来就是解析后的值，不需要再展开一次。
//     「是不是 ${VAR} 形式」要问 Config::env_var_of()（拿展开前的原值）。
//   - 本函数只读 config，不调任何 load_*，不会有循环。
//   - workbuddy 是桥接层，不校验 key，直接返回空（与控制面一致）。
std::string resolve_provider_key(const fish::config::Config& c,
                                 const std::string& current_section,
                                 const std::string& provider) {
  if (provider.empty() || provider == "workbuddy") return {};
  // "none" 是「不启用」，不该从别处借一把 key 过来（借了也没人用，
  // 但会让日志/控制面显示「key 已就绪」，误导用户）。
  if (provider == "none") return {};

  // 判断某个 section 的 api_key 能不能用在自己身上：
  //   - 明文 key（用户手填的）→ 始终用自己的
  //   - ${VAR} 且 VAR 绑的厂商 == 当前 provider → 用自己的
  //   - ${VAR} 且 VAR 绑的厂商 != 当前 provider → 跳过（是别人的 key）
  //   - ${VAR} 但变量名映射不出厂商 → 用自己的（不猜）
  auto usable = [&](const std::string& section) -> std::string {
    const std::string key = c.get_or(section + ".api_key", "");
    if (key.empty()) return {};
    const auto var = c.env_var_of(section + ".api_key");
    if (!var.has_value()) return key;   // 明文 → 始终用自己的
    const std::string owner = provider_of_env_var(*var);
    if (owner.empty() || owner == provider) return key;
    // 这把 key 绑的是别的厂商 → 不能用
    ::fish::log::log_line(::fish::log::LogLevel::kInfo, "capability",
                          "api_key_bound_to_other_provider")
        .field("section", section)
        .field("env_var", *var)
        .field("key_owner", owner)
        .field("current_provider", provider);
    return {};
  };

  // 1. 自己的
  if (std::string mine = usable(current_section); !mine.empty()) {
    return mine;
  }

  // 2. 同提供方的其他能力
  static constexpr const char* kKeySections[] = {
      "capability.chat",     "capability.search", "capability.document",
      "capability.embedding", "capability.stt",   "capability.tts",
  };
  for (const char* sec : kKeySections) {
    if (current_section == sec) continue;
    const std::string other = sec;
    // 用 section_vendor 而不是裸 provider：chat 的 provider 是 dialect
    // （"openai"），厂商要从 base_url 认，否则永远匹配不上。
    if (section_vendor(c, other) != provider) continue;
    if (std::string k = usable(other); !k.empty()) return k;
  }

  // 3. 官方专用环境变量兜底：很多部署只放这一把
  if (provider == "deepseek") {
    if (const char* e = std::getenv("DEEPSEEK_API_KEY"); e != nullptr && *e != '\0') {
      return e;
    }
  }
  return {};
}

// 导出版本（供 fish.doc 使用）。名字加 _for 后缀，避免与模块内的
// 私有实现同名冲突（C++ 模块里同名会歧义）。
std::string resolve_provider_key_for(const fish::config::Config& c,
                                     const std::string& current_section,
                                     const std::string& provider) {
  return resolve_provider_key(c, current_section, provider);
}

ChatConfig load_chat_config(const fish::config::Config& c) {
  ChatConfig cfg;
  cfg.provider = c.get_or("capability.chat.provider", "deepseek");
  cfg.base_url = c.get_or("capability.chat.base_url", "https://api.deepseek.com");
  cfg.api_key = c.get_or("capability.chat.api_key", "");
  cfg.model = c.get_or("capability.chat.model", "deepseek-flash");
  // 0 / 负数 = 不发该字段，用上游默认值
  cfg.max_tokens = static_cast<int>(c.get_int_or("capability.chat.max_tokens", 0));
  cfg.temperature = c.get_double_or("capability.chat.temperature", -1.0);
  // 思考强度**锁死 high**：产品决定，控制面 PUT 已拒绝修改这个字段
  // （fish.control.cppm 返回 effort_locked:high）。这里只是兜底默认值：
  // 出厂 config.toml 没有这个键，早期默认空串导致该字段从未发出去。
  // 配置里手写别的合法档位仍会被发送，非法档位在 HttpChat 里退回 high。
  // 默认值取自 locked_reasoning_effort()，与控制面回显同一个来源。
  cfg.reasoning_effort =
      c.get_or("capability.chat.reasoning_effort", locked_reasoning_effort());
  cfg.timeout_ms = static_cast<int>(c.get_int_or("capability.chat.timeout_ms", 30000));
  // DeepSeek 服务端联网工具：默认开（default_deepseek_web_tools）。
  // 真正生效还要端点是 DeepSeek 官方——判据见 server_web_tools_active()。
  cfg.deepseek_web_tools = c.get_bool_or("capability.chat.deepseek_web_tools",
                                         default_deepseek_web_tools());
  // 开关开着但端点不是 DeepSeek 官方时，明确说一声。
  //
  // 不说的话，用户以为「服务端联网搜索已开启」，实际搜索走的是本地
  // 工具（端点非官方时服务端工具根本不生效）——两套路径的计费、引用、
  // 行为都不同，用户无从判断自己用的是哪一套。
  if (cfg.deepseek_web_tools && !is_deepseek_endpoint(cfg.provider, cfg.base_url)) {
    ::fish::log::log_line(::fish::log::LogLevel::kInfo, "capability",
                          "deepseek_web_tools_inactive")
        .field("provider", cfg.provider)
        .field("base_url", cfg.base_url)
        .field("hint",
               "端点非 DeepSeek 官方，服务端联网工具不生效，使用本地搜索");
  }
  // supports_assistant_image 不在这里读配置：
  // 由 fish.probe 探测后由 main 注入，避免配置过时。
  return cfg;
}

EmbeddingConfig load_embedding_config(const fish::config::Config& c) {
  EmbeddingConfig cfg;
  cfg.provider = c.get_or("capability.embedding.provider", "siliconflow");
  cfg.base_url = c.get_or("capability.embedding.base_url", "https://api.siliconflow.cn/v1");
  // 自己配了就用自己，否则复用同提供方其他能力填过的 key
  cfg.api_key = resolve_provider_key(c, "capability.embedding", cfg.provider);
  cfg.model = c.get_or("capability.embedding.model", "BAAI/bge-m3");
  cfg.dimension = static_cast<int>(c.get_int_or("capability.embedding.dimension", 1024));
  cfg.path = c.get_or("capability.embedding.path", "");
  cfg.dimensions_field = c.get_or("capability.embedding.dimensions_field", "");
  cfg.extra_json = c.get_or("capability.embedding.extra_json", "");
  cfg.timeout_ms = static_cast<int>(c.get_int_or("capability.embedding.timeout_ms", 20000));
  return cfg;
}

SttConfig load_stt_config(const fish::config::Config& c) {
  SttConfig cfg;
  cfg.provider = c.get_or("capability.stt.provider", "siliconflow");
  cfg.base_url = c.get_or("capability.stt.base_url", "https://api.siliconflow.cn/v1");
  // 从 siliconflow 切到 deepseek 时，config.toml 里的 capability.stt.api_key
  // 往往还是绑死的 ${SILICONFLOW_API_KEY}，直接读会 401。
  // 复用同提供方（这里指 deepseek）在 chat/search/document 填过的 key。
  cfg.api_key = resolve_provider_key(c, "capability.stt", cfg.provider);
  cfg.model = c.get_or("capability.stt.model", "FunAudioLLM/SenseVoiceSmall");
  cfg.language = c.get_or("capability.stt.language", "auto");
  cfg.path = c.get_or("capability.stt.path", "");
  cfg.need_wav16k = c.get_bool_or("capability.stt.need_wav16k", false);
  cfg.max_upload_bytes = static_cast<std::size_t>(
      c.get_int_or("capability.stt.max_upload_bytes", 0));
  cfg.timeout_ms = static_cast<int>(c.get_int_or("capability.stt.timeout_ms", 30000));
  return cfg;
}

TtsConfig load_tts_config(const fish::config::Config& c) {
  TtsConfig cfg;
  cfg.provider = c.get_or("capability.tts.provider", "edge");
  cfg.base_url = c.get_or("capability.tts.base_url", "");
  // 同 STT：edge / volcengine 不需要 key，其余厂商复用同提供方的 key
  cfg.api_key = resolve_provider_key(c, "capability.tts", cfg.provider);
  cfg.voice = c.get_or("capability.tts.voice", "zh-CN-XiaoyiNeural");
  cfg.format = c.get_or("capability.tts.format", "mp3");
  cfg.model = c.get_or("capability.tts.model", "");
  cfg.path = c.get_or("capability.tts.path", "");
  cfg.app_id = c.get_or("capability.tts.app_id", "");
  cfg.cluster = c.get_or("capability.tts.cluster", "");
  cfg.extra_json = c.get_or("capability.tts.extra_json", "");
  cfg.timeout_ms = static_cast<int>(c.get_int_or("capability.tts.timeout_ms", 20000));
  return cfg;
}

SearchConfig load_search_config(const fish::config::Config& c) {
  SearchConfig cfg;
  cfg.provider = c.get_or("capability.search.provider", "none");
  cfg.base_url = c.get_or("capability.search.base_url", "");
  cfg.path = c.get_or("capability.search.path", "");
  // 搜索选 DeepSeek 官方时，key 通常只在 chat 里填过——复用
  cfg.api_key = resolve_provider_key(c, "capability.search", cfg.provider);
  cfg.api_engine = c.get_or("capability.search.api_engine", "");
  cfg.max_results = static_cast<int>(c.get_int_or("capability.search.max_results", 5));
  cfg.timeout_ms = static_cast<int>(c.get_int_or("capability.search.timeout_ms", 15000));
  cfg.search_need_content =
      c.get_bool_or("capability.search.need_content", false);
  cfg.fetch_max_chars =
      static_cast<std::size_t>(c.get_int_or("capability.search.fetch_max_chars", 6000));
  cfg.allow_private_hosts =
      c.get_bool_or("capability.search.allow_private_hosts", false);
  cfg.fetch_max_bytes = static_cast<std::size_t>(
      c.get_int_or("capability.search.fetch_max_bytes", 2 * 1024 * 1024));
  return cfg;
}

ImageConfig load_image_config(const fish::config::Config& c) {
  ImageConfig cfg;
  cfg.provider = c.get_or("capability.image.provider", "none");
  cfg.base_url = c.get_or("capability.image.base_url", "");
  cfg.api_key = c.get_or("capability.image.api_key", "");
  cfg.model = c.get_or("capability.image.model", "");
  cfg.default_size = c.get_or("capability.image.default_size", "1024x1024");
  cfg.timeout_ms = static_cast<int>(c.get_int_or("capability.image.timeout_ms", 120000));
  return cfg;
}

}  // namespace fish::capability
