// 把「工作区 + 会话」翻译成 Windows Terminal 的一串标签页，并拉起来。
#pragma once

#include <string>
#include <vector>

#include "common.h"
#include "scanner.h"

namespace crg {

struct LaunchItem {
  std::string cwd;
  std::string tool;       // codebuddy | claude
  std::string sessionId;
  std::string title;
};

struct LaunchOptions {
  bool useContinue = false;  // 用 -c 让 CLI 自己找最近会话，而不是指定 id
  int window = 0;
  bool dryRun = false;
};

struct LaunchResult {
  bool ok = false;
  std::string error;
  std::string command;   // 便于前端展示/排错
  bool fallback = false; // wt 没转过去，改用普通控制台窗口开的
};

// 按工具拼出续接命令（sessionId 为空就是裸命令）。外部终端和独立窗口共用同一套规则
std::string buildResumeCommand(const std::string& tool, const std::string& sessionId);

// 一个工作区默认只恢复最近用过的那一个 CLI；openTwo 时两个都恢复
std::vector<LaunchItem> pickItems(const Workspace& ws, bool openTwo);

LaunchResult launch(const std::vector<LaunchItem>& items, const LaunchOptions& opts);

// 资源管理器定位到目录
bool revealInExplorer(const std::string& path);

}  // namespace crg
