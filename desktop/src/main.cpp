// Win32 宿主：本地 HTTP 服务跑在后台线程，界面用 WebView2 加载。
#include "common.h"

#include <shellapi.h>
#include <tlhelp32.h>
#include <wrl.h>

#include <cctype>
#include <cstdio>
#include <string>
#include <unordered_set>
#include <vector>

#include "WebView2.h"
#include "autostart.h"
#include "backend.h"

#pragma comment(lib, "WebView2LoaderStatic.lib")

using namespace Microsoft::WRL;

namespace {

const wchar_t* kClassName = L"CliResumeWindow";
const wchar_t* kTitle = L"CLI Resume — 终端现场";

struct AppState {
  crg::Backend* backend = nullptr;
  ICoreWebView2Controller* controller = nullptr;
  ICoreWebView2* webview = nullptr;
  HINSTANCE hInst = nullptr;
  std::string baseUrl;
  std::string shotPath;
  std::string autoUi;
  std::string profile;      // 空 = 主窗口；否则是工作区窗口
  bool hotkeyOk = false;
  bool trayAdded = false;
  bool shotMode = false;
  bool hidHintShown = false;
  UINT taskbarMsg = 0;
};

AppState g_app;

constexpr UINT WM_APP_CAPTURE = WM_APP + 1;
constexpr UINT WM_APP_AUTO = WM_APP + 2;
constexpr UINT WM_APP_TRAY = WM_APP + 3;
constexpr UINT kTrayId = 1001;

void capturePreview(ICoreWebView2* webview, const std::string& outPath, HWND hwnd);

std::string getArgValue(const std::string& name, const std::string& def) {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) return def;
  std::string out = def;
  std::wstring wname = crg::utf8ToWide(name);
  for (int i = 1; i + 1 < argc; ++i) {
    if (wname == argv[i]) {
      out = crg::wideToUtf8(argv[i + 1]);
      break;
    }
  }
  LocalFree(argv);
  return out;
}

// 不带值的开关，例如 --demo
bool hasArg(const std::string& name) {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) return false;
  bool found = false;
  std::wstring wname = crg::utf8ToWide(name);
  for (int i = 1; i < argc; ++i) {
    if (wname == argv[i]) {
      found = true;
      break;
    }
  }
  LocalFree(argv);
  return found;
}

void resizeWebView(HWND hwnd) {
  if (!g_app.controller) return;
  RECT rc;
  GetClientRect(hwnd, &rc);
  g_app.controller->put_Bounds(rc);
}

