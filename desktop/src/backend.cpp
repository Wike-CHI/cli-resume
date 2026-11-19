#include "backend.h"

#include "autostart.h"

#include <algorithm>
#include <cstdlib>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>
#include <thread>

#include <shobjidl.h>

#include "launcher.h"
#include "scanner.h"

namespace crg {
namespace {

const char* mimeOf(const std::string& ext) {
  if (ext == "html" || ext == "htm") return "text/html; charset=utf-8";
  if (ext == "css") return "text/css; charset=utf-8";
  if (ext == "js") return "application/javascript; charset=utf-8";
  if (ext == "json") return "application/json; charset=utf-8";
  if (ext == "svg") return "image/svg+xml";
  if (ext == "png") return "image/png";
  if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
  if (ext == "ico") return "image/x-icon";
  if (ext == "woff2") return "font/woff2";
  return "application/octet-stream";
}

bool readWholeFile(const std::string& path, std::string& out) {
  std::ifstream f(utf8ToWide(path), std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

json defaultSettings() {
  return json{
      {"maxAgeHours", 24 * 30},
      {"openTwo", false},
      {"useContinue", false},
      {"window", 0},
      {"theme", "neon"},  // "neon"（80s CRT）| "win98"
      {"termMode", "embedded"},  // "embedded" | "external"（Windows Terminal）| "term"（独立终端窗口）
      {"restoreLayout", true},   // 启动时按上次的窗格铺开并自动 --resume
      {"llmUrl", "http://127.0.0.1:11434"},  // 本地模型地址（Ollama / OpenAI 兼容都认）
      {"llmModel", ""},                       // 留空自动挑（优先 qwen 系）
      {"exclude", json::array({"\\temp\\", "\\system32", "\\node_modules\\", "\\appdata\\local\\temp"})},
  };
}

// 数一下某个目录下（限定层数）指定后缀的文件。探测阶段只数文件，不解析内容
int countFilesDeep(const std::wstring& dir, int depth, const wchar_t* ext) {
  int n = 0;
  WIN32_FIND_DATAW fd{};
  HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return 0;
  do {
    const std::wstring name = fd.cFileName;
    if (name == L"." || name == L"..") continue;
    const std::wstring full = dir + L"\\" + name;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      if (depth > 0) n += countFilesDeep(full, depth - 1, ext);
      continue;
    }
    const size_t el = wcslen(ext);
    if (name.size() > el && name.compare(name.size() - el, el, ext) == 0) ++n;
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return n;
}

int countLines(const std::wstring& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return 0;
  int n = 0;
  std::string line;
  while (std::getline(f, line)) {
    if (!line.empty()) ++n;
  }
  return n;
}

// qoder 的会话是 projects/<项目>/<sessionId>/state.json，数 state.json 就行
int countQoderSessions(const std::wstring& root) {
  int n = 0;
  WIN32_FIND_DATAW fd{};
  HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return 0;
  do {
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
    if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
    n += countFilesDeep(root + L"\\" + fd.cFileName, 1, L".json");
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return n;
}

bool dirExists(const std::wstring& p) {
  const DWORD a = GetFileAttributesW(p.c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// 本机装了哪些 coding agent、各自有多少会话。已接入的会标 full，其余只报数量
json detectAgents() {
  const std::wstring home = utf8ToWide(homeDir());
  json out = json::array();

  struct Row {
    const char* id;
    const char* name;
    const char* support;
    std::wstring dir;
    int sessions;
  };
  const Row rows[] = {
      {"codebuddy", "CodeBuddy CLI", "full", home + L"\\.codebuddy\\projects",
       countFilesDeep(home + L"\\.codebuddy\\projects", 1, L".jsonl")},
      {"claude", "Claude Code", "full", home + L"\\.claude\\projects",
       countFilesDeep(home + L"\\.claude\\projects", 1, L".jsonl")},
      // codex 的会话文件里混着大量 subagent（guardian）rollout，数文件会虚高；
      // session_index 里才是用户真正开过的线程
      {"codex", "Codex CLI", "full", home + L"\\.codex\\sessions",
       countLines(home + L"\\.codex\\session_index.jsonl")},
      {"kimi", "Kimi CLI", "full", home + L"\\.kimi-code\\sessions",
       countFilesDeep(home + L"\\.kimi-code\\sessions", 2, L"state.json")},
      {"qoder", "Qoder", "detect", home + L"\\.qoder\\projects",
       countQoderSessions(home + L"\\.qoder\\projects")},
      {"qwen", "Qwen Code", "detect", home + L"\\.qwen", 0},
  };
  for (const auto& r : rows) {
    const bool found = dirExists(r.dir);
    if (!found) continue;
    out.push_back({{"id", r.id},
                   {"name", r.name},
                   {"support", r.support},
                   {"found", true},
                   {"sessions", r.sessions}});
  }
  return json{{"ok", true}, {"agents", out}};
}

bool spawnWindow(const std::string& slug, const std::string& extraArgs, std::string& err);

bool spawnWorkspaceWindow(const std::string& slug, const std::string& cwd, std::string& err) {
  return spawnWindow(slug, "--only \"" + cwd + "\"", err);
}

bool spawnWindow(const std::string& slug, const std::string& extraArgs, std::string& err) {
  wchar_t exe[MAX_PATH] = {0};
  if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
    err = "拿不到自己的路径";
    return false;
  }
  std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --win " + utf8ToWide(slug) + L" " +
                     utf8ToWide(extraArgs);
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
    err = "创建窗口失败（" + std::to_string(GetLastError()) + "）";
    return false;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  logf("spawn window slug=" + slug + " args=" + extraArgs);
  return true;
}



// 系统剪贴板：WebView2 里 navigator.clipboard 在没有用户手势时会被拒绝，
// 而桌面应用本来就能直接读写剪贴板，走 Win32 更稳
std::string clipboardGet() {
  std::string out;
  for (int i = 0; i < 5; ++i) {  // 剪贴板可能被别的程序短暂占用
    if (!OpenClipboard(nullptr)) {
      Sleep(20);
      continue;
    }
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
      const auto* w = static_cast<const wchar_t*>(GlobalLock(h));
      if (w) {
        out = wideToUtf8(w);
        GlobalUnlock(h);
      }
    }
    CloseClipboard();
    break;
  }
  return out;
}

bool clipboardSet(const std::string& text) {
  const std::wstring w = utf8ToWide(text);
  for (int i = 0; i < 5; ++i) {
    if (!OpenClipboard(nullptr)) {
      Sleep(20);
      continue;
    }
    EmptyClipboard();
    const size_t bytes = (w.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!h) {
      CloseClipboard();
      return false;
    }
    void* p = GlobalLock(h);
    memcpy(p, w.c_str(), bytes);
    GlobalUnlock(h);
    SetClipboardData(CF_UNICODETEXT, h);  // 成功后所有权归系统，不要 GlobalFree
    CloseClipboard();
    return true;
  }
  return false;
}

// httplib 的 bind 带 SO_REUSEADDR，Windows 上这会让第二个进程「绑定成功」同一个端口，
// 「绑不上就换下一个」的逻辑根本不会触发 —— 两个窗口于是抢同一个端口，页面会连到
// 别人的后端去。所以先用 connect 探一下，有人听就直接跳过
bool portInUse(int p) {
  SOCKET sk = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sk == INVALID_SOCKET) return false;
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<u_short>(p));
  a.sin_addr.s_addr = inet_addr("127.0.0.1");
  const bool used = ::connect(sk, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0;
  closesocket(sk);
  return used;
}

// 环境变量里的代理指向一个没人监听的端口时，终端里的 claude/codebuddy 会报
// "Connection refused"，可那行错误出现在终端里，很难让人联想到是环境变量过期。
// 这里主动连一下，把结论摆到诊断区
json proxyStatus() {
  const char* keys[] = {"HTTPS_PROXY", "https_proxy", "HTTP_PROXY", "http_proxy",
                        "ALL_PROXY", "all_proxy"};
  std::string raw;
  for (const char* k : keys) {
    if (const char* v = std::getenv(k)) {
      if (*v) {
        raw = v;
        break;
      }
    }
  }
  json out = {{"set", false}, {"url", ""}, {"alive", true}};
  if (raw.empty()) return out;
  out["set"] = true;
  out["url"] = raw;

  std::string host = raw;
  size_t p = host.find("://");
  if (p != std::string::npos) host = host.substr(p + 3);
  p = host.find('@');  // 去掉 user:pass@
  if (p != std::string::npos) host = host.substr(p + 1);
  p = host.find('/');
  if (p != std::string::npos) host = host.substr(0, p);

  int port = 0;
  const size_t colon = host.find_last_of(':');
  if (colon != std::string::npos) {
    port = std::atoi(host.c_str() + colon + 1);
    host = host.substr(0, colon);
  }
  if (port <= 0) port = 80;
  out["host"] = host;
  out["port"] = port;

  bool alive = false;
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) == 0 && res) {
    SOCKET sk = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sk != INVALID_SOCKET) {
      u_long nb = 1;
      ioctlsocket(sk, FIONBIO, &nb);
      ::connect(sk, res->ai_addr, static_cast<int>(res->ai_addrlen));
      fd_set wf;
      FD_ZERO(&wf);
      FD_SET(sk, &wf);
      timeval tv{0, 250000};
      if (select(0, nullptr, &wf, nullptr, &tv) > 0) {
        int err = 0;
        int len = sizeof(err);
        if (getsockopt(sk, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len) == 0 &&
            err == 0) {
          alive = true;
        }
      }
      closesocket(sk);
    }
    freeaddrinfo(res);
  }
  out["alive"] = alive;
  return out;
}

// ---------------- 工作空间（用户手动添加的目录） ----------------
// 扫描只能发现「有过会话的目录」。想在一个全新的仓库里开第一场对话，
// 就得先把目录本身登记下来 —— 存 workspaces.json，和收藏一样放 dataDir，跨窗口共享。

std::string normDir(std::string p) {
  for (auto& c : p) {
    if (c == '/') c = '\\';
  }
  while (p.size() > 3 && p.back() == '\\') p.pop_back();
  // 盘符统一大写，否则 D:\x 和 d:\x 会被当成两个工作区
  if (p.size() >= 2 && p[1] == ':') {
    p[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(p[0])));
  }
  return p;
}

