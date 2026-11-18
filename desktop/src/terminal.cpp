#include "terminal.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif

namespace crg {
namespace {

// 缓冲上限：刚打开时的 banner 就那么点，留 512KB 足够，也防止长期不连把内存吃掉
constexpr size_t kPendingMax = 512 * 1024;

// 子进程挂在 job 上：应用被强杀时 ConPTY 里的 node 不留在后台
HANDLE g_job = nullptr;

void ensureJob() {
  if (g_job) return;
  g_job = CreateJobObjectW(nullptr, nullptr);
  if (!g_job) return;
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
  li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &li, sizeof(li));
}

// 继承当前环境，补上终端变量；同名旧值先剔掉，否则 SetEnvironmentVariable 追加不生效
std::vector<wchar_t> envBlock() {
  std::vector<std::wstring> kv;
  if (LPWCH env = GetEnvironmentStringsW()) {
    for (LPWCH p = env; *p;) {
      std::wstring s(p);
      p += s.size() + 1;
      if (s.rfind(L"TERM=", 0) == 0 || s.rfind(L"COLORTERM=", 0) == 0) continue;
      kv.push_back(s);
    }
    FreeEnvironmentStringsW(env);
  }
  kv.push_back(L"TERM=xterm-256color");
  kv.push_back(L"COLORTERM=truecolor");
  std::vector<wchar_t> out;
  for (const auto& s : kv) {
    out.insert(out.end(), s.begin(), s.end());
    out.push_back(0);
  }
  out.push_back(0);
  return out;
}

std::wstring shellPath() {
  wchar_t buf[MAX_PATH * 2];
  if (SearchPathW(nullptr, L"pwsh.exe", nullptr, MAX_PATH * 2, buf, nullptr) > 0) return buf;
  if (SearchPathW(nullptr, L"powershell.exe", nullptr, MAX_PATH * 2, buf, nullptr) > 0) return buf;
  return L"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
}

// 起哪个 shell：cmd 用 /k 留在里面，pwsh / powershell 用 -NoLogo
std::wstring buildCommandLine(const std::string& shell) {
  wchar_t sys[MAX_PATH] = L"C:\\Windows\\System32";
  GetSystemDirectoryW(sys, MAX_PATH);
  if (shell == "cmd") return L"\"" + std::wstring(sys) + L"\\cmd.exe\" /k";
  if (shell == "powershell") {
    return L"\"" + std::wstring(sys) + L"\\WindowsPowerShell\\v1.0\\powershell.exe\" -NoLogo";
  }
  return L"\"" + shellPath() + L"\" -NoLogo";
}

std::string newId() {
  static std::atomic<uint32_t> seq{0};
  char buf[32];
  std::snprintf(buf, sizeof buf, "t%04x%04x", (unsigned)(GetTickCount() & 0xFFFF),
                (unsigned)(seq.fetch_add(1) & 0xFFFF));
  return buf;
}

COORD clampSize(int cols, int rows) {
  return COORD{(SHORT)std::max(20, std::min(500, cols)), (SHORT)std::max(5, std::min(300, rows))};
}

}  // namespace

TermSession::~TermSession() { stop(); }

