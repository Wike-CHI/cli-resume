// 内嵌终端：ConPTY 起一个真实 shell，输出以字节流推给前端的 xterm.js。
// 和 launcher.cpp 的区别是「不开新窗口」——进程由应用自己持有，关掉面板即回收。
#pragma once

// CreatePseudoConsole 只在 _WIN32_WINNT >= 0x0A00 时由 windows.h 声明，
// 这行必须早于 common.h（它里面 include 了 windows.h）
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "common.h"

namespace crg {

class TermSession : public std::enable_shared_from_this<TermSession> {
 public:
  ~TermSession();

  // cwd 为空时用用户主目录；cmd 为空就只开一个 shell
  bool start(const std::string& cwd, const std::string& cmd, const std::string& shell,
             int cols, int rows);
  void write(const std::string& bytes);
  void resize(int cols, int rows);
  void stop();

  // 取走积压的输出（首次连接时就是启动阶段的 banner + 提示符）。
  // timeoutMs 内没数据且进程还在就返回空串；进程退出后补一次提示，只给一次。
  std::string drain(int timeoutMs);
  bool exited() const { return exited_.load(); }

  std::string id;
  std::string title;
  // 谁在接管输出：新连接进来就把代号加一，旧连接的泵线程看到代号变了会自己退出，
  // 避免两个客户端同时 drain 把输出切碎
  std::atomic<int> attachGen_{0};

 private:
  void readLoop();
  void push(const char* p, size_t n);
  // shell 起来之后再灌命令，否则 pwsh 可能把第一行当成启动参数丢掉
  void flushBoot();

  HPCON hpc_ = nullptr;
  HANDLE inW_ = nullptr;
  HANDLE outR_ = nullptr;
  PROCESS_INFORMATION pi_{};
  std::thread th_;

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::string pending_;
  std::string bootCmd_;
  bool bootSent_ = false;
  std::atomic<bool> exited_{false};
  bool exitSent_ = false;
  std::atomic<bool> stopping_{false};

  std::mutex writeMu_;
  std::mutex resizeMu_;
};

class TermManager {
 public:
  // 开一个会话，失败时 err 里带原因
  std::shared_ptr<TermSession> open(const std::string& cwd, const std::string& cmd,
                                    const std::string& shell, const std::string& title,
                                    int cols, int rows, std::string& err);
  std::shared_ptr<TermSession> find(const std::string& id);
  void close(const std::string& id);
  void closeAll();

 private:
  std::mutex mu_;
  std::map<std::string, std::shared_ptr<TermSession>> map_;
};

}  // namespace crg
