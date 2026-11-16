// 跨会话全文搜索：把两个 CLI 落盘的 jsonl 正文抽出来，在本地建一份索引。
//
// 索引放在 %LOCALAPPDATA%\CliResumeGui\search\ 下，一个会话一个文件：
// 第一行是元信息（cwd / 标题 / 源文件 mtime 和大小），之后每行一条消息片段。
// 这样重启时只要源文件没动过，连读都不用读，直接信索引里的 mtime。
//
// 只收「人说的话」和助手的回答，tool_result（整屏命令输出）不进索引 —— 那部分
// 占了文件体积的八九成，搜出来也没有用。
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "common.h"

namespace crg {

struct SearchHit {
  std::string cwd;
  std::string title;
  std::string tool;
  std::string sessionId;
  std::string role;   // "user" | "asst" | "tool"
  int64_t ts = 0;
  int line = 0;
  // 命中点拆成三段返回，前端拼 <mark> 时不用按字节切 UTF-8
  std::string before;
  std::string hit;
  std::string after;
};

struct SearchReply {
  std::vector<SearchHit> hits;
  int matchedSessions = 0;
  int totalMatches = 0;
  int64_t elapsedMs = 0;
  bool ready = false;
  int files = 0;       // 已处理的会话文件
  int totalFiles = 0;  // 总共要处理多少个
  int entries = 0;     // 索引里有多少条消息
};

class SearchIndex {
 public:
  ~SearchIndex();

  // dataDir: %LOCALAPPDATA%\CliResumeGui，索引落在它下面的 search 子目录
  void start(const std::string& dataDir);
  void stop();

  SearchReply query(const std::string& q, int limit) const;

  bool ready() const { return ready_.load(); }
  int files() const { return done_.load(); }
  int totalFiles() const { return total_.load(); }

 private:
  struct Entry {
    std::string role;
    int64_t ts = 0;
    int line = 0;
    std::string text;
  };
  struct Doc {
    std::string tool;
    std::string sessionId;
    std::string cwd;
    std::string title;
    int64_t mtime = 0;
    int64_t size = 0;
    int64_t ts = 0;
    std::vector<Entry> entries;
  };

  struct Src {
    std::wstring path;
    std::string tool;
    std::string sessionId;
    int64_t mtime = 0;
    int64_t size = 0;
  };

  void run();
  void loadIndexFiles();
  static void parseSource(const Src& src, Doc& doc);
  void writeIndexFile(const Doc& doc) const;
  std::string indexPath(const std::string& key) const;
  static std::string keyOf(const std::string& tool, const std::string& sid);

  std::string dir_;
  mutable std::mutex mu_;
  std::unordered_map<std::string, Doc> docs_;
  std::thread worker_;
  std::atomic<bool> stopFlag_{false};
  std::atomic<bool> ready_{false};
  std::atomic<int> done_{0};
  std::atomic<int> total_{0};
};

}  // namespace crg
