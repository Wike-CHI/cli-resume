#include "scanner.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <map>
#include <set>
#include <unordered_map>

namespace crg {
namespace {

constexpr DWORD kSniffBytes = 64 * 1024;

int64_t fileTimeToMs(const FILETIME& ft) {
  ULARGE_INTEGER u;
  u.LowPart = ft.dwLowDateTime;
  u.HighPart = ft.dwHighDateTime;
  // FILETIME 以 100ns 为单位，起点 1601-01-01；减去到 1970 的偏移
  return static_cast<int64_t>(u.QuadPart / 10000ULL) - 11644473600000LL;
}

bool pathExists(const std::wstring& p, bool* isDir) {
  DWORD attr = GetFileAttributesW(p.c_str());
  if (attr == INVALID_FILE_ATTRIBUTES) return false;
  if (isDir) *isDir = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
  return true;
}

bool readFileHead(const std::wstring& path, DWORD maxBytes, std::string& out) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  out.assign(maxBytes, '\0');
  DWORD got = 0;
  BOOL ok = ReadFile(h, out.data(), maxBytes, &got, nullptr);
  CloseHandle(h);
  if (!ok) return false;
  out.resize(got);
  return true;
}

// ai-title 是追加写在文件末尾附近的，所以标题得从尾巴上找
bool readFileTail(const std::wstring& path, DWORD maxBytes, std::string& out) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0) {
    CloseHandle(h);
    return false;
  }
  LONGLONG start = size.QuadPart > (LONGLONG)maxBytes ? size.QuadPart - maxBytes : 0;
  LARGE_INTEGER pos;
  pos.QuadPart = start;
  if (!SetFilePointerEx(h, pos, nullptr, FILE_BEGIN)) {
    CloseHandle(h);
    return false;
  }
  DWORD want = static_cast<DWORD>(size.QuadPart - start);
  out.assign(want, '\0');
  DWORD got = 0;
  BOOL ok = ReadFile(h, out.data(), want, &got, nullptr);
  CloseHandle(h);
  if (!ok) return false;
  out.resize(got);
  return true;
}

bool readFileAll(const std::wstring& path, std::string& out) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 || size.QuadPart > (64LL << 20)) {
    CloseHandle(h);
    return false;
  }
  out.assign(static_cast<size_t>(size.QuadPart), '\0');
  DWORD got = 0;
  BOOL ok = ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr);
  CloseHandle(h);
  if (!ok) return false;
  out.resize(got);
  return true;
}

// 从 "cwd":"xxx" 里手工取出值，用于整行 JSON 解析失败的兜底
std::string scanForCwd(const std::string& line, size_t from) {
  const std::string key = "\"cwd\"";
  size_t p = line.find(key, from);
  if (p == std::string::npos) return "";
  p += key.size();
  while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) ++p;
  if (p >= line.size() || line[p] != ':') return "";
  ++p;
  while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) ++p;
  if (p >= line.size() || line[p] != '"') return "";
  ++p;
  std::string out;
  while (p < line.size()) {
    char c = line[p];
    if (c == '"') break;
    if (c == '\\' && p + 1 < line.size()) {
      char e = line[p + 1];
      p += 2;
      switch (e) {
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        default: out += e; break;  // \" \\ \/ 等
      }
      continue;
    }
    out += c;
    ++p;
  }
  return out;
}

// 会话文件可能几 MB，cwd 就在前几条消息里，只嗅探文件头
std::string sniffCwdIn(const std::string& head) {
  size_t pos = 0;
  while ((pos = head.find("\"cwd\"", pos)) != std::string::npos) {
    size_t lineStart = head.rfind('\n', pos);
    lineStart = (lineStart == std::string::npos) ? 0 : lineStart + 1;
    size_t lineEnd = head.find('\n', pos);
    if (lineEnd == std::string::npos) lineEnd = head.size();
    std::string line = head.substr(lineStart, lineEnd - lineStart);
    // 整行通常是合法 JSON；解析成功就最可靠（能正确处理 \\ 与 \uXXXX）
    try {
      json j = json::parse(line);
      std::string c = jstr(j, "cwd");
      if (!c.empty()) return c;
    } catch (...) {
      // 落到手工提取
    }
    std::string c = scanForCwd(line, 0);
    if (!c.empty()) return c;
    pos += 5;
  }
  return "";
}

