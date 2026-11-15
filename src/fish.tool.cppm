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
import fish.meme;

export namespace fish::tool {

// 一次工具调用的请求
struct Call {
  std::string name;
  // 模型给的参数（JSON 字符串，原样保存便于日志）
  std::string arguments_json;
  // 当前会话键（编排层填）。
  //
  // 为什么需要：有些工具要把「学到的东西」写进**这个会话**的记忆
  // （比如 lookup_meme 学到新梗 → 记进当前群的 traits/facts）。
  // 工具自己不知道自己在跟谁说话，所以由调用方告知。
  std::string conversation_id;
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
  [[nodiscard]] std::string openai_tools_json(
      const std::function<bool(const std::string&)>& filter = {}) const;
  Outcome run(const Call& call);

 private:
  mutable std::mutex mu_;
  std::map<std::string, std::shared_ptr<ITool>> tools_;
};

// ── 内置工具 ────────────────────────────────────────────

// 记忆检索：在长期记忆（facts/episodes）里搜相关内容
class MemorySearchTool final : public ITool {
 public:
  // search 回调由外部注入（避免 tool 直接依赖 memory 模块）。
  //
  // 第一个参数是**会话键**（来自 Call::conversation_id，编排层填）。
  // 长期记忆的索引按会话隔离——跨会话召回是隐私泄漏（A 群的事实
  // 不该被 B 群搜到），所以检索必须知道自己在哪个会话里。
  // 空表示拿不到会话上下文，检索层会返回空结果而不是跨会话乱捞。
  using Searcher = std::function<std::vector<std::string>(
      const std::string& conv_id, const std::string& query, int top_k)>;

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
  // 形象库里的一张图：路径 + 说明（说明就是给模型看的"内容是什么"）
  struct Hit {
    std::string path;
    std::string caption;
  };
  // 列出形象库全部图片。不做关键词匹配——直接把清单交给模型挑。
  using Lister = std::function<std::vector<Hit>()>;

  explicit ImageSearchTool(Lister l) : list_(std::move(l)) {}

  [[nodiscard]] std::string name() const override { return "send_image"; }
  [[nodiscard]] std::string description() const override {
    return "查看并发送你自己的形象库图片。\n"
           "第一次调用不要带 index：会返回**全部图片的清单**（序号 + 每张的内容说明）。\n"
           "然后你读清单，挑出最贴合当前对话的一张，再带 index 调一次发出去。\n"
           "注意：没有「搜索」这回事，每次不带 index 都返回完整清单，"
           "所以不要反复用不同的词试探——清单里就是全部可用的图。";
  }
  [[nodiscard]] std::string parameters_schema() const override;
  Outcome run(const Call& call) override;

 private:
  Lister list_;
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

// 网络热梗检索：本地库优先，未命中时联网学并记住。
// 与 web_search 分开：热梗是高频、短小的查询，走本地库零成本；
// 联网只在本地没有时才发生，且结果会写回本地 + 记进当前会话记忆。
class MemeLookupTool final : public ITool {
 public:
  struct Hit {
    std::string term;     // 梗名
    std::string meaning;  // 释义
    std::string usage;    // 用法示例
    std::string source;   // 来源：本地库 / provider 名（写回时存进 origin）
  };
  // 本地检索（同步、零成本）
  using LocalSearcher = std::function<std::vector<Hit>(const std::string& query)>;
  // 外部补充。为空 = 纯本地模式（不联网）。
  //
  // error 出参：调用失败时写入原因（网络错、401、空结果等）。
  // 空字符串 = 调用成功（哪怕一条结果都没有）。
  // 这样「provider 没启用」和「搜了但没搜到」在给模型的文案里能分开说。
  using RemoteSearcher =
      std::function<std::vector<Hit>(const std::string& query, std::string* error)>;
  // 写回本地库（可空）。返回实际新增条数。
  using Importer = std::function<int(const std::vector<Hit>& hits)>;
  // 把学到的梗记进当前会话的长期记忆（可空）。
  // conv_id 为空（没带会话键）时不调用。
  using MemoryWriter = std::function<void(const std::string& conv_id,
                                          const std::string& term,
                                          const std::string& meaning)>;

