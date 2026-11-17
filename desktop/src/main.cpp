// Win32 宿主：本地 HTTP 服务跑在后台线程，界面用 WebView2 加载。
#include "common.h"

#include <shellapi.h>
#include <wrl.h>

#include <string>

#include "WebView2.h"
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
  std::string baseUrl;
  std::string shotPath;
  std::string autoUi;
};

AppState g_app;

constexpr UINT WM_APP_CAPTURE = WM_APP + 1;
constexpr UINT WM_APP_AUTO = WM_APP + 2;

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

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_SIZE:
      resizeWebView(hwnd);
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
    case WM_DESTROY:
      PostQuitMessage(0);
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

  // 单实例：WebView2 的 user data 目录只能被一个进程持有，重复启动会直接把控制器创建打挂
  // 截图模式（--shot）不参与，方便自动化验证
  bool shotMode = !getArgValue("--shot", "").empty();
  HANDLE once = nullptr;
  if (!shotMode) {
    once = CreateMutexW(nullptr, TRUE, L"Local\\CliResumeGui_SingleInstance");
    if (once && GetLastError() == ERROR_ALREADY_EXISTS) {
      HWND existing = FindWindowW(kClassName, nullptr);
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

  crg::Backend backend(webRoot);
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
  wc.hbrBackground = CreateSolidBrush(
      backend.theme() == "win98" ? RGB(192, 192, 192) : RGB(5, 2, 11));
  wc.lpszClassName = kClassName;
  RegisterClassExW(&wc);

  // CW_USEDEFAULT 交给系统放置时，窗口大小每次会被系统再调一遍，
  // 截图就一张大一张小；自己算居中位置，尺寸才稳定
  const int kWinW = 1180, kWinH = 820;
  int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
  int wx = (sw - kWinW) / 2, wy = (sh - kWinH) / 2;
  if (wx < 0) wx = 0;
  if (wy < 0) wy = 0;

  HWND hwnd = CreateWindowExW(0, kClassName, kTitle, WS_OVERLAPPEDWINDOW, wx, wy,
                              kWinW, kWinH, nullptr, nullptr, hInst, nullptr);
  if (!hwnd) return 1;
  ShowWindow(hwnd, SW_SHOW);

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
                            g_app.webview = webview;
                            g_app.webview->AddRef();
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
