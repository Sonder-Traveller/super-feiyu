// src/fish.tool.cppm
// 工具调用：让模型能主动查东西（网络搜索、本地图片、记忆检索）。
//
// 设计要点：
//   - 工具是「能力」，不是「功能」：接口统一，换实现不影响调用方
//   - 每个工具自己声明 JSON Schema，模型据此决定何时调用
//   - 执行有超时和结果大小上限，防止一个工具拖死整个对话
//   - 工具失败不中断对话：把错误作为 tool 结果返回，模型自行决定怎么说
//
// 安全：
//   - 工具只能访问白名单内的资源（见 fish.guard）
//   - 结果会截断，避免注入超长内容污染上下文
module;

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

export module fish.tool;

import fish.core;
import fish.log;

export namespace fish::tool {

// 一次工具调用的请求
struct Call {
  std::string name;
  // 模型给的参数（JSON 字符串，原样保存便于日志）
  std::string arguments_json;
};

// 工具执行结果
struct Outcome {
  bool ok{false};
  // 给模型看的内容（纯文本，会被截断）
  std::string content;
  // 失败原因（仅日志用，不直接给模型）
  std::string error;
  // 要随回复一起发出去的本地图片路径（可为空）。
  //
  // 为什么加这个：search_images 找到了图，但结果只是一行文本路径，
  // 模型会把路径当正文念出来（「喏，`data/memory/portraits/x.png`」），
  // 用户看到的是路径字符串而不是图片。
  // 现在工具可以把「这张图要发出去」明确告诉管线，
  // 由管线负责真正上传，模型只负责说话。
  std::vector<std::string> image_paths;
  // 要随回复一起发出去的本地语音文件路径。
  // send_voice 工具用：TTS 合成落盘后把路径交给管线，
  // 管线按通道能力决定是否真的能发（微信/QQ 已支持 mp3）。
  std::vector<std::string> voice_paths;
};

// 工具定义
class ITool {
 public:
  virtual ~ITool() = default;
  [[nodiscard]] virtual std::string name() const = 0;
  [[nodiscard]] virtual std::string description() const = 0;
  // JSON Schema（object 类型），供模型判断参数
  [[nodiscard]] virtual std::string parameters_schema() const = 0;
  virtual Outcome run(const Call& call) = 0;
};

// 工具注册表：按名字查找并执行
class Registry {
 public:
  void add(std::shared_ptr<ITool> tool);
  [[nodiscard]] std::shared_ptr<ITool> find(const std::string& name) const;
  [[nodiscard]] bool empty() const;
  [[nodiscard]] std::size_t size() const;
  // 生成 OpenAI tools 数组（空注册表返回空数组）
  [[nodiscard]] std::string openai_tools_json() const;
  Outcome run(const Call& call);

 private:
  mutable std::mutex mu_;
  std::map<std::string, std::shared_ptr<ITool>> tools_;
};

// ── 内置工具 ────────────────────────────────────────────

// 记忆检索：在长期记忆（facts/episodes）里搜相关内容
class MemorySearchTool final : public ITool {
 public:
  // search 回调由外部注入（避免 tool 直接依赖 memory 模块）
  using Searcher = std::function<std::vector<std::string>(const std::string& query, int top_k)>;
  explicit MemorySearchTool(Searcher s) : search_(std::move(s)) {}
  [[nodiscard]] std::string name() const override { return "search_memory"; }
  [[nodiscard]] std::string description() const override {
    return "在长期记忆里检索与查询相关的事实和过往对话。"
           "当对方提到很久以前的事、或你需要回忆背景时使用。";
  }
  [[nodiscard]] std::string parameters_schema() const override;
  Outcome run(const Call& call) override;

 private:
  Searcher search_;
};

// 本地图片检索：在形象库里按标签/描述找图
class ImageSearchTool final : public ITool {
 public:
  // 一条命中的图片：路径 + 说明
  struct Hit {
    std::string path;
    std::string caption;
  };
  using Searcher = std::function<std::vector<Hit>(const std::string& query)>;
  explicit ImageSearchTool(Searcher s) : search_(std::move(s)) {}

  // 一次最多发几张。形象库是本地文件，一次发太多会刷屏，
  // 也容易被平台判成异常。
  static constexpr int kMaxSend = 1;

  [[nodiscard]] std::string name() const override { return "send_image"; }
  [[nodiscard]] std::string description() const override {
    return "发一张你自己的图片给对方。在自己的形象库里按场景/标签找，"
           "找到的图会随你的回复一起发出。适合对方想看你、或聊到相关场景时用。";
  }
  [[nodiscard]] std::string parameters_schema() const override;
  Outcome run(const Call& call) override;

