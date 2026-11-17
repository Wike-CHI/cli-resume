#include "stats.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <map>
#include <string>

namespace crg {
namespace {

struct Tot {
  int64_t in = 0;
  int64_t out = 0;
  int64_t cacheRead = 0;
  int64_t cacheWrite = 0;
  int64_t calls = 0;
  double cost = 0;
};

int64_t tokensOf(const Tot& t) {
  return t.in + t.out + t.cacheRead + t.cacheWrite;
}

std::string dayStr(std::time_t t) {
  std::tm local{};
  localtime_s(&local, &t);
  char buf[16] = {0};
  std::strftime(buf, sizeof(buf), "%Y-%m-%d", &local);
  return buf;
}

std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// 只放能对外确认的价目（美元 / 百万 token），按 Anthropic 公开价。
// 订阅制或本地模型（kimi-for-coding、deepseek、qwen…）没有可用的按量价，留空。
struct Bench {
  const char* key;
  ModelPrice p;
};

const Bench kPrices[] = {
    {"opus", {15.0, 75.0, 1.5, 18.75}},
    {"sonnet", {3.0, 15.0, 0.3, 3.75}},
    {"haiku", {1.0, 5.0, 0.1, 1.25}},
};

void addTo(Tot& t, const UsageBucket& b) {
  t.in += b.in;
  t.out += b.out;
  t.cacheRead += b.cacheRead;
  t.cacheWrite += b.cacheWrite;
  t.calls += b.calls;
  ModelPrice p;
  if (priceOf(b.model, p)) {
    t.cost += (static_cast<double>(b.in) * p.in + static_cast<double>(b.out) * p.out +
               static_cast<double>(b.cacheRead) * p.cacheRead +
               static_cast<double>(b.cacheWrite) * p.cacheWrite) /
              1e6;
  }
}

json totJson(const Tot& t) {
  return json{{"in", t.in},
              {"out", t.out},
              {"cacheRead", t.cacheRead},
              {"cacheWrite", t.cacheWrite},
              {"calls", t.calls},
              {"tokens", tokensOf(t)},
              {"cost", t.cost}};
}

}  // namespace

bool priceOf(const std::string& model, ModelPrice& p) {
  const std::string m = lower(model);
  for (const auto& b : kPrices) {
    if (m.find(b.key) != std::string::npos) {
      p = b.p;
      return true;
    }
  }
  return false;
}

json buildStats(const std::vector<DocUsage>& all, int days, bool ready, int files, int totalFiles) {
  if (days < 1) days = 1;
  if (days > 365) days = 365;

  const std::time_t now = std::time(nullptr);
  std::vector<std::string> dayList;
  dayList.reserve(days);
  for (int i = days - 1; i >= 0; --i) {
    dayList.push_back(dayStr(now - static_cast<std::time_t>(i) * 86400));
  }
  const std::string from = dayList.front();

  std::map<std::string, Tot> byDay;
  std::map<std::string, Tot> byModel;
  Tot total;
  std::vector<std::pair<std::string, Tot>> sessions;

  for (const auto& d : all) {
    Tot self;
    bool touched = false;
    for (const auto& b : d.buckets) {
      if (b.day < from) continue;  // ISO 日期串可以直接比大小
      touched = true;
      addTo(byDay[b.day], b);
      addTo(byModel[b.model], b);
      addTo(self, b);
    }
    if (!touched) continue;
    total.in += self.in;
    total.out += self.out;
    total.cacheRead += self.cacheRead;
    total.cacheWrite += self.cacheWrite;
    total.calls += self.calls;
    total.cost += self.cost;
    sessions.emplace_back(d.sessionId + "\x1f" + d.tool, self);
  }

  // 按天补齐空白天，图上的横轴才是连续的
  json daily = json::array();
  for (const auto& day : dayList) {
    const Tot& t = byDay[day];
    json o = totJson(t);
    o["d"] = day;
    daily.push_back(o);
  }

  json models = json::array();
  std::vector<std::string> unpriced;
  for (const auto& kv : byModel) {
    if (tokensOf(kv.second) == 0) continue;  // <synthetic> 这类占位行不值得占一行
    ModelPrice p;
    const bool priced = priceOf(kv.first, p);
    if (!priced && tokensOf(kv.second) > 0) unpriced.push_back(kv.first);
    json o = totJson(kv.second);
    o["model"] = kv.first;
    o["priced"] = priced;
    models.push_back(o);
  }
  std::sort(models.begin(), models.end(), [](const json& a, const json& b) {
    return a["tokens"].get<int64_t>() > b["tokens"].get<int64_t>();
  });

  // 会话排行：只留前 8，够看出钱花在哪
  std::sort(sessions.begin(), sessions.end(), [](const auto& a, const auto& b) {
    return tokensOf(a.second) > tokensOf(b.second);
  });
  std::map<std::string, const DocUsage*> meta;
  for (const auto& d : all) meta[d.sessionId + "\x1f" + d.tool] = &d;

  json top = json::array();
  for (size_t i = 0; i < sessions.size() && i < 8; ++i) {
    const auto& key = sessions[i].first;
    auto it = meta.find(key);
    if (it == meta.end()) continue;
    const DocUsage* d = it->second;
    json o = totJson(sessions[i].second);
    o["sessionId"] = d->sessionId;
    o["tool"] = d->tool;
    o["cwd"] = d->cwd;
    o["title"] = d->title;
    top.push_back(o);
  }

  std::sort(unpriced.begin(), unpriced.end());
  json out = {{"ok", true},
              {"ready", ready},
              {"files", files},
              {"totalFiles", totalFiles},
              {"days", days},
              {"from", from},
              {"to", dayList.back()},
              {"total", totJson(total)},
              {"daily", daily},
              {"models", models},
              {"top", top},
              {"unpriced", unpriced}};
  return out;
}

}  // namespace crg
