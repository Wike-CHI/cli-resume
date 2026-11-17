#include "search.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <sstream>
#include <unordered_set>

namespace crg {
namespace {

// 单行超过这个长度基本是工具输出（整屏 diff、命令回显），跳过不解析
constexpr size_t kMaxLineBytes = 200 * 1024;
// 一条消息留这么多字：足够命中，也不至于把 40MB 的会话全搬进内存
constexpr size_t kEntryTextBytes = 700;
// 单个会话的索引上限，长会话后面部分会被截断（前面通常是事情的起因，更值得搜）
constexpr size_t kSessionBytes = 1536 * 1024;
constexpr size_t kSessionEntries = 5000;

char lowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

std::string lowerCopy(const std::string& s) {
  std::string o = s;
  for (auto& c : o) c = lowerAscii(c);
  return o;
}

// 大小写不敏感的子串查找。hay 里中文是原样字节，只有 ASCII 做折叠
size_t findCI(const std::string& hay, const std::string& needle) {
  if (needle.empty() || hay.size() < needle.size()) return std::string::npos;
  const size_t n = needle.size();
  const size_t last = hay.size() - n;
  for (size_t i = 0; i <= last; ++i) {
    if (lowerAscii(hay[i]) != needle[0]) continue;
    size_t k = 1;
    while (k < n && lowerAscii(hay[i + k]) == needle[k]) ++k;
    if (k == n) return i;
  }
  return std::string::npos;
}

// 多行正文压成一行：命中结果里一行一条，换行会把列表撑开
std::string flatten(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  bool space = false;
  for (char c : in) {
    unsigned char u = static_cast<unsigned char>(c);
    if (u == '\n' || u == '\r' || u == '\t' || u == ' ' || u < 0x20) {
      space = true;
      continue;
    }
    if (space && !out.empty()) out += ' ';
    space = false;
    out += c;
  }
  return out;
}


// codex 的会话标题在 session_index.jsonl 里（thread_name），比正文首条消息准得多。
// 整个进程读一次就够：索引构建期间这个文件基本不变。
const std::unordered_map<std::string, std::string>& codexTitles() {
  static std::unordered_map<std::string, std::string> map;
  static std::once_flag once;
  std::call_once(once, [] {
    std::ifstream in(utf8ToWide(homeDir() + "\\.codex\\session_index.jsonl"), std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
      if (line.find("\"thread_name\"") == std::string::npos) continue;
      try {
        json j = json::parse(line);
        std::string id = jstr(j, "id");
        std::string name = jstr(j, "thread_name");
        if (!id.empty() && !name.empty()) map[id] = utf8Truncate(name, 120);
      } catch (...) {
      }
    }
  });
  return map;
}

// "2026-11-17T01:57:22.351Z" → epoch ms（按 UTC 解析）。失败返回 0
int64_t isoToMs(const std::string& s) {
  if (s.size() < 19) return 0;
  int Y = 0, M = 0, D = 0, h = 0, m = 0, sec = 0;
  if (std::sscanf(s.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &Y, &M, &D, &h, &m, &sec) != 6) return 0;
  int ms = 0;
  const char* dot = std::strchr(s.c_str() + 19, '.');
  if (dot) ms = std::atoi(dot + 1);
  std::tm tm{};
  tm.tm_year = Y - 1900;
  tm.tm_mon = M - 1;
  tm.tm_mday = D;
  tm.tm_hour = h;
  tm.tm_min = m;
  tm.tm_sec = sec;
  return static_cast<int64_t>(_mkgmtime(&tm)) * 1000 + ms;
}

// 时间戳 → 本地日期。统计按「我这边的今天」算，不是 UTC 日
std::string dayOf(const json& ts) {
  int64_t ms = 0;
  if (ts.is_number()) {
    ms = ts.get<int64_t>();
  } else if (ts.is_string()) {
    ms = isoToMs(ts.get<std::string>());
  }
  if (ms <= 0) return "";
  const std::time_t t = static_cast<std::time_t>(ms / 1000);
  std::tm local{};
  localtime_s(&local, &t);
  char buf[16] = {0};
  std::strftime(buf, sizeof(buf), "%Y-%m-%d", &local);
  return buf;
}

int64_t numOf(const json& o, const char* k) {
  auto it = o.find(k);
  if (it == o.end() || !it->is_number()) return 0;
  return it->get<int64_t>();
}

UsageBucket* bucketFor(std::vector<UsageBucket>& v, const std::string& day,
                       const std::string& model) {
  for (auto& b : v) {
    if (b.day == day && b.model == model) return &b;
  }
  UsageBucket b;
  b.day = day;
  b.model = model;
  v.push_back(std::move(b));
  return &v.back();
}

// CLI 会把注入内容也记成 user 消息（system-reminder、local-command-stdout…），
// 这些不是人说的话
bool isNoise(const std::string& t) {
  size_t i = 0;
  while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r')) ++i;
  if (i >= t.size()) return true;
  return t[i] == '<';
}

bool isDirW(const std::wstring& p) {
  DWORD a = GetFileAttributesW(p.c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::vector<std::wstring> listDirW(const std::wstring& dir) {
  std::vector<std::wstring> out;
  WIN32_FIND_DATAW fd{};
  HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return out;
  do {
    if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
    out.push_back(fd.cFileName);
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return out;
}

int64_t fileTimeMs(const FILETIME& ft) {
  ULARGE_INTEGER u;
  u.LowPart = ft.dwLowDateTime;
  u.HighPart = ft.dwHighDateTime;
  return static_cast<int64_t>(u.QuadPart / 10000ULL) - 11644473600000LL;
}

// 流式按行读：43MB 的会话也能一屏一屏过，不用整个塞进内存
bool forEachLine(const std::wstring& path, const std::function<void(const std::string&)>& fn) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;

  std::string buf;
  buf.reserve(1 << 20);
  std::vector<char> chunk(1 << 20);
  DWORD got = 0;
  while (ReadFile(h, chunk.data(), static_cast<DWORD>(chunk.size()), &got, nullptr) && got > 0) {
    buf.append(chunk.data(), got);
    size_t pos = 0;
    while (true) {
      size_t nl = buf.find('\n', pos);
      if (nl == std::string::npos) break;
      fn(buf.substr(pos, nl - pos));
      pos = nl + 1;
    }
    buf.erase(0, pos);
    if (buf.size() > (64u << 20)) break;  // 单行离谱地长，放弃这一份
  }
  if (!buf.empty()) fn(buf);
  CloseHandle(h);
  return true;
}

// claude 的 timestamp 是 ISO 字符串，codebuddy 是毫秒数
int64_t tsOf(const json& j, int64_t def) {
  json v = jval(j, "timestamp");
  if (v.is_number_integer()) return v.get<int64_t>();
  if (v.is_number_float()) return static_cast<int64_t>(v.get<double>());
  if (!v.is_string()) return def;

  std::string s = v.get<std::string>();
  int yr = 0, mo = 0, dy = 0, hh = 0, mi = 0, ss = 0;
  if (std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &yr, &mo, &dy, &hh, &mi, &ss) != 6) return def;
  struct tm t {};
  t.tm_year = yr - 1900;
  t.tm_mon = mo - 1;
  t.tm_mday = dy;
  t.tm_hour = hh;
  t.tm_min = mi;
  t.tm_sec = ss;
  time_t utc = _mkgmtime64(&t);
  if (utc <= 0) return def;
  return static_cast<int64_t>(utc) * 1000;
}

// 从 content 里取正文。数组形式的块只认文本块，工具调用留个名字和参数开头
std::string textOfContent(const json& content) {
  if (content.is_string()) return content.get<std::string>();
  if (!content.is_array()) return "";

  std::string out;
  for (const auto& b : content) {
    if (!b.is_object()) continue;
    const std::string ty = jstr(b, "type");
    if (ty == "text" || ty == "input_text" || ty == "output_text") {
      std::string t = jstr(b, "text");
      if (t.empty()) continue;
      if (!out.empty()) out += '\n';
      out += t;
    } else if (ty == "tool_use") {
      std::string in = dumpJson(jval(b, "input"));
      if (in.empty() || in == "null") continue;
      out += " [" + jstr(b, "name") + "] " + utf8Truncate(flatten(in), 180);
    }
    // tool_result / thinking 之类的不要：前者是命令回显，后者没检索价值
  }
  return out;
}

std::string sidFromFile(const std::wstring& fname) {
  std::string sid = wideToUtf8(fname);
  if (sid.size() > 6) sid.resize(sid.size() - 6);  // 去掉 .jsonl
  return sid;
}

// 索引文件名：会话 id 是 uuid，直接拼也不会出事，但还是把非安全字符换掉
std::string safeName(const std::string& key) {
  std::string out;
  out.reserve(key.size());
  for (char c : key) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '-' || c == '_';
    out += ok ? c : '_';
  }
  return out;
}

}  // namespace

std::string SearchIndex::keyOf(const std::string& tool, const std::string& sid) {
  return tool + "_" + sid;
}

SearchIndex::~SearchIndex() { stop(); }

void SearchIndex::start(const std::string& dataDir) {
  dir_ = dataDir + "\\search";
  CreateDirectoryW(utf8ToWide(dir_).c_str(), nullptr);
  stopFlag_ = false;
  worker_ = std::thread([this] { run(); });
}

void SearchIndex::stop() {
  stopFlag_ = true;
  if (worker_.joinable()) worker_.join();
}

std::string SearchIndex::indexPath(const std::string& key) const {
  return dir_ + "\\" + safeName(key) + ".jsonl";
}

// 第一行元信息 + 每行一条消息。写的时候整体覆盖，改一个会话只碰一个文件
void SearchIndex::writeIndexFile(const Doc& doc) const {
  const std::string path = indexPath(keyOf(doc.tool, doc.sessionId));
  std::ofstream f(utf8ToWide(path), std::ios::binary | std::ios::trunc);
  if (!f) return;
  json usage = json::array();
  for (const auto& b : doc.usage) {
    usage.push_back({{"d", b.day}, {"m", b.model},  {"i", b.in},
                     {"o", b.out}, {"cr", b.cacheRead}, {"cw", b.cacheWrite},
                     {"n", b.calls}});
  }
  f << dumpJson(json{{"v", 5},
                     {"tool", doc.tool},
                     {"sid", doc.sessionId},
                     {"cwd", doc.cwd},
                     {"title", doc.title},
                     {"mtime", doc.mtime},
                     {"size", doc.size},
                     {"ts", doc.ts},
                     {"usage", usage}})
    << "\n";
  for (const auto& e : doc.entries) {
    f << dumpJson(json{{"r", e.role}, {"t", e.ts}, {"l", e.line}, {"x", e.text}}) << "\n";
  }
}

void SearchIndex::parseSource(const Src& src, Doc& doc) {
  doc.tool = src.tool;
  doc.sessionId = src.sessionId;
  doc.mtime = src.mtime;
  doc.size = src.size;

  // kimi 的正文在 agents/main/wire.jsonl，但 cwd 和标题只写在同级的 state.json 里。
  // 反向推两段目录即可，不用让上游把上下文一起传进来
  if (src.tool == "kimi") {
    std::wstring dir = src.path;
    for (int i = 0; i < 3; ++i) {  // wire.jsonl → main → agents → 会话目录
      const size_t at = dir.find_last_of(L"\\/");
      if (at == std::string::npos) break;
      dir.erase(at);
    }
    std::ifstream sf(dir + L"\\state.json", std::ios::binary);
    if (sf) {
      std::stringstream ss;
      ss << sf.rdbuf();
      try {
        json st = json::parse(ss.str());
        doc.cwd = jstr(st, "cwd");
        std::string t = jstr(st, "title");
        if (t.empty()) t = jstr(st, "lastPrompt");
        // kimi 的标题可能是「提示词 + K Kimi Thinking + 推理」拼的，切在标记处
        const size_t mark = t.find(" Kimi Thinking");
        if (mark != std::string::npos && mark > 0) t.resize(mark);
        if (!t.empty()) doc.title = utf8Truncate(flatten(t), 120);
      } catch (...) {
      }
    }
  }

  std::string firstUser;
  bool subagent = false;  // codex 的子线程，整份丢弃
  size_t bytes = 0;
  int lineNo = 0;
  std::unordered_set<std::string> seenUsage;  // claude 同一条消息会被写多行，只算一次

  forEachLine(src.path, [&](const std::string& line) {
    const int cur = lineNo++;
    if (line.size() < 8 || line.size() > kMaxLineBytes) return;
    // 先做便宜的字符串筛，避免为了几行元数据去解析整份 JSON
    const bool hasTitle = line.find("\"aiTitle\"") != std::string::npos;
    const bool maybeUsage = line.find("\"usage\"") != std::string::npos ||
                            line.find("\"rawUsage\"") != std::string::npos;
    if (!hasTitle && !maybeUsage && line.find("\"text\"") == std::string::npos) return;

    json j;
    try {
      j = json::parse(line);
    } catch (...) {
      return;
    }
    if (!j.is_object()) return;

    if (hasTitle) {
      std::string t = jstr(j, "aiTitle");
      if (!t.empty()) doc.title = utf8Truncate(flatten(t), 120);
    }

    if (doc.cwd.empty()) doc.cwd = jstr(j, "cwd");

    // token 用量：codebuddy 在 providerData.rawUsage（模型名也在这），claude 在 message.usage。
    // 新版 codebuddy 两处都有，优先 rawUsage，否则模型名会丢，统计里全成 unknown
    if (maybeUsage) {
      json msg = jval(j, "message");
      json pd = jval(j, "providerData");
      json usage;
      std::string model;
      std::string uid;
      if (pd.is_object() && pd.contains("rawUsage") && pd["rawUsage"].is_object()) {
        usage = pd["rawUsage"];
        model = jstr(pd, "model");
        if (model.empty()) model = jstr(pd, "requestModelName");
        uid = jstr(pd, "messageId");
      } else if (msg.is_object() && msg.contains("usage") && msg["usage"].is_object()) {
        usage = msg["usage"];
        model = jstr(msg, "model");
        uid = jstr(msg, "id");
      }
      if (model.empty() && pd.is_object()) model = jstr(pd, "model");
      if (!usage.is_null() && usage.is_object() && !usage.empty()) {
        if (uid.empty() || seenUsage.insert(uid).second) {
          const std::string day = dayOf(jval(j, "timestamp"));
          if (!day.empty()) {
            UsageBucket* b = bucketFor(doc.usage, day, model.empty() ? "unknown" : model);
            // 两家字段名不同，别同时加，否则有 input_tokens 的会算两遍
            b->in += usage.contains("input_tokens") ? numOf(usage, "input_tokens")
                                                    : numOf(usage, "prompt_tokens");
            b->out += usage.contains("output_tokens") ? numOf(usage, "output_tokens")
                                                      : numOf(usage, "completion_tokens");
            b->cacheRead += numOf(usage, "cache_read_input_tokens");
            b->cacheWrite += numOf(usage, "cache_creation_input_tokens");
            b->calls += 1;
          }
        }
      }
    }

    // 两个 CLI 的格式不同：codebuddy 是 type=message + role，claude 是 type 就是角色
    const std::string type = jstr(j, "type");

    // codex 的用量单独一条记录，字段名也不一样：input 里含 cached，算钱前要拆开
    if (type == "token_usage_record") {
      json p = jval(j, "payload");
      json usage = jval(p, "usage");
      if (usage.is_object() && !usage.empty()) {
        const std::string day = dayOf(jval(j, "timestamp"));
        if (!day.empty()) {
          UsageBucket* b = bucketFor(doc.usage, day, "codex");
          const int64_t cached = numOf(usage, "cached_input_tokens");
          const int64_t in = numOf(usage, "input_tokens");
          b->in += in > cached ? in - cached : 0;
          b->cacheRead += cached;
          b->cacheWrite += numOf(usage, "cache_write_input_tokens");
          b->out += numOf(usage, "output_tokens");
          b->calls += 1;
        }
      }
      return;
    }

    // kimi 的用量是独立事件，字段名自成一套：inputOther / inputCacheRead / inputCacheCreation
    if (type == "usage.record") {
      json usage = jval(j, "usage");
      if (usage.is_object() && !usage.empty()) {
        const std::string day = dayOf(jval(j, "time"));  // kimi 用 time，不是 timestamp
        if (!day.empty()) {
          const std::string model = jstr(j, "model");
          UsageBucket* b = bucketFor(doc.usage, day, model.empty() ? "kimi" : model);
          b->in += numOf(usage, "inputOther");
          b->cacheRead += numOf(usage, "inputCacheRead");
          b->cacheWrite += numOf(usage, "inputCacheCreation");
          b->out += numOf(usage, "output");
          b->calls += 1;
        }
      }
      return;
    }

    std::string role;
    json content;
    if (type == "message") {
      role = jstr(j, "role");
      content = jval(j, "content");
    } else if (type == "user" || type == "assistant") {
      role = type;
      json msg = jval(j, "message");
      content = msg.is_object() ? jval(msg, "content") : jval(j, "content");
    } else if (type == "session_meta") {
      // codex：cwd 只在这里出现，subagent 标记也只在这里
      json p = jval(j, "payload");
      if (doc.cwd.empty()) doc.cwd = jstr(p, "cwd");
      json src = jval(p, "source");
      if (src.is_object() && src.contains("subagent")) subagent = true;
      if (jstr(p, "thread_source").rfind("guardian", 0) == 0) subagent = true;
      return;
    } else if (type == "response_item") {
      json p = jval(j, "payload");
      const std::string pt = jstr(p, "type");
      if (pt == "message") {
        role = jstr(p, "role") == "user" ? "user" : "asst";
        content = jval(p, "content");
      } else if (pt == "function_call" || pt == "custom_tool_call") {
        std::string name = jstr(p, "name");
        std::string args = jstr(p, "arguments");
        if (args.empty()) {
          json a = jval(p, "input");
          if (!a.is_null()) args = dumpJson(a);
        }
        content = json::array({{{"type", "text"}, {"text", "工具 " + name + " " + args}}});
        role = "tool";
      } else {
        return;  // reasoning / encrypted 之类不进索引
      }
    } else if (type == "agent.message.appended") {
      // kimi：外层 message 是信封，message.message 才是真正的消息
      json m = jval(jval(j, "message"), "message");
      if (!m.is_object()) return;
      role = jstr(m, "role");  // user / assistant / tool
      content = jval(m, "content");
      // 工具调用不在 content 里，单独一枚 toolCalls；拼成一行让命令本身也能搜到
      json tc = jval(m, "toolCalls");
      if (tc.is_array() && !tc.empty()) {
        std::string line;
        for (const auto& c : tc) {
          const std::string nm = jstr(c, "name");
          const std::string args = jstr(c, "arguments");
          if (nm.empty()) continue;
          line += (line.empty() ? "" : "\n") + ("工具 " + nm + " " + args);
        }
        if (!line.empty()) {
          if (!content.is_array()) content = json::array();
          content.push_back({{"type", "text"}, {"text", line}});
        }
      }
    } else if (type == "summary") {
      // 老会话的摘要就是首条用户消息，拿来当兜底标题
      if (firstUser.empty()) {
        std::string s = jstr(j, "summary");
        if (!isNoise(s)) firstUser = flatten(s);
      }
      return;
    } else {
      return;
    }

    // 子代理的旁支对话不进索引，否则搜一个词能蹦出十几条重复
    json side = jval(j, "isSidechain");
    if (side.is_boolean() && side.get<bool>()) return;

    std::string text = textOfContent(content);
    if (text.empty()) return;
    text = flatten(text);
    if (isNoise(text)) return;

    const int64_t ts = tsOf(j, doc.mtime);
    if (doc.ts == 0 || (ts > 0 && ts < doc.ts)) doc.ts = ts;
    // codex 的第一条 user 往往是整份 AGENTS.md 注入，拿来当标题又长又没意义
    const bool injected = text.rfind("# AGENTS.md", 0) == 0 ||
                          text.rfind("The following is", 0) == 0;
    if (firstUser.empty() && role == "user" && !text.empty() && !injected) firstUser = text;

    if (doc.entries.size() >= kSessionEntries || bytes >= kSessionBytes) return;
    Entry e;
    e.role = role == "user" ? "user" : (role == "assistant" ? "asst" : "tool");
    e.ts = ts;
    e.line = cur;
    e.text = utf8Truncate(text, kEntryTextBytes);
    bytes += e.text.size();
    doc.entries.push_back(std::move(e));
  });

  if (doc.tool == "codex") {
    const auto& titles = codexTitles();
    auto it = titles.find(doc.sessionId);
    if (it != titles.end()) doc.title = it->second;
  }
  if (doc.title.empty() && !firstUser.empty()) doc.title = utf8Truncate(firstUser, 48);
  if (doc.ts == 0) doc.ts = doc.mtime;
  // codex 的 guardian 审查线程也是独立 rollout，正文全是「Guardian review」，
  // 进索引只会把真实命中淹掉（实测 96 条命中里 80 条是它）
  if (subagent) doc.entries.clear();
}

// 上一个版本留下的索引，先读回内存，这样重建期间也能搜（结果可能略旧）
void SearchIndex::loadIndexFiles() {
  for (const auto& fname : listDirW(utf8ToWide(dir_))) {
    if (fname.size() < 7) continue;
    if (fname.compare(fname.size() - 6, 6, L".jsonl") != 0) continue;
    const std::string path = dir_ + "\\" + wideToUtf8(fname);
    std::ifstream f(utf8ToWide(path), std::ios::binary);
    if (!f) continue;

    Doc doc;
    std::string line;
    bool first = true;
    bool oldVersion = false;
    while (std::getline(f, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.empty()) continue;
      json j;
      try {
        j = json::parse(line);
      } catch (...) {
        continue;
      }
      if (first) {
        first = false;
        // 老版本索引没有 usage，直接不认，让下面的全量扫描重建一份
        if (j.value("v", 0) != 5) {
          oldVersion = true;
          break;
        }
        doc.tool = jstr(j, "tool");
        doc.sessionId = jstr(j, "sid");
        doc.cwd = jstr(j, "cwd");
        doc.title = jstr(j, "title");
        doc.mtime = j.value("mtime", (int64_t)0);
        doc.size = j.value("size", (int64_t)0);
        doc.ts = j.value("ts", (int64_t)0);
        if (j.contains("usage") && j["usage"].is_array()) {
          for (const auto& u : j["usage"]) {
            UsageBucket b;
            b.day = jstr(u, "d");
            b.model = jstr(u, "m");
            b.in = numOf(u, "i");
            b.out = numOf(u, "o");
            b.cacheRead = numOf(u, "cr");
            b.cacheWrite = numOf(u, "cw");
            b.calls = static_cast<int>(numOf(u, "n"));
            if (!b.day.empty()) doc.usage.push_back(std::move(b));
          }
        }
        continue;
      }
      Entry e;
      e.role = jstr(j, "r", "user");
      e.ts = j.value("t", (int64_t)0);
      e.line = j.value("l", 0);
      e.text = jstr(j, "x");
      if (!e.text.empty()) doc.entries.push_back(std::move(e));
    }
    if (oldVersion) continue;
    if (doc.sessionId.empty() || doc.entries.empty()) continue;
    {
      std::lock_guard<std::mutex> lk(mu_);
      docs_[keyOf(doc.tool, doc.sessionId)] = std::move(doc);
    }
  }
}

void SearchIndex::run() {
  loadIndexFiles();

  // 枚举各家的会话文件：codebuddy / claude 是 projects/<目录>/<sid>.jsonl，
  // codex 多一层日期：sessions/YYYY/MM/DD/rollout-*.jsonl
  std::vector<Src> srcs;
  struct Root {
    const char* tool;
    std::string dir;
    bool byDate;
  };
  const Root roots[] = {
      {"codebuddy", homeDir() + "\\.codebuddy\\projects", false},
      {"claude", homeDir() + "\\.claude\\projects", false},
      {"codex", homeDir() + "\\.codex\\sessions", true},
  };

  auto addFile = [&](const std::wstring& full, const std::wstring& fname, const char* tool) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(full.c_str(), GetFileExInfoStandard, &fad)) return;
    if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return;
    Src s;
    s.path = full;
    s.tool = tool;
    s.sessionId = sidFromFile(fname);
    // codex 文件名是 rollout-<时间>-<uuid>，索引只认最后的 uuid，否则 --resume 用不了
    if (s.tool == "codex" && s.sessionId.size() > 36) {
      s.sessionId = s.sessionId.substr(s.sessionId.size() - 36);
    }
    s.mtime = fileTimeMs(fad.ftLastWriteTime);
    s.size = (int64_t)(((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow);
    srcs.push_back(std::move(s));
  };

  for (const auto& root : roots) {
    if (stopFlag_) return;
    const std::wstring base = utf8ToWide(root.dir);
    if (!isDirW(base)) continue;
    if (root.byDate) {
      for (const auto& y : listDirW(base)) {
        if (y.size() != 4) continue;
        const std::wstring wy = base + L"\\" + y;
        if (!isDirW(wy)) continue;
        for (const auto& mo : listDirW(wy)) {
          if (mo.size() != 2) continue;
          const std::wstring wm = wy + L"\\" + mo;
          if (!isDirW(wm)) continue;
          for (const auto& d : listDirW(wm)) {
            if (d.size() != 2) continue;
            const std::wstring wd = wm + L"\\" + d;
            if (!isDirW(wd)) continue;
            for (const auto& f : listDirW(wd)) {
              if (f.size() < 7 || f.compare(f.size() - 6, 6, L".jsonl") != 0) continue;
              addFile(wd + L"\\" + f, f, root.tool);
            }
          }
        }
      }
    } else {
      for (const auto& proj : listDirW(base)) {
        const std::wstring projPath = base + L"\\" + proj;
        if (!isDirW(projPath)) continue;
        for (const auto& f : listDirW(projPath)) {
          if (f.size() < 7 || f.compare(f.size() - 6, 6, L".jsonl") != 0) continue;
          addFile(projPath + L"\\" + f, f, root.tool);
        }
      }
    }
  }
  // kimi：sessions/<工作区>/session_<uuid>/agents/main/wire.jsonl。
  // 会话 id 就是目录名（kimi -S 认这个），文件名 wire.jsonl 本身没有信息；
  // 子 agent（agents/agent-3 之类）不进索引，否则一个任务能搜出好几份
  if (!stopFlag_) {
    const std::wstring kbase = utf8ToWide(homeDir() + "\\.kimi-code\\sessions");
    if (isDirW(kbase)) {
      for (const auto& wd : listDirW(kbase)) {
        const std::wstring wdp = kbase + L"\\" + wd;
        if (!isDirW(wdp)) continue;
        for (const auto& sess : listDirW(wdp)) {
          if (sess.rfind(L"session_", 0) != 0) continue;
          // 借文件名把会话名递进去：sidFromFile 去掉 .jsonl 就是 session_<uuid>
          addFile(wdp + L"\\" + sess + L"\\agents\\main\\wire.jsonl", sess + L".jsonl", "kimi");
        }
      }
    }
  }
  total_ = static_cast<int>(srcs.size());

  std::unordered_set<std::string> alive;
  for (const auto& s : srcs) {
    if (stopFlag_) return;
    const std::string key = keyOf(s.tool, s.sessionId);
    alive.insert(key);

    bool fresh = false;
    {
      std::lock_guard<std::mutex> lk(mu_);
      auto it = docs_.find(key);
      fresh = it != docs_.end() && it->second.mtime == s.mtime && it->second.size == s.size;
    }
    if (!fresh) {
      Doc doc;
      parseSource(s, doc);
      if (!doc.entries.empty()) {
        writeIndexFile(doc);
        std::lock_guard<std::mutex> lk(mu_);
        docs_[key] = std::move(doc);
      }
    }
    done_ = done_ + 1;
  }

  // 源文件已经没了的会话，索引也一并清掉
  {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto it = docs_.begin(); it != docs_.end();) {
      if (alive.count(it->first)) {
        ++it;
        continue;
      }
      DeleteFileW(utf8ToWide(indexPath(it->first)).c_str());
      it = docs_.erase(it);
    }
  }