// --ui 的一个动作 → 一段 JS（外层 IIFE 由调用方拼）。
// 拆成这样是为了能连用，例如 --ui theme,expand 先换皮肤再展开。
std::string actionScript(const std::string& act) {
  if (act == "settings") {
    return "document.getElementById('settings-btn').click();";
  }
  if (act == "search") {
    return "var s=document.getElementById('search');s.value='ai';"
           "s.dispatchEvent(new Event('input'));";
  }
  if (act == "demoterm") {
    return "termAddPane({cwd:'D:/work/acme-portal',cmd:'codebuddy',title:'codebuddy · acme-portal',tool:'codebuddy'});setTimeout(function(){termAddPane({cwd:'D:/work/data-pipeline',cmd:'claude',title:'claude · data-pipeline',tool:'claude'});},600);";
  }
  if (act == "promosearch") {
    // 宣传素材：示例数据里能命中的词
    return "var s=document.getElementById('search');s.value='hydration';"
           "s.dispatchEvent(new Event('input'));";
  }
  if (act == "deep") {
    // 走真实的回车路径：填词 → 派发 keydown，验证搜索框的回车绑定
    return "var s=document.getElementById('search');s.value='流式';s.focus();"
           "s.dispatchEvent(new KeyboardEvent('keydown',{key:'Enter',bubbles:true,cancelable:true}));";
  }
  if (act == "hit") {
    // 点第一条命中，验证「搜到 → 直接进那个会话」
    return "var r=document.querySelectorAll('.hit-row');if(r.length)r[0].click();";
  }
  if (act == "theme") {
    return "var b=document.getElementById('theme-btn');if(b)b.click();";
  }
  if (act == "expand") {
    // 只展开第一个目录：截图里既能看到对话列表，也能看到下面几张未展开的卡片
    return "var c=document.querySelectorAll('.ws-card');if(c.length)c[0].click();";
  }
  if (act == "export") {
    // 点第一条会话的「导出」：验证前端动作链（示例模式下只会弹提示）
    return "var b=document.querySelector('[data-export]');if(b)b.click();";
  }
  if (act == "doctor") {
    // 把诊断区滚进视野并把文本打到日志：验证各 agent 的检测结果
    return "var d=document.getElementById('doctor');if(d){d.scrollIntoView({block:'end'});"
           "fetch('/api/log',{method:'POST',body:'doctor '+d.innerText.replace(/\n/g,' | ')});}";
  }
  if (act == "cmdtest") {
    // 只做验证：把第一个 codex 会话的启动命令打到后端日志，不真的开终端
    return "var w=S.all.find(function(x){return x.sessions&&x.sessions.some(function(s){return s.tool==='codex';});});"
           "if(w){var s=w.sessions.find(function(x){return x.tool==='codex';});"
           "fetch('/api/log',{method:'POST',body:'cmdtest '+termCmdFor(s)});}";
  }
  if (act == "favonly") {
    // 勾上「只看收藏」：验证收藏过滤与置顶顺序
    return "var c=document.getElementById('opt-fav');if(c){c.checked=true;c.dispatchEvent(new Event('change'));}";
  }
  if (act == "moremenu") {
    // 卡片「⋯」的溢出菜单
    return "var b=document.querySelector('[data-acts]');if(b)b.click();";
  }
  if (act == "newmenu") {
    // 打开「新会话」的 CLI 菜单：验证浮层定位与四套皮肤的样式
    return "var b=document.querySelector('[data-newsess]');if(b)b.click();";
  }
  if (act == "wsform") {
    // 展开左栏「新建工作空间」表单。截图时先把终端面板收起来，
    // 否则左栏高度不够，表单会被挤到可视区外
    return "var d=document.getElementById('termdock');if(d)d.hidden=true;"
           "var f=document.getElementById('ws-form');if(f){f.hidden=false;f.scrollIntoView({block:'center'});}";
  }
  if (act == "stats") {
    // 打开 token 统计视图：验证汇总数字与图表的渲染
    return "document.getElementById('stats-btn').click();";
  }
  if (act == "brief") {
    // 点第一个目录的「复盘」，验证本地模型链路和结果渲染
    return "var b=document.querySelector('[data-brief]');if(b)b.click();";
  }
  if (act == "enter") {
    // 展开第一个目录并点进第一条对话，验证「点一下就进去」这条路
    return "var c=document.querySelectorAll('.ws-card');if(c.length){c[0].click();"
           "var r=document.querySelectorAll('.sess-row');if(r.length)r[0].click();}";
  }
  if (act == "select") {
    return "var b=document.querySelectorAll('.ws-check');"
           "for(var i=0;i<Math.min(3,b.length);i++){b[i].checked=true;}"
           "if(window.__onSelChange)window.__onSelChange();";
  }
  if (act == "split") {
    // 两个 pane 并排 + 一个不同命令，验证「分窗口并行」
    return "var t=window.termAddPane;if(t){t({cwd:'C:/Windows',cmd:'echo PANE-ONE',title:'echo'});"
           "setTimeout(function(){t({cwd:'C:/Users',cmd:'echo PANE-TWO',title:'pwd'});},500);}";
  }
  if (act == "term") {
    // 打开内嵌终端面板，截图里能看到真实 shell 的输出
    return "var b=document.getElementById('term-btn');if(b)b.click();";
  }
  return "";  // 不认识的动作就当没有，截图仍是首屏原貌
}

