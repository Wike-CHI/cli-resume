// 本地模型复盘：把会话的首尾喂给本机的 Ollama（或任何 OpenAI 兼容服务），
// 生成「做了 / 卡在 / 下一步」三行。
//
// 为什么不写死 Ollama：本机同时装过 Ollama 和 LM Studio，端口和协议各不相同。
// 这里按 /api/tags 和 /v1/models 自动判断，谁在跑就用谁。
#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "common.h"
#include "search.h"

namespace crg {

struct LlmStatus {
  bool ok = false;
  std::string api;  // "ollama" | "openai"
  std::string url;
  std::string model;  // 实际会用的模型（配置为空时自动挑）
  std::vector<std::string> models;
  std::string error;
};

class Summarizer {
 public:
  // dataDir: %LOCALAPPDATA%\CliResumeGui，摘要缓存落在它下面的 summary 子目录
  void start(const std::string& dataDir);

  // 探测哪个协议在跑。结果缓存 20 秒，避免每次点都打两枪。
  LlmStatus probe(const std::string& url, const std::string& model, bool force);

  // 返回给前端的 json：
  // { ok, text, model, models, cached, elapsedMs, llmOk, llmError, entries }
  // ok=false 只表示「没拿到会话」；模型不可用时 text 为空、llmOk=false，
  // 前端据此退回原始线索。
  json summarize(const SessionDigest& d, const std::string& url, const std::string& model,
                 bool force);

 private:
  std::string cachePath(const std::string& tool, const std::string& sid) const;
  bool readCache(const std::string& p, const SessionDigest& d, const std::string& model,
                 std::string& text) const;
  void writeCache(const std::string& p, const SessionDigest& d, const std::string& model,
                  const std::string& text) const;

  std::string dir_;
  std::mutex callMu_;   // 本机模型通常只有一个，串行调用，别同时压两份
  std::mutex probeMu_;
  int64_t probedAt_ = 0;
  std::string probedUrl_;
  std::string probedModel_;
  LlmStatus cachedProbe_;
};

}  // namespace crg