// 用户粘进来的内容常带一层引号（"C:\path\...），标题里看着别扭
std::string stripLeadingQuotes(const std::string& s) {
  size_t i = 0;
  while (i < s.size()) {
    char c = s[i];
    if (c == '"' || c == '\'' || c == '`') {
      ++i;
      continue;
    }
    // 中文左双引号 “ / 左单引号 ‘
    if (i + 2 < s.size() && static_cast<unsigned char>(s[i]) == 0xE2 &&
        static_cast<unsigned char>(s[i + 1]) == 0x80 &&
        (static_cast<unsigned char>(s[i + 2]) == 0x9C ||
         static_cast<unsigned char>(s[i + 2]) == 0x98)) {
      i += 3;
      continue;
    }
    break;
  }
  return s.substr(i);
}

// 压成单行 + 按码点截断，避免把半个汉字切出来
std::string oneLine(const std::string& s, size_t maxChars) {
  const std::string src = stripLeadingQuotes(s);
  std::string out;
  out.reserve(src.size() > maxChars * 3 ? maxChars * 3 : src.size());
  bool pendingSpace = false;
  size_t chars = 0;
  for (size_t i = 0; i < src.size();) {
    unsigned char c = static_cast<unsigned char>(src[i]);
    size_t len = 1;
    if ((c & 0xE0) == 0xC0) len = 2;
    else if ((c & 0xF0) == 0xE0) len = 3;
    else if ((c & 0xF8) == 0xF0) len = 4;
    if (i + len > src.size()) len = src.size() - i;

    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      pendingSpace = !out.empty();
    } else {
      if (pendingSpace) {
        out += ' ';
        ++chars;
        pendingSpace = false;
      }
      if (chars >= maxChars) {
        out += "…";
        break;
      }
      out.append(src, i, len);
      ++chars;
    }
    i += len;
  }
  return out;
}

// 两个 CLI 都会写 type=ai-title 的行，字段名都是 aiTitle。
// 它会随对话推进反复追加，所以取尾部块里最后一个。
std::string findLastAiTitle(const std::string& tail) {
  std::string best;
  size_t pos = 0;
  // 块首行可能是被截断的半行，跳过
  size_t nl = tail.find('\n');
  if (nl != std::string::npos) pos = nl + 1;
  while (pos < tail.size()) {
    size_t end = tail.find('\n', pos);
    if (end == std::string::npos) end = tail.size();
    if (end > pos) {
      const char* kAi = "ai-title";
      size_t hit = tail.find(kAi, pos);
      if (hit != std::string::npos && hit < end) {
        try {
          json j = json::parse(tail.substr(pos, end - pos));
          std::string t = jstr(j, "aiTitle");
          if (!t.empty()) best = t;
        } catch (...) {
          // 半截行解析失败就跳过，下一次追加会补上完整记录
        }
      }
    }
    pos = end + 1;
  }
  return best;
}

// CLI 会把自己的注入内容也记成 user 消息（system-reminder、local-command-stdout…），
// 这些不是用户说的话，拿来当标题只会是一堆尖括号
bool isNoise(const std::string& t) {
  size_t i = 0;
  while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r')) ++i;
  if (i >= t.size()) return true;
  if (t[i] == '<') return true;
  return false;
}