// 把窗口拉出来 / 收起来（热键和托盘菜单共用）。拉起时顺手把光标放进搜索框。
// WebView2 是 6 个独立进程（浏览器/渲染/GPU/网络/存储/crashpad），
// EmptyWorkingSet 只管得到自己，要逐个把子进程树的工作集也还回去。
// 挂起之后再调，页面不会重绘；恢复时缺的页按需读回来。
void trimChildProcesses() {
  const DWORD me = GetCurrentProcessId();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return;

  std::vector<std::pair<DWORD, DWORD>> all;  // pid, ppid
  PROCESSENTRY32W e{};
  e.dwSize = sizeof(e);
  if (Process32FirstW(snap, &e)) {
    do {
      all.emplace_back(e.th32ProcessID, e.th32ParentProcessID);
    } while (Process32NextW(snap, &e));
  }
  CloseHandle(snap);

  // 从自己出发把整棵子树找出来（msedgewebview2 自己还会再生一层）
  std::unordered_set<DWORD> mine;
  mine.insert(me);
  for (bool grew = true; grew;) {
    grew = false;
    for (const auto& k : all) {
      if (mine.count(k.second) && !mine.count(k.first)) {
        mine.insert(k.first);
        grew = true;
      }
    }
  }

  int n = 0;
  for (const auto& k : all) {
    if (k.first == me || !mine.count(k.first)) continue;
    HANDLE h = OpenProcess(PROCESS_SET_QUOTA, FALSE, k.first);
    if (!h) continue;
    SetProcessWorkingSetSize(h, (SIZE_T)-1, (SIZE_T)-1);
    CloseHandle(h);
    ++n;
  }
  crg::logf("trimmed child processes=" + std::to_string(n));
}

// 藏起来以后继续跑是不划算的：CRT 光带一直在动、时钟每秒一跳、渲染进程还占着显存。
// 常驻托盘一整晚，这些空转会在弱显卡上拖累桌面合成器 —— 表现就是应用先白、桌面跟着白。
// 所以窗口不可见时把 WebView 挂起并把内存目标降到 LOW，重新显示时再恢复。
void suspendWebView(bool suspend) {
  if (!g_app.webview) return;
  crg::logf(std::string("webview suspend=") + (suspend ? "1" : "0"));
  // IsVisible 在 controller 上，不在 webview 上
  if (g_app.controller) g_app.controller->put_IsVisible(suspend ? FALSE : TRUE);

  Microsoft::WRL::ComPtr<ICoreWebView2_3> wv3;
  if (SUCCEEDED(g_app.webview->QueryInterface(IID_PPV_ARGS(&wv3))) && wv3) {
    if (suspend) {
      wv3->TrySuspend(
          Callback<ICoreWebView2TrySuspendCompletedHandler>(
              [](HRESULT hr, BOOL ok) -> HRESULT {
                crg::logf("TrySuspend hr=" + std::to_string(hr) + " suspended=" + (ok ? "1" : "0"));
                // 挂起成功之后再回收，免得跟正在跑的渲染抢页
                if (ok) trimChildProcesses();
                return S_OK;
              })
              .Get());
    } else {
      wv3->Resume();
    }
  }
  Microsoft::WRL::ComPtr<ICoreWebView2_19> wv19;
  if (SUCCEEDED(g_app.webview->QueryInterface(IID_PPV_ARGS(&wv19))) && wv19) {
    wv19->put_MemoryUsageTargetLevel(
        suspend ? COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_LOW
                : COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_NORMAL);
  }
  // 挂起后把工作集还给系统：收进托盘的工具不该占着几百 MB 不放。
  // 再点开时缺的页会按需重新读回来，代价只是第一次重绘慢一点
  if (suspend) {
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
  }
}

void toggleWindow(HWND hwnd) {
  const bool focused = IsWindowVisible(hwnd) && !IsIconic(hwnd) && GetForegroundWindow() == hwnd;
  if (focused) {
    ShowWindow(hwnd, SW_HIDE);
    suspendWebView(true);
    return;
  }
  ShowWindow(hwnd, IsIconic(hwnd) ? SW_RESTORE : SW_SHOW);
  suspendWebView(false);
  SetForegroundWindow(hwnd);
  if (g_app.webview) {
    g_app.webview->ExecuteScript(
        L"var s=document.getElementById('search');if(s){s.focus();s.select();}", nullptr);
  }
}

