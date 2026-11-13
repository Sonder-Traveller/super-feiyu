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
  using Searcher = std::function<std::vector<std::string>(const std::string& query)>;
  explicit ImageSearchTool(Searcher s) : search_(std::move(s)) {}
  [[nodiscard]] std::string name() const override { return "search_images"; }
  [[nodiscard]] std::string description() const override {
    return "在自己的形象库里按场景/标签查找图片。"
           "当需要发一张符合当前情境的照片时使用。";
  }
  [[nodiscard]] std::string parameters_schema() const override;
  Outcome run(const Call& call) override;

 private:
  Searcher search_;
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
    return "搜索互联网获取最新信息。当对方问及近期事件、"
           "你不确定的事实、或需要外部资料时使用。";
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
  std::vector<std::string> hits;
  if (search_) hits = search_(query);

  if (hits.empty()) {
    out.ok = true;
    out.content = "形象库里没有匹配的图片。";
    return out;
  }
  std::string s = "找到以下图片（路径 | 说明）：\n";
  for (const auto& h : hits) s += "  " + h + "\n";
  out.ok = true;
  out.content = detail::truncate(std::move(s));
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
  out.ok = true;
  out.content = detail::truncate(std::move(s));
  return out;
}

}  // namespace fish::tool