// 没有 ai-title 的老会话，退回到「用户第一句话」当标题
std::string firstUserText(const std::string& head) {
  size_t pos = 0;
  while (pos < head.size()) {
    size_t end = head.find('\n', pos);
    if (end == std::string::npos) end = head.size();
    std::string line = head.substr(pos, end - pos);
    pos = end + 1;
    if (line.empty()) continue;
    if (line.find("\"summary\"") == std::string::npos &&
        line.find("\"user\"") == std::string::npos &&
        line.find("\"message\"") == std::string::npos) {
      continue;
    }

    json j;
    try {
      j = json::parse(line);
    } catch (...) {
      continue;
    }

    // codebuddy 的 summary 记的就是首条用户消息原文，最干净
    std::string sum = jstr(j, "summary");
    if (!sum.empty() && !isNoise(sum)) return sum;

    json msg = jval(j, "message");
    json content = msg.is_object() ? jval(msg, "content") : jval(j, "content");
    if (content.is_string()) {
      std::string t = content.get<std::string>();
      if (!isNoise(t)) return t;
      continue;
    }
    if (content.is_array()) {
      for (const auto& b : content) {
        const std::string ty = jstr(b, "type");
        // 跳过 tool_result 之类的非对话块
        if (ty != "text" && ty != "input_text" && ty != "output_text") continue;
        std::string t = jstr(b, "text");
        if (!t.empty() && !isNoise(t)) return t;
      }
    }
  }
  return "";
}

struct Sniffed {
  std::string cwd;
  std::string title;
  bool empty = false;  // 只有命令事件、没有一句人话的会话
};

// cwd 在文件头，标题在文件尾，分两次读，都不读整份文件
Sniffed sniffSession(const std::wstring& path, const std::string& mappedCwd) {
  Sniffed out;
  out.cwd = mappedCwd;

  std::string head;
  if (readFileHead(path, kSniffBytes, head) && !head.empty()) {
    if (out.cwd.empty()) out.cwd = sniffCwdIn(head);
    out.title = firstUserText(head);
  }
  if (out.cwd.empty()) return out;

  std::string tail;
  if (readFileTail(path, kSniffBytes / 2, tail) && !tail.empty()) {
    std::string t = findLastAiTitle(tail);
    if (!t.empty()) out.title = t;  // 模型给的名字比原话更短更准
  }
  out.title = oneLine(out.title, 48);
  // 标题都取不到，说明这个会话里没有任何对话内容，恢复进去也是空窗口
  if (out.title.empty()) out.empty = true;
  return out;
}

