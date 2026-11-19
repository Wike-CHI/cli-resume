#include "launcher.h"

#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

#include <shellapi.h>

namespace crg {
namespace {

struct Shell {
  std::wstring exe;
  std::vector<std::wstring> args;
};

// 优先 pwsh 7，退回 Windows PowerShell 5.1
Shell detectShell() {
  if (SearchPathW(nullptr, L"pwsh.exe", nullptr, 0, nullptr, nullptr) > 0) {
    return {L"pwsh.exe", {L"-NoExit", L"-Command"}};
  }
  return {L"powershell.exe", {L"-NoExit", L"-Command"}};
}

// 按 CommandLineToArgvW 的规则给单个参数加引号，反斜杠要成对翻倍
std::string quoteArg(const std::string& a) {
  if (a.empty()) return "\"\"";
  if (a.find_first_of(" \t\n\v\"") == std::string::npos) return a;
  std::string out;
  out.reserve(a.size() + 4);
  out += '"';
  for (size_t i = 0; i < a.size();) {
    size_t bs = 0;
    while (i < a.size() && a[i] == '\\') {
      ++bs;
      ++i;
    }
    if (i == a.size()) {
      out.append(bs * 2, '\\');
      break;
    }
    if (a[i] == '"') {
      out.append(bs * 2 + 1, '\\');
      out += '"';
    } else {
      out.append(bs, '\\');
      out += a[i];
    }
    ++i;
  }
  out += '"';
  return out;
}

std::string resumeCommand(const LaunchItem& it, bool useContinue) {
  // codex 的续接是子命令，参数也不叫 --resume，别按 claude 那套拼
  if (it.tool == "codex") {
    if (useContinue || it.sessionId.empty()) return "codex resume --last";
    return "codex resume " + it.sessionId;
  }
  // kimi 用 -S：id 就是 session_<uuid>（目录名），不带的话会进交互选择
  if (it.tool == "kimi") {
    if (useContinue || it.sessionId.empty()) return "kimi -c";
    return "kimi -S " + it.sessionId;
  }
  const char* bin = (it.tool == "claude") ? "claude" : "codebuddy";
  if (useContinue || it.sessionId.empty()) return bin;
  return std::string(bin) + " --resume " + it.sessionId;
}

// 标签名直接用会话标题，一眼能认出是哪个对话
std::string tabTitle(const Workspace& ws, const SessionRef& s) {
  std::string t = s.title.empty() ? ws.name : s.title;
  if (t.size() > 64) {
    // 别把 UTF-8 码点切坏
    size_t cut = 64;
    while (cut > 0 && (static_cast<unsigned char>(t[cut]) & 0xC0) == 0x80) --cut;
    t = t.substr(0, cut) + "…";
  }
  return t;
}

// wt 把新标签塞进「已经存在的」Terminal 窗口时，不会把那个窗口提到前台：
// 窗口最小化就还原不了，被别的窗口盖住就一动不动。用户看到的就是「点了没反应」。
// 这里补一步，把 Terminal 的窗口叫到前面来。
void focusTerminalWindow() {
  Sleep(320);  // 给 wt 一点时间把标签建出来
  HWND h = FindWindowW(L"CASCADIA_HOSTING_WINDOW_CLASS", nullptr);
  if (!h) {
    logf("terminal window not found for focus");
    return;
  }
  if (IsIconic(h)) {
    ShowWindow(h, SW_RESTORE);
  } else {
    ShowWindow(h, SW_SHOW);
  }

  // 单纯 SetForegroundWindow 会被系统的「抢焦点保护」挡掉（应用不在前台时经常失败）。
  // 临时挂到前台窗口的输入队列上，就能合法地把它提到最前
  const HWND fg = GetForegroundWindow();
  const DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
  const DWORD myThread = GetCurrentThreadId();
  const bool attached = fgThread && fgThread != myThread &&
                        AttachThreadInput(myThread, fgThread, TRUE);
  BringWindowToTop(h);
  SetForegroundWindow(h);
  if (attached) AttachThreadInput(myThread, fgThread, FALSE);

  logf("terminal window focused");
}

// wt 失败时的退路：直接起一个独立控制台窗口。系统把「默认终端」设成
// Windows Terminal 时，这样开出来的仍然是 WT 窗口，只是拿不到「指定窗口/标签页」的能力
LaunchResult fallbackConsole(const std::vector<LaunchItem>& items, const Shell& shell,
                             const LaunchOptions& opts, LaunchResult res) {
  int started = 0;
  std::string lastErr;
  for (const auto& it : items) {
    std::string cmd = wideToUtf8(shell.exe);
    for (const auto& a : shell.args) cmd += " " + wideToUtf8(a);
    cmd += " " + quoteArg(resumeCommand(it, opts.useContinue));

    std::wstring wcmd = utf8ToWide(cmd);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr,
                       utf8ToWide(it.cwd).c_str(), &si, &pi)) {
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);
      ++started;
    } else {
      lastErr = "CreateProcess 失败（" + std::to_string(GetLastError()) + "）";
    }
  }
  if (started > 0) {
    logf("fallback console started=" + std::to_string(started));
    res.ok = true;
    res.fallback = true;
    return res;
  }
  res.ok = false;
  res.error = "外部终端启动失败：" + lastErr;
  return res;
}

}  // namespace