  MemeLookupTool(LocalSearcher local, RemoteSearcher remote, Importer importer,
                 MemoryWriter memory = {})
      : local_(std::move(local)),
        remote_(std::move(remote)),
        importer_(std::move(importer)),
        memory_(std::move(memory)) {}

  [[nodiscard]] std::string name() const override { return "lookup_meme"; }
  [[nodiscard]] std::string description() const override {
    return "查网络热梗 / 网络用语 / 缩写 / 谐音梗 / 黑话是什么意思、怎么用。\n"
           "\n"
           "【什么时候用】遇到下面三种情况，**先查清楚再回答**：\n"
           "1. 陌生词汇：对方说了个你不认识的词、缩写、谐音梗、网络黑话。\n"
           "2. 无规则文字组合：看着不像正常词的一串字——拼音缩写、叠词、\n"
           "   字母数字混排（比如 yyds、xswl、emo、栓Q 这种）。\n"
           "3. 难以理解的事物：对方说的话、发的梗图文字你看不懂是什么意思。\n"
           "\n"
           "【怎么用】query 只填那个词本身（yyds、栓Q、尊嘟假嘟），\n"
           "不要把整句话填进去。\n"
           "\n"
           "【它怎么工作】先查你自己的热梗库，命中就直接返回释义和用法，\n"
           "不联网；库里没有时会联网查一次，学到之后写回热梗库并记住——\n"
           "同一个梗下次直接命中，不会再联网。查到的释义要**用来回答**，\n"
           "不要原样背给用户。\n"
           "\n"
           "【边界】只查网络用语 / 梗 / 黑话。查一般事实（比如「DeepSeek 是什么」\n"
           "「今天天气」）用 web_search，别用这个工具。\n"
           "\n"
           "【没查到怎么办】如果它说没查到，就如实说不认识，或直接问对方，\n"
           "**不要自己编一个解释**。";
  }
  [[nodiscard]] std::string parameters_schema() const override;
  Outcome run(const Call& call) override;

