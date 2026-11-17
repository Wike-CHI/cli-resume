// 会话导出 Markdown：重读源 jsonl，把一次会话还原成能直接读的稿子。
// 不从索引取，因为索引按搜索需要截断到 700 字，导出要全文。
#pragma once

#include <string>

#include "common.h"

namespace crg {

struct ExportResult {
  bool ok = false;
  std::string path;
  std::string error;
  int turns = 0;
  int64_t bytes = 0;
};

// tool: codebuddy / claude；sid: 会话 id。
// cwd/title 只用于命名和表头兜底，找不到会话时不会因此失败。
ExportResult exportSession(const std::string& tool, const std::string& sid,
                           const std::string& cwd, const std::string& title);

}  // namespace crg