 private:
  Searcher search_;
};

// 语音发送：把一段文字合成语音发给对方
//
// 为什么做成工具而不是 [VOICE] 文本标记：
//   标记方案要求模型在正文里嵌特殊字符串，管线再解析剥离——
//   模型有时会把标记当台词抄出来（实测气泡里出现字面 "[VOICE]"），
//   还要记忆里额外处理。工具调用是结构化的，参数即内容，
//   成功/失败有明确反馈，记忆侧也干净。
class SendVoiceTool final : public ITool {
 public:
  // TTS 合成回调：文本 → mp3 本地路径。由 main 注入（tool 不依赖 capability）。
  using Synthesizer = std::function<std::string(const std::string& text)>;
  explicit SendVoiceTool(Synthesizer s) : synth_(std::move(s)) {}

  [[nodiscard]] std::string name() const override { return "send_voice"; }
  [[nodiscard]] std::string description() const override {
    return "把一句话用你的声音发给对方。适合语气需要、对方在用语音聊天、"
           "或唱一句歌词的场合。参数就是你要念出来的话，"
           "会用你自己的说话风格念。不要长篇大论，一两句为宜。";
  }
  [[nodiscard]] std::string parameters_schema() const override;
  Outcome run(const Call& call) override;

 private:
  Synthesizer synth_;
};

// 网络搜索：查外部信息
class WebSearchTool final : public ITool {
 public:
  struct Hit {
    std::string title;
    std::string url;
    std::string snippet;
  };
  using Searcher = std::function<std::vector<Hit>(const std::string& query, int max_results)>;
  explicit WebSearchTool(Searcher s) : search_(std::move(s)) {}
  [[nodiscard]] std::string name() const override { return "web_search"; }
  [[nodiscard]] std::string description() const override {
    return "搜索互联网获取信息。适用于：近期事件、你不确定的事实、需要外部资料。\n"
           "提高结果质量的技巧：\n"
           "- 优先搜原始出处（官方文档、当事人账号），而不是转述文章\n"
           "- 加 site: 限定可信站点，如 site:github.com、site:gov.cn\n"
           "- 加 after:2026 限定时间，避免搜到过时内容\n"
           "- 用「原文」「出处」「官方」这类词找一手来源\n"
           "注意：搜索结果里混着大量营销号和 AI 生成内容，"
           "多条结果说法一致也可能是因为互相转载。拿到结果后要自己判断可信度。";
  }
  [[nodiscard]] std::string parameters_schema() const override;
  Outcome run(const Call& call) override;

 private:
  Searcher search_;
};

// 网页抓取：把 URL 的内容取回来（Markdown 正文）
class WebFetchTool final : public ITool {
 public:
  struct Page {
    std::string url;
    std::string title;
    std::string content;
  };
  using Fetcher = std::function<Page(const std::string& url)>;
  explicit WebFetchTool(Fetcher f) : fetch_(std::move(f)) {}
  [[nodiscard]] std::string name() const override { return "web_fetch"; }
  [[nodiscard]] std::string description() const override {
    return "抓取指定网页的正文内容（Markdown）。"
           "当 web_search 找到链接、或对方给了链接需要你读内容时使用。";
  }
  [[nodiscard]] std::string parameters_schema() const override;
  Outcome run(const Call& call) override;

 private:
  Fetcher fetch_;
};

// 网络热梗检索：本地库优先，未命中时可选联网补充。
// 与 web_search 分开：热梗是高频、短小的查询，走本地库零成本；
// 联网只在本地没有时才发生，且结果会写回本地。
class MemeLookupTool final : public ITool {
 public:
  struct Hit {
    std::string term;
    std::string meaning;
    std::string usage;
  };
  // 本地检索（同步、零成本）
  using LocalSearcher = std::function<std::vector<Hit>(const std::string& query)>;
  // 外部补充（可空；为空时纯本地）
  using RemoteSearcher = std::function<std::vector<Hit>(const std::string& query)>;
  // 写回本地库（可空）
  using Importer = std::function<int(const std::vector<Hit>& hits)>;

  MemeLookupTool(LocalSearcher local, RemoteSearcher remote, Importer importer)
      : local_(std::move(local)),
        remote_(std::move(remote)),
        importer_(std::move(importer)) {}

  [[nodiscard]] std::string name() const override { return "lookup_meme"; }
  [[nodiscard]] std::string description() const override {
    return "查询网络热梗/网络用语的含义和用法。"
           "当对方用了你不认识的梗、或你想用某个梗但不确定含义时使用。"
           "优先查本地库；本地没有且允许联网时会去搜索并记住。";
  }
  [[nodiscard]] std::string parameters_schema() const override;
  Outcome run(const Call& call) override;

