// 本地 HTTP 服务：扫描结果、恢复动作、设置读写、静态页面。
#pragma once

#include <mutex>
#include <string>

#include "common.h"
#include "httplib.h"
#include "search.h"
#include "summary.h"
#include "terminal.h"

namespace crg {

class Backend {
 public:
  explicit Backend(std::string webRoot);
  bool start();
  void stop();
  int port() const { return port_; }
  std::string url() const;
  std::string theme() const;  // 当前外观主题，决定窗口底色

 private:
  void setupRoutes();
  json scanCached(int maxAgeHours, bool force);
  json loadSettings() const;
  void saveSettings(const json& s) const;
  // 终端布局（开着哪几个窗格、各自在哪个目录、面板多高）单独存一个文件：
  // 它每开一个窗格就写一次，和「用户点保存」才变的设置不是一回事
  json loadLayout() const;
  void saveLayout(const json& l) const;
  bool handleStatic(const std::string& path, httplib::Response& res);

  httplib::Server srv_;
  std::string webRoot_;
  std::string settingsPath_;
  std::string layoutPath_;
  std::string dataDir_;
  int port_ = 0;
  TermManager terms_;
  SearchIndex search_;
  Summarizer summary_;

  std::mutex scanMu_;
  json cached_ = json::object();
  int64_t cachedAt_ = 0;
  int cachedAge_ = -1;
};

}  // namespace crg
