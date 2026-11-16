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
  f << dumpJson(json{{"v", 1},
                     {"tool", doc.tool},
                     {"sid", doc.sessionId},
                     {"cwd", doc.cwd},
                     {"title", doc.title},
                     {"mtime", doc.mtime},
                     {"size", doc.size},
                     {"ts", doc.ts}})
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

  std::string firstUser;
  size_t bytes = 0;
  int lineNo = 0;

  forEachLine(src.path, [&](const std::string& line) {
    const int cur = lineNo++;
    if (line.size() < 8 || line.size() > kMaxLineBytes) return;
    // 先做便宜的字符串筛，避免为了几行元数据去解析整份 JSON
    const bool hasTitle = line.find("\"aiTitle\"") != std::string::npos;
    if (!hasTitle && line.find("\"text\"") == std::string::npos) return;

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

    // 两个 CLI 的格式不同：codebuddy 是 type=message + role，claude 是 type 就是角色
    const std::string type = jstr(j, "type");
    std::string role;
    json content;
    if (type == "message") {
      role = jstr(j, "role");
      content = jval(j, "content");
    } else if (type == "user" || type == "assistant") {
      role = type;
      json msg = jval(j, "message");
      content = msg.is_object() ? jval(msg, "content") : jval(j, "content");
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
    if (firstUser.empty() && role == "user" && !text.empty()) firstUser = text;

    if (doc.entries.size() >= kSessionEntries || bytes >= kSessionBytes) return;
    Entry e;
    e.role = role == "user" ? "user" : (role == "assistant" ? "asst" : "tool");
    e.ts = ts;
    e.line = cur;
    e.text = utf8Truncate(text, kEntryTextBytes);
    bytes += e.text.size();
    doc.entries.push_back(std::move(e));
  });

  if (doc.title.empty() && !firstUser.empty()) doc.title = utf8Truncate(firstUser, 48);
  if (doc.ts == 0) doc.ts = doc.mtime;
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
        doc.tool = jstr(j, "tool");
        doc.sessionId = jstr(j, "sid");
        doc.cwd = jstr(j, "cwd");
        doc.title = jstr(j, "title");
        doc.mtime = j.value("mtime", (int64_t)0);
        doc.size = j.value("size", (int64_t)0);
        doc.ts = j.value("ts", (int64_t)0);
        continue;
      }
      Entry e;
      e.role = jstr(j, "r", "user");
      e.ts = j.value("t", (int64_t)0);
      e.line = j.value("l", 0);
      e.text = jstr(j, "x");
      if (!e.text.empty()) doc.entries.push_back(std::move(e));
    }
    if (doc.sessionId.empty() || doc.entries.empty()) continue;
    {
      std::lock_guard<std::mutex> lk(mu_);
      docs_[keyOf(doc.tool, doc.sessionId)] = std::move(doc);
    }
  }
}

void SearchIndex::run() {
  loadIndexFiles();

  // 枚举两边的会话文件
  std::vector<Src> srcs;
  const std::pair<const char*, std::string> roots[] = {
      {"codebuddy", homeDir() + "\\.codebuddy\\projects"},
      {"claude", homeDir() + "\\.claude\\projects"},
  };
  for (const auto& root : roots) {
    if (stopFlag_) return;
    const std::wstring base = utf8ToWide(root.second);
    if (!isDirW(base)) continue;
    for (const auto& proj : listDirW(base)) {
      const std::wstring projPath = base + L"\\" + proj;
      if (!isDirW(projPath)) continue;
      for (const auto& fname : listDirW(projPath)) {
        if (fname.size() < 7) continue;
        if (fname.compare(fname.size() - 6, 6, L".jsonl") != 0) continue;
        const std::wstring full = projPath + L"\\" + fname;
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (!GetFileAttributesExW(full.c_str(), GetFileExInfoStandard, &fad)) continue;
        if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        Src s;
        s.path = full;
        s.tool = root.first;
        s.sessionId = sidFromFile(fname);
        s.mtime = fileTimeMs(fad.ftLastWriteTime);
        s.size = (int64_t)(((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow);
        srcs.push_back(std::move(s));
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

}  // namespace crg