 private:
  LocalSearcher local_;
  RemoteSearcher remote_;
  Importer importer_;
};

}  // namespace fish::tool

// ═══ 实现 ═══════════════════════════════════════════════
module :private;

namespace fish::tool {

using json = nlohmann::json;

namespace detail {

// 搜索结果的来源可信度警示。
//
// 为什么需要：网络搜索结果里混着大量营销号、AI 生成的 SEO 内容、
// 个人自设。实测教训：查"大肥鱼人设"时，多家媒体和百科互相转载同一篇
// 营销号文章，把「傲娇」「叫用户鱼片」这类小范围自设写成通稿，
// 交叉验证看起来"多来源一致"，其实同源。
//
// 所以每次给模型搜索结果，都要附上这段，让它自己判断可信度。
constexpr const char* kSourceWarning =
    "\n\n【关于以上搜索结果的提醒】\n"
    "1. 多条结果说法一致，不代表是对的——营销号会互相转载，"
    "看起来「多来源」其实同源。\n"
    "2. 区分这三类，可信度从高到低：\n"
    "   - 一手事实（官方文档、当事人原话、可核实的原始记录）\n"
    "   - 二手转述（媒体报道、百科词条）—— 可能失真\n"
    "   - 营销号/自媒体/SEO 内容 —— 常把猜测写成定论，不要采信\n"
    "3. 遇到「官方认证」「正统设定」「据说」这类措辞，要警惕。\n"
    "4. 如果搜索结果里有矛盾，如实告诉对方「网上说法不一致」，"
    "不要自己挑一个当成事实。\n"
    "5. 不确定就说不确定。编一个答案被拆穿，比直接说不知道难看得多。";

// 工具结果给模型看之前的截断，避免超长内容污染上下文
constexpr std::size_t kMaxContentBytes = 4000;

std::string truncate(std::string s, std::size_t limit = kMaxContentBytes) {
  if (s.size() <= limit) return s;
  // 按 UTF-8 边界回退，避免截断半个汉字
  std::size_t cut = limit;
  while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
  s.resize(cut);
  s += "\n…（内容过长已截断）";
  return s;
}

// 解析参数，失败返回空对象
json parse_args(const std::string& raw) {
  if (raw.empty()) return json::object();
  try {
    auto j = json::parse(raw);
    return j.is_object() ? j : json::object();
  } catch (...) {
    return json::object();
  }
}

std::string get_str(const json& j, const char* key, std::size_t max_len = 512) {
  if (!j.contains(key) || !j[key].is_string()) return {};
  std::string v = j[key].get<std::string>();
  if (v.size() > max_len) v.resize(max_len);
  return v;
}

}  // namespace detail

// ── Registry ────────────────────────────────────────────

void Registry::add(std::shared_ptr<ITool> tool) {
  if (!tool) return;
  std::lock_guard<std::mutex> lock(mu_);
  tools_[tool->name()] = std::move(tool);
}

std::shared_ptr<ITool> Registry::find(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mu_);
  const auto it = tools_.find(name);
  return it == tools_.end() ? nullptr : it->second;
}

bool Registry::empty() const {
  std::lock_guard<std::mutex> lock(mu_);
  return tools_.empty();
}

std::size_t Registry::size() const {
  std::lock_guard<std::mutex> lock(mu_);
  return tools_.size();
}

std::string Registry::openai_tools_json() const {
  json arr = json::array();
  {
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& [name, tool] : tools_) {
      json params;
      try {
        params = json::parse(tool->parameters_schema());
      } catch (...) {
        params = json{{"type", "object"}, {"properties", json::object()}};
      }
      arr.push_back({
          {"type", "function"},
          {"function",
           {{"name", tool->name()},
            {"description", tool->description()},
            {"parameters", params}}},
      });
    }
  }
  return arr.empty() ? std::string{} : arr.dump();
}

Outcome Registry::run(const Call& call) {
  auto tool = find(call.name);
  if (!tool) {
    Outcome o;
    o.error = "unknown_tool:" + call.name;
    o.content = "没有这个工具：" + call.name;
    return o;
  }
  try {
    return tool->run(call);
  } catch (const std::exception& e) {
    Outcome o;
    o.error = std::string("tool_threw:") + e.what();
    o.content = "工具执行出错。";
    ::fish::log::log_line(::fish::log::LogLevel::kWarn, "tool", "threw")
        .field("tool", call.name)
        .field("detail", e.what());
    return o;
  }
}

// ── MemorySearchTool ────────────────────────────────────