std::vector<std::wstring> listDir(const std::wstring& dir) {
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

// claude 的目录名是绝对路径把非字母数字全换成 '-'，不可逆，
// 所以用 ~/.claude.json 里的真实路径反算编码来对表
std::unordered_map<std::string, std::string> loadClaudeCwdMap() {
  std::unordered_map<std::string, std::string> map;
  std::string raw;
  std::wstring cfg = utf8ToWide(homeDir()) + L"\\.claude.json";
  if (!readFileAll(cfg, raw) || raw.empty()) return map;
  try {
    json doc = json::parse(raw);
    json projects = jval(doc, "projects", json::object());
    if (!projects.is_object()) return map;
    for (auto it = projects.begin(); it != projects.end(); ++it) {
      std::string enc = encodeProjectDir(it.key());
      std::transform(enc.begin(), enc.end(), enc.begin(),
                     [](unsigned char c) { return (char)std::tolower(c); });
      map[enc] = it.key();
    }
  } catch (const std::exception& e) {
    logf(std::string("解析 .claude.json 失败: ") + e.what());
  }
  return map;
}

struct RawSession {
  std::string tool;
  std::string cwd;
  std::string sessionId;
  std::string title;
  int64_t mtime = 0;
};

// claude 那边存的是正斜杠路径（D:/AI/xxx），codebuddy 是反斜杠，
// 不统一就会把同一个目录算成两个工作区
std::string normalizePath(const std::string& p) {
  std::string out = p;
  for (auto& c : out) {
    if (c == '/') c = '\\';
  }
  while (out.size() > 3 && out.back() == '\\') out.pop_back();
  return out;
}

std::string lowerKey(const std::string& p) {
  std::string k = normalizePath(p);
  std::transform(k.begin(), k.end(), k.begin(),
                 [](unsigned char c) { return (char)std::tolower(c); });
  return k;
}

void collectTool(const std::string& tool, const std::string& rootUtf8, int64_t cutoff,
                 const std::unordered_map<std::string, std::string>& claudeMap,
                 std::vector<RawSession>& out, bool* found) {
  std::wstring root = utf8ToWide(rootUtf8);
  bool isDir = false;
  if (!pathExists(root, &isDir) || !isDir) {
    if (found) *found = false;
    return;
  }
  if (found) *found = true;

  for (const auto& projDir : listDir(root)) {
    std::wstring projPath = root + L"\\" + projDir;
    bool projIsDir = false;
    if (!pathExists(projPath, &projIsDir) || !projIsDir) continue;

    std::string mapped;
    if (tool == "claude" && !claudeMap.empty()) {
      std::string key = wideToUtf8(projDir);
      std::transform(key.begin(), key.end(), key.begin(),
                     [](unsigned char c) { return (char)std::tolower(c); });
      auto it = claudeMap.find(key);
      if (it != claudeMap.end()) mapped = it->second;
    }

    for (const auto& fname : listDir(projPath)) {
      std::wstring wf = fname;
      if (wf.size() < 6) continue;
      if (wf.compare(wf.size() - 6, 6, L".jsonl") != 0) continue;

      std::wstring full = projPath + L"\\" + wf;
      WIN32_FILE_ATTRIBUTE_DATA fad{};
      if (!GetFileAttributesExW(full.c_str(), GetFileExInfoStandard, &fad)) continue;
      if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

      int64_t mtime = fileTimeToMs(fad.ftLastWriteTime);
      if (cutoff > 0 && mtime < cutoff) continue;

      Sniffed sn = sniffSession(full, mapped);
      if (sn.cwd.empty() || sn.empty) continue;

      std::string sid = wideToUtf8(wf);
      sid.resize(sid.size() - 6);  // 去掉 .jsonl
      out.push_back({tool, sn.cwd, sid, sn.title, mtime});
    }
  }
}

std::string baseNameOf(const std::string& p) {
  std::string s = p;
  while (!s.empty() && (s.back() == '\\' || s.back() == '/')) s.pop_back();
  size_t pos = s.find_last_of("\\/");
  std::string name = (pos == std::string::npos) ? s : s.substr(pos + 1);
  if (name.empty()) return p;

  // 常在 bin/src 这类子目录里跑 CLI，单看末级分不清是哪个项目，补上一级
  std::vector<std::string> parts;
  size_t start = 0;
  while (start <= s.size()) {
    size_t sep = s.find_first_of("\\/", start);
    if (sep == std::string::npos) {
      parts.push_back(s.substr(start));
      break;
    }
    parts.push_back(s.substr(start, sep - start));
    start = sep + 1;
  }
  if (parts.size() < 2) return name;

  static const std::set<std::string> generic = {"bin", "src", "build",  "dist",  "lib",
                                                "obj", "out", "test",   "tests", "app",
                                                "web", "docs", "public", "scripts", "tools"};
  std::string lower = name;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return (char)std::tolower(c); });
  if (name.size() <= 3 || generic.count(lower)) {
    return parts[parts.size() - 2] + "\\" + name;
  }
  return name;
}

