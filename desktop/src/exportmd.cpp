#include "exportmd.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>

#include "json.hpp"

namespace crg {
namespace {

struct Turn {
  bool assistant = false;
  std::string body;
  int64_t ts = 0;
};

bool isNoise(const std::string& t) {
  size_t i = 0;
  while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r')) ++i;
  return i >= t.size() || t[i] == '<';
}

std::string trim(const std::string& s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

int64_t isoToMs(const std::string& s) {
  if (s.size() < 19) return 0;
  int Y = 0, M = 0, D = 0, h = 0, m = 0, sec = 0;
  if (std::sscanf(s.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &Y, &M, &D, &h, &m, &sec) != 6) return 0;
  std::tm tm{};
  tm.tm_year = Y - 1900;
  tm.tm_mon = M - 1;
  tm.tm_mday = D;
  tm.tm_hour = h;
  tm.tm_min = m;
  tm.tm_sec = sec;
  return static_cast<int64_t>(_mkgmtime(&tm)) * 1000;
}

int64_t tsOf(const json& j) {
  json t = jval(j, "timestamp");
  if (t.is_number()) return t.get<int64_t>();
  if (t.is_string()) return isoToMs(t.get<std::string>());
  return 0;
}

std::string fmtTime(int64_t ms) {
  if (ms <= 0) return "";
  const std::time_t t = static_cast<std::time_t>(ms / 1000);
  std::tm local{};
  localtime_s(&local, &t);
  char buf[32] = {0};
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &local);
  return buf;
}

std::string oneLine(const std::string& s, size_t maxBytes) {
  std::string o;
  o.reserve(s.size() < maxBytes ? s.size() : maxBytes);
  bool space = false;
  for (char c : s) {
    if (o.size() >= maxBytes) {
      o += "…";
      break;
    }
    const unsigned char u = static_cast<unsigned char>(c);
    if (u == '\n' || u == '\r' || u == '\t') {
      space = true;
      continue;
    }
    if (space && !o.empty()) o += ' ';
    space = false;
    o += (c == '`') ? '\'' : c;
  }
  return o;
}

std::string sanitize(const std::string& s) {
  std::string o;
  for (char c : s) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20 || std::strchr("\\/:*?\"<>|", c)) {
      o += '_';
    } else {
      o += c;
    }
    if (o.size() >= 90) break;  // 文件名别太长，中文一个字三字节
  }
  o = trim(o);
  while (!o.empty() && (o.back() == ' ' || o.back() == '.')) o.pop_back();
  return o.empty() ? std::string("session") : o;
}