std::string MemorySearchTool::parameters_schema() const {
  return R"({
    "type": "object",
    "properties": {
      "query": {"type": "string", "description": "要检索的关键内容"},
      "top_k": {"type": "integer", "description": "返回条数，默认 5", "minimum": 1, "maximum": 20}
    },
    "required": ["query"]
  })";
}

Outcome MemorySearchTool::run(const Call& call) {
  Outcome out;
  const auto args = detail::parse_args(call.arguments_json);
  const std::string query = detail::get_str(args, "query");
  if (query.empty()) {
    out.error = "missing_query";
    out.content = "需要提供 query 参数。";
    return out;
  }
  int top_k = 5;
  if (args.contains("top_k") && args["top_k"].is_number()) {
    top_k = args["top_k"].get<int>();
    if (top_k < 1) top_k = 1;
    if (top_k > 20) top_k = 20;
  }

  std::vector<std::string> hits;
  if (search_) hits = search_(query, top_k);

  if (hits.empty()) {
    out.ok = true;
    out.content = "记忆里没有找到相关内容。";
    return out;
  }
  std::string s;
  for (std::size_t i = 0; i < hits.size(); ++i) {
    s += std::to_string(i + 1) + ". " + hits[i] + "\n";
  }
  out.ok = true;
  out.content = detail::truncate(std::move(s));
  return out;
}

// ── ImageSearchTool ─────────────────────────────────────

std::string ImageSearchTool::parameters_schema() const {
  return R"({
    "type": "object",
    "properties": {
      "query": {"type": "string", "description": "场景或标签，如「吃东西」「生气」"}
    },
    "required": ["query"]
  })";
}

Outcome ImageSearchTool::run(const Call& call) {
  Outcome out;
  const auto args = detail::parse_args(call.arguments_json);
  const std::string query = detail::get_str(args, "query");
  if (query.empty()) {
    out.error = "missing_query";
    out.content = "需要提供 query 参数。";
    return out;
  }
  std::vector<Hit> hits;
  if (search_) hits = search_(query);

  if (hits.empty()) {
    out.ok = true;
    out.content = "形象库里没有匹配的图片。可以就这样回话，别编造照片内容。";
    return out;
  }

  // 取第一张作为要发出去的图。
  // 工具结果里**不要再给路径**：模型会把路径当正文念出来，
  // 用户看到的是 "data/memory/portraits/xxx.png" 这种字符串。
  // 只告诉它「发了什么」，路径由 image_paths 交给管线处理。
  const auto& pick = hits.front();
  out.image_paths.push_back(pick.path);

  std::string s = "已经找到并发出下面这张图（你不用再描述路径）：\n";
  s += "  图片内容：" + (pick.caption.empty() ? std::string("一张照片") : pick.caption) + "\n";
  s += "  请用一两句自然的话带过，不要写出文件路径，也不要描述你没看到过的画面细节。\n";
  out.ok = true;
  out.content = detail::truncate(std::move(s));
  return out;
}

// ── SendVoiceTool ───────────────────────────────────────

std::string SendVoiceTool::parameters_schema() const {
  return R"({
    "type": "object",
    "properties": {
      "text": {"type": "string", "description": "要念出来的话，一两句以内，保持你的说话风格"}
    },
    "required": ["text"]
  })";
}

Outcome SendVoiceTool::run(const Call& call) {
  Outcome out;
  const auto args = detail::parse_args(call.arguments_json);
  const std::string text = detail::get_str(args, "text");
  if (text.empty()) {
    out.error = "missing_text";
    out.content = "需要提供 text 参数。";
    return out;
  }
  if (text.size() > 300) {
    out.content = "要念的话太长了，语音只适合一两句。请用普通文本回复剩下的内容。";
    out.ok = false;
    return out;
  }

  const std::string path = synth_ ? synth_(text) : std::string();
  if (path.empty()) {
    out.ok = false;
    out.error = "tts_failed";
    out.content = "语音合成失败。请直接用文字回复，不要提语音的事。";
    return out;
  }

  out.voice_paths.push_back(path);
  out.ok = true;
  out.content = "语音已经发出，内容是：「" + text + "」。"
                "接下来可以再用文字补一句，也可以就此打住。";
  return out;
}

// ── WebSearchTool ───────────────────────────────────────

std::string WebSearchTool::parameters_schema() const {
  return R"({
    "type": "object",
    "properties": {
      "query": {"type": "string", "description": "搜索关键词"},
      "max_results": {"type": "integer", "description": "返回条数，默认 5", "minimum": 1, "maximum": 10}
    },
    "required": ["query"]
  })";
}