 private:
  LocalSearcher local_;
  RemoteSearcher remote_;
  Importer importer_;
  MemoryWriter memory_;
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
// 工具结果不截断。
//
// 原来有个 4000 字节上限（后来加到 256KB），对「形象库清单」这种需要
// 完整列举的结果都是错的：截断了模型就以为库里就这么多，找不到用户要的图。
// 模型上下文 1M tokens，工具结果几万字节完全不是负担——上限应该由
// 上下文预算决定，不该在工具层拍一个数字。
// 这里只保留一个恒等包装，调用点不用改，也不再有截断行为。
std::string truncate(std::string s, std::size_t /*unused*/ = 0) {
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

// 读整数参数。模型有时给 "3" 字符串、有时给 3 数字，两种都收。
// 缺省返回 -1（区别于合法的 0）。
long long get_int(const json& j, const char* key) {
  if (!j.contains(key)) return -1;
  const auto& v = j[key];
  if (v.is_number_integer()) return v.get<long long>();
  if (v.is_string()) {
    try {
      return std::stoll(v.get<std::string>());
    } catch (...) {
      return -1;
    }
  }
  return -1;
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

std::string Registry::openai_tools_json(const std::function<bool(const std::string&)>& filter) const {
  json arr = json::array();
  {
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& [name, tool] : tools_) {
      // 按通道能力过滤：微信不支持语音气泡，就不该把 send_voice 暴露给它。
      // 模型看不到工具就不会调，避免白跑一次 TTS 再被静默丢弃。
      if (filter && !filter(name)) continue;
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
  if (search_) hits = search_(call.conversation_id, query, top_k);

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
      "index": {"type": "integer",
                "description": "清单里的序号。不填则只返回清单让你挑；填了就发那一张"}
    }
  })";
}

Outcome ImageSearchTool::run(const Call& call) {
  Outcome out;
  const auto args = detail::parse_args(call.arguments_json);
  const long long idx_in = detail::get_int(args, "index");

  const std::vector<Hit> all = list_ ? list_() : std::vector<Hit>{};
  if (all.empty()) {
    out.ok = true;
    out.content = "形象库是空的，没有图可发。直接用文字回话。";
    return out;
  }

  // 没给 index：把完整清单交给模型，让它自己挑。
  // 这里不做任何关键词匹配、不做随机兜底——发哪张是模型的表达意图，
  // 我们只负责把「有哪些图、每张是什么内容」如实摆出来。
  //
  // 全量列出：模型上下文 1M tokens，129 条清单约 5KB，完全不值得截断。
  // 截断反而让模型以为「库里就这么多」，找不到用户想要的那张。
  if (idx_in < 0) {
    std::string s = "形象库共 " + std::to_string(all.size()) + " 张，清单：\n";
    for (std::size_t i = 0; i < all.size(); ++i) {
      s += "[" + std::to_string(i) + "] " +
           (all[i].caption.empty() ? "一张照片" : all[i].caption) + "\n";
    }
    s += "选一张最合适的，带 index 参数再调用一次 send_image。";
    out.ok = true;
    out.content = detail::truncate(std::move(s));
    return out;
  }

  // 给了 index：发那一张
  const auto idx = static_cast<std::size_t>(
      (idx_in >= 0 && static_cast<std::size_t>(idx_in) < all.size())
          ? idx_in
          : 0);
  const auto& pick = all[idx];
  out.image_paths.push_back(pick.path);
  std::string s = "已经发出这张图：「" +
                  (pick.caption.empty() ? std::string("一张照片") : pick.caption) +
                  "」。用一两句自然的话带过，不要写文件路径。";
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
      "query": {"type": "string", "description": "要查的梗 / 网络用语本身，如 yyds、栓Q、尊嘟假嘟"}
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

  // 1. 本地优先：热梗库是高频查询，走本地零成本零延迟。
  //    命中即返回，**绝不联网**（省调用，也省配额）。
  std::vector<Hit> hits;
  if (local_) hits = local_(query);

  if (!hits.empty()) {
    std::string s = "（来源：本地热梗库，已知）\n";
    for (const auto& h : hits) {
      s += h.term + "：" + h.meaning;
      if (!h.usage.empty()) s += "\n  用法：" + h.usage;
      s += "\n";
    }
    s += "\n用上面的释义来回答对方，别把这段原文背给他。";
    out.ok = true;
    out.content = detail::truncate(std::move(s));
    return out;
  }

  // 2. 本地没有 → 联网学。
  //
  // 降级要分清三种情况，给模型的说法不能一样：
  //   a. 没有联网后端（provider 没配 / 没 key）→ 明说「未启用联网检索」
  //   b. 联网调用失败（网络错 / 401 / 超时）→ 明说「联网检索失败」
  //   c. 联网成功但没搜到 → 明说「没查到」
  if (!remote_) {
    out.ok = true;
    out.content =
        "本地热梗库里没有「" + query + "」，而且当前未启用联网检索，没法学到它。\n"
        "你可以直接问对方这是什么意思，或者把对方的原话念出来问。\n"
        "（如果你已经知道这个梗是什么意思，可以直接说，不要装不知道。）";
    ::fish::log::log_line(::fish::log::LogLevel::kInfo, "tool", "meme_remote_disabled")
        .field("query", query);
    return out;
  }

  std::string remote_error;
  std::vector<Hit> remote_hits;
  try {
    remote_hits = remote_(query, &remote_error);
  } catch (const std::exception& e) {
    remote_error = std::string("exception:") + e.what();
  } catch (...) {
    remote_error = "unknown_exception";
  }

  if (!remote_error.empty()) {
    out.ok = true;
    out.content =
        "本地热梗库里没有「" + query + "」，联网检索也失败了（" + remote_error + "），"
        "所以这次没学到它。\n"
        "不要瞎编这个词的意思——可以问对方，或者如实说不知道。";
    ::fish::log::log_line(::fish::log::LogLevel::kWarn, "tool", "meme_remote_failed")
        .field("query", query)
        .field("detail", remote_error);
    return out;
  }

  if (remote_hits.empty()) {
    out.ok = true;
    out.content =
        "本地热梗库里没有「" + query + "」，联网也没搜到关于它的解释。\n"
        "不要瞎编这个词的意思——可以问对方，或者如实说不知道。";
    ::fish::log::log_line(::fish::log::LogLevel::kInfo, "tool", "meme_remote_empty")
        .field("query", query);
    return out;
  }

  // 3. 质量闸门：抓回来的内容是不是真的在讲这个梗？
  //
  // 联网搜「666」「班味」这类词很容易搜到同名的东西。
  // 判据是「梗名是否出现在抓回来的文本里」——
  // 这是校验**抓取结果与查询是否相关**，不是用关键词判断模型意图。
  //
  // 注意：只用**抓回来的内容**（释义 + 用法）来判断，
  // 不能把 hit.term 算进去——term 是我们自己从查询串派生的，
  // 把它算进去等于「查询串出现在结果里」，闸门就永远开着。
  //
  // 不相关的内容**不写回**（否则本地库会被垃圾污染，越查越烂），
  // 只把原文给模型看，并明说「没查到可靠解释」。
  std::string learned_term;
  std::vector<Hit> good;
  std::vector<Hit> rejected;
  for (const auto& h : remote_hits) {
    const std::string blob = h.meaning + " " + h.usage;
    std::string matched;
    if (fish::meme::is_relevant_with_context(query, blob, &matched)) {
      Hit copy = h;
      if (copy.term.empty()) copy.term = matched;
      good.push_back(std::move(copy));
    } else {
      rejected.push_back(h);
    }
  }

  if (good.empty()) {
    std::string s = "本地没有「" + query + "」，联网搜到的内容看起来不是在讲这个梗，"
                    "所以**没有记住**，也不能当成解释。\n";
    for (const auto& h : rejected) {
      s += "搜到的东西（仅供参考，可能不相干）：\n";
      if (!h.term.empty()) s += "  - " + h.term + "\n";
      if (!h.meaning.empty()) s += "    " + h.meaning + "\n";
    }
    s += "\n不要照抄上面的内容当解释。可以问对方是什么意思。";
    out.ok = true;
    out.content = detail::truncate(std::move(s));
    ::fish::log::log_line(::fish::log::LogLevel::kWarn, "tool", "meme_irrelevant")
        .field("query", query)
        .field("rejected", static_cast<long long>(rejected.size()));
    return out;
  }

  // 4. 写回本地库（失败不影响主链路：吞掉异常 + warn 日志）
  int added = 0;
  if (importer_) {
    try {
      added = importer_(good);
    } catch (const std::exception& e) {
      ::fish::log::log_line(::fish::log::LogLevel::kWarn, "tool", "meme_import_failed")
          .field("query", query)
          .field("detail", e.what());
    } catch (...) {
      ::fish::log::log_line(::fish::log::LogLevel::kWarn, "tool", "meme_import_failed")
          .field("query", query)
          .field("detail", "unknown_exception");
    }
  }

  // 5. 记进当前会话的长期记忆（traits + facts）。
  //    让模型知道「这个人/这个群在用这个梗」，而不是全世界都知道。
  if (memory_ && !call.conversation_id.empty()) {
    for (const auto& h : good) {
      try {
        memory_(call.conversation_id, h.term, h.meaning);
      } catch (const std::exception& e) {
        ::fish::log::log_line(::fish::log::LogLevel::kWarn, "tool", "meme_memory_failed")
            .field("query", query)
            .field("detail", e.what());
      } catch (...) {
        ::fish::log::log_line(::fish::log::LogLevel::kWarn, "tool", "meme_memory_failed")
            .field("query", query)
            .field("detail", "unknown_exception");
      }
    }
  }

  ::fish::log::log_line(::fish::log::LogLevel::kInfo, "tool", "meme_learned")
      .field("query", query)
      .field("added", static_cast<long long>(added));

  // 6. 结果给模型：明确说「刚查到并已记住」
  std::string s = "（来源：联网检索，刚查到并已记住，下次直接本地命中）\n";
  for (const auto& h : good) {
    s += h.term + "：" + h.meaning;
    if (!h.usage.empty()) s += "\n  用法：" + h.usage;
    s += "\n";
  }
  // 热梗领域营销号最多：同一个梗的解释经常被抄来抄去还抄错，
  // 必须提醒模型别把搜索到的说法当成定论。
  s += detail::kSourceWarning;
  out.ok = true;
  out.content = detail::truncate(std::move(s));
  return out;
}

}  // namespace fish::tool