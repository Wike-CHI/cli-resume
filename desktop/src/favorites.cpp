#include "favorites.h"

#include <fstream>
#include <sstream>

namespace crg {

void Favorites::start(const std::string& dataDir) {
  path_ = dataDir + "\\favorites.json";
}

json Favorites::load() const {
  json out = json::object();
  out["items"] = json::array();
  std::ifstream f(utf8ToWide(path_), std::ios::binary);
  if (!f) return out;
  std::ostringstream ss;
  ss << f.rdbuf();
  try {
    json j = json::parse(ss.str());
    if (j.is_object() && j.contains("items") && j["items"].is_array()) return j;
  } catch (...) {
    // 文件坏了就当没有收藏，不要拦住启动
  }
  return out;
}

void Favorites::save(const json& j) const {
  // 先写临时文件再替换：多个窗口同时收藏时，读到的要么是旧的要么是新的，
  // 不会读到写了一半的 JSON
  const std::string tmp = path_ + ".tmp";
  {
    std::ofstream f(utf8ToWide(tmp), std::ios::binary | std::ios::trunc);
    if (!f) return;
    f << dumpJson(j);
  }
  MoveFileExW(utf8ToWide(tmp).c_str(), utf8ToWide(path_).c_str(), MOVEFILE_REPLACE_EXISTING);
}

json Favorites::list() const {
  std::lock_guard<std::mutex> lk(mu_);
  json j = load();
  return json{{"ok", true}, {"items", j["items"]}};
}

bool Favorites::toggle(const std::string& tool, const std::string& sid, const std::string& cwd,
                       const std::string& title, bool& on) {
  if (sid.empty()) return false;
  std::lock_guard<std::mutex> lk(mu_);
  json j = load();
  json items = json::array();
  bool found = false;
  bool removed = false;
  for (const auto& it : j["items"]) {
    if (jstr(it, "tool") == tool && jstr(it, "sessionId") == sid) {
      found = true;
      removed = true;  // 再点一次就是取消收藏
      continue;
    }
    items.push_back(it);
  }
  if (!found) {
    items.push_back({{"tool", tool},
                     {"sessionId", sid},
                     {"cwd", cwd},
                     {"title", title},
                     {"at", nowMs()}});
  }
  save(json{{"items", items}});
  on = !removed;
  return true;
}

}  // namespace crg