Outcome WebSearchTool::run(const Call& call) {
  Outcome out;
  const auto args = detail::parse_args(call.arguments_json);
  const std::string query = detail::get_str(args, "query");
  if (query.empty()) {
    out.error = "missing_query";
    out.content = "需要提供 query 参数。";
    return out;
  }
  int n = 5;
  if (args.contains("max_results") && args["max_results"].is_number()) {
    n = args["max_results"].get<int>();
    if (n < 1) n = 1;
    if (n > 10) n = 10;
  }

  std::vector<Hit> hits;
  if (search_) hits = search_(query, n);

  if (hits.empty()) {
    out.ok = true;
    out.content = "没有搜索到结果。";
    return out;
  }
  std::string s;
  for (std::size_t i = 0; i < hits.size(); ++i) {
    s += std::to_string(i + 1) + ". " + hits[i].title;
    if (!hits[i].snippet.empty()) s += "\n   " + hits[i].snippet;
    // 不发链接：QQ 平台会拦截含链接的消息
    if (!hits[i].url.empty()) s += "\n   来源编号 " + std::to_string(i + 1);
    s += "\n";
  }
  // 附来源可信度警示：搜索结果里混着大量营销号，必须让模型自己判断
  s += detail::kSourceWarning;
  out.ok = true;
  out.content = detail::truncate(std::move(s));
  return out;
}

// ── WebFetchTool ────────────────────────────────────────

std::string WebFetchTool::parameters_schema() const {
  return R"({
    "type": "object",
    "properties": {
      "url": {"type": "string", "description": "要抓取的网页地址（http/https）"}
    },
    "required": ["url"]
  })";
}

Outcome WebFetchTool::run(const Call& call) {
  Outcome out;
  const auto args = detail::parse_args(call.arguments_json);
  const std::string url = detail::get_str(args, "url", 8192);
  if (url.empty()) {
    out.error = "missing_url";
    out.content = "需要提供 url 参数。";
    return out;
  }
  if (!fetch_) {
    out.error = "no_fetcher";
    out.content = "抓取功能未启用。";
    return out;
  }

  const auto page = fetch_(url);
  if (page.content.empty()) {
    out.ok = true;
    out.content = "没能取到这个页面的内容。";
    return out;
  }

  std::string s;
  if (!page.title.empty()) s += page.title + "\n\n";
  s += page.content;
  // 抓到的页面同样可能是营销号或 AI 生成内容
  s += detail::kSourceWarning;
  out.ok = true;
  out.content = detail::truncate(std::move(s));
  return out;
}

// ── MemeLookupTool ──────────────────────────────────────
std::string MemeLookupTool::parameters_schema() const {
  return R"({
    "type": "object",
    "properties": {
      "query": {"type": "string", "description": "要查的梗或网络用语"}
    },
    "required": ["query"]
  })";
}

Outcome MemeLookupTool::run(const Call& call) {
  Outcome out;
  const auto args = detail::parse_args(call.arguments_json);
  const std::string query = detail::get_str(args, "query");
  if (query.empty()) {
    out.error = "missing_query";
    out.content = "需要提供 query 参数。";
    return out;
  }

  // 1. 本地优先：热梗库是高频查询，走本地零成本零延迟
  std::vector<Hit> hits;
  if (local_) hits = local_(query);

  std::string source = "本地库";

  // 2. 本地没命中才联网（P12：像语音那样自动降级——
  //    没有 remote 就静默用本地结果，不报错）
  if (hits.empty() && remote_) {
    auto remote_hits = remote_(query);
    if (!remote_hits.empty()) {
      hits = remote_hits;
      source = "联网搜索";
      // 3. 写回本地，下次直接用
      if (importer_) {
        const int added = importer_(hits);
        if (added > 0) {
          ::fish::log::log_line(::fish::log::LogLevel::kInfo, "tool", "meme_learned")
              .field("query", query)
              .field("added", static_cast<long long>(added));
        }
      }
    }
  }

  if (hits.empty()) {
    out.ok = true;
    out.content = "没查到这个词的含义。可以问对方是什么意思。";
    return out;
  }

  std::string s = "（来源：" + source + "）\n";
  for (const auto& h : hits) {
    s += h.term + "：" + h.meaning;
    if (!h.usage.empty()) s += "\n  用法：" + h.usage;
    s += "\n";
  }
  // 热梗领域营销号最多：同一个梗的解释经常被抄来抄去还抄错，
  // 必须提醒模型别把搜索到的说法当成定论。
  if (source == "联网搜索") {
    s += detail::kSourceWarning;
  }
  out.ok = true;
  out.content = detail::truncate(std::move(s));
  return out;
}

}  // namespace fish::tool