bool TermSession::start(const std::string& cwd, const std::string& cmd,
                        const std::string& shell, int cols, int rows) {
  ensureJob();
  id = newId();

  SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  HANDLE inR = nullptr, outW = nullptr;
  if (!CreatePipe(&inR, &inW_, &sa, 0)) return false;
  if (!CreatePipe(&outR_, &outW, &sa, 0)) {
    CloseHandle(inR);
    CloseHandle(inW_);
    inW_ = nullptr;
    return false;
  }

  HRESULT hr = CreatePseudoConsole(clampSize(cols, rows), inR, outW, 0, &hpc_);
  if (FAILED(hr)) {
    CloseHandle(inR);
    CloseHandle(outW);
    logf("CreatePseudoConsole failed hr=" + std::to_string((long long)hr));
    return false;
  }

  STARTUPINFOEXW si{};
  si.StartupInfo.cb = sizeof(si);
  // 这一组是全功能的开关：不设 STARTF_USESTDHANDLES，子进程会继承宿主进程的
  // 标准句柄，shell 的 banner 直接写进宿主 stdout，伪终端里则一片空白
  si.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
  si.StartupInfo.hStdInput = nullptr;
  si.StartupInfo.hStdOutput = nullptr;
  si.StartupInfo.hStdError = nullptr;
  SIZE_T bytes = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
  si.lpAttributeList =
      (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0, bytes ? bytes : 1);
  if (!si.lpAttributeList) return false;
  InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &bytes);
  // 伪终端句柄要传值本身（系统按 HPCON 读），不是它的地址
  BOOL upOk = UpdateProcThreadAttribute(si.lpAttributeList, 0,
                                        PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hpc_,
                                        sizeof(HPCON), nullptr, nullptr);
  (void)upOk;

  std::wstring line = buildCommandLine(shell);
  std::wstring wcwd = utf8ToWide(cwd.empty() ? homeDir() : cwd);
  std::vector<wchar_t> env = envBlock();
  DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT;
  BOOL ok = CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, flags, env.data(),
                           wcwd.c_str(), &si.StartupInfo, &pi_);
  // 伪终端已经交给子进程了，本地这对句柄留到出进程之后再放
  CloseHandle(inR);
  CloseHandle(outW);

  DeleteProcThreadAttributeList(si.lpAttributeList);
  HeapFree(GetProcessHeap(), 0, si.lpAttributeList);
  if (!ok) {
    logf("CreateProcess for conpty failed err=" + std::to_string(GetLastError()));
    return false;
  }
  if (g_job) AssignProcessToJobObject(g_job, pi_.hProcess);

  th_ = std::thread([this] { readLoop(); });

  bootCmd_ = cmd;
  if (!bootCmd_.empty()) {
    // 正常路径是 readLoop 收到第一段输出（提示符）后再灌命令；
    // 这里再放一个兜底，避免某些 shell 迟迟不打提示符导致命令永远发不出去
    auto self = shared_from_this();
    std::thread([self] {
      std::this_thread::sleep_for(std::chrono::milliseconds(1500));
      self->flushBoot();
    }).detach();
  }
  return true;
}

void TermSession::flushBoot() {
  std::string cmd;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (bootSent_ || bootCmd_.empty()) return;
    bootSent_ = true;
    cmd = bootCmd_;
  }
  write(cmd + "\r");
}

void TermSession::readLoop() {
  char buf[8192];
  bool first = true;
  for (;;) {
    DWORD n = 0;
    BOOL ok = ReadFile(outR_, buf, sizeof(buf), &n, nullptr);
    if (!ok || n == 0) break;
    push(buf, n);
    if (first) {
      first = false;
      // 提示符出来了，shell 已经能接受命令
      flushBoot();
    }
  }
  DWORD code = 0;
  GetExitCodeProcess(pi_.hProcess, &code);
  logf("term readLoop end id=" + id + " exit=" + std::to_string(code));
  exited_.store(true);
  cv_.notify_all();
}

void TermSession::push(const char* p, size_t n) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    pending_.append(p, n);
    if (pending_.size() > kPendingMax) {
      // 不能从中间截断：半个 ANSI 序列会让 xterm 的解析器停在「等后续字节」的状态，
      // 之后所有输出都被当成那个序列的一部分吞掉——表现就是终端彻底不动了。
      // 整段丢弃 + 显式重置，宁可少一段历史，也不要让画面死住
      pending_.assign("\r\n\x1b[0m\x1b[90m[输出过多，前面一段已丢弃]\x1b[0m\r\n");
    }
  }
  cv_.notify_one();
}