bool excluded(const std::string& cwdLower, const std::vector<std::string>& rules) {
  for (const auto& r : rules) {
    if (r.empty()) continue;
    std::string rl = r;
    std::transform(rl.begin(), rl.end(), rl.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    if (cwdLower.find(rl) != std::string::npos) return true;
  }
  return false;
}

// ---------------- codex ----------------
// 布局是 sessions/YYYY/MM/DD/rollout-*.jsonl，日期目录比 claude 多一层。
// session_meta 里带 cwd 和 id；subagent（guardian 审查）的会话要跳过，
// 否则同一个动作会在列表里刷出好几条。
struct CodexSniff {
  std::string cwd;
  std::string sid;
  std::string title;
  bool subagent = false;
};

CodexSniff sniffCodex(const std::wstring& path) {
  CodexSniff out;
  std::string head;
  if (!readFileHead(path, kSniffBytes, head) || head.empty()) return out;

  size_t pos = 0;
  int lines = 0;
  while (pos < head.size() && lines < 400) {
    size_t nl = head.find('\n', pos);
    if (nl == std::string::npos) nl = head.size();
    std::string line = head.substr(pos, nl - pos);
    pos = nl + 1;
    ++lines;
    if (line.size() < 8) continue;

    if (out.cwd.empty() && line.find("\"session_meta\"") != std::string::npos) {
      try {
        json j = json::parse(line);
        json p = jval(j, "payload");
        if (p.is_object()) {
          out.cwd = jstr(p, "cwd");
          out.sid = jstr(p, "id");
          json src = jval(p, "source");
          if (src.is_object() && src.contains("subagent")) out.subagent = true;
          if (jstr(p, "thread_source").rfind("guardian", 0) == 0) out.subagent = true;
        }
      } catch (...) {
      }
      continue;
    }

    if (out.title.empty() && line.find("\"response_item\"") != std::string::npos &&
        line.find("\"user\"") != std::string::npos) {
      try {
        json j = json::parse(line);
        json p = jval(j, "payload");
        if (jstr(p, "type") == "message" && jstr(p, "role") == "user") {
          json content = jval(p, "content");
          if (content.is_array()) {
            for (const auto& c : content) {
              if (jstr(c, "type") != "input_text") continue;
              std::string t = jstr(c, "text");
              if (t.empty()) continue;
              // 这几类都是 CLI 塞进来的上下文，不是人说的话
              if (t[0] == '<' || t.rfind("# AGENTS.md", 0) == 0 ||
                  t.rfind("The following is", 0) == 0) {
                continue;
              }
              out.title = t;
              break;
            }
          }
        }
      } catch (...) {
      }
    }
  }
  out.title = oneLine(out.title, 48);
  return out;
}

void collectCodex(const std::string& root, int64_t cutoff, std::vector<RawSession>& out,
                  bool* found) {
  const std::wstring base = utf8ToWide(root);
  bool isDir = false;
  if (!pathExists(base, &isDir) || !isDir) return;
  if (found) *found = true;

  // session_index.jsonl 里有 codex 自己写的 thread_name（真正的标题），
  // 正文第一条往往是巨大的 AGENTS.md 注入，抓出来当标题又长又没意义
  std::unordered_map<std::string, std::string> titles;
  {
    std::ifstream f(utf8ToWide(homeDir() + "\\.codex\\session_index.jsonl"), std::ios::binary);
    std::string line;
    while (std::getline(f, line)) {
      if (line.find("\"thread_name\"") == std::string::npos) continue;
      try {
        json j = json::parse(line);
        const std::string id = jstr(j, "id");
        const std::string name = jstr(j, "thread_name");
        if (!id.empty() && !name.empty()) titles[id] = oneLine(name, 48);
      } catch (...) {
      }
    }
  }

  int taken = 0;
  for (const auto& y : listDir(base)) {
    if (y.size() != 4) continue;
    const std::wstring wy = base + L"\\" + y;
    for (const auto& m : listDir(wy)) {
      if (m.size() != 2) continue;
      const std::wstring wm = wy + L"\\" + m;
      for (const auto& d : listDir(wm)) {
        if (d.size() != 2) continue;
        const std::wstring wd = wm + L"\\" + d;
        for (const auto& f : listDir(wd)) {
          if (f.size() < 7) continue;
          if (f.compare(f.size() - 6, 6, L".jsonl") != 0) continue;

          const std::wstring full = wd + L"\\" + f;
          WIN32_FILE_ATTRIBUTE_DATA fad{};
          if (!GetFileAttributesExW(full.c_str(), GetFileExInfoStandard, &fad)) continue;
          if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
          const int64_t mtime = fileTimeToMs(fad.ftLastWriteTime);
          if (cutoff > 0 && mtime < cutoff) continue;

          std::string sid = wideToUtf8(f);
          sid.resize(sid.size() - 6);
          if (sid.size() > 36) sid = sid.substr(sid.size() - 36);  // 文件名尾部的 UUID

          CodexSniff sn = sniffCodex(full);
          if (sn.subagent || sn.cwd.empty()) continue;

          auto hit = titles.find(sid);
          const std::string title = hit != titles.end() ? hit->second : sn.title;
          if (title.empty()) continue;

          out.push_back({"codex", sn.cwd, sid, title, mtime});
          if (++taken > 800) return;  // 会话再多也不至于让人翻八百条
        }
      }
    }
  }
  logf("codex sessions=" + std::to_string(taken));
}

// kimi 给的标题有时是「提示词 + K Kimi Thinking <推理过程>」拼起来的，
// 直接截 48 字会把 "K Kimi Thinking" 一起带出来。在第一个标记处切断
std::string cleanKimiTitle(std::string t) {
  static const char* marks[] = {" Kimi Thinking", " K Kimi", "Thinking\nThe user"};
  for (const char* m : marks) {
    const size_t at = t.find(m);
    if (at != std::string::npos && at > 0) t.resize(at);
  }
  while (!t.empty() && (t.back() == ' ' || t.back() == '\n' || t.back() == '\r')) t.pop_back();
  if (t.size() > 2 && t[t.size() - 1] == 'K' && t[t.size() - 2] == ' ') t.resize(t.size() - 2);
  return t;
}

// kimi：sessions/<工作区>/session_<uuid>/{state.json, agents/main/wire.jsonl}
// 会话 id 就是目录名（kimi -S 直接认），state.json 里有 cwd / 标题 / 归档标记
void collectKimi(const std::string& root, int64_t cutoff, std::vector<RawSession>& out,
                 bool* found) {
  const std::wstring base = utf8ToWide(root);
  bool isDir = false;
  if (!pathExists(base, &isDir) || !isDir) return;
  if (found) *found = true;

  int taken = 0;
  int skippedArchived = 0;
  for (const auto& wd : listDir(base)) {
    const std::wstring wdp = base + L"\\" + wd;
    for (const auto& sess : listDir(wdp)) {
      if (sess.rfind(L"session_", 0) != 0) continue;
      const std::wstring sd = wdp + L"\\" + sess;
      // 只开过没说过话的会话没有正文，列出来是噪音
      if (!pathExists(sd + L"\\agents\\main\\wire.jsonl", nullptr)) continue;

      std::ifstream f(sd + L"\\state.json", std::ios::binary);
      if (!f) continue;
      std::stringstream ss;
      ss << f.rdbuf();
      json st;
      try {
        st = json::parse(ss.str());
      } catch (...) {
        continue;
      }
      if (!st.is_object()) continue;
      // 归档的会话 kimi 自己默认也不列
      json arch = jval(st, "archived");
      if (arch.is_boolean() && arch.get<bool>()) {
        ++skippedArchived;
        continue;
      }

      const std::string cwd = jstr(st, "cwd");
      if (cwd.empty()) continue;
      std::string title = jstr(st, "title");
      if (title.empty()) title = jstr(st, "lastPrompt");
      if (title.empty()) title = "(未命名会话)";

      int64_t mtime = 0;
      json up = jval(st, "updatedAt");
      if (up.is_number()) mtime = up.get<int64_t>();
      if (mtime <= 0) {
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExW((sd + L"\\state.json").c_str(), GetFileExInfoStandard, &fad)) {
          mtime = fileTimeToMs(fad.ftLastWriteTime);
        }
      }
      if (cutoff > 0 && mtime < cutoff) continue;

      out.push_back({"kimi", cwd, wideToUtf8(sess), oneLine(cleanKimiTitle(title), 48), mtime});
      if (++taken > 800) return;
    }
  }
  logf("kimi sessions=" + std::to_string(taken) +
       " archived=" + std::to_string(skippedArchived));
}

}  // namespace

