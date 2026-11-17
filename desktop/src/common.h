// 公共依赖与基础设施：日志、编码转换、安全 JSON 取值。
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "json.hpp"

using json = nlohmann::json;

namespace crg {

std::wstring utf8ToWide(const std::string& s);
std::string wideToUtf8(const std::wstring& s);
int64_t nowMs();

// 按 UTF-8 字符边界截断，避免切出半个汉字
std::string utf8Truncate(const std::string& s, size_t maxBytes);
// 序列化 JSON：遇到非法 UTF-8 用替换符兜底，绝不抛异常
std::string dumpJson(const json& j);
// 读字符串字段：字段缺失 / 为 null / 类型不符时回退默认值（json::value 遇 null 会抛）
std::string jstr(const json& o, const char* key, const std::string& def = "");
// 读任意字段：缺失或为 null 时回退默认值
json jval(const json& o, const char* key, json def = json());

void logInit(const std::string& filePath);
void logf(const std::string& msg);

// 用户主目录（C:\Users\xxx），不带结尾斜杠
std::string homeDir();

// 8 位十六进制短哈希。给「一个目录一个窗口」做稳定 id：
// 同一个目录永远落到同一个窗口，换进程也不变
std::string shortHash(const std::string& s);

}  // namespace crg
