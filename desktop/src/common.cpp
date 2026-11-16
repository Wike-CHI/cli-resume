#include "common.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>

namespace crg {

std::wstring utf8ToWide(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring out(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
  return out;
}

std::string wideToUtf8(const std::wstring& s) {
  if (s.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
  std::string out(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n, nullptr, nullptr);
  return out;
}

int64_t nowMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string utf8Truncate(const std::string& s, size_t maxBytes) {
  if (s.size() <= maxBytes) return s;
  size_t cut = maxBytes;
  while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
  return s.substr(0, cut);
}

std::string dumpJson(const json& j) {
  return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

std::string jstr(const json& o, const char* key, const std::string& def) {
  if (!o.is_object()) return def;
  auto it = o.find(key);
  if (it == o.end() || !it->is_string()) return def;
  return it->get<std::string>();
}

json jval(const json& o, const char* key, json def) {
  if (!o.is_object()) return def;
  auto it = o.find(key);
  if (it == o.end() || it->is_null()) return def;
  return *it;
}

std::string homeDir() {
  const char* up = std::getenv("USERPROFILE");
  if (up && *up) return std::string(up);
  char buf[MAX_PATH] = {0};
  if (GetEnvironmentVariableA("USERPROFILE", buf, MAX_PATH) > 0) return std::string(buf);
  return "C:\\Users\\Default";
}

namespace {
std::mutex g_logMu;
FILE* g_logFile = nullptr;
}  // namespace

void logInit(const std::string& filePath) {
  std::lock_guard<std::mutex> lk(g_logMu);
  if (g_logFile) return;
  // _SH_DENYWR：允许别的进程读，只独占写，方便排查
  g_logFile = _fsopen(filePath.c_str(), "a", _SH_DENYWR);
}

void logf(const std::string& msg) {
  char stamp[32] = {0};
  std::time_t t = std::time(nullptr);
  std::tm tmv{};
  localtime_s(&tmv, &t);
  std::strftime(stamp, sizeof(stamp), "%H:%M:%S", &tmv);
  std::string line = std::string("[") + stamp + "] " + msg + "\n";
  OutputDebugStringA(line.c_str());
  std::lock_guard<std::mutex> lk(g_logMu);
  if (g_logFile) {
    std::fputs(line.c_str(), g_logFile);
    std::fflush(g_logFile);
  }
}

}  // namespace crg
