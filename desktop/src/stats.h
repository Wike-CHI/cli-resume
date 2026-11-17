// token / 成本统计：把索引里按天分好的用量汇总成前端要的形状。
// 计价只覆盖查得到公开单价的模型（Claude 系列），其余只算 token 不算钱，
// 免得给一个看似精确、其实是编出来的数字。
#pragma once

#include <string>
#include <vector>

#include "common.h"
#include "search.h"

namespace crg {

struct ModelPrice {
  double in = 0;          // 美元 / 百万 token
  double out = 0;
  double cacheRead = 0;
  double cacheWrite = 0;
};

// 按模型名里的关键词匹配；返回 false 表示这个模型没有公开单价
bool priceOf(const std::string& model, ModelPrice& p);

json buildStats(const std::vector<DocUsage>& all, int days, bool ready, int files, int totalFiles);

}  // namespace crg
