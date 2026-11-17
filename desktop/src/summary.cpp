#include "summary.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#include "httplib.h"

namespace crg {
namespace {

// 改了提示词就 +1，旧缓存自动作废
constexpr int kPromptVersion = 1;
constexpr int kMaxPromptBytes = 9000;
constexpr int kTurnBytes = 420;

const char* const kSystemPrompt =
    "你是开发工作日志助手。用户会给你一段命令行编码会话的片段（可能中英混合）。\n"
    "只输出三行，不要寒暄、不要 Markdown、不要代码块、不要额外标题：\n"
    "做了：<上次这个会话推进了什么，40 字以内>\n"
    "卡在：<当前未完成、报错或待确认的点；没有就写「没有明显卡点」，40 字以内>\n"
    "下一步：<根据上下文最合理的下一步，40 字以内>\n"
    "用中文。只写材料里能看出来的东西，推不出来就写「不确定」。\n"
    "/no_think";

std::string trimCopy(const std::string& in) {
  size_t b = in.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  size_t e = in.find_last_not_of(" \t\r\n");
  return in.substr(b, e - b + 1);
}

// 用户可能把 http://127.0.0.1:1234/v1 整条粘进来，去掉末尾的 /v1 再拼
std::string originOf(const std::string& url, bool& ok) {
  std::string u = trimCopy(url);
  while (!u.empty() && u.back() == '/') u.pop_back();
  if (u.rfind("http://", 0) != 0) {
    ok = false;  // https 需要 OpenSSL 版本，本地模型也基本都是 http
    return "";
  }
  if (u.size() > 3 && u.compare(u.size() - 3, 3, "/v1") == 0) u.erase(u.size() - 3);
  while (!u.empty() && u.back() == '/') u.pop_back();
  ok = true;
  return u;
}

// qwen3 在思考模式下会先吐  thinking…<｜end▁of▁thinking｜>，提示词里已经带 /no_think，
// 这里再兜一层：真出现了就整段去掉，别把思考过程当复盘显示
std::string stripThink(const std::string& in) {
  std::string s = in;
  const std::string end1 = "</think>";
  const std::string end2 = "<｜end▁of▁thinking｜>";
  for (;;) {
    size_t a = s.find(" thinking");
    if (a == std::string::npos) break;
    size_t b1 = s.find(end1, a);
    size_t b2 = s.find(end2, a);
    size_t end = std::string::npos;
    size_t endLen = 0;
    if (b1 != std::string::npos) {
      end = b1;
      endLen = end1.size();
    }
    if (b2 != std::string::npos && (end == std::string::npos || b2 < end)) {
      end = b2;
      endLen = end2.size();
    }
    if (end == std::string::npos) {
      s.erase(a);  // 只有开头没有结尾：整段都是思考，内容为空
      break;
    }
    s.erase(a, end - a + endLen);
  }
  return trimCopy(s);
}

std::string roleName(const std::string& r) {
  if (r == "user") return "用户";
  if (r == "tool") return "工具";
  return "助手";
}

std::string oneTurn(const DigestTurn& t) {
  return roleName(t.role) + "：" + utf8Truncate(t.text, kTurnBytes);
}

std::string buildPrompt(const SessionDigest& d) {
  std::ostringstream body;
  body << "会话目录：" << d.cwd << "\n";
  if (!d.title.empty()) body << "会话标题：" << d.title << "\n";
  body << "共 " << d.totalEntries << " 条消息，下面是开头和最近的进展。\n\n";

  if (!d.head.empty()) {
    body << "[开头]\n";
    for (const auto& t : d.head) body << oneTurn(t) << "\n";
    body << "\n";
  }
  body << "[最近]\n";
  // 从最新往回装，超长时先丢早的，保证「最后在做什么」一定在里面
  std::vector<std::string> tail;
  size_t used = 0;
  for (auto it = d.tail.rbegin(); it != d.tail.rend(); ++it) {
    std::string line = oneTurn(*it);
    if (used + line.size() > kMaxPromptBytes) break;
    used += line.size() + 1;
    tail.push_back(std::move(line));
  }
  for (auto it = tail.rbegin(); it != tail.rend(); ++it) body << *it << "\n";
  return body.str();
}

std::string safeName(const std::string& s) {
  std::string out;
  for (char c : s) {
    unsigned char u = static_cast<unsigned char>(c);
    out += (std::isalnum(u) || c == '-' || c == '_') ? c : '_';
  }
  return out.empty() ? std::string("x") : out;
}

std::string joinModels(const std::vector<std::string>& v) {
  std::string out;
  for (size_t i = 0; i < v.size() && i < 8; ++i) {
    if (i) out += "、";
    out += v[i];
  }
  return out;
}

}  // namespace

void Summarizer::start(const std::string& dataDir) {
  dir_ = dataDir + "\\summary";
  CreateDirectoryA(dir_.c_str(), nullptr);
}

LlmStatus Summarizer::probe(const std::string& url, const std::string& model, bool force) {
  std::lock_guard<std::mutex> lk(probeMu_);
  const int64_t now = nowMs();
  // 点一次探测两枪太浪费，20 秒内复用；但换了地址或模型就必须重探
  if (!force && probedAt_ && now - probedAt_ < 20000 && probedUrl_ == url && probedModel_ == model) {
    return cachedProbe_;
  }

  LlmStatus st;
  st.url = url;
  bool ok = false;
  const std::string base = originOf(url, ok);
  if (!ok) {
    st.error = "模型地址要以 http:// 开头（当前：" + url + "）";
    cachedProbe_ = st;
    probedAt_ = now;
    probedUrl_ = url;
    probedModel_ = model;
    return st;
  }

  httplib::Client cli(base);
  cli.set_connection_timeout(2, 0);
  cli.set_read_timeout(4, 0);

  // 先问 Ollama 的原生接口，再问 OpenAI 兼容接口
  auto tags = cli.Get("/api/tags");
  if (tags && tags->status == 200) {
    try {
      json j = json::parse(tags->body);
      if (j.contains("models") && j["models"].is_array()) {
        for (const auto& m : j["models"]) {
          std::string n = jstr(m, "name");
          if (n.empty()) n = jstr(m, "model");
          if (!n.empty()) st.models.push_back(n);
        }
      }
      st.api = "ollama";
    } catch (...) {
      // 不是预期格式，当作不可用，继续试下一种
    }
  }

  if (st.api.empty()) {
    auto v1 = cli.Get("/v1/models");
    if (v1 && v1->status == 200) {
      try {
        json j = json::parse(v1->body);
        if (j.contains("data") && j["data"].is_array()) {
          for (const auto& m : j["data"]) {
            std::string n = jstr(m, "id");
            if (!n.empty()) st.models.push_back(n);
          }
        }
        st.api = "openai";
      } catch (...) {
      }
    }
  }

  if (st.api.empty()) {
    if (!tags) {
      st.error = "连不上 " + base + "（本地模型没启动？）";
    } else {
      st.error = "服务有响应但不是 Ollama / OpenAI 兼容接口（HTTP " +
                 std::to_string(tags->status) + "）";
    }
    cachedProbe_ = st;
    probedAt_ = now;
    probedUrl_ = url;
    probedModel_ = model;
    return st;
  }

  if (st.models.empty()) {
    st.error = "服务在跑，但本机一个模型都没有（先 pull / 下载一个）";
  } else if (!model.empty()) {
    if (std::find(st.models.begin(), st.models.end(), model) != st.models.end()) {
      st.model = model;
      st.ok = true;
    } else {
      st.error = "本机没有模型 " + model + "（可用：" + joinModels(st.models) + "）";
    }
  } else {
    // 没配置就自动挑：优先 qwen 系，其次第一个
    st.model = st.models.front();
    for (const auto& m : st.models) {
      std::string low = m;
      std::transform(low.begin(), low.end(), low.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (low.find("qwen") != std::string::npos) {
        st.model = m;
        break;
      }
    }
    st.ok = true;
  }

  cachedProbe_ = st;
  probedAt_ = now;
  probedUrl_ = url;
  probedModel_ = model;
  return st;
}

std::string Summarizer::cachePath(const std::string& tool, const std::string& sid) const {
  return dir_ + "\\" + safeName(tool) + "_" + safeName(sid) + ".json";
}

bool Summarizer::readCache(const std::string& p, const SessionDigest& d, const std::string& model,
                           std::string& text) const {
  std::ifstream f(utf8ToWide(p), std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  try {
    json j = json::parse(ss.str());
    if (jval(j, "v", json(0)).get<int>() != kPromptVersion) return false;
    if (jval(j, "mtime", json(0)).get<int64_t>() != d.mtime) return false;
    if (jval(j, "size", json(0)).get<int64_t>() != d.size) return false;
    if (jstr(j, "model") != model) return false;
    text = jstr(j, "text");
    return !text.empty();
  } catch (...) {
    return false;
  }
}

void Summarizer::writeCache(const std::string& p, const SessionDigest& d, const std::string& model,
                            const std::string& text) const {
  json j{{"v", kPromptVersion}, {"mtime", d.mtime}, {"size", d.size},
         {"model", model},       {"at", nowMs()},      {"text", text}};
  std::ofstream f(utf8ToWide(p), std::ios::binary | std::ios::trunc);
  if (!f) return;
  f << dumpJson(j);
}

json Summarizer::summarize(const SessionDigest& d, const std::string& url, const std::string& model,
                           bool force) {
  json out = {{"ok", false}};
  if (!d.found) {
    out["error"] = d.ready ? "索引里没有这个会话" : "索引还在建，稍后再试";
    out["ready"] = d.ready;
    return out;
  }
  out["ok"] = true;
  out["title"] = d.title;
  out["cwd"] = d.cwd;
  out["entries"] = d.totalEntries;
  out["ready"] = d.ready;

  LlmStatus st = probe(url, model, force);
  out["llmOk"] = st.ok;
  out["api"] = st.api;
  out["model"] = st.model;
  out["models"] = st.models;
  if (!st.api.empty()) out["llmUrl"] = st.url;
  if (!st.ok) {
    out["llmError"] = st.error;
    return out;
  }

  const std::string cache = cachePath(d.tool, d.sessionId);
  std::string text;
  if (!force && readCache(cache, d, st.model, text)) {
    out["text"] = text;
    out["cached"] = true;
    out["elapsedMs"] = 0;
    return out;
  }

  // 模型一次只答一个：并发的窗口不会把显存挤爆，也免得同一会话算两遍
  std::lock_guard<std::mutex> lk(callMu_);
  if (!force && readCache(cache, d, st.model, text)) {
    out["text"] = text;
    out["cached"] = true;
    out["elapsedMs"] = 0;
    return out;
  }

  bool ok = false;
  const std::string base = originOf(url, ok);
  const int64_t t0 = nowMs();

  json body;
  const char* path = nullptr;
  if (st.api == "openai") {
    path = "/v1/chat/completions";
    body = {{"model", st.model},
            {"stream", false},
            {"temperature", 0.2},
            {"max_tokens", 260},
            {"messages",
             json::array({{{"role", "system"}, {"content", kSystemPrompt}},
                          {{"role", "user"}, {"content", buildPrompt(d)}}})}};
  } else {
    path = "/api/chat";
    body = {{"model", st.model},
            {"stream", false},
            {"think", false},  // 新版本 Ollama 用它关掉 qwen3 的思考段
            {"options", {{"temperature", 0.2}, {"num_predict", 260}}},
            {"messages",
             json::array({{{"role", "system"}, {"content", kSystemPrompt}},
                          {{"role", "user"}, {"content", buildPrompt(d)}}})}};
  }

  httplib::Client cli(base);
  cli.set_connection_timeout(3, 0);
  cli.set_read_timeout(180, 0);  // 8B 头一次加载要几十秒，给足
  auto r = cli.Post(path, body.dump(), "application/json");
  out["elapsedMs"] = nowMs() - t0;

  if (!r) {
    out["llmOk"] = false;
    out["llmError"] = std::string("模型没有响应：") + httplib::to_string(r.error());
    return out;
  }
  if (r->status != 200) {
    out["llmOk"] = false;
    out["llmError"] = "模型返回 HTTP " + std::to_string(r->status) + "：" +
                      utf8Truncate(trimCopy(r->body), 200);
    return out;
  }

  try {
    json j = json::parse(r->body);
    std::string content;
    if (st.api == "openai") {
      content = j["choices"][0]["message"]["content"].get<std::string>();
    } else {
      content = j["message"]["content"].get<std::string>();
    }
    content = stripThink(content);
    if (content.empty()) {
      out["llmOk"] = false;
      out["llmError"] = "模型返回了空内容";
      return out;
    }
    out["text"] = content;
    out["cached"] = false;
    writeCache(cache, d, st.model, content);
  } catch (const std::exception& e) {
    out["llmOk"] = false;
    out["llmError"] = std::string("模型返回的不是预期 JSON：") + e.what();
  }
  return out;
}

}  // namespace crg
