// 开机自启：只写 HKCU 的 Run 键，不需要管理员权限，也不装服务。
#pragma once

#include <string>

namespace crg {

// 当前是否已登记（只看值在不在，不比对路径）
bool autostartEnabled();

// 打开时写入「"<exe>" --tray」，关掉时删除该值
bool setAutostart(bool on, std::string& err);

}  // namespace crg
