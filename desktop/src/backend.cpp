#include "backend.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

#include "launcher.h"
#include "scanner.h"

namespace crg {
namespace {

const char* mimeOf(const std::string& ext) {
  if (ext == "html" || ext == "htm") return "text/html; charset=utf-8";
  if (ext == "css") return "text/css; charset=utf-8";
  if (ext == "js") return "application/javascript; charset=utf-8";
  if (ext == "json") return "application/json; charset=utf-8";
  if (ext == "svg") return "image/svg+xml";
  if (ext == "png") return "image/png";
  if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
  if (ext == "ico") return "image/x-icon";
  if (ext == "woff2") return "font/woff2";
  return "application/octet-stream";
}

bool readWholeFile(const std::string& path, std::string& out) {
  std::ifstream f(utf8ToWide(path), std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

json defaultSettings() {
  return json{
      {"maxAgeHours", 24 * 30},
      {"openTwo", false},
      {"useContinue", false},
      {"window", 0},
      {"exclude", json::array({"\\temp\\", "\\system32", "\\node_modules\\", "\\appdata\\local\\temp"})},
  };
}

}  // namespace

Backend::Backend(std::string webRoot) : webRoot_(std::move(webRoot)) {
  const char* la = std::getenv("LOCALAPPDATA");
  std::string base = la && *la ? std::string(la) : (homeDir() + "\\AppData\\Local");
  settingsPath_ = base + "\\CliResumeGui\\settings.json";
  CreateDirectoryA((base + "\\CliResumeGui").c_str(), nullptr);
}

std::string Backend::url() const {
  return "http://127.0.0.1:" + std::to_string(port_) + "/";
}

json Backend::loadSettings() const {
  json def = defaultSettings();
  std::string raw;
  if (!readWholeFile(settingsPath_, raw) || raw.empty()) return def;
  try {
    json j = json::parse(raw);
    if (!j.is_object()) return def;
    for (auto it = def.begin(); it != def.end(); ++it) {
      auto hit = j.find(it.key());
      if (hit != j.end() && !hit->is_null()) def[it.key()] = *hit;
    }
  } catch (...) {
    // 配置坏了就用默认值，不要让界面起不来
  }
  return def;
}

void Backend::saveSettings(const json& s) const {
  std::ofstream f(utf8ToWide(settingsPath_), std::ios::binary | std::ios::trunc);
  if (!f) return;
  f << dumpJson(s);
}

json Backend::scanCached(int maxAgeHours, bool force) {
  std::lock_guard<std::mutex> lk(scanMu_);
  int64_t now = nowMs();
  // 界面刷新很频繁，扫描结果留 20 秒，避免每次点都重扫一遍
  if (!force && !cached_.empty() && cachedAge_ == maxAgeHours && now - cachedAt_ < 20000) {
    json out = cached_;
    out["cached"] = true;
    return out;
  }

  json settings = loadSettings();
  ScanOptions opts;
  opts.maxAgeHours = maxAgeHours;
  if (settings.contains("exclude") && settings["exclude"].is_array()) {
    for (const auto& e : settings["exclude"]) {
      if (e.is_string()) opts.exclude.push_back(e.get<std::string>());
    }
  }

  ScanResult r = scanAll(opts);

  json arr = json::array();
  for (const auto& ws : r.workspaces) {
    json sessions = json::array();
    for (const auto& s : ws.sessions) {
      sessions.push_back({{"tool", s.tool},
                          {"sessionId", s.sessionId},
                          {"mtime", s.mtime},
                          {"title", s.title}});
    }
    arr.push_back({{"cwd", ws.cwd},
                   {"name", ws.name},
                   {"lastActive", ws.lastActive},
                   {"sessionCount", ws.sessionCount},
                   {"sessions", sessions}});
  }

  json out = {{"workspaces", arr},
              {"stats", {{"totalSessions", r.totalSessions},
                         {"workspaces", (int)r.workspaces.size()},
                         {"elapsedMs", r.elapsedMs}}},
              {"roots", {{"codebuddy", r.codebuddyRoot},
                         {"claude", r.claudeRoot},
                         {"codebuddyFound", r.codebuddyFound},
                         {"claudeFound", r.claudeFound}}},
              {"scannedAt", nowMs()}};

  cached_ = out;
  cachedAt_ = now;
  cachedAge_ = maxAgeHours;
  out["cached"] = false;
  return out;
}

bool Backend::handleStatic(const std::string& path, httplib::Response& res) {
  std::string p = path;
  if (p == "/" || p.empty()) p = "/index.html";
  if (p.find("..") != std::string::npos) return false;

  std::string full = webRoot_ + p;
  std::replace(full.begin(), full.end(), '/', '\\');
  std::string body;
  if (!readWholeFile(full, body)) return false;

  size_t dot = p.find_last_of('.');
  std::string ext = (dot == std::string::npos) ? "" : p.substr(dot + 1);
  res.set_header("Cache-Control", "no-store");
  res.set_content(body, mimeOf(ext));
  return true;
}

void Backend::setupRoutes() {
  srv_.Get("/api/health", [](const httplib::Request&, httplib::Response& res) {
    res.set_content("{\"ok\":true}", "application/json; charset=utf-8");
  });

  srv_.Get("/api/scan", [this](const httplib::Request& req, httplib::Response& res) {
    int maxAge = 24 * 30;
    bool force = false;
    try {
      if (req.has_param("maxAge")) maxAge = std::stoi(req.get_param_value("maxAge"));
      if (req.has_param("refresh")) force = req.get_param_value("refresh") == "1";
    } catch (...) {
    }
    json out = scanCached(maxAge, force);
    out["settings"] = loadSettings();
    res.set_header("Cache-Control", "no-store");
    res.set_content(dumpJson(out), "application/json; charset=utf-8");
  });

  srv_.Post("/api/open", [this](const httplib::Request& req, httplib::Response& res) {
    json out;
    try {
      json body = json::parse(req.body);
      json items = jval(body, "items", json::array());
      std::vector<LaunchItem> list;
      if (items.is_array()) {
        for (const auto& it : items) {
          LaunchItem li;
          li.cwd = jstr(it, "cwd");
          li.tool = jstr(it, "tool", "codebuddy");
          li.sessionId = jstr(it, "sessionId");
          li.title = jstr(it, "title");
          if (li.cwd.empty()) continue;
          if (li.title.empty()) {
            size_t pos = li.cwd.find_last_of("\\/");
            li.title = (pos == std::string::npos ? li.cwd : li.cwd.substr(pos + 1)) + " · " + li.tool;
          }
          list.push_back(li);
        }
      }
      LaunchOptions opts;
      opts.useContinue = jval(body, "useContinue", json(false)).get<bool>();
      opts.window = jval(body, "window", json(0)).get<int>();
      opts.dryRun = jval(body, "dryRun", json(false)).get<bool>();

      LaunchResult r = launch(list, opts);
      out = {{"ok", r.ok}, {"error", r.error}, {"command", r.command},
             {"count", (int)list.size()}};
    } catch (const std::exception& e) {
      out = {{"ok", false}, {"error", std::string("请求解析失败：") + e.what()}};
    }
    res.set_content(dumpJson(out), "application/json; charset=utf-8");
  });

  srv_.Post("/api/reveal", [](const httplib::Request& req, httplib::Response& res) {
    json out = {{"ok", false}};
    try {
      json body = json::parse(req.body);
      std::string path = jstr(body, "path");
      out["ok"] = !path.empty() && revealInExplorer(path);
    } catch (...) {
    }
    res.set_content(dumpJson(out), "application/json; charset=utf-8");
  });

  srv_.Get("/api/settings", [this](const httplib::Request&, httplib::Response& res) {
    res.set_content(dumpJson(loadSettings()), "application/json; charset=utf-8");
  });

  srv_.Post("/api/settings", [this](const httplib::Request& req, httplib::Response& res) {
    json out = {{"ok", false}};
    try {
      json body = json::parse(req.body);
      json current = loadSettings();
      for (auto it = body.begin(); it != body.end(); ++it) current[it.key()] = it.value();
      saveSettings(current);
      // 设置改了，缓存作废
      {
        std::lock_guard<std::mutex> lk(scanMu_);
        cachedAt_ = 0;
      }
      out = {{"ok", true}, {"settings", current}};
    } catch (const std::exception& e) {
      out["error"] = e.what();
    }
    res.set_content(dumpJson(out), "application/json; charset=utf-8");
  });

  srv_.Post("/api/log", [](const httplib::Request& req, httplib::Response& res) {
    logf("[page] " + req.body.substr(0, 400));
    res.set_content("{}", "application/json; charset=utf-8");
  });

  srv_.Get(R"(/.*)", [this](const httplib::Request& req, httplib::Response& res) {
    if (!handleStatic(req.path, res)) {
      res.status = 404;
      res.set_content("not found", "text/plain; charset=utf-8");
    }
  });
}

bool Backend::start() {
  setupRoutes();
  for (int p = 8712; p < 8760; ++p) {
    if (srv_.bind_to_port("127.0.0.1", p)) {
      port_ = p;
      break;
    }
  }
  if (port_ == 0) {
    logf("没有可用端口");
    return false;
  }
  // 已经 bind 过了，必须用 listen_after_bind；再调 listen 会重复 bind 失败，
  // 结果是端口看着占上了但根本没有 accept 循环，页面要等超时才能加载。
  std::thread([this] { srv_.listen_after_bind(); }).detach();
  Sleep(60);  // 等 accept 循环真正跑起来，避免首个请求打空
  logf("http listening on 127.0.0.1:" + std::to_string(port_));
  return true;
}

void Backend::stop() { srv_.stop(); }

}  // namespace crg