std::string TermSession::drain(int timeoutMs) {
  std::unique_lock<std::mutex> lk(mu_);
  if (pending_.empty() && !exited_.load()) {
    cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                 [&] { return !pending_.empty() || exited_.load(); });
  }
  std::string out;
  out.swap(pending_);
  if (out.empty() && exited_.load() && !exitSent_) {
    exitSent_ = true;
    out = "\r\n\x1b[90m[进程已退出]\x1b[0m\r\n";
  }
  return out;
}

void TermSession::write(const std::string& bytes) {
  std::lock_guard<std::mutex> lk(writeMu_);
  if (!inW_) return;
  DWORD n = 0;
  WriteFile(inW_, bytes.data(), (DWORD)bytes.size(), &n, nullptr);
}

void TermSession::resize(int cols, int rows) {
  // 单独一把锁：ResizePseudoConsole 偶尔会在 conhost 里卡一下，
  // 和写输入共用一把锁的话，缩放的瞬间键盘输入就跟着停了
  std::lock_guard<std::mutex> lk(resizeMu_);
  if (!hpc_) return;
  ResizePseudoConsole(hpc_, clampSize(cols, rows));
}

void TermSession::stop() {
  if (stopping_.exchange(true)) return;
  // 先让 shell 走正常退出路径，再由 ClosePseudoConsole 收尾
  if (pi_.hProcess) TerminateProcess(pi_.hProcess, 0);
  if (hpc_) {
    ClosePseudoConsole(hpc_);
    hpc_ = nullptr;
  }
  {
    std::lock_guard<std::mutex> lk(writeMu_);
    if (inW_) {
      CloseHandle(inW_);
      inW_ = nullptr;
    }
  }
  if (pi_.hProcess) {
    WaitForSingleObject(pi_.hProcess, 1000);
    CloseHandle(pi_.hThread);
    CloseHandle(pi_.hProcess);
    pi_ = {};
  }
  if (th_.joinable()) th_.join();
  if (outR_) {
    CloseHandle(outR_);
    outR_ = nullptr;
  }
  cv_.notify_all();
}

// ---------------- 管理器 ----------------

std::shared_ptr<TermSession> TermManager::open(const std::string& cwd, const std::string& cmd,
                                              const std::string& shell, const std::string& title,
                                              int cols, int rows, std::string& err) {
  // 布局里记的目录可能已经被删了。以前这种情况每次启动都是一扇起不来的死窗格，
  // 看着就像「终端坏了」——退回主目录，至少还能用
  std::string useCwd = cwd;
  if (!useCwd.empty()) {
    const DWORD a = GetFileAttributesW(utf8ToWide(useCwd).c_str());
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
      logf("term cwd missing, fallback to home: " + useCwd);
      useCwd.clear();  // start() 里空 cwd 就是用户主目录
    }
  }

  auto s = std::make_shared<TermSession>();
  if (!s->start(useCwd, cmd, shell, cols, rows)) {
    err = "终端启动失败：ConPTY 或 shell 创建不出来";
    return nullptr;
  }
  s->title = title;
  std::lock_guard<std::mutex> lk(mu_);
  map_[s->id] = s;
  logf("term open id=" + s->id + " cwd=" + useCwd + " cmd_len=" + std::to_string(cmd.size()));
  return s;
}

std::shared_ptr<TermSession> TermManager::find(const std::string& id) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = map_.find(id);
  return it == map_.end() ? nullptr : it->second;
}

void TermManager::close(const std::string& id) {
  std::shared_ptr<TermSession> s;
  {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = map_.find(id);
    if (it == map_.end()) return;
    s = it->second;
    map_.erase(it);
  }
  s->stop();
  logf("term close id=" + id);
}

void TermManager::closeAll() {
  std::map<std::string, std::shared_ptr<TermSession>> copy;
  {
    std::lock_guard<std::mutex> lk(mu_);
    copy.swap(map_);
  }
  for (auto& kv : copy) kv.second->stop();
}

}  // namespace crg