std::wstring findSessionFile(const std::string& tool, const std::string& sid) {
  const wchar_t* root = (tool == "claude") ? L"\\.claude\\projects" : L"\\.codebuddy\\projects";
  const std::wstring base = utf8ToWide(homeDir()) + root;
  const std::wstring want = utf8ToWide(sid) + L".jsonl";

  WIN32_FIND_DATAW fd{};
  HANDLE h = FindFirstFileW((base + L"\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return L"";
  std::wstring found;
  do {
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
    if (fd.cFileName[0] == L'.') continue;
    const std::wstring cand = base + L"\\" + fd.cFileName + L"\\" + want;
    if (GetFileAttributesW(cand.c_str()) != INVALID_FILE_ATTRIBUTES) {
      found = cand;
      break;
    }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return found;
}

std::string readFileUtf8(const std::wstring& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return "";
  std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  return s;
}

void appendTool(std::string& body, const std::string& name, const std::string& args) {
  if (!body.empty() && body.back() != '\n') body += "\n\n";
  body += "> 工具 `" + (name.empty() ? std::string("unknown") : name) + "`：" +
          (args.empty() ? std::string("(无参数)") : "`" + oneLine(args, 300) + "`");
  body += "\n";
}

}  // namespace

ExportResult exportSession(const std::string& tool, const std::string& sid, const std::string& cwd,
                           const std::string& title) {
  ExportResult res;
  if (sid.empty()) {
    res.error = "缺少会话 id";
    return res;
  }
  const std::wstring file = findSessionFile(tool, sid);
  if (file.empty()) {
    res.error = "找不到这个会话的源文件（可能已被清理）";
    return res;
  }

  std::string raw = readFileUtf8(file);
  if (raw.empty()) {
    res.error = "会话文件是空的";
    return res;
  }

  std::vector<Turn> turns;
  std::string sessionTitle;
  std::string sessionCwd;
  std::string lastUid;
  int64_t firstTs = 0;
  int64_t lastTs = 0;

  size_t pos = 0;
  while (pos < raw.size()) {
    size_t nl = raw.find('\n', pos);
    if (nl == std::string::npos) nl = raw.size();
    std::string line = raw.substr(pos, nl - pos);
    pos = nl + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.size() < 8) continue;

    json j;
    try {
      j = json::parse(line);
    } catch (...) {
      continue;
    }
    if (!j.is_object()) continue;

    const std::string t = jstr(j, "type");
    if (t == "ai-title") {
      std::string a = jstr(j, "aiTitle");
      if (!a.empty()) sessionTitle = a;
      continue;
    }
    if (sessionCwd.empty()) sessionCwd = jstr(j, "cwd");

    // claude：type 就是角色，正文在 message.content 的块里
    if (t == "user" || t == "assistant") {
      json msg = jval(j, "message");
      if (!msg.is_object()) continue;
      json content = jval(msg, "content");
      json side = jval(j, "isSidechain");
      if (side.is_boolean() && side.get<bool>()) continue;

      const bool assistant = (t == "assistant");
      const std::string uid = jstr(msg, "id");
      const int64_t ts = tsOf(j);

      std::string text;
      std::string tools;
      auto takeBlocks = [&](const json& blocks) {
        if (blocks.is_array()) {
          for (const auto& b : blocks) {
            const std::string bt = jstr(b, "type");
            if (bt == "text") {
              std::string x = jstr(b, "text");
              if (!x.empty()) text += (text.empty() ? "" : "\n\n") + x;
            } else if (bt == "tool_use") {
              json in = jval(b, "input");
              appendTool(tools, jstr(b, "name"), in.is_null() ? "" : in.dump());
            }
          }
        } else if (blocks.is_string()) {
          text = blocks.get<std::string>();
        }
      };
      takeBlocks(content);

      if (assistant) {
        if (text.empty() && tools.empty()) continue;
        // 同一条回复会被拆成多行写（claude 每个 block 一行），按 message id 合并；
        // 只有工具调用、没有正文的片段也并进上一条，否则稿子里全是孤零零的「## 助手」
        const bool sameMsg = !uid.empty() && uid == lastUid && !turns.empty() && turns.back().assistant;
        const bool onlyTools = text.empty() && !turns.empty() && turns.back().assistant;
        if (sameMsg || onlyTools) {
          Turn& back = turns.back();
          if (!text.empty()) back.body += (back.body.empty() ? "" : "\n\n") + text;
          if (!tools.empty()) back.body += (back.body.empty() ? "" : "\n\n") + tools;
        } else {
          Turn tn;
          tn.assistant = true;
          tn.ts = ts;
          tn.body = text;
          if (!tools.empty()) tn.body += (tn.body.empty() ? "" : "\n\n") + tools;
          turns.push_back(std::move(tn));
        }
        lastUid = uid;
      } else {
        if (!text.empty() && !isNoise(text)) {
          Turn tn;
          tn.assistant = false;
          tn.ts = ts;
          tn.body = trim(text);
          turns.push_back(std::move(tn));
          lastUid.clear();
        }
      }
      if (ts > 0) {
        if (!firstTs) firstTs = ts;
        lastTs = ts;
      }
      continue;
    }

    // codebuddy：type=message 是正文，function_call 是工具调用
    if (t == "message") {
      std::string role = jstr(j, "role");
      json content = jval(j, "content");
      std::string text;
      if (content.is_array()) {
        for (const auto& b : content) {
          const std::string bt = jstr(b, "type");
          if (bt == "input_text" || bt == "output_text" || bt == "text") {
            std::string x = jstr(b, "text");
            if (!x.empty()) text += (text.empty() ? "" : "\n\n") + x;
          }
        }
      } else if (content.is_string()) {
        text = content.get<std::string>();
      }
      if (text.empty()) continue;
      if (role == "user" && isNoise(text)) continue;

      Turn tn;
      tn.assistant = (role != "user");
      tn.ts = tsOf(j);
      tn.body = trim(text);
      turns.push_back(std::move(tn));
      lastUid.clear();
      if (tn.ts > 0) {
        if (!firstTs) firstTs = tn.ts;
        lastTs = tn.ts;
      }
      continue;
    }

    if (t == "function_call") {
      std::string name = jstr(j, "name");
      json args = jval(j, "arguments");
      std::string argStr = args.is_string() ? args.get<std::string>() : args.dump();
      std::string body;
      appendTool(body, name, argStr);
      body = trim(body);

      // 连续的调用并进上一条助手消息里，读起来是一段动作而不是一串孤立引用
      if (!turns.empty() && turns.back().assistant) {
        turns.back().body += "\n\n" + body;
      } else {
        Turn tn;
        tn.assistant = true;
        tn.ts = tsOf(j);
        tn.body = body;
        turns.push_back(std::move(tn));
      }
      const int64_t ts = tsOf(j);
      if (ts > 0) {
        if (!firstTs) firstTs = ts;
        lastTs = ts;
      }
    }
  }

  if (turns.empty()) {
    res.error = "这个会话没有可导出的正文";
    return res;
  }

  const std::string headTitle = !sessionTitle.empty() ? sessionTitle : (title.empty() ? "会话记录" : title);
  const std::string dir = sessionCwd.empty() ? cwd : sessionCwd;

  std::string md;
  md += "# " + headTitle + "\n\n";
  md += "- 目录：`" + (dir.empty() ? std::string("未知") : dir) + "`\n";
  md += "- 工具：" + (tool.empty() ? std::string("unknown") : tool) + "\n";
  md += "- 会话：`" + sid + "`\n";
  if (firstTs) {
    md += "- 时间：" + fmtTime(firstTs) + " → " + fmtTime(lastTs) + "\n";
  }
  md += "- 消息：" + std::to_string(turns.size()) + " 条\n";
  md += "- 导出：" + fmtTime(nowMs()) + " · cli-resume\n\n---\n";
  for (const auto& t : turns) {
    md += t.assistant ? "\n## 助手\n\n" : "\n## 你\n\n";
    md += t.body + "\n";
  }

  // 落到「文档\cli-resume\」，一个会话一个文件，重名加序号
  std::string outDir = homeDir() + "\\Documents\\cli-resume";
  if (!CreateDirectoryA(outDir.c_str(), nullptr) &&
      GetLastError() != ERROR_ALREADY_EXISTS) {
    outDir = homeDir() + "\\cli-resume-exports";
    CreateDirectoryA(outDir.c_str(), nullptr);
  }

  std::string base = sanitize(title.empty() ? sessionTitle : title);
  if (base == "session" && !sessionTitle.empty()) base = sanitize(sessionTitle);
  const std::string stamp = [&] {
    const std::time_t t = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &t);
    char buf[32] = {0};
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M", &local);
    return std::string(buf);
  }();

  std::string path = outDir + "\\" + base + "-" + stamp + ".md";
  for (int i = 2; i < 100; ++i) {
    if (GetFileAttributesW(utf8ToWide(path).c_str()) == INVALID_FILE_ATTRIBUTES) break;
    path = outDir + "\\" + base + "-" + stamp + "-" + std::to_string(i) + ".md";
  }

  std::ofstream f(utf8ToWide(path), std::ios::binary | std::ios::trunc);
  if (!f) {
    res.error = "写文件失败（目录不可写）：" + path;
    return res;
  }
  f << md;
  f.close();

  WIN32_FILE_ATTRIBUTE_DATA fad{};
  if (GetFileAttributesExW(utf8ToWide(path).c_str(), GetFileExInfoStandard, &fad)) {
    res.bytes =
        static_cast<int64_t>((static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow);
  }
  res.ok = true;
  res.path = path;
  res.turns = static_cast<int>(turns.size());
  logf("export ok " + path + " turns=" + std::to_string(res.turns));
  return res;
}

}  // namespace crg