  ready_ = true;
}

SearchReply SearchIndex::query(const std::string& q, int limit) const {
  SearchReply r;
  r.ready = ready_.load();
  r.files = done_.load();
  r.totalFiles = total_.load();
  const int64_t t0 = nowMs();

  std::string needle = lowerCopy(q);
  // 去掉首尾空白，太短的不搜（一个字符会命中几千处，没意义）
  size_t b = needle.find_first_not_of(" \t\r\n");
  size_t e = needle.find_last_not_of(" \t\r\n");
  if (b == std::string::npos) {
    r.elapsedMs = nowMs() - t0;
    return r;
  }
  needle = needle.substr(b, e - b + 1);
  if (needle.size() < 2) {
    r.elapsedMs = nowMs() - t0;
    return r;
  }

  std::vector<SearchHit> all;
  int sessions = 0;
  {
    std::lock_guard<std::mutex> lk(mu_);
    int total = 0;
    for (const auto& kv : docs_) {
      const Doc& d = kv.second;
      total += static_cast<int>(d.entries.size());
      bool hitThis = false;
      for (const auto& en : d.entries) {
        const size_t pos = findCI(en.text, needle);
        if (pos == std::string::npos) continue;
        hitThis = true;

        SearchHit h;
        h.cwd = d.cwd;
        h.title = d.title;
        h.tool = d.tool;
        h.sessionId = d.sessionId;
        h.role = en.role;
        h.ts = en.ts;
        h.line = en.line;

        // 前后各留一点上下文，切的时候退到 UTF-8 字符边界，别切出半个汉字
        size_t s = pos > 36 ? pos - 36 : 0;
        while (s > 0 && (static_cast<unsigned char>(en.text[s]) & 0xC0) == 0x80) --s;
        size_t end = (std::min)(en.text.size(), pos + needle.size() + 64);
        while (end < en.text.size() && (static_cast<unsigned char>(en.text[end]) & 0xC0) == 0x80) ++end;

        h.before = en.text.substr(s, pos - s);
        h.hit = en.text.substr(pos, needle.size());
        h.after = en.text.substr(pos + needle.size(), end - pos - needle.size());
        if (s > 0) h.before = "…" + h.before;
        if (end < en.text.size()) h.after += "…";
        all.push_back(std::move(h));
      }
      if (hitThis) sessions += 1;
    }
    r.entries = total;
  }

  r.matchedSessions = sessions;
  r.totalMatches = static_cast<int>(all.size());
  // 最近发生的先看：同一批结果里按时间倒序
  std::sort(all.begin(), all.end(), [](const SearchHit& a, const SearchHit& b) {
    if (a.ts != b.ts) return a.ts > b.ts;
    return a.line > b.line;
  });
  if (limit > 0 && static_cast<int>(all.size()) > limit) all.resize(limit);
  r.hits = std::move(all);
  r.elapsedMs = nowMs() - t0;
  return r;
}