std::string leafOf(const std::string& p) {
  const size_t at = p.find_last_of("\\/");
  return at == std::string::npos ? p : p.substr(at + 1);
}

std::string keyOfDir(const std::string& p) {
  std::string k = normDir(p);
  std::transform(k.begin(), k.end(), k.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return k;
}

bool dirExistsUtf8(const std::string& p) {
  const DWORD a = GetFileAttributesW(utf8ToWide(p).c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

json loadExtraWs(const std::string& path) {
  std::ifstream f(utf8ToWide(path), std::ios::binary);
  if (!f) return json::array();
  std::stringstream ss;
  ss << f.rdbuf();
  try {
    json j = json::parse(ss.str());
    if (j.is_array()) return j;
    if (j.is_object() && j.contains("items") && j["items"].is_array()) return j["items"];
  } catch (...) {
  }
  return json::array();
}

bool saveExtraWs(const std::string& path, const json& arr) {
  std::ofstream f(utf8ToWide(path + ".tmp"), std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f << dumpJson(json{{"items", arr}});
  f.close();
  return MoveFileExW(utf8ToWide(path + ".tmp").c_str(), utf8ToWide(path).c_str(),
                     MOVEFILE_REPLACE_EXISTING) != FALSE;
}

// 「这个目录上次用的是哪家 CLI」：一个 cwd → tool 的字典。
// 只是新会话菜单的默认高亮，丢了不影响任何功能，所以坏了就当没有
json loadLastTool(const std::string& path) {
  std::ifstream f(utf8ToWide(path), std::ios::binary);
  if (!f) return json::object();
  std::stringstream ss;
  ss << f.rdbuf();
  try {
    json j = json::parse(ss.str());
    if (j.is_object() && j.contains("items") && j["items"].is_object()) return j["items"];
  } catch (...) {
  }
  return json::object();
}

bool saveLastTool(const std::string& path, const json& map) {
  std::ofstream f(utf8ToWide(path + ".tmp"), std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f << dumpJson(json{{"items", map}});
  f.close();
  return MoveFileExW(utf8ToWide(path + ".tmp").c_str(), utf8ToWide(path).c_str(),
                     MOVEFILE_REPLACE_EXISTING) != FALSE;
}

// 系统文件夹选择框。跑在自己的 STA 线程里：httplib 的工作线程不一定初始化过 COM，
// 而且模态框会阻塞，不能占着请求线程。
std::string pickFolder() {
  std::string result;
  std::thread t([&result] {
    const HRESULT hrInit =
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&dlg)))) {
      DWORD opts = 0;
      dlg->GetOptions(&opts);
      dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
      dlg->SetTitle(L"选一个目录作为工作空间");
      if (SUCCEEDED(dlg->Show(nullptr))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
          PWSTR p = nullptr;
          if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
            result = wideToUtf8(p);
            CoTaskMemFree(p);
          }
          item->Release();
        }
      }
      dlg->Release();
    }
    if (SUCCEEDED(hrInit)) CoUninitialize();
  });
  t.join();
  return result;
}

}  // namespace