std::vector<LaunchItem> pickItems(const Workspace& ws, bool openTwo) {
  std::vector<LaunchItem> out;
  if (ws.sessions.empty()) return out;
  // ws.sessions 已按 mtime 倒序，第一条就是最近用过的那个会话
  if (!openTwo) {
    out.push_back({ws.cwd, ws.sessions.front().tool, ws.sessions.front().sessionId,
                   tabTitle(ws, ws.sessions.front())});
    return out;
  }
  // 两个 CLI 各开一个最近会话
  std::set<std::string> seen;
  for (const auto& s : ws.sessions) {
    if (!seen.insert(s.tool).second) continue;
    out.push_back({ws.cwd, s.tool, s.sessionId, tabTitle(ws, s)});
  }
  return out;
}

LaunchResult launch(const std::vector<LaunchItem>& items, const LaunchOptions& opts) {
  LaunchResult res;
  if (items.empty()) {
    res.error = "没有要恢复的标签页";
    return res;
  }

  Shell shell = detectShell();
  std::string cmdline = "wt.exe -w " + std::to_string(opts.window);
  for (size_t i = 0; i < items.size(); ++i) {
    const auto& it = items[i];
    if (i > 0) cmdline += " ;";
    cmdline += " new-tab --title " + quoteArg(it.title);
    cmdline += " -d " + quoteArg(it.cwd);
    cmdline += " " + wideToUtf8(shell.exe);
    for (const auto& a : shell.args) cmdline += " " + wideToUtf8(a);
    // 整条 resume 命令是 -Command 的一个参数，含空格必须整体加引号
    cmdline += " " + quoteArg(resumeCommand(it, opts.useContinue));
  }

  res.command = cmdline;
  if (opts.dryRun) {
    res.ok = true;
    return res;
  }

  // wt.exe 的失败是「静默」的：它把参数转交给已运行的 Terminal 进程，
  // 转不过去（比如那边是管理员权限、或者参数被拒）就直接退出，什么都不显示。
  // 所以这里接住 stderr 并等它退出，失败了再退回普通控制台窗口
  std::string tmpDir = ".";
  if (const char* la = std::getenv("LOCALAPPDATA")) tmpDir = std::string(la) + "\\CliResumeGui";
  CreateDirectoryW(utf8ToWide(tmpDir).c_str(), nullptr);
  const std::string errPath = tmpDir + "\\wt_error.txt";
  DeleteFileW(utf8ToWide(errPath).c_str());
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE hErr = CreateFileW(utf8ToWide(errPath).c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

  std::wstring wcmd = utf8ToWide(cmdline);
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  if (hErr != INVALID_HANDLE_VALUE) {
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hErr;
    si.hStdError = hErr;
  }
  PROCESS_INFORMATION pi{};
  BOOL ok = CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, hErr != INVALID_HANDLE_VALUE,
                           CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  if (!ok) {
    if (hErr != INVALID_HANDLE_VALUE) CloseHandle(hErr);
    DWORD err = GetLastError();
    res.error = "wt.exe 启动失败（GetLastError=" + std::to_string(err) +
                "）。确认已安装 Windows Terminal。";
    logf(res.error + " cmd=" + cmdline);
    return fallbackConsole(items, shell, opts, res);
  }

  // 成功的 wt 立刻退出、没有任何输出；有输出就说明它拒绝了这条命令
  DWORD code = 0;
  WaitForSingleObject(pi.hProcess, 1500);
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);

  std::string wtErr;
  if (hErr != INVALID_HANDLE_VALUE) {
    CloseHandle(hErr);
    std::ifstream f(utf8ToWide(errPath), std::ios::binary);
    if (f) {
      std::stringstream ss;
      ss << f.rdbuf();
      wtErr = ss.str();
    }
  }

  if (code != 0 || !wtErr.empty()) {
    logf("wt 拒绝执行 code=" + std::to_string(code) + " stderr=" + wtErr);
    return fallbackConsole(items, shell, opts, res);
  }
  // 只有「塞进已有窗口」才会出现不跳前台的问题；-1 是新开窗口，自己会出来
  if (opts.window >= 0) focusTerminalWindow();
  logf("launched tabs=" + std::to_string(items.size()));
  res.ok = true;
  return res;
}

bool revealInExplorer(const std::string& path) {
  std::wstring w = utf8ToWide(path);
  HINSTANCE r = ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  return reinterpret_cast<INT_PTR>(r) > 32;
}

}  // namespace crg