std::string encodeProjectDir(const std::string& p) {
  std::string out;
  out.reserve(p.size());
  for (size_t i = 0; i < p.size();) {
    unsigned char c = static_cast<unsigned char>(p[i]);
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
      out += static_cast<char>(c);
      ++i;
      continue;
    }
    // 一个完整 UTF-8 码点整体换成一个 '-'，否则含中文的路径会被拆成多个 '-'
    size_t len = 1;
    if ((c & 0xE0) == 0xC0) len = 2;
    else if ((c & 0xF0) == 0xE0) len = 3;
    else if ((c & 0xF8) == 0xF0) len = 4;
    if (i + len > p.size()) len = p.size() - i;
    out += '-';
    i += len;
  }
  return out;
}

ScanResult scanAll(const ScanOptions& opts) {
  int64_t t0 = nowMs();
  ScanResult res;
  res.codebuddyRoot = homeDir() + "\\.codebuddy\\projects";
  res.claudeRoot = homeDir() + "\\.claude\\projects";
  res.codexRoot = homeDir() + "\\.codex\\sessions";
  res.kimiRoot = homeDir() + "\\.kimi-code\\sessions";

  int64_t cutoff = opts.maxAgeHours > 0 ? t0 - (int64_t)opts.maxAgeHours * 3600LL * 1000LL : 0;

  std::vector<RawSession> rows;
  if (opts.includeCodebuddy) {
    collectTool("codebuddy", res.codebuddyRoot, cutoff, {}, rows, &res.codebuddyFound);
  }
  if (opts.includeClaude) {
    auto map = loadClaudeCwdMap();
    collectTool("claude", res.claudeRoot, cutoff, map, rows, &res.claudeFound);
  }
  if (opts.includeCodex) {
    collectCodex(res.codexRoot, cutoff, rows, &res.codexFound);
  }
  if (opts.includeKimi) {
    collectKimi(res.kimiRoot, cutoff, rows, &res.kimiFound);
  }
  res.totalSessions = static_cast<int>(rows.size());

  // 按目录聚合成工作区，目录下的每个会话都留着，界面上可以直接点进去
  std::map<std::string, Workspace> byCwd;
  for (const auto& r : rows) {
    std::string key = lowerKey(r.cwd);
    if (excluded(key, opts.exclude)) continue;

    auto it = byCwd.find(key);
    if (it == byCwd.end()) {
      Workspace ws;
      ws.cwd = normalizePath(r.cwd);
      ws.name = baseNameOf(ws.cwd);
      it = byCwd.emplace(key, std::move(ws)).first;
    }
    Workspace& ws = it->second;
    ws.sessions.push_back({r.tool, r.sessionId, r.mtime, r.title});
    ws.sessionCount += 1;
    ws.lastActive = (std::max)(ws.lastActive, r.mtime);
  }

  for (auto& kv : byCwd) {
    Workspace& ws = kv.second;
    // 目录已经没了就别再拉起来
    bool isDir = false;
    if (!pathExists(utf8ToWide(ws.cwd), &isDir) || !isDir) continue;
    std::sort(ws.sessions.begin(), ws.sessions.end(),
              [](const SessionRef& a, const SessionRef& b) { return a.mtime > b.mtime; });
    if (ws.sessions.size() > kMaxSessionsPerWs) ws.sessions.resize(kMaxSessionsPerWs);
    res.workspaces.push_back(std::move(ws));
  }

  std::sort(res.workspaces.begin(), res.workspaces.end(),
            [](const Workspace& a, const Workspace& b) { return a.lastActive > b.lastActive; });

  // 界面上的「共 N 个对话」要和列表里能点开的数量对得上，
  // 目录已删除的那些会话不算进去
  int shown = 0;
  for (const auto& w : res.workspaces) shown += w.sessionCount;
  res.totalSessions = shown;

  res.elapsedMs = nowMs() - t0;
  return res;
}

}  // namespace crg
