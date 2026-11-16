#include "launcher.h"

#include <set>

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

  std::wstring wcmd = utf8ToWide(cmdline);
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  // wt.exe 把命令转交给已有的 Terminal 进程后就退出，这里不必等它
  BOOL ok = CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                           nullptr, &si, &pi);
  if (!ok) {
    DWORD err = GetLastError();
    res.error = "wt.exe 启动失败（GetLastError=" + std::to_string(err) +
                "）。确认已安装 Windows Terminal。";
    logf(res.error + " cmd=" + cmdline);
    return res;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
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
