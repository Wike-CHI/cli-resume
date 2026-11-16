// 扫描 codebuddy / claude 的会话历史，还原出「上次在哪些目录干活」。
// 会话文件里每条消息都带 cwd，所以不需要用户维护项目清单。
#pragma once

#include <string>
#include <vector>

#include "common.h"

namespace crg {

struct SessionRef {
  std::string tool;       // "codebuddy" | "claude"
  std::string sessionId;
  int64_t mtime = 0;
  std::string title;      // 会话标题：优先 claude/codebuddy 自己写的 ai-title，退回首条用户输入
};

struct Workspace {
  std::string cwd;
  std::string name;       // 末级目录名，前端当标题用
  int64_t lastActive = 0;
  int sessionCount = 0;   // 这个目录下一共有多少个历史会话（截断前的真实数量）
  std::vector<SessionRef> sessions;  // 按 mtime 倒序，最多 kMaxSessionsPerWs 条
};

// 一个目录下最多列这么多个会话，再多前端也翻不动
constexpr size_t kMaxSessionsPerWs = 60;

struct ScanOptions {
  int maxAgeHours = 24 * 30;  // 只看这么多天内活跃过的会话；<=0 表示不限
  bool includeCodebuddy = true;
  bool includeClaude = true;
  std::vector<std::string> exclude;  // 子串匹配，命中就跳过（大小写不敏感）
};

struct ScanResult {
  std::vector<Workspace> workspaces;
  int totalSessions = 0;
  int64_t elapsedMs = 0;
  std::string codebuddyRoot;
  std::string claudeRoot;
  bool codebuddyFound = false;
  bool claudeFound = false;
};

ScanResult scanAll(const ScanOptions& opts);

// 供自检用：把绝对路径按 claude 的规则编码成目录名（非字母数字 → '-'）
std::string encodeProjectDir(const std::string& p);

}  // namespace crg