Backend::Backend(std::string webRoot, std::string profile) : webRoot_(std::move(webRoot)) {
  const char* la = std::getenv("LOCALAPPDATA");
  std::string base = la && *la ? std::string(la) : (homeDir() + "\\AppData\\Local");
  dataDir_ = base + "\\CliResumeGui";
  settingsPath_ = dataDir_ + "\\settings.json";
  CreateDirectoryA(dataDir_.c_str(), nullptr);

  // 多个窗口同时跑时，搜索索引和布局不能共用：
  // 两个进程同时重写同一个索引文件会互相截断，布局也会互相覆盖
  profile_ = std::move(profile);
  profileDir_ = dataDir_;
  if (!profile_.empty()) {
    profileDir_ = dataDir_ + "\\win-" + profile_;
    CreateDirectoryA(profileDir_.c_str(), nullptr);
  }
  layoutPath_ = profileDir_ + "\\layout.json";
}

std::string Backend::url() const {
  return "http://127.0.0.1:" + std::to_string(port_) + "/";
}

json Backend::loadSettings() const {
  json def = defaultSettings();
  std::string raw;
  if (!readWholeFile(settingsPath_, raw) || raw.empty()) return def;
  try {
    json j = json::parse(raw);
    if (!j.is_object()) return def;
    for (auto it = def.begin(); it != def.end(); ++it) {
      auto hit = j.find(it.key());
      if (hit != j.end() && !hit->is_null()) def[it.key()] = *hit;
    }
  } catch (...) {
    // 配置坏了就用默认值，不要让界面起不来
  }
  return def;
}

void Backend::saveSettings(const json& s) const {
  std::ofstream f(utf8ToWide(settingsPath_), std::ios::binary | std::ios::trunc);
  if (!f) return;
  f << dumpJson(s);
}

// 布局文件坏了就当没有：界面照常起来，只是不恢复窗格。
// 这里不做字段校验，panes 里放什么由前端解释，后端只负责搬运。
json Backend::loadLayout() const {
  json def = json{{"visible", false}, {"height", 0}, {"active", 0}, {"panes", json::array()}};
  std::string raw;
  if (!readWholeFile(layoutPath_, raw) || raw.empty()) return def;
  try {
    json j = json::parse(raw);
    if (!j.is_object()) return def;
    if (!j.contains("panes") || !j["panes"].is_array()) return def;
    return j;
  } catch (...) {
    return def;
  }
}

void Backend::saveLayout(const json& l) const {
  std::ofstream f(utf8ToWide(layoutPath_), std::ios::binary | std::ios::trunc);
  if (!f) return;
  f << dumpJson(l);
}

// 宿主在创建窗口时就要知道底色，否则深色主题启动会闪一块浅色
std::string Backend::theme() const {
  return jstr(loadSettings(), "theme", "neon");
}

json Backend::scanCached(int maxAgeHours, bool force) {
  std::lock_guard<std::mutex> lk(scanMu_);
  int64_t now = nowMs();
  // 界面刷新很频繁，扫描结果留 20 秒，避免每次点都重扫一遍
  if (!force && !cached_.empty() && cachedAge_ == maxAgeHours && now - cachedAt_ < 20000) {
    json out = cached_;
    out["cached"] = true;
    return out;
  }

  json settings = loadSettings();
  ScanOptions opts;
  opts.maxAgeHours = maxAgeHours;
  if (settings.contains("exclude") && settings["exclude"].is_array()) {
    for (const auto& e : settings["exclude"]) {
      if (e.is_string()) opts.exclude.push_back(e.get<std::string>());
    }
  }

  ScanResult r = scanAll(opts);

  json arr = json::array();
  for (const auto& ws : r.workspaces) {
    json sessions = json::array();
    for (const auto& s : ws.sessions) {
      sessions.push_back({{"tool", s.tool},
                          {"sessionId", s.sessionId},
                          {"mtime", s.mtime},
                          {"title", s.title}});
    }
    arr.push_back({{"cwd", ws.cwd},
                   {"name", ws.name},
                   {"lastActive", ws.lastActive},
                   {"sessionCount", ws.sessionCount},
                   {"sessions", sessions}});
  }

  json out = {{"workspaces", arr},
              {"stats", {{"totalSessions", r.totalSessions},
                         {"workspaces", (int)r.workspaces.size()},
                         {"elapsedMs", r.elapsedMs}}},
              {"roots", {{"codebuddy", r.codebuddyRoot},
                         {"claude", r.claudeRoot},
                         {"codebuddyFound", r.codebuddyFound},
                         {"claudeFound", r.claudeFound}}},
              {"scannedAt", nowMs()}};

  cached_ = out;
  cachedAt_ = now;
  cachedAge_ = maxAgeHours;
  out["cached"] = false;
  return out;
}

