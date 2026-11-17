// 会话收藏：跨窗口共享，落在 %LOCALAPPDATA%\CliResumeGui\favorites.json。
// 收藏是「置顶」的唯一依据：同一个目录里，收藏的会话排在前面。
#pragma once

#include <mutex>
#include <string>

#include "common.h"

namespace crg {

class Favorites {
 public:
  void start(const std::string& dataDir);

  json list() const;

  // 切换收藏，on 返回切换后的状态。key 是 tool + sessionId
  bool toggle(const std::string& tool, const std::string& sid, const std::string& cwd,
              const std::string& title, bool& on);

 private:
  json load() const;
  void save(const json& j) const;

  std::string path_;
  mutable std::mutex mu_;
};

}  // namespace crg