SessionDigest SearchIndex::digest(const std::string& tool, const std::string& sid, int headN,
                                  int tailN) const {
  SessionDigest out;
  out.ready = ready_.load();
  std::lock_guard<std::mutex> lk(mu_);
  auto it = docs_.find(keyOf(tool, sid));
  if (it == docs_.end()) return out;

  const Doc& d = it->second;
  out.found = true;
  out.cwd = d.cwd;
  out.title = d.title;
  out.tool = d.tool;
  out.sessionId = d.sessionId;
  out.ts = d.ts;
  out.mtime = d.mtime;
  out.size = d.size;

  const int n = static_cast<int>(d.entries.size());
  out.totalEntries = n;
  if (!n) return out;

  auto turn = [](const Entry& e) {
    DigestTurn t;
    t.role = e.role;
    t.text = e.text;
    t.ts = e.ts;
    return t;
  };

  const int head = (std::min)(headN, n);
  for (int i = 0; i < head; ++i) out.head.push_back(turn(d.entries[i]));
  // 头尾挨着的时候不要重复取同一条：从 head 之后开始算尾部
  const int tailFrom = (std::max)(head, n - tailN);
  for (int i = tailFrom; i < n; ++i) out.tail.push_back(turn(d.entries[i]));
  return out;
}

std::vector<DocUsage> SearchIndex::usageAll() const {
  std::lock_guard<std::mutex> lk(mu_);
  std::vector<DocUsage> out;
  out.reserve(docs_.size());
  for (const auto& kv : docs_) {
    const Doc& d = kv.second;
    if (d.usage.empty()) continue;
    DocUsage u;
    u.tool = d.tool;
    u.sessionId = d.sessionId;
    u.cwd = d.cwd;
    u.title = d.title;
    u.ts = d.ts;
    u.buckets = d.usage;
    out.push_back(std::move(u));
  }
  return out;
}

}  // namespace crg