bool Backend::handleStatic(const std::string& path, httplib::Response& res) {
  std::string p = path;
  if (p == "/" || p.empty()) p = "/index.html";
  if (p.find("..") != std::string::npos) return false;

  std::string full = webRoot_ + p;
  std::replace(full.begin(), full.end(), '/', '\\');
  std::string body;
  if (!readWholeFile(full, body)) return false;

  size_t dot = p.find_last_of('.');
  std::string ext = (dot == std::string::npos) ? "" : p.substr(dot + 1);
  res.set_header("Cache-Control", "no-store");
  res.set_content(body, mimeOf(ext));
  return true;
}

void Backend::setupRoutes() {
  srv_.Get("/api/health", [](const httplib::Request&, httplib::Response& res) {
    res.set_content("{\"ok\":true}", "application/json; charset=utf-8");
  });

  srv_.Get("/api/scan", [this](const httplib::Request& req, httplib::Response& res) {
    int maxAge = 24 * 30;
    bool force = false;
    try {
      if (req.has_param("maxAge")) maxAge = std::stoi(req.get_param_value("maxAge"));
      if (req.has_param("refresh")) force = req.get_param_value("refresh") == "1";
    } catch (...) {
    }
    json out = scanCached(maxAge, force);

    // 把用户手动添加、但还没有任何会话的目录并进来：
    // 这类目录搜不到历史，却是「现在就开工」的入口，得一直留在列表里
    const std::string wsPath = dataDir_ + "\\workspaces.json";
    json extra = loadExtraWs(wsPath);
    if (extra.is_array() && !extra.empty()) {
      json& arr = out["workspaces"];
      std::set<std::string> have;
      for (const auto& w : arr) have.insert(keyOfDir(jstr(w, "cwd")));
      bool addedAny = false;
      for (const auto& e : extra) {
        const std::string cwd = normDir(jstr(e, "cwd"));
        if (cwd.empty() || have.count(keyOfDir(cwd))) continue;
        if (!dirExistsUtf8(cwd)) continue;  // 目录被删了就别再挂着
        arr.push_back({{"cwd", cwd},
                       {"name", jstr(e, "name").empty() ? leafOf(cwd) : jstr(e, "name")},
                       // 用登记时间当活跃时间，新加的会排到前面；扫描出来的目录
                       // 按真实会话时间排，两者不冲突
                       {"lastActive", jval(e, "at").is_number() ? jval(e, "at").get<int64_t>() : nowMs()},
                       {"sessionCount", 0},
                       {"sessions", json::array()},
                       {"added", true}});
        addedAny = true;
      }

      // scanAll 是按 lastActive 排好的，合并进来的在末尾。
      // 新登记的工作空间「现在就要用」，得按同样的规则重排一次才到得了前面
      if (addedAny) {
        std::sort(arr.begin(), arr.end(), [](const json& a, const json& b) {
          return a.value("lastActive", (int64_t)0) > b.value("lastActive", (int64_t)0);
        });
      }
    }

    out["settings"] = loadSettings();
    res.set_header("Cache-Control", "no-store");
    res.set_content(dumpJson(out), "application/json; charset=utf-8");
  });

  // ---------------- 工作空间与目录选择 ----------------
  // 系统文件夹选择框。返回 ok=false 表示用户取消，前端不当作错误
  srv_.Get("/api/pickdir", [](const httplib::Request&, httplib::Response& res) {
    res.set_header("Cache-Control", "no-store");
    const std::string p = pickFolder();
    res.set_content(dumpJson(json{{"ok", !p.empty()}, {"path", p}}),
                    "application/json; charset=utf-8");
  });

  srv_.Get("/api/workspaces", [this](const httplib::Request&, httplib::Response& res) {
    json out = json::array();
    for (const auto& e : loadExtraWs(dataDir_ + "\\workspaces.json")) {
      const std::string cwd = normDir(jstr(e, "cwd"));
      if (cwd.empty()) continue;
      out.push_back({{"cwd", cwd},
                     {"name", jstr(e, "name").empty() ? leafOf(cwd) : jstr(e, "name")},
                     {"exists", dirExistsUtf8(cwd)}});
    }
    res.set_header("Cache-Control", "no-store");
    res.set_content(dumpJson(json{{"ok", true}, {"items", out}}),
                    "application/json; charset=utf-8");
  });

  // 登记一个目录；create=true 时允许顺手把不存在的目录建出来
  srv_.Post("/api/workspaces", [this](const httplib::Request& req, httplib::Response& res) {
    json out = {{"ok", false}};
    try {
      json body = json::parse(req.body);
      const std::string cwd = normDir(jstr(body, "cwd"));
      const bool create = jval(body, "create").is_boolean() && jval(body, "create").get<bool>();

      bool ok = false;
      std::string err;
      if (cwd.empty()) {
        err = "路径为空";
      } else if (!dirExistsUtf8(cwd)) {
        if (create && CreateDirectoryW(utf8ToWide(cwd).c_str(), nullptr)) {
          ok = true;
        } else {
          err = create ? "创建目录失败" : "目录不存在";
        }
      } else {
        ok = true;
      }

      if (ok) {
        const std::string path = dataDir_ + "\\workspaces.json";
        json items = loadExtraWs(path);
        bool dup = false;
        for (const auto& e : items) {
          if (keyOfDir(jstr(e, "cwd")) == keyOfDir(cwd)) {
            dup = true;
            break;
          }
        }
        if (!dup) {
          items.push_back({{"cwd", cwd}, {"name", leafOf(cwd)}, {"at", nowMs()}});
          if (!saveExtraWs(path, items)) {
            ok = false;
            err = "写入 workspaces.json 失败";
          }
        }
        out["duplicate"] = dup;
      }

      out["ok"] = ok;
      out["cwd"] = cwd;
      out["name"] = leafOf(cwd);
      if (!ok) out["error"] = err;
    } catch (const std::exception& e) {
      out["error"] = e.what();
    }
    res.set_content(dumpJson(out), "application/json; charset=utf-8");
  });

  // 每个目录上次选过哪家 CLI：新会话菜单拿它当默认项
  srv_.Get("/api/lasttool", [this](const httplib::Request&, httplib::Response& res) {
    res.set_header("Cache-Control", "no-store");
    res.set_content(dumpJson(json{{"ok", true},
                                {"items", loadLastTool(dataDir_ + "\\lasttool.json")}}),
                    "application/json; charset=utf-8");
  });

  srv_.Post("/api/lasttool", [this](const httplib::Request& req, httplib::Response& res) {
    json out = {{"ok", false}};
    try {
      json body = json::parse(req.body);
      const std::string cwd = keyOfDir(jstr(body, "cwd"));
      const std::string tool = jstr(body, "tool");
      if (cwd.empty() || tool.empty()) {
        out["error"] = "缺少 cwd 或 tool";
      } else {
        const std::string path = dataDir_ + "\\lasttool.json";
        json map = loadLastTool(path);
        map[cwd] = tool;
        out["ok"] = saveLastTool(path, map);
      }
    } catch (const std::exception& e) {
      out["error"] = e.what();
    }
    res.set_content(dumpJson(out), "application/json; charset=utf-8");
  });

  // 只移除手动登记的目录；扫描出来的工作区删不掉（它由会话文件决定）
  srv_.Delete("/api/workspaces", [this](const httplib::Request& req, httplib::Response& res) {
    json out = {{"ok", false}};
    const std::string want = keyOfDir(req.has_param("cwd") ? req.get_param_value("cwd") : "");
    if (want.empty()) {
      out["error"] = "缺少 cwd";
      res.set_content(dumpJson(out), "application/json; charset=utf-8");
      return;
    }
    const std::string path = dataDir_ + "\\workspaces.json";
    json items = loadExtraWs(path);
    json kept = json::array();
    int removed = 0;
    for (const auto& e : items) {
      if (keyOfDir(jstr(e, "cwd")) == want) {
        ++removed;
        continue;
      }
      kept.push_back(e);
    }
    out["ok"] = removed > 0 && saveExtraWs(path, kept);
    out["removed"] = removed;
    res.set_content(dumpJson(out), "application/json; charset=utf-8");
  });

  // 剪贴板：终端里的复制粘贴走这里，不依赖浏览器权限
  srv_.Get("/api/clipboard", [](const httplib::Request&, httplib::Response& res) {
    res.set_header("Cache-Control", "no-store");
    res.set_content(dumpJson(json{{"ok", true}, {"text", clipboardGet()}}),
                    "application/json; charset=utf-8");
  });

  srv_.Post("/api/clipboard", [](const httplib::Request& req, httplib::Response& res) {
    json out = {{"ok", false}};
    try {
      json body = json::parse(req.body);
      out["ok"] = clipboardSet(jstr(body, "text"));
    } catch (const std::exception& e) {
      out["error"] = e.what();
    }
    res.set_content(dumpJson(out), "application/json; charset=utf-8");
  });

  // 代理体检：环境变量指向的端口有没有人在听
  srv_.Get("/api/proxy", [](const httplib::Request&, httplib::Response& res) {
    res.set_header("Cache-Control", "no-store");
    res.set_content(dumpJson(proxyStatus()), "application/json; charset=utf-8");
  });

  // 跨会话正文检索：索引在后台建，没建完时前端显示进度，不影响列表使用
  srv_.Get("/api/search", [this](const httplib::Request& req, httplib::Response& res) {
    std::string q = req.has_param("q") ? req.get_param_value("q") : "";
    int limit = 300;
    try {
      if (req.has_param("limit")) limit = std::stoi(req.get_param_value("limit"));
    } catch (...) {
    }
    SearchReply r = search_.query(q, limit);

    json hits = json::array();
    for (const auto& h : r.hits) {
      hits.push_back({{"cwd", h.cwd},
                      {"title", h.title},
                      {"tool", h.tool},
                      {"sessionId", h.sessionId},
                      {"role", h.role},
                      {"ts", h.ts},
                      {"before", h.before},
                      {"hit", h.hit},
                      {"after", h.after}});
    }
    json out = {{"ok", true},
                {"ready", r.ready},
                {"files", r.files},
                {"totalFiles", r.totalFiles},
                {"entries", r.entries},
                {"matched", r.matchedSessions},
                {"matches", r.totalMatches},
                {"elapsedMs", r.elapsedMs},
                {"hits", hits}};
    res.set_header("Cache-Control", "no-store");
    res.set_content(dumpJson(out), "application/json; charset=utf-8");
  });

  // ---------------- 本地模型复盘 ----------------
  // 会话首尾交给本机模型，产出「做了 / 卡在 / 下一步」。模型可以是 Ollama，
  // 也可以是任何 OpenAI 兼容服务（LM Studio、vLLM…），由 probe 自动判断。
  srv_.Get("/api/llm", [this](const httplib::Request& req, httplib::Response& res) {
    json s = loadSettings();
    LlmStatus st = summary_.probe(jstr(s, "llmUrl"), jstr(s, "llmModel"), req.has_param("force"));
    json out = {{"ok", st.ok},       {"api", st.api},   {"url", st.url},
                {"model", st.model}, {"models", st.models}, {"error", st.error}};
    res.set_header("Cache-Control", "no-store");
    res.set_content(dumpJson(out), "application/json; charset=utf-8");
  });

  srv_.Get("/api/summary", [this](const httplib::Request& req, httplib::Response& res) {
    std::string tool = req.has_param("tool") ? req.get_param_value("tool") : "";
    std::string sid = req.has_param("sid") ? req.get_param_value("sid") : "";
    json s = loadSettings();
    // 头部 2 条建立上下文，尾部 26 条交代进度
    SessionDigest d = search_.digest(tool, sid, 2, 26);
    json out = summary_.summarize(d, jstr(s, "llmUrl"), jstr(s, "llmModel"),
                                  req.has_param("force"));
    out["tool"] = tool;
    out["sessionId"] = sid;

    // 原始线索：模型没起来时前端拿它垫底，总比一个红叉有用
    json turns = json::array();
    const int n = static_cast<int>(d.tail.size());
    const int from = n > 8 ? n - 8 : 0;
    for (int i = from; i < n; ++i) {
      turns.push_back({{"role", d.tail[i].role},
                       {"text", utf8Truncate(d.tail[i].text, 260)},
                       {"ts", d.tail[i].ts}});
    }
    out["turns"] = turns;
    res.set_header("Cache-Control", "no-store");
    res.set_content(dumpJson(out), "application/json; charset=utf-8");
  });

  // ---------------- 统计与导出 ----------------
  // 统计只看索引里现成的用量小计，不再扫源文件；索引没建完时先给已有部分
  srv_.Get("/api/stats", [this](const httplib::Request& req, httplib::Response& res) {
    int days = 30;
    try {
      if (req.has_param("days")) days = std::stoi(req.get_param_value("days"));
    } catch (...) {
    }
    json out = buildStats(search_.usageAll(), days, search_.ready(), search_.files(),
                          search_.totalFiles());
    res.set_header("Cache-Control", "no-store");
    res.set_content(dumpJson(out), "application/json; charset=utf-8");
  });

  srv_.Post("/api/export", [this](const httplib::Request& req, httplib::Response& res) {
    json out = {{"ok", false}};
    try {
      json body = json::parse(req.body);
      ExportResult r = exportSession(jstr(body, "tool"), jstr(body, "sid"), jstr(body, "cwd"),
                                     jstr(body, "title"));
      out = {{"ok", r.ok}, {"path", r.path}, {"error", r.error},
             {"turns", r.turns}, {"bytes", r.bytes}};
    } catch (const std::exception& e) {
      out["error"] = std::string("请求解析失败：") + e.what();
    }
    res.set_header("Cache-Control", "no-store");
    res.set_content(dumpJson(out), "application/json; charset=utf-8");
  });

  // 本机装了哪些 agent（已接入的 + 只检测的），给左栏自检用
  srv_.Get("/api/agents", [](const httplib::Request&, httplib::Response& res) {
    res.set_header("Cache-Control", "no-store");
    res.set_content(dumpJson(detectAgents()), "application/json; charset=utf-8");
  });

  // ---------------- 收藏 ----------------
  // 跨窗口共享（存在 dataDir 而不是 profileDir）：主窗口里收藏的，工作区窗口也认
  srv_.Get("/api/favorites", [this](const httplib::Request&, httplib::Response& res) {
    res.set_header("Cache-Control", "no-store");
    res.set_content(dumpJson(fav_.list()), "application/json; charset=utf-8");
  });

  srv_.Post("/api/favorites/toggle", [this](const httplib::Request& req, httplib::Response& res) {
    json out = {{"ok", false}};
    try {
      json body = json::parse(req.body);
      bool on = false;
      if (fav_.toggle(jstr(body, "tool"), jstr(body, "sid"), jstr(body, "cwd"),
                      jstr(body, "title"), on)) {
        out = {{"ok", true}, {"on", on}};
      } else {
        out["error"] = "缺少会话 id";
      }
    } catch (const std::exception& e) {
      out["error"] = e.what();
    }
    res.set_content(dumpJson(out), "application/json; charset=utf-8");
  });

  // ---------------- 窗口与开机自启 ----------------
  srv_.Get("/api/autostart", [](const httplib::Request&, httplib::Response& res) {
    json out = {{"ok", true}, {"on", autostartEnabled()}};
    res.set_header("Cache-Control", "no-store");
    res.set_content(dumpJson(out), "application/json; charset=utf-8");
  });

  srv_.Post("/api/autostart", [](const httplib::Request& req, httplib::Response& res) {
    json out = {{"ok", false}};
    try {
      json body = json::parse(req.body);
      const bool on = jval(body, "on", json(false)).is_boolean()
                          ? jval(body, "on", json(false)).get<bool>()
                          : false;
      std::string err;
      if (setAutostart(on, err)) {
        out = {{"ok", true}, {"on", autostartEnabled()}};
      } else {
        out["error"] = err;
      }
    } catch (const std::exception& e) {
      out["error"] = e.what();
    }
    res.set_content(dumpJson(out), "application/json; charset=utf-8");
  });

  // 独立终端窗口：不列目录，只放终端。已经开着就把它提到前台
  srv_.Post("/api/termwin/new", [](const httplib::Request&, httplib::Response& res) {
    json out = {{"ok", false}};
    const std::wstring cls = L"CliResumeWin_term";
    if (HWND h = FindWindowW(cls.c_str(), nullptr)) {
      ShowWindow(h, IsIconic(h) ? SW_RESTORE : SW_SHOW);
      SetForegroundWindow(h);
      out = {{"ok", true}, {"focused", true}};
    } else {
      std::string err;
      if (spawnWindow("term", "--term", err)) {
        out = {{"ok", true}, {"focused", false}};
      } else {
        out["error"] = err;
      }
    }
    res.set_content(dumpJson(out), "application/json; charset=utf-8");
  });

  // 把会话送进独立终端窗口：写一份「待开清单」，再叫醒 / 拉起那扇窗。
  // 跨进程没法直接调对方的接口，用同一个 dataDir 下的文件传话最省事
  srv_.Post("/api/termwin/open", [this](const httplib::Request& req, httplib::Response& res) {
    json out = {{"ok", false}};
    try {
      json body = json::parse(req.body);
      json items = jval(body, "items", json::array());
      if (!items.is_array() || items.empty()) {
        out["error"] = "没有要打开的会话";
        res.set_content(dumpJson(out), "application/json; charset=utf-8");
        return;
      }

      const std::string dir = dataDir_ + "\win-term";
      CreateDirectoryA(dir.c_str(), nullptr);
      const std::string path = dir + "\queue.json";

      json queue = json::array();
      {
        std::ifstream in(utf8ToWide(path), std::ios::binary);
        if (in) {
          std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
          try {
            auto old = json::parse(raw);
            if (old.contains("items") && old["items"].is_array()) queue = old["items"];
          } catch (...) {
          }
        }
      }
      for (const auto& it : items) {
        const std::string tool = jstr(it, "tool");
        const std::string sid = jstr(it, "sessionId");
        queue.push_back(json{{"cwd", jstr(it, "cwd")},
                             {"tool", tool},
                             {"sessionId", sid},
                             {"title", jstr(it, "title")},
                             {"cmd", buildResumeCommand(tool, sid)}});
      }
      {
        std::ofstream f(utf8ToWide(path), std::ios::binary);
        f << dumpJson(json{{"items", queue}});
      }

      out = {{"ok", true}, {"queued", static_cast<int>(items.size())}};
      const std::wstring cls = L"CliResumeWin_term";
      if (HWND h = FindWindowW(cls.c_str(), nullptr)) {
        ShowWindow(h, IsIconic(h) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(h);
        out["focused"] = true;
      } else {
        std::string err;
        if (spawnWindow("term", "--term", err)) {
          out["focused"] = false;
        } else {
          out["error"] = err;
        }
      }
      logf("termwin open queued=" + std::to_string(items.size()));
    } catch (const std::exception& e) {
      out["error"] = std::string("请求解析失败：") + e.what();
    }
    res.set_content(dumpJson(out), "application/json; charset=utf-8");
  });

  // 终端窗口自己来取待开清单（取完即删）
  srv_.Get("/api/termwin/queue", [this](const httplib::Request&, httplib::Response& res) {
    json out = {{"ok", true}, {"items", json::array()}};
    const std::string path = dataDir_ + "\win-term\queue.json";
    std::ifstream f(utf8ToWide(path), std::ios::binary);
    if (f) {
      std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
      f.close();
      try {
        auto j = json::parse(raw);
        if (j.contains("items")) out["items"] = j["items"];
      } catch (...) {
      }
      DeleteFileW(utf8ToWide(path).c_str());
    }
    res.set_header("Cache-Control", "no-store");
    res.set_content(dumpJson(out), "application/json; charset=utf-8");
  });

  // 一个目录一个窗口：已经在跑就把那扇窗提到前台，否则起一个新进程。
  // 窗口/进程之间不共享索引，避免两个进程同时重写同一份索引文件
  srv_.Post("/api/window/new", [this](const httplib::Request& req, httplib::Response& res) {
    json out = {{"ok", false}};
    try {
      json body = json::parse(req.body);
      std::string cwd = jstr(body, "cwd");
      if (cwd.empty()) {
        out["error"] = "缺少目录";
      } else {
        std::string key = cwd;
        for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const std::string slug = shortHash(key);
        const std::wstring cls = utf8ToWide("CliResumeWin_" + slug);
        if (HWND h = FindWindowW(cls.c_str(), nullptr)) {
          ShowWindow(h, IsIconic(h) ? SW_RESTORE : SW_SHOW);
          SetForegroundWindow(h);
          out = {{"ok", true}, {"focused", true}};
        } else {
          std::string err;
          if (spawnWorkspaceWindow(slug, cwd, err)) {
            out = {{"ok", true}, {"focused", false}, {"slug", slug}};
          } else {
            out["error"] = err;
          }
        }
      }
    } catch (const std::exception& e) {
      out["error"] = e.what();
    }
    res.set_header("Cache-Control", "no-store");
    res.set_content(dumpJson(out), "application/json; charset=utf-8");
  });

  srv_.Post("/api/open", [this](const httplib::Request& req, httplib::Response& res) {
    json out;
    try {      json body = json::parse(req.body);
      json items = jval(body, "items", json::array());
      std::vector<LaunchItem> list;
      if (items.is_array()) {
        for (const auto& it : items) {
          LaunchItem li;
          li.cwd = jstr(it, "cwd");
          li.tool = jstr(it, "tool", "codebuddy");
          li.sessionId = jstr(it, "sessionId");
          li.title = jstr(it, "title");
          if (li.cwd.empty()) continue;
          if (li.title.empty()) {
            size_t pos = li.cwd.find_last_of("\\/");
            li.title = (pos == std::string::npos ? li.cwd : li.cwd.substr(pos + 1)) + " · " + li.tool;
          }
          list.push_back(li);
        }
      }
      LaunchOptions opts;
      opts.useContinue = jval(body, "useContinue", json(false)).get<bool>();
      opts.window = jval(body, "window", json(0)).get<int>();
      opts.dryRun = jval(body, "dryRun", json(false)).get<bool>();

      LaunchResult r = launch(list, opts);
      out = {{"ok", r.ok}, {"error", r.error}, {"command", r.command},
             {"count", (int)list.size()}, {"fallback", r.fallback}};
    } catch (const std::exception& e) {
      out = {{"ok", false}, {"error", std::string("请求解析失败：") + e.what()}};
    }
    res.set_content(dumpJson(out), "application/json; charset=utf-8");
  });

  srv_.Post("/api/reveal", [](const httplib::Request& req, httplib::Response& res) {
    json out = {{"ok", false}};
    try {
      json body = json::parse(req.body);
      std::string path = jstr(body, "path");
      out["ok"] = !path.empty() && revealInExplorer(path);
    } catch (...) {
    }
    res.set_content(dumpJson(out), "application/json; charset=utf-8");
  });

  srv_.Get("/api/settings", [this](const httplib::Request&, httplib::Response& res) {
    res.set_content(dumpJson(loadSettings()), "application/json; charset=utf-8");
  });

  srv_.Post("/api/settings", [this](const httplib::Request& req, httplib::Response& res) {
    json out = {{"ok", false}};
    try {
      json body = json::parse(req.body);
      json current = loadSettings();
      for (auto it = body.begin(); it != body.end(); ++it) current[it.key()] = it.value();
      saveSettings(current);
      // 设置改了，缓存作废
      {
        std::lock_guard<std::mutex> lk(scanMu_);
        cachedAt_ = 0;
      }
      out = {{"ok", true}, {"settings", current}};
    } catch (const std::exception& e) {
      out["error"] = e.what();
    }
    res.set_content(dumpJson(out), "application/json; charset=utf-8");
  });

  srv_.Post("/api/log", [](const httplib::Request& req, httplib::Response& res) {
    logf("[page] " + req.body.substr(0, 400));
    res.set_content("{}", "application/json; charset=utf-8");
  });

  // ---------------- 终端布局 ----------------
  // 前端每开/关一个窗格、拖完高度就 POST 一次（自带 350ms 防抖），
  // 下次启动读回来，原样铺开。存的是「怎么起来」而不是会话状态：
  // cmd 里已经带着 --resume，重启后还是那个会话。
  srv_.Get("/api/layout", [this](const httplib::Request&, httplib::Response& res) {
    res.set_header("Cache-Control", "no-store");
    res.set_content(dumpJson(loadLayout()), "application/json; charset=utf-8");
  });

  srv_.Post("/api/layout", [this](const httplib::Request& req, httplib::Response& res) {
    json out = {{"ok", false}};
    try {
      json body = json::parse(req.body);
      if (!body.is_object() || !body.contains("panes") || !body["panes"].is_array()) {
        out["error"] = "布局格式不对：需要 panes 数组";
      } else {
        saveLayout(body);
        out = {{"ok", true}, {"panes", body["panes"].size()}};
      }
    } catch (const std::exception& e) {
      out["error"] = e.what();
    }
    res.set_content(dumpJson(out), "application/json; charset=utf-8");
  });

  // ---------------- 内嵌终端 ----------------
  // 前端每个 pane 连一条 WebSocket：二进制帧是 PTY 的原始字节（进出都走它），
  // 文本帧只放控制消息，目前就一个 resize。
  srv_.Post("/api/term/open", [this](const httplib::Request& req, httplib::Response& res) {
    json out = {{"ok", false}};
    try {
      json body = json::parse(req.body);
      int cols = jval(body, "cols", json(100)).get<int>();
      int rows = jval(body, "rows", json(28)).get<int>();
      std::string err;
      auto s = terms_.open(jstr(body, "cwd"), jstr(body, "cmd"), jstr(body, "shell"),
                           jstr(body, "title"), cols, rows, err);
      if (s) {
        out = {{"ok", true}, {"id", s->id}, {"title", s->title}};
      } else {
        out["error"] = err;
      }
    } catch (const std::exception& e) {
      out["error"] = std::string("请求解析失败：") + e.what();
    }
    res.set_header("Cache-Control", "no-store");
    res.set_content(dumpJson(out), "application/json; charset=utf-8");
  });

  srv_.Post("/api/term/resize", [this](const httplib::Request& req, httplib::Response& res) {
    try {
      json body = json::parse(req.body);
      if (auto s = terms_.find(jstr(body, "id"))) {
        s->resize(jval(body, "cols", json(100)).get<int>(),
                  jval(body, "rows", json(28)).get<int>());
      }
    } catch (...) {
    }
    res.set_content("{\"ok\":true}", "application/json; charset=utf-8");
  });

  srv_.Post("/api/term/close", [this](const httplib::Request& req, httplib::Response& res) {
    try {
      terms_.close(jstr(json::parse(req.body), "id"));
    } catch (...) {
    }
    res.set_content("{\"ok\":true}", "application/json; charset=utf-8");
  });

  srv_.WebSocket("/api/term/ws", [this](const httplib::Request& req, httplib::ws::WebSocket& ws) {
    std::string id = req.has_param("id") ? req.get_param_value("id") : "";
    auto s = terms_.find(id);
    if (!s) {
      ws.close(httplib::ws::CloseStatus::PolicyViolation, "no such terminal");
      return;
    }
    // 输出方向单开一个线程，handler 自己的线程专心阻塞读键盘输入
    std::atomic<bool> stopPump{false};
    std::thread pump([&] {
      while (!stopPump.load() && ws.is_open()) {
        std::string chunk = s->drain(200);
        if (chunk.empty()) continue;
        if (!ws.send(chunk.data(), chunk.size())) break;
      }
    });

    std::string msg;
    while (true) {
      auto r = ws.read(msg);
      if (r == httplib::ws::ReadResult::Fail) break;
      if (r == httplib::ws::ReadResult::Timeout) continue;
      if (r == httplib::ws::ReadResult::Binary) {
        s->write(msg);
      } else if (r == httplib::ws::ReadResult::Text) {
        try {
          json j = json::parse(msg);
          if (jstr(j, "t") == "resize") {
            s->resize(jval(j, "cols", json(100)).get<int>(),
                      jval(j, "rows", json(28)).get<int>());
          }
        } catch (...) {
        }
      }
    }
    stopPump.store(true);
    if (pump.joinable()) pump.join();
    // 面板关掉、页面刷新都会走到这里：会话跟着回收，不在后台留 node 进程
    terms_.close(id);
  });

  srv_.Get(R"(/.*)", [this](const httplib::Request& req, httplib::Response& res) {
    if (!handleStatic(req.path, res)) {
      res.status = 404;
      res.set_content("not found", "text/plain; charset=utf-8");
    }
  });
}

bool Backend::start() {
  setupRoutes();
  // 索引尽早开始建：第一次要通读几十上百个会话文件，早点跑完用户少等
  // 独立终端窗口用不到全文索引和复盘摘要：历史大的机器上会白占几百 MB
  if (profile_ != "term") {
    search_.start(profileDir_);
    summary_.start(profileDir_);
  }
  fav_.start(dataDir_);  // 收藏跨窗口共享，不跟 profile 走
  // 每个 WebSocket 终端独占一个线程（阻塞在 read 上），默认 8 个线程池不够
  // 两三个终端再叠加页面请求就会排队
  srv_.new_task_queue = [] { return new httplib::ThreadPool(32); };
  for (int p = 8712; p < 8760; ++p) {
    if (portInUse(p)) continue;
    if (srv_.bind_to_port("127.0.0.1", p)) {
      port_ = p;
      break;
    }
  }
  if (port_ == 0) {
    logf("没有可用端口");
    return false;
  }
  // 已经 bind 过了，必须用 listen_after_bind；再调 listen 会重复 bind 失败，
  // 结果是端口看着占上了但根本没有 accept 循环，页面要等超时才能加载。
  std::thread([this] { srv_.listen_after_bind(); }).detach();
  Sleep(60);  // 等 accept 循环真正跑起来，避免首个请求打空
  logf("http listening on 127.0.0.1:" + std::to_string(port_));
  return true;
}

void Backend::stop() {
  search_.stop();
  terms_.closeAll();
  srv_.stop();
}

}  // namespace crg