void addTrayIcon(HWND hwnd) {
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.hWnd = hwnd;
  nid.uID = kTrayId;
  nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  nid.uCallbackMessage = WM_APP_TRAY;
  nid.hIcon = (HICON)LoadImageW(g_app.hInst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                LR_SHARED);
  if (!nid.hIcon) nid.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));  // IDI_APPLICATION
  lstrcpynW(nid.szTip, L"CLI Resume — 终端现场", ARRAYSIZE(nid.szTip));
  g_app.trayAdded = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
  crg::logf(std::string("tray icon ") + (g_app.trayAdded ? "ok" : "failed"));
}

void removeTrayIcon(HWND hwnd) {
  if (!g_app.trayAdded) return;
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.hWnd = hwnd;
  nid.uID = kTrayId;
  Shell_NotifyIconW(NIM_DELETE, &nid);
  g_app.trayAdded = false;
}

void showTrayMenu(HWND hwnd) {
  HMENU m = CreatePopupMenu();
  const bool visible = IsWindowVisible(hwnd) && !IsIconic(hwnd);
  AppendMenuW(m, MF_STRING, 1, visible ? L"隐藏窗口" : L"显示窗口");
  AppendMenuW(m, MF_STRING | (crg::autostartEnabled() ? MF_CHECKED : 0), 2, L"开机自启");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, 3, L"退出");
  POINT pt{};
  GetCursorPos(&pt);
  // 不先抢前台，菜单点外面不会消失
  SetForegroundWindow(hwnd);
  TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, nullptr);
  DestroyMenu(m);
  PostMessageW(hwnd, WM_NULL, 0, 0);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_SIZE:
      resizeWebView(hwnd);
      // 最小化 = 看不见 = 没必要继续画，顺手把渲染进程挂起
      if (g_app.webview && !g_app.shotMode) suspendWebView(wp == SIZE_MINIMIZED);
      return 0;
    case WM_APP_CAPTURE: {
      crg::logf("WM_APP_CAPTURE shot=" + g_app.shotPath);
      // 指针停在某一行上会让截图里多出一条 hover 高亮，先把它挪到列标题条上，
      // 那里没有 hover 样式，图才干净
      RECT rc;
      if (GetWindowRect(hwnd, &rc)) SetCursorPos(rc.left + 420, rc.top + 46);
      if (g_app.webview && !g_app.shotPath.empty()) capturePreview(g_app.webview, g_app.shotPath, hwnd);
      return 0;
    }
    case WM_APP_AUTO: {
      // 调试用：--ui 可以逗号串多个动作（theme / settings / search / expand / enter / select）。
      // 动作之间留 280ms，让界面有机会重绘再截下一屏。
      if (g_app.webview) {
        std::string js = "(function(){var a=[";
        std::string cur;
        auto flush = [&]() {
          if (cur.empty()) return;
          std::string body = actionScript(cur);
          if (!body.empty()) js += "function(){" + body + "},";
          cur.clear();
        };
        for (char c : g_app.autoUi) {
          if (c == ',') flush();
          else cur.push_back(c);
        }
        flush();
        js += "];var i=0;(function next(){if(i>=a.length)return;a[i++]();setTimeout(next,280);})();})()";
        std::wstring wjs = crg::utf8ToWide(js);
        crg::logf("auto ui: " + g_app.autoUi);
        g_app.webview->ExecuteScript(wjs.c_str(), nullptr);
      }
      return 0;
    }
    case WM_HOTKEY: {
      // Ctrl+Shift+Space：前台就收起，否则拉起来（托盘菜单里的显示/隐藏同一条路径）
      if (wp != 1) break;
      crg::logf("hotkey toggle");
      toggleWindow(hwnd);
      return 0;
    }
    case WM_APP_TRAY:
      if (lp == WM_LBUTTONDBLCLK) {
        ShowWindow(hwnd, IsIconic(hwnd) ? SW_RESTORE : SW_SHOW);
        suspendWebView(false);
        SetForegroundWindow(hwnd);
      } else if (lp == WM_RBUTTONUP) {
        showTrayMenu(hwnd);
      }
      return 0;
    case WM_COMMAND:
      switch (LOWORD(wp)) {
        case 1:
          toggleWindow(hwnd);
          return 0;
        case 2: {
          std::string err;
          if (!crg::setAutostart(!crg::autostartEnabled(), err)) crg::logf("autostart fail " + err);
          return 0;
        }
        case 3:
          DestroyWindow(hwnd);  // 真退出：WM_DESTROY 会摘掉托盘图标
          return 0;
      }
      break;
    case WM_CLOSE:
      // 点 X 只是收进托盘：这是个随时要唤醒的工具，关掉再等 6 秒重建索引不划算。
      // 截图模式必须真的退出，否则 capture 完之后进程挂着不走
      if (g_app.shotMode) {
        DestroyWindow(hwnd);
        return 0;
      }
      ShowWindow(hwnd, SW_HIDE);
      suspendWebView(true);
      if (!g_app.hidHintShown && g_app.trayAdded) {
        g_app.hidHintShown = true;
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd;
        nid.uID = kTrayId;
        nid.uFlags = NIF_INFO;
        nid.dwInfoFlags = NIIF_INFO;
        lstrcpynW(nid.szInfoTitle, L"CLI Resume 还在后台", ARRAYSIZE(nid.szInfoTitle));
        lstrcpynW(nid.szInfo, L"Ctrl+Shift+Space 唤出；右键托盘图标可退出", ARRAYSIZE(nid.szInfo));
        Shell_NotifyIconW(NIM_MODIFY, &nid);
      }
      return 0;
    case WM_DESTROY:
      if (g_app.hotkeyOk) UnregisterHotKey(hwnd, 1);
      removeTrayIcon(hwnd);
      PostQuitMessage(0);
      return 0;
  }
  // 资源管理器重启后托盘图标会消失，系统用 TaskbarCreated 通知所有程序补一个
  if (g_app.taskbarMsg && msg == g_app.taskbarMsg) {
    g_app.trayAdded = false;
    addTrayIcon(hwnd);
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

std::wstring exeDirW() {
  wchar_t buf[MAX_PATH] = {0};
  GetModuleFileNameW(nullptr, buf, MAX_PATH);
  std::wstring p(buf);
  size_t pos = p.find_last_of(L"\\/");
  return pos == std::wstring::npos ? L"." : p.substr(0, pos);
}

void capturePreview(ICoreWebView2* webview, const std::string& outPath, HWND hwnd) {
  IStream* stream = nullptr;
  if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) || !stream) return;
  webview->AddRef();
  auto handler = Callback<ICoreWebView2CapturePreviewCompletedHandler>(
      [webview, stream, outPath, hwnd](HRESULT hr) -> HRESULT {
        if (SUCCEEDED(hr)) {
          HGLOBAL mem = nullptr;
          if (SUCCEEDED(GetHGlobalFromStream(stream, &mem))) {
            void* data = GlobalLock(mem);
            SIZE_T size = GlobalSize(mem);
            if (data && size) {
              FILE* f = nullptr;
              if (fopen_s(&f, outPath.c_str(), "wb") == 0) {
                fwrite(data, 1, size, f);
                fclose(f);
              }
            }
            GlobalUnlock(mem);
          }
          crg::logf("capture ok -> " + outPath);
        } else {
          crg::logf("capture failed hr=" + std::to_string(hr));
        }
        stream->Release();
        webview->Release();
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return S_OK;
      });
  HRESULT hr = webview->CapturePreview(COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, stream, handler.Get());
  if (FAILED(hr)) {
    crg::logf("CapturePreview 调用失败 hr=" + std::to_string(hr));
    stream->Release();
    webview->Release();
    PostMessageW(hwnd, WM_CLOSE, 0, 0);
  }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  // 截图模式（--shot）不参与单实例，方便自动化验证
  bool shotMode = !getArgValue("--shot", "").empty();
  g_app.shotMode = shotMode;

  // 工作区窗口：--only <dir> 锁一个目录，--win <slug> 指定窗口 id（不给就按目录算）。
  // 每个窗口一个进程、一份 WebView2 数据目录、一份索引 —— 不共享就不会互相写坏
  const std::string onlyCwd = getArgValue("--only", "");
  std::string winSlug = getArgValue("--win", "");
  if (winSlug.empty() && !onlyCwd.empty()) {
    std::string key = onlyCwd;
    for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    winSlug = crg::shortHash(key);
  }
  g_app.profile = winSlug;
  const bool trayMode = hasArg("--tray");
  const std::wstring clsName =
      winSlug.empty() ? std::wstring(kClassName) : (L"CliResumeWin_" + crg::utf8ToWide(winSlug));

  HANDLE once = nullptr;
  if (!shotMode) {
    const std::wstring mtx = winSlug.empty()
                                 ? std::wstring(L"Local\\CliResumeGui_SingleInstance")
                                 : (L"Local\\CliResumeGui_" + crg::utf8ToWide(winSlug));
    once = CreateMutexW(nullptr, TRUE, mtx.c_str());
    if (once && GetLastError() == ERROR_ALREADY_EXISTS) {
      HWND existing = FindWindowW(clsName.c_str(), nullptr);
      if (existing) {
        ShowWindow(existing, SW_RESTORE);
        SetForegroundWindow(existing);
      }
      return 0;
    }
  }

  std::string webRoot = getArgValue("--web", crg::wideToUtf8(exeDirW() + L"\\web"));
  std::string dataDir = crg::wideToUtf8(exeDirW()) + "\\data";
  CreateDirectoryA(dataDir.c_str(), nullptr);
  crg::logInit(dataDir + "\\app.log");
  crg::logf("=== CLI Resume GUI 启动 ===");
  crg::logf("web=" + webRoot);

  crg::Backend backend(webRoot, winSlug);
  if (!backend.start()) {
    MessageBoxW(nullptr, L"本地服务启动失败，请查看 data\\app.log", kTitle, MB_ICONERROR);
    return 1;
  }
  g_app.backend = &backend;
  g_app.baseUrl = backend.url();

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = WndProc;
  wc.hInstance = hInst;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  // 图标来自 src\app.rc；资源缺失时返回空，窗口退回系统默认图标，不影响启动
  wc.hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0,
                               LR_DEFAULTSIZE | LR_SHARED);
  wc.hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                 GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                 LR_SHARED);
  // 底色要跟当前主题一致，否则 WebView2 首帧之前会闪一块不属于该主题的颜色
  // 每套皮肤一个底色：首帧之前窗口就是这个色，选错了启动会闪一下
  const std::string theme = backend.theme();
  COLORREF bg = RGB(5, 2, 11);  // 霓虹
  if (theme == "amber") bg = RGB(8, 5, 0);
  else if (theme == "win98") bg = RGB(192, 192, 192);
  else if (theme == "xp") bg = RGB(236, 233, 216);
  wc.hbrBackground = CreateSolidBrush(bg);
  wc.lpszClassName = clsName.c_str();
  RegisterClassExW(&wc);

  // CW_USEDEFAULT 交给系统放置时，窗口大小每次会被系统再调一遍，
  // 截图就一张大一张小；自己算居中位置，尺寸才稳定
  // --size 1600x1000：截图和录屏时用更大的窗口，1080p 素材不至于糊。
  // 不带就用默认尺寸
  int kWinW = 1180, kWinH = 820;
  {
    const std::string sz = getArgValue("--size", "");
    const size_t x = sz.find('x');
    if (x != std::string::npos) {
      const int w = atoi(sz.c_str());
      const int h = atoi(sz.c_str() + x + 1);
      if (w >= 900 && w <= 3840 && h >= 600 && h <= 2160) {
        kWinW = w;
        kWinH = h;
      }
    }
  }
  int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
  int wx = (sw - kWinW) / 2, wy = (sh - kWinH) / 2;
  if (wx < 0) wx = 0;
  if (wy < 0) wy = 0;

  // 工作区窗口的标题写目录名，任务栏里一眼分得清
  std::wstring title = kTitle;
  if (!onlyCwd.empty()) {
    std::string name = onlyCwd;
    size_t sep = name.find_last_of("\\/");
    if (sep != std::string::npos && sep + 1 < name.size()) name = name.substr(sep + 1);
    title = L"CLI Resume — " + crg::utf8ToWide(name);
  }

  HWND hwnd = CreateWindowExW(0, clsName.c_str(), title.c_str(), WS_OVERLAPPEDWINDOW, wx, wy,
                              kWinW, kWinH, nullptr, nullptr, hInst, nullptr);
  if (!hwnd) return 1;
  g_app.hInst = hInst;
  // 托盘常驻；--tray（开机自启）时窗口先不弹，等热键或托盘叫它
  addTrayIcon(hwnd);
  g_app.taskbarMsg = RegisterWindowMessageW(L"TaskbarCreated");
  if (!trayMode) ShowWindow(hwnd, SW_SHOW);

  // 全局热键只给主窗口：热键是系统级独占资源，工作区窗口抢了会让主窗口失效
  if (winSlug.empty()) {
    g_app.hotkeyOk = RegisterHotKey(hwnd, 1, MOD_CONTROL | MOD_SHIFT, VK_SPACE) != 0;
    crg::logf(std::string("hotkey ctrl+shift+space ") +
              (g_app.hotkeyOk ? "ok" : ("failed err=" + std::to_string(GetLastError()))));
  }

  std::wstring userData = exeDirW() + L"\\data\\WebView2";
  // --demo：加载内置示例数据，不扫本机历史也不拉起终端，用来截图或给人预览界面
  std::string baseUrl = backend.url();
  // backend.url() 自带尾斜杠，直接接 hash，别再补一个 "/"
  if (hasArg("--demo")) baseUrl += "#demo";
  // --skin win98 直接指定皮肤，截图时不必先点一次切换；
  // 示例模式和非示例模式都认，后者走 #skin= 由 app.js 读
  std::string skin = getArgValue("--skin", "");
  if (!skin.empty()) {
    baseUrl += baseUrl.find('#') == std::string::npos ? "#skin=" : "&skin=";
    baseUrl += skin;
  }
  if (!onlyCwd.empty()) {
    // 目录里有中文和空格，hash 参数必须转义，否则 app.js 那边读出来是残的
    std::string enc;
    for (unsigned char c : onlyCwd) {
      if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/' || c == ':') {
        enc += static_cast<char>(c);
      } else {
        char buf[4];
        std::snprintf(buf, sizeof(buf), "%%%02X", c);
        enc += buf;
      }
    }
    baseUrl += baseUrl.find('#') == std::string::npos ? "#only=" : "&only=";
    baseUrl += enc;
  }
  std::wstring url = crg::utf8ToWide(baseUrl);

  auto envHandler = Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
      [hwnd, url](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
        crg::logf("env callback hr=" + std::to_string(hr));
        if (FAILED(hr) || !env) {
          crg::logf("创建 WebView2 环境失败");
          MessageBoxW(hwnd, L"WebView2 运行时初始化失败，请安装 Microsoft Edge WebView2 Runtime。",
                      kTitle, MB_ICONERROR);
          return hr;
        }
        env->CreateCoreWebView2Controller(
            hwnd, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                      [hwnd, url](HRESULT hr2, ICoreWebView2Controller* controller) -> HRESULT {
                        crg::logf("controller callback hr=" + std::to_string(hr2));
                        if (FAILED(hr2) || !controller) {
                          crg::logf("创建 WebView2 控制器失败");
                          return hr2;
                        }
                        g_app.controller = controller;
                        controller->AddRef();
                        resizeWebView(hwnd);
                        ICoreWebView2* webview = nullptr;
                        controller->get_CoreWebView2(&webview);
                        if (webview) {
                          // 正常模式也要留住这个指针：热键往里注入聚焦脚本、隐藏时挂起、
                          // 崩溃后重载，都要用它。之前只在 --shot 分支里赋值，
                          // 结果这些功能在真实使用中其实一直是空转
                          g_app.webview = webview;
                          g_app.webview->AddRef();
                          crg::logf("webview ready (normal mode pointer set)");

                          // 渲染进程挂掉的直接表现就是一屏白，而且不会自己恢复。
                          // 接管这个事件：记日志 + 重载页面，带冷却避免崩溃-重载死循环。
                          // 浏览器进程退出是另一种情况（整个 WebView 没了），重载没有意义
                          EventRegistrationToken failTok{};
                          webview->add_ProcessFailed(
                              Callback<ICoreWebView2ProcessFailedEventHandler>(
                                  [](ICoreWebView2*,
                                     ICoreWebView2ProcessFailedEventArgs* args) -> HRESULT {
                                    COREWEBVIEW2_PROCESS_FAILED_KIND kind{};
                                    args->get_ProcessFailedKind(&kind);
                                    crg::logf("webview process failed kind=" +
                                              std::to_string((int)kind));
                                    if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED)
                                      return S_OK;
                                    static ULONGLONG lastReload = 0;
                                    const ULONGLONG now = GetTickCount64();
                                    if (now - lastReload < 15000) {
                                      crg::logf("webview reload suppressed (cooldown)");
                                      return S_OK;
                                    }
                                    lastReload = now;
                                    if (g_app.webview) {
                                      crg::logf("webview reloading after process failure");
                                      g_app.webview->Reload();
                                    }
                                    return S_OK;
                                  })
                                  .Get(),
                              &failTok);

                          ICoreWebView2Settings* settings = nullptr;
                          webview->get_Settings(&settings);
                          if (settings) {
                            settings->put_AreDefaultContextMenusEnabled(FALSE);
                            settings->put_IsStatusBarEnabled(FALSE);
                            settings->put_AreDevToolsEnabled(TRUE);
                            settings->Release();
                          }

                          // 调试辅助：--shot 截图后退出，--wait 控制等待毫秒
                          std::string shotPath = getArgValue("--shot", "");
                          int waitMs = 4000;
                          {
                            std::string w = getArgValue("--wait", "");
                            if (!w.empty()) waitMs = atoi(w.c_str());
                          }
                          if (!shotPath.empty()) {
                            g_app.shotPath = shotPath;
                            g_app.autoUi = getArgValue("--ui", "");
                            EventRegistrationToken tok{};
                            webview->add_NavigationCompleted(
                                Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                    [hwnd, waitMs](ICoreWebView2*,
                                                   ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                                      crg::logf("navigation completed, wait=" + std::to_string(waitMs));
                                      std::thread([hwnd, waitMs] {
                                        Sleep(1200);
                                        PostMessageW(hwnd, WM_APP_AUTO, 0, 0);
                                        Sleep(waitMs);
                                        PostMessageW(hwnd, WM_APP_CAPTURE, 0, 0);
                                      }).detach();
                                      return S_OK;
                                    })
                                    .Get(),
                                &tok);
                          }

                          webview->Navigate(url.c_str());
                          webview->Release();
                        }
                        return S_OK;
                      })
                  .Get());
        return S_OK;
      });

  HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, userData.c_str(), nullptr, envHandler.Get());
  if (FAILED(hr)) {
    crg::logf("CreateCoreWebView2EnvironmentWithOptions 失败 hr=" + std::to_string(hr));
    MessageBoxW(hwnd, L"未检测到 WebView2 Runtime，请先安装后再运行。", kTitle, MB_ICONERROR);
    return 1;
  }

  MSG msg;
  while (GetMessage(&msg, nullptr, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }

  if (g_app.controller) {
    g_app.controller->Close();
    g_app.controller->Release();
    g_app.controller = nullptr;
  }
  // 给 Edge 子进程一点时间释放 user data 目录，否则下一次启动会拿到 ERROR_BUSY
  Sleep(200);
  backend.stop();
  (void)once;
  crg::logf("=== 退出 ===");
  return 0;
}
